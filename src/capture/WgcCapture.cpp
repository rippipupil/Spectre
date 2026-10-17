// Captura con Windows.Graphics.Capture: API oficial, sin inyección en el
// juego, entrega texturas D3D11 directamente (cero copias a la CPU).

#include "capture/CaptureSource.h"
#include "common/Compat.h"
#include "common/Util.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <chrono>
#include <mutex>
#include <thread>

namespace spectre {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
using winrt::Windows::Foundation::Metadata::ApiInformation;

bool WgcSupported() {
  try {
    return wgc::GraphicsCaptureSession::IsSupported();
  } catch (...) {
    return false;
  }
}

bool WgcBorderless() {
  // Sin IsBorderRequired (Windows 10) WGC dibuja un marco amarillo permanente.
  // En Windows 11 hay que pedir permiso para quitarlo; a las apps de escritorio
  // se les concede sin preguntar. Se pide una vez, en un hilo MTA para no
  // bloquear el apartamento STA de la interfaz.
  static std::once_flag once;
  static bool allowed = false;
  std::call_once(once, [] {
    try {
      if (!ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                                             L"IsBorderRequired"))
        return;
      if (!ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureAccess")) {
        allowed = true;
        return;
      }
      std::thread([] {
        try {
          winrt::init_apartment(winrt::apartment_type::multi_threaded);
          auto op = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless);
          if (op.wait_for(std::chrono::seconds(3)) == winrt::Windows::Foundation::AsyncStatus::Completed)
            allowed = op.GetResults() == winrt::Windows::Security::Authorization::AppCapabilityAccess::
                                             AppCapabilityAccessStatus::Allowed;
          winrt::uninit_apartment();
        } catch (...) {
        }
      }).join();
    } catch (...) {
    }
    Log("WGC: captura sin borde %s", allowed ? "disponible" : "no disponible");
  });
  return allowed;
}

namespace {

class WgcCapture final : public CaptureSource {
public:
  ~WgcCapture() override { Stop(); }
  const char* Name() const override { return "Windows.Graphics.Capture"; }

  bool Start(ID3D11Device* device, const CaptureTarget& target, UINT fps, bool cursor) override {
    try {
      device_ = device;
      device_->GetImmediateContext(&context_);
      // Se admiten hasta 2×fps copias: con una rejilla de 1/fps sin sincronizar
      // con la del encoder, en monitores de 144 Hz salían frames duplicados y
      // saltados según la fase. Cuesta como mucho fps copias de textura extra.
      period_ = QpcFrequency() / (2 * std::max<UINT>(fps, 1));
      nextDue_ = 0;
      stopping_ = false;

      ComPtr<IDXGIDevice> dxgi;
      if (FAILED(device_.As(&dxgi))) return false;
      winrt::com_ptr<::IInspectable> inspectable;
      winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
      winrtDevice_ = inspectable.as<wdx::Direct3D11::IDirect3DDevice>();

      auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
      interop::IGraphicsCaptureItemInterop* interop = nullptr;
      winrt::check_hresult(static_cast<::IUnknown*>(winrt::get_abi(factory))
                               ->QueryInterface(interop::IID_IGraphicsCaptureItemInterop,
                                                reinterpret_cast<void**>(&interop)));
      wgc::GraphicsCaptureItem item{nullptr};
      const auto itemIid = winrt::guid_of<wgc::GraphicsCaptureItem>();
      HRESULT hr = target.window
                       ? interop->CreateForWindow(target.window, reinterpret_cast<const IID&>(itemIid),
                                                  winrt::put_abi(item))
                       : interop->CreateForMonitor(target.monitor, reinterpret_cast<const IID&>(itemIid),
                                                   winrt::put_abi(item));
      interop->Release();
      winrt::check_hresult(hr);
      item_ = item;
      size_ = item_.Size();

      pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
          winrtDevice_, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
      session_ = pool_.CreateCaptureSession(item_);

      const wchar_t* sessionClass = L"Windows.Graphics.Capture.GraphicsCaptureSession";
      if (ApiInformation::IsPropertyPresent(sessionClass, L"IsCursorCaptureEnabled"))
        session_.IsCursorCaptureEnabled(cursor);
      if (ApiInformation::IsPropertyPresent(sessionClass, L"IsBorderRequired")) {
        try {
          session_.IsBorderRequired(false);
        } catch (...) {
          // Windows 10 antiguo: el borde amarillo no se puede quitar.
        }
      }
      if (ApiInformation::IsPropertyPresent(sessionClass, L"MinUpdateInterval")) {
        // Windows 11 24H2+: el propio sistema limita la frecuencia de captura.
        // Medio periodo: con 1/fps exacto un monitor de 144 Hz solo entregaría
        // uno de cada tres refrescos (48 fps).
        session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan(10'000'000 / (2 * std::max<UINT>(fps, 1))));
      }

      frameArrived_ = pool_.FrameArrived(winrt::auto_revoke, [this](auto&& sender, auto&&) { OnFrame(sender); });
      closed_ = item_.Closed(winrt::auto_revoke, [this](auto&&, auto&&) {
        Log("WGC: el objetivo de captura se cerró");
        lost_ = true;
      });
      session_.StartCapture();
      Log("WGC: captura iniciada (%dx%d)", size_.Width, size_.Height);
      return true;
    } catch (const winrt::hresult_error& e) {
      Log("WGC: no se pudo iniciar: 0x%08X %s", unsigned(e.code()), Narrow(e.message()).c_str());
      Stop();
      return false;
    }
  }

