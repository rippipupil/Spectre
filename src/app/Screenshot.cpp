#include "app/Screenshot.h"

#include "app/CrashHandler.h"
#include "app/TextImage.h"
#include "common/Util.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace spectre {

using Microsoft::WRL::ComPtr;

namespace {

// Desktop Duplication solo funciona con el adaptador que tiene el monitor.
ComPtr<IDXGIAdapter1> AdapterFor(HMONITOR monitor) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC desc;
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) return adapter;
      output.Reset();
    }
    adapter.Reset();
  }
  return nullptr;
}

bool CreateDevice(IDXGIAdapter1* adapter, ComPtr<ID3D11Device>* device, ComPtr<ID3D11DeviceContext>* context) {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, UINT(std::size(levels)), D3D11_SDK_VERSION,
                                 device->GetAddressOf(), nullptr, context->GetAddressOf());
  return SUCCEEDED(hr);
}

// Espera el primer fotograma de la fuente (hasta 1,5 s).
CapturedFrame FirstFrame(CaptureSource& source) {
  CapturedFrame frame;
  for (int waited = 0; waited < 1500; waited += 10) {
    frame = source.Latest();
    if (frame.sequence > 0 && frame.texture) break;
    Sleep(10);
  }
  return frame;
}

// Fotograma en BGR de 24 bits (sin alfa: algunos juegos dejan el alfa a 0 y el
// PNG saldría transparente). La fuente ya está parada: nadie más usa este
// contexto y no hace falta GpuMutex, así la captura en curso del replay no
// espera a esta copia.
bool ReadFrame(const CapturedFrame& frame, ID3D11Device* device, ID3D11DeviceContext* context,
               std::vector<uint8_t>* bgr, UINT* width, UINT* height) {
  if (frame.sequence == 0 || !frame.texture || frame.width == 0 || frame.height == 0) return false;

  D3D11_TEXTURE2D_DESC desc;
  frame.texture->GetDesc(&desc);
  desc.Width = frame.width;
  desc.Height = frame.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;

  D3D11_BOX box{0, 0, 0, frame.width, frame.height, 1};
  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture.Get(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
  const UINT w = frame.width, h = frame.height;
  bgr->resize(size_t(w) * 3 * h);
  for (UINT y = 0; y < h; ++y) {
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
    uint8_t* dst = bgr->data() + size_t(y) * w * 3;
    for (UINT x = 0; x < w; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 0];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }
  context->Unmap(staging.Get(), 0);
  *width = w;
  *height = h;
  return true;
}

bool Encode(const std::wstring& path, bool jpeg, const std::vector<uint8_t>& bgr, UINT w, UINT h,
            std::string* error) {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (SUCCEEDED(hr))
    hr = wic->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder);
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
  if (SUCCEEDED(hr) && jpeg && props) {
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = 0.92f;
    props->Write(1, &option, &value);
  }
  if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
  if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr) && format != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;
  if (SUCCEEDED(hr))
    hr = frame->WritePixels(h, w * 3, UINT(bgr.size()), const_cast<BYTE*>(bgr.data()));
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  if (FAILED(hr)) {
    char buf[96];
    snprintf(buf, sizeof(buf), "No se pudo guardar la captura (0x%08X)", unsigned(hr));
    *error = buf;
    // El frame y el encoder también tienen el archivo abierto: si no se
    // sueltan antes, no se puede borrar y queda uno a medias.
    props.Reset();
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    DeleteFileW(path.c_str());
    return false;
  }
  return true;
}

bool Capture(const ScreenshotRequest& request, std::string* error) {
  HMONITOR monitor = request.target.window ? MonitorFromWindow(request.target.window, MONITOR_DEFAULTTOPRIMARY)
                                           : request.target.monitor;
  std::vector<uint8_t> bgr;
  UINT w = 0, h = 0;
  bool grabbed = false;
  // WGC primero (sirve para ventanas y para juegos con flip model); DDA para
  // Windows sin WGC o si WGC no puede con este objetivo.
  for (int attempt = 0; attempt < 2 && !grabbed; ++attempt) {
    bool wgc = attempt == 0;
    if (wgc && !WgcSupported()) continue;
    CaptureTarget target = request.target;
    if (!wgc) {
      target.window = nullptr; // DDA captura monitores
      target.monitor = monitor;
    }
    ComPtr<IDXGIAdapter1> adapter = wgc ? nullptr : AdapterFor(monitor);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateDevice(adapter.Get(), &device, &context)) continue;
    std::unique_ptr<CaptureSource> source = wgc ? CreateWgcCapture() : CreateDdaCapture();
    if (!source || !source->Start(device.Get(), target, 60, request.cursor)) continue;
    CapturedFrame frame = FirstFrame(*source);
    source->Stop();
    grabbed = ReadFrame(frame, device.Get(), context.Get(), &bgr, &w, &h);
    if (!grabbed) Log("Captura de pantalla: %s no entregó ningún fotograma", source->Name());
  }
  if (!grabbed) {
    *error = "No se pudo hacer la captura de pantalla. Mira spectre.log.";
    return false;
  }
  // Ya se tiene el fotograma: comprimir un PNG a 4K son cientos de ms de CPU
  // y no deben quitárselos al juego.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  if (!Encode(Widen(request.path), request.jpeg, bgr, w, h, error)) return false;
  Log("Captura de pantalla guardada: %s (%ux%u)", request.path.c_str(), w, h);
  return true;
}

} // namespace

