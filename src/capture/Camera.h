#pragma once

#include <d3d11.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
//
// Ni Start ni Stop esperan a la cámara: buscarla, abrirla y cerrarla va en su
// propio hilo. Abrir una cámara puede tardar segundos (o no volver nunca con
// algunos drivers, o si otra app la tiene cogida), y quien llama es el hilo de
// la interfaz con el panel abierto encima del juego.
class Camera {
public:
  // Mensajes para el usuario (no se pudo abrir...). Se llama desde el hilo de la cámara.
  using Notify = std::function<void(const std::string& message, bool error)>;

  ~Camera() { Stop(); }
  // false solo si Media Foundation no existe (Windows N). El resultado de abrir
  // la cámara llega después: al log y, si falla, a notify. userAsked: la acaba
  // de activar el usuario, y sus errores se avisan aunque se acaben de avisar.
  bool Start(ID3D11Device* device, const std::string& id, UINT wantWidth, Notify notify, bool userAsked = false);
  // Pide al hilo que pare y lo suelta: termina solo, aunque la cámara tarde.
  void Stop();

  // Llamar con GpuMutex tomado.
  bool Latest(ComPtr<ID3D11Texture2D>* tex, UINT* width, UINT* height);

  // Un aviso, una sola vez, si lleva demasiado sin dar ni un frame (abriéndose
  // o atascada); vacío si va bien. Desde el hilo de la interfaz.
  std::string TakeStall();

  // Al salir: espera (como mucho timeoutMs) a que terminen los hilos de las
  // cámaras ya paradas, antes de MFShutdown.
  static void WaitForThreads(DWORD timeoutMs);

private:
  struct Session;
  static void Run(std::shared_ptr<Session> s, std::wstring id, UINT wantWidth);

  std::shared_ptr<Session> session_; // se cambia con GpuMutex tomado (Latest lo lee así)
};

} // namespace spectre
