#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace spectre {

using Microsoft::WRL::ComPtr;

struct CaptureTarget {
  HMONITOR monitor = nullptr; // captura de monitor
  HWND window = nullptr;      // captura de ventana (tiene prioridad si no es null)
};

struct MonitorInfo {
  HMONITOR handle = nullptr;
  RECT rect{};
  bool primary = false;
  std::string name;
};
std::vector<MonitorInfo> EnumerateMonitors();

// Último frame capturado. La textura es propiedad de la fuente; quien la use
// debe hacerlo con GpuMutex tomado.
struct CapturedFrame {
  ComPtr<ID3D11Texture2D> texture;
  UINT width = 0;
  UINT height = 0;
  uint64_t sequence = 0;
  int64_t qpc = 0; // cuándo llegó (QueryPerformanceCounter)
};

class CaptureSource {
public:
  virtual ~CaptureSource() = default;
  virtual bool Start(ID3D11Device* device, const CaptureTarget& target, UINT fps, bool cursor) = 0;
  virtual void Stop() = 0;
  virtual const char* Name() const = 0;

  // Devuelve el último frame (puede repetirse si no hay uno nuevo).
  CapturedFrame Latest() {
    std::lock_guard lock(mutex_);
    return latest_;
  }
  // true si el objetivo dejó de existir (ventana cerrada, acceso perdido).
  bool Lost() const { return lost_; }

protected:
  // Copia src (región w x h) a la textura interna. Llamar con GpuMutex tomado.
  void Publish(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT w, UINT h);

  std::mutex mutex_;
  CapturedFrame latest_;
  std::atomic<bool> lost_{false};
  ComPtr<ID3D11Texture2D> buffer_;
};

std::unique_ptr<CaptureSource> CreateWgcCapture();
std::unique_ptr<CaptureSource> CreateDdaCapture();
bool WgcSupported();
// true si WGC puede capturar sin el marco amarillo (Windows 11 con permiso).
bool WgcBorderless();

} // namespace spectre