  void Stop() override {
    {
      // FrameArrived llega en hilos del pool: revoke() no espera a un callback
      // en curso, así que se marca la parada bajo el mismo mutex que OnFrame.
      std::lock_guard lock(frameMutex_);
      stopping_ = true;
    }
    frameArrived_.revoke();
    closed_.revoke();
    try {
      if (session_) session_.Close();
      if (pool_) pool_.Close();
    } catch (...) {
    }
    std::lock_guard lock(frameMutex_);
    session_ = nullptr;
    pool_ = nullptr;
    item_ = nullptr;
    winrtDevice_ = nullptr;
    context_.Reset();
    device_.Reset();
  }

private:
  void OnFrame(const wgc::Direct3D11CaptureFramePool& sender) {
    std::lock_guard guard(frameMutex_);
    if (stopping_) return;
    auto frame = sender.TryGetNextFrame();
    if (!frame) return;
    auto contentSize = frame.ContentSize();

    // Si el monitor refresca a mucho más que el vídeo, no se copian frames que
    // el encoder nunca va a usar. Se mide contra una rejilla de 1/(2·fps) con
    // medio periodo de margen: medir desde la última copia descartaba demasiados
    // (un monitor de 144 Hz se quedaba en 48 fps).
    int64_t now = QpcNow();
    bool resized = contentSize.Width != size_.Width || contentSize.Height != size_.Height;
    if (!resized && nextDue_ != 0 && now < nextDue_ - period_ / 2) return;
    nextDue_ = (nextDue_ == 0 || now - nextDue_ > period_) ? now + period_ : nextDue_ + period_;

    try {
      auto surface = frame.Surface();
      auto* access = static_cast<::IUnknown*>(winrt::get_abi(surface));
      interop::IDirect3DDxgiInterfaceAccess* dxgiAccess = nullptr;
      if (access && SUCCEEDED(access->QueryInterface(interop::IID_IDirect3DDxgiInterfaceAccess,
                                           reinterpret_cast<void**>(&dxgiAccess)))) {
        ComPtr<ID3D11Texture2D> tex;
        dxgiAccess->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.GetAddressOf()));
        dxgiAccess->Release();
        if (tex) {
          GpuLock lock(GpuMutex());
          Publish(device_.Get(), context_.Get(), tex.Get(), UINT(contentSize.Width), UINT(contentSize.Height));
        }
      }
    } catch (...) {
    }
    frame.Close();

    if (resized && contentSize.Width > 0 && contentSize.Height > 0) {
      size_ = contentSize;
      pool_.Recreate(winrtDevice_, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
    }
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  wdx::Direct3D11::IDirect3DDevice winrtDevice_{nullptr};
  wgc::GraphicsCaptureItem item_{nullptr};
  wgc::Direct3D11CaptureFramePool pool_{nullptr};
  wgc::GraphicsCaptureSession session_{nullptr};
  wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived_;
  wgc::GraphicsCaptureItem::Closed_revoker closed_;
  winrt::Windows::Graphics::SizeInt32 size_{};
  int64_t period_ = 0;
  int64_t nextDue_ = 0;
  std::mutex frameMutex_;
  bool stopping_ = false;
};

} // namespace

std::unique_ptr<CaptureSource> CreateWgcCapture() { return std::make_unique<WgcCapture>(); }

} // namespace spectre
