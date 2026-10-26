#pragma once

// La webcam se pinta encima del vídeo (Pipeline::Render). Para poder moverla,
// cambiarla de tamaño o quitarla después en el editor, lo que el juego tenía
// debajo se codifica aparte en una pista pequeña del mismo clip, «Fondo de la
// cámara x,y,w,h» (el recuadro en píxeles del vídeo), con un keyframe cada
// 0,25 s y el mismo pts que el fotograma del vídeo sobre el que se pintó la
// cámara: así el editor sabe en qué fotogramas estaba (ver
// CamBackgroundReader en VideoEdit).
//
// Solo con encoder por GPU y mientras se ve la cámara: un segundo encoder
// H.264 del mismo fabricante, del tamaño del recuadro (un 6 % de los píxeles
// con la cámara al 25 %). Abrirlo y cerrarlo va en un hilo propio: el de vídeo
// nunca espera. Si la cámara cambia de sitio, la pista nueva va aparte.

#include "capture/CaptureSource.h"
#include "core/ReplayBuffer.h"
#include "encode/Quality.h"
#include "encode/VideoEncoder.h"
#include "gpu/VideoConverter.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

class CamBackground {
public:
  CamBackground() = default;
  CamBackground(const CamBackground&) = delete;
  CamBackground& operator=(const CamBackground&) = delete;
  ~CamBackground() { Stop(); }

  // encoder: el del vídeo (se abre su versión H.264); plan: el del vídeo, de
  // outWidth x outHeight a fps.
  void Start(ID3D11Device* device, ID3D11DeviceContext* context, std::shared_ptr<ReplayBuffer> buffer,
             const std::string& encoder, const QualityPlan& plan, int outWidth, int outHeight, int fps);
  // Con el hilo de vídeo ya parado. Espera a que se abra o cierre su encoder.
  void Stop();

  // Hilo de vídeo, después de codificar el fotograma `index` de `frame`:
  // dónde se pintó la cámara (píxeles de la salida) o nullptr si no se pintó.
  void Frame(int64_t index, const CapturedFrame& frame, const RECT* rect);
  // Pista del anillo que recibe ahora el fondo; -1 si ninguna.
  int ActiveStream() const { return activeStream_; }

  // kbps de la pista en el peor caso con la cámara al sizePercent % del ancho
  // (para el tope de RAM del replay).
  static int MaxKbps(const QualityPlan& plan, int outWidth, int outHeight, int sizePercent);

private:
  struct Aux {
    VideoEncoder encoder;
    VideoConverter converter;
    RECT rect{};
    int stream = -1; // pista en el anillo (al adoptarlo el hilo de vídeo)
    int errors = 0;
  };

  void Run();
  std::unique_ptr<Aux> Open(const RECT& rect);
  void Close(std::unique_ptr<Aux> aux);
  void Retire(std::unique_ptr<Aux> aux);
  void Disable(const char* why);

  // Ajustes (fijos entre Start y Stop).
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  std::shared_ptr<ReplayBuffer> buffer_;
  std::string encoder_;
  QualityPlan plan_;
  int outWidth_ = 0, outHeight_ = 0, fps_ = 60;

  // Solo el hilo de vídeo.
  std::unique_ptr<Aux> active_;
  bool asked_ = false;
  RECT askedRect_{};
  ULONGLONG lastSeen_ = 0;
  int tracks_ = 0;
  bool disabled_ = false;

  // Con mutex_: lo que se le pide al hilo y lo que devuelve.
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::optional<RECT> openRect_;
  std::unique_ptr<Aux> ready_;
  std::vector<std::unique_ptr<Aux>> closing_;
  std::atomic<bool> openFailed_{false};
  std::atomic<int> activeStream_{-1};
};

} // namespace spectre
