// Fallback: DXGI Desktop Duplication. Solo monitor completo y sin cursor,
// pero funciona en Windows 10 anteriores a 1903 y con drivers donde WGC falla.

#include "capture/CaptureSource.h"
#include "common/Util.h"

#include <dxgi1_2.h>

#include <thread>

namespace spectre {

namespace {

class DdaCapture final : public CaptureSource {
public:
  ~DdaCapture() override { Stop(); }
  const char* Name() const override { return "DXGI Desktop Duplication"; }

  bool Start(ID3D11Device* device, const CaptureTarget& target, UINT fps, bool /*cursor*/) override {
    if (!target.monitor) return false;
    device_ = device;
    device_->GetImmediateContext(&context_);
    monitor_ = target.monitor;
    fps_ = std::max<UINT>(fps, 1);
    if (!Duplicate()) return false;
    running_ = true;
    thread_ = std::thread([this] { Run(); });
    Log("DDA: captura iniciada");
    return true;
  }

  void Stop() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
  }

private:
  bool Duplicate() {
    duplication_.Reset();
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_OUTPUT_DESC desc;
      output->GetDesc(&desc);
      if (desc.Monitor == monitor_) break;
      output.Reset();
    }
    if (!output) {
      Log("DDA: el monitor no está conectado a la GPU elegida");
      return false;
    }
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) return false;
    HRESULT hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
      Log("DDA: DuplicateOutput falló: 0x%08X", unsigned(hr));
      return false;
    }
    return true;
  }

  void Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const int64_t minInterval = QpcFrequency() / fps_ - QpcFrequency() / 1000;
    int64_t lastCopy = 0;
    while (running_) {
      if (!duplication_) {
        Sleep(250);
        if (!Duplicate()) continue;
      }
      DXGI_OUTDUPL_FRAME_INFO info{};
      ComPtr<IDXGIResource> resource;
      HRESULT hr = duplication_->AcquireNextFrame(100, &info, &resource);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
      if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Cambio de modo, UAC, pantalla completa exclusiva... se reintenta.
        duplication_.Reset();
        continue;
      }
      if (FAILED(hr)) {
        Log("DDA: AcquireNextFrame falló: 0x%08X", unsigned(hr));
        lost_ = true;
        break;
      }
      int64_t now = QpcNow();
      if (info.LastPresentTime.QuadPart != 0 && now - lastCopy >= minInterval) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) {
          D3D11_TEXTURE2D_DESC d;
          tex->GetDesc(&d);
          GpuLock lock(GpuMutex());
          Publish(device_.Get(), context_.Get(), tex.Get(), d.Width, d.Height);
          lastCopy = now;
        }
      }
      resource.Reset();
      duplication_->ReleaseFrame();
    }
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  HMONITOR monitor_ = nullptr;
  UINT fps_ = 60;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

} // namespace

std::unique_ptr<CaptureSource> CreateDdaCapture() { return std::make_unique<DdaCapture>(); }

} // namespace spectre
