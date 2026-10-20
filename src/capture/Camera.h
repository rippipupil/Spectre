#pragma once

#include <d3d11.h>
#include <mfidl.h>
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

// Webcam con Media Foundation. Deja el último frame en una textura BGRA para
// componerla en la GPU. Pide a la cámara el formato más pequeño que llegue al
// ancho con el que se va a ver (como mucho 1280x720): decodificar y convertir
// cada frame se hace en la CPU, y a 640x360 cuesta la cuarta parte que a 720p.
class Camera {
public:
  ~Camera() { Stop(); }
  bool Start(ID3D11Device* device, const std::string& id, UINT wantWidth = 1280);
  void Stop();

  // Llamar con GpuMutex tomado.
  bool Latest(ComPtr<ID3D11Texture2D>* tex, UINT* width, UINT* height);

private:
  void Run(std::wstring id, UINT wantWidth);
  void ReleaseSource();

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> texture_;
  UINT width_ = 0, height_ = 0;
  bool hasFrame_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex sourceMutex_;
  ComPtr<IMFMediaSource> source_;
};

} // namespace spectre
