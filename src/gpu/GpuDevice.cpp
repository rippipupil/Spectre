#include "gpu/GpuDevice.h"

#include "common/Util.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

namespace spectre {

namespace {

// Planificación de la GPU por hardware (HAGS) con D3DKMT, como lo registra
// OBS. Las estructuras se declaran aquí porque no todas las cabeceras las
// traen. -1 = no se sabe (Windows 10 anterior a 2004 o driver sin WDDM 2.7).
int HagsEnabled(LUID luid) {
  struct OpenAdapterFromLuid {
    LUID AdapterLuid;
    UINT hAdapter;
  };
  struct QueryAdapterInfo {
    UINT hAdapter;
    int Type;
    void* pPrivateDriverData;
    UINT PrivateDriverDataSize;
  };
  struct CloseAdapter {
    UINT hAdapter;
  };
  constexpr int kWddm27Caps = 70; // KMTQAITYPE_WDDM_2_7_CAPS
  HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  if (!gdi) return -1;
  auto open = reinterpret_cast<LONG(APIENTRY*)(OpenAdapterFromLuid*)>(
      reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid")));
  auto query = reinterpret_cast<LONG(APIENTRY*)(const QueryAdapterInfo*)>(
      reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")));
  auto close = reinterpret_cast<LONG(APIENTRY*)(const CloseAdapter*)>(
      reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTCloseAdapter")));
  if (!open || !query || !close) return -1;
  OpenAdapterFromLuid adapter{luid, 0};
  if (open(&adapter) != 0) return -1;
  UINT caps = 0; // D3DKMT_WDDM_2_7_CAPS: bit 0 la admite, bit 1 activada
  QueryAdapterInfo info{adapter.hAdapter, kWddm27Caps, &caps, sizeof(caps)};
  LONG status = query(&info);
  CloseAdapter closeInfo{adapter.hAdapter};
  close(&closeInfo);
  if (status != 0) return -1;
  return (caps & 2) ? 1 : 0;
}

} // namespace

ComPtr<IDXGIAdapter1> GpuDevice::HighPerformanceAdapter() {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

  ComPtr<IDXGIFactory6> factory6;
  if (SUCCEEDED(factory.As(&factory6))) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         SUCCEEDED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        IID_PPV_ARGS(&adapter)));
         ++i) {
      DXGI_ADAPTER_DESC1 desc;
      adapter->GetDesc1(&desc);
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return adapter;
      adapter.Reset();
    }
  }
  ComPtr<IDXGIAdapter1> adapter;
  factory->EnumAdapters1(0, &adapter);
  return adapter;
}

ComPtr<IDXGIAdapter1> GpuDevice::AdapterForMonitor(HMONITOR monitor, ComPtr<IDXGIOutput>* outOutput) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC desc;
      output->GetDesc(&desc);
      if (desc.Monitor == monitor) {
        if (outOutput) *outOutput = output;
        return adapter;
      }
      output.Reset();
    }
    adapter.Reset();
  }
  return nullptr;
}

bool GpuDevice::Create(IDXGIAdapter1* adapter) {
  Reset();
  adapter_ = adapter ? ComPtr<IDXGIAdapter1>(adapter) : HighPerformanceAdapter();

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDevice(adapter_.Get(), adapter_ ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device_,
                                 nullptr, &context_);
  if (FAILED(hr)) {
    // Algunos drivers antiguos no exponen VIDEO_SUPPORT: sin él no hay
    // VideoProcessor, pero seguimos para poder informar del error.
    flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    hr = D3D11CreateDevice(adapter_.Get(), adapter_ ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &context_);
  }
  if (FAILED(hr)) {
    Log("D3D11CreateDevice falló: 0x%08X", unsigned(hr));
    return false;
  }

  if (!adapter_) {
    ComPtr<IDXGIDevice> dxgi;
    device_.As(&dxgi);
    ComPtr<IDXGIAdapter> a;
    dxgi->GetAdapter(&a);
    a.As(&adapter_);
  }
  DXGI_ADAPTER_DESC1 desc{};
  adapter_->GetDesc1(&desc);
  adapterName_ = Narrow(desc.Description);
  vendorId_ = desc.VendorId;

  ComPtr<ID3D11Multithread> mt;
  if (SUCCEEDED(context_.As(&mt))) mt->SetMultithreadProtected(TRUE);

  // Versión del driver: con NVIDIA, el encoder depende de ella (las cabeceras
  // de NVENC de FFmpeg 8.1 piden la 570 o más; con una anterior se cae a Media
  // Foundation). Las cifras finales son la versión comercial: 32.0.15.6094 es
  // la 560.94.
  char driver[96] = "desconocido";
  LARGE_INTEGER umd{};
  if (SUCCEEDED(adapter_->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
    unsigned part3 = HIWORD(umd.LowPart), part4 = LOWORD(umd.LowPart);
    int n = snprintf(driver, sizeof(driver), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), part3, part4);
    if (vendorId_ == 0x10DE && n > 0) {
      unsigned version = (part3 % 10) * 10000 + part4;
      snprintf(driver + n, sizeof(driver) - size_t(n), " (NVIDIA %u.%02u%s)", version / 100, version % 100,
               version < 57000 ? ", AVISO: NVENC pide la 570 o más" : "");
    }
  }
  int hags = HagsEnabled(desc.AdapterLuid);
  Log("GPU: %s (vendor 0x%04X), driver %s, planificación por hardware (HAGS) %s", adapterName_.c_str(), vendorId_,
      driver, hags < 0 ? "sin datos" : hags ? "activada" : "desactivada");
  return true;
}

void GpuDevice::Reset() {
  context_.Reset();
  device_.Reset();
  adapter_.Reset();
  adapterName_.clear();
  vendorId_ = 0;
}

void GpuDevice::BoostGpuPriority() {
  // OBS no la cambia en gráficas Intel (libobs-d3d11, InitDevice): se hace
  // igual.
  if (vendorId_ == 0x8086) {
    Log("Prioridad de GPU del proceso: sin cambios (gráfica Intel)");
    return;
  }
  // D3DKMT_SCHEDULINGPRIORITYCLASS: 3 = ABOVE_NORMAL, 4 = HIGH.
  using SetPriorityFn = LONG(APIENTRY*)(HANDLE, int);
  auto fn = reinterpret_cast<SetPriorityFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass")));
  if (fn) {
    LONG status = fn(GetCurrentProcess(), 4);
    if (status != 0) status = fn(GetCurrentProcess(), 3);
    Log("Prioridad de GPU del proceso: %s", status == 0 ? "subida" : "sin cambios (requiere permisos)");
  }
  ComPtr<IDXGIDevice> dxgi;
  if (device_ && SUCCEEDED(device_.As(&dxgi))) dxgi->SetGPUThreadPriority(7);
}

} // namespace spectre
