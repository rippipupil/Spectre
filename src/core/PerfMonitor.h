#pragma once

#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace spectre {

// Lo que mide el hilo de vídeo entre dos lecturas del monitor de rendimiento.
struct VideoCounters {
  uint64_t slots = 0;    // ranuras de la rejilla atendidas
  uint64_t encoded = 0;  // frames que aceptó el encoder
  uint64_t skipped = 0;  // ranuras saltadas: el hilo de vídeo llegó tarde
  uint64_t repeated = 0; // ranuras sin frame nuevo de la captura
  uint64_t captured = 0; // frames nuevos que publicó la captura
  uint64_t inPhase = 0;  // ranuras con la rejilla en fase con la captura
  double encodeSumMs = 0;
  double encodeMaxMs = 0;
  double lateMaxMs = 0; // lo más tarde que despertó el hilo respecto a su ranura
  static constexpr double kBinMs = 0.5;
  std::array<uint32_t, 40> encodeHist{}; // la última casilla cuenta 19,5 ms o más

  void AddEncode(double ms);
  void Merge(const VideoCounters& o);
  double EncodePercentileMs(double p) const;
};

// Hilo de baja prioridad que cada 5 s recoge los contadores del pipeline y
// cada minuto escribe en spectre.log una línea con lo que cuesta Spectre:
// captura, codificación, CPU, RAM, VRAM y uso de la GPU (de Spectre y del
// motor más ocupado, que suele ser el juego). Si en algún tramo de 5 s el
// hilo de vídeo llega tarde, lo escribe en el momento (como mucho cada 30 s).
// Todo se lee sin bloquear a los hilos de captura.
class PerfMonitor {
public:
  struct Sample {
    VideoCounters video; // tramo desde la lectura anterior (el pipeline los pone a cero)
    std::string extra;   // replay, audio...
  };
  using Source = std::function<void(Sample*)>;

  ~PerfMonitor() { Stop(); }
  void Start(IDXGIAdapter1* adapter, int fps, Source source);
  void Stop();

  // Lo que decide si un juego va a dar lag y que así no hay que preguntar:
  // Windows, monitores y sus Hz (cuál se captura), Game Bar y su grabación en
  // segundo plano, modo de juego y otros programas de captura abiertos. Lo
  // fijo se escribe una vez por proceso; monitores y programas, cuando cambian.
  static void LogEnvironment(HMONITOR captured);

private:
  void Run();

  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
  LUID luid_{};
  double periodMs_ = 1000.0 / 60;
  Source source_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
};

} // namespace spectre