bool ReadImageFile(const std::string& path, int maxSide, PhotoImage* image, std::string* error) {
  auto fail = [&](const char* what, HRESULT hr) {
    char buf[128];
    snprintf(buf, sizeof(buf), "No se pudo leer la imagen (%s, 0x%08X)", what, unsigned(hr));
    *error = buf;
    return false;
  };
  ComPtr<IWICImagingFactory> wic;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  if (FAILED(hr)) return fail("WIC", hr);
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  hr = wic->CreateDecoderFromFilename(Widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                      &decoder);
  if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) return fail("abrir", hr);
  UINT w = 0, h = 0;
  frame->GetSize(&w, &h);
  if (w == 0 || h == 0) return fail("tamaño", E_FAIL);
  ComPtr<IWICBitmapSource> source = frame;
  if (maxSide > 0 && (w > UINT(maxSide) || h > UINT(maxSide))) {
    const double k = double(maxSide) / double(std::max(w, h));
    const UINT sw = std::max(1u, UINT(w * k)), sh = std::max(1u, UINT(h * k));
    ComPtr<IWICBitmapScaler> scaler;
    hr = wic->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(source.Get(), sw, sh, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) return fail("reducir", hr);
    source = scaler;
    w = sw;
    h = sh;
  }
  // BGRA con el alfa ya aplicado sobre negro: las capturas son opacas.
  ComPtr<IWICFormatConverter> converter;
  hr = wic->CreateFormatConverter(&converter);
  if (SUCCEEDED(hr))
    hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) return fail("convertir", hr);
  image->width = int(w);
  image->height = int(h);
  image->bgra.resize(size_t(w) * size_t(h) * 4);
  hr = converter->CopyPixels(nullptr, w * 4, UINT(image->bgra.size()), image->bgra.data());
  if (FAILED(hr)) return fail("leer", hr);
  for (size_t i = 3; i < image->bgra.size(); i += 4) image->bgra[i] = 255;
  return true;
}

void TakeScreenshot(ScreenshotRequest request, std::function<void(bool ok, const std::string& text)> done) {
  auto pending = std::make_shared<PendingWrite>();
  std::thread([request = std::move(request), done = std::move(done), pending] {
    InstallThreadCrashHandler();
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WGC y WIC
    std::string error;
    bool ok = false;
    try {
      ok = Capture(request, &error);
    } catch (const std::exception& e) {
      error = std::string("No se pudo hacer la captura: ") + e.what();
      Log("Captura de pantalla: excepción: %s", e.what());
    }
    if (done) done(ok, ok ? request.path : error);
    if (SUCCEEDED(co)) CoUninitialize();
  }).detach();
}

void SaveImageAsync(std::string path, bool jpeg, std::vector<uint8_t> bgr, unsigned width, unsigned height,
                    std::function<void(bool ok, const std::string& text)> done) {
  auto pending = std::make_shared<PendingWrite>();
  std::thread([path = std::move(path), jpeg, bgr = std::move(bgr), width, height, done = std::move(done), pending] {
    InstallThreadCrashHandler();
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::string error;
    bool ok = false;
    try {
      ok = Encode(Widen(path), jpeg, bgr, width, height, &error);
      if (ok) Log("Fotograma guardado: %s (%ux%u)", path.c_str(), width, height);
    } catch (const std::exception& e) {
      error = std::string("No se pudo guardar el fotograma: ") + e.what();
      Log("Fotograma: excepción: %s", e.what());
    }
    if (done) done(ok, ok ? path : error);
    if (SUCCEEDED(co)) CoUninitialize();
  }).detach();
}

void SavePhotoEditAsync(std::string input, std::string output, PhotoSettings settings, bool jpeg,
                        std::function<void(bool ok, const std::string& text)> done) {
  auto pending = std::make_shared<PendingWrite>();
  std::thread([input = std::move(input), output = std::move(output), settings = std::move(settings), jpeg,
               done = std::move(done), pending] {
    InstallThreadCrashHandler();
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::string error;
    bool ok = false;
    try {
      PhotoImage source, result;
      PhotoRender options;
      options.finalSize = true;
      if (ReadImageFile(input, 16384, &source, &error)) {
        if (!RenderPhoto(settings, source, RasterizeText, options, &result)) error = "No se pudo editar la imagen";
        else ok = Encode(Widen(output), jpeg, PhotoToBgr(result), UINT(result.width), UINT(result.height), &error);
      }
      if (ok) Log("Imagen editada: %s (%dx%d)", output.c_str(), result.width, result.height);
      else Log("Imagen editada: %s", error.c_str());
    } catch (const std::exception& e) {
      error = std::string("No se pudo guardar la imagen: ") + e.what();
      Log("Imagen editada: excepción: %s", e.what());
    }
    if (done) done(ok, ok ? output : error);
    if (SUCCEEDED(co)) CoUninitialize();
  }).detach();
}

} // namespace spectre
