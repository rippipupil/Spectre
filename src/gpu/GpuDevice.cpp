#include "gpu/GpuDevice.h"

#include "common/Util.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

namespace spectre {

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

  Log("GPU: %s (vendor 0x%04X)", adapterName_.c_str(), vendorId_);
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
