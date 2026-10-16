#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

using Microsoft::WRL::ComPtr;

struct CameraDevice {
  std::string id; // symbolic link (UTF-8)
  std::string name;
};
std::vector<CameraDevice> EnumerateCameras();

// Webcam con Media Foundation. Pide como máximo 1280x720 a 30 fps y deja el
// último frame en una textura BGRA para componerla en la GPU.
class Camera {
public:
  ~Camera() { Stop(); }
  bool Start(ID3D11Device* device, const std::string& id);
  void Stop();

  // Llamar con GpuMutex tomado.
  bool Latest(ComPtr<ID3D11Texture2D>* tex, UINT* width, UINT* height);

private:
  void Run(std::wstring id);

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> texture_;
  UINT width_ = 0, height_ = 0;
  bool hasFrame_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

} // namespace spectre
