#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>

namespace spectre {

using Microsoft::WRL::ComPtr;

class GpuDevice {
public:
  // adapter == nullptr: elige la GPU de alto rendimiento (la dedicada en portátiles).
  bool Create(IDXGIAdapter1* adapter = nullptr);
  void Reset();

  ID3D11Device* Device() const { return device_.Get(); }
  ID3D11DeviceContext* Context() const { return context_.Get(); }
  IDXGIAdapter1* Adapter() const { return adapter_.Get(); }
  const std::string& AdapterName() const { return adapterName_; }
  UINT VendorId() const { return vendorId_; }

  // Sube la prioridad de planificación de la GPU para que el encoder no se
  // quede sin turno cuando el juego está al 100 %. Igual que hace OBS.
  void BoostGpuPriority();

  static ComPtr<IDXGIAdapter1> HighPerformanceAdapter();
  // Adaptador que tiene conectado el monitor (necesario para DXGI Desktop Duplication).
  static ComPtr<IDXGIAdapter1> AdapterForMonitor(HMONITOR monitor, ComPtr<IDXGIOutput>* output = nullptr);

private:
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGIAdapter1> adapter_;
  std::string adapterName_;
  UINT vendorId_ = 0;
};

} // namespace spectre
