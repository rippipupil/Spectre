#pragma once

#include "encode/Media.h"

#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <vector>

namespace spectre {

// Anillo de paquetes YA codificados (no frames): 60 s a 20 Mbps son ~150 MB
// en lugar de los ~11 GB que ocuparían en NV12 sin comprimir.
class ReplayBuffer {
public:
  // Se guardan 5 s más de los pedidos: un keyframe (uno cada 2 s) antes del
  // inicio del clip, más lo que entra mientras SaveClip espera al audio (hasta
  // 1,5 s).
  static constexpr double kExtraSeconds = 5.0;

  void Configure(const StreamList& streams, double seconds, size_t maxBytes);
  void Clear();
  // Una pista más con la captura en marcha (el fondo de la webcam, ver
  // CamBackground); devuelve su índice. Hasta Configure no se quita ninguna,
  // así que los índices de un Snapshot valen con las pistas de Streams().
  int AddStream(const StreamDesc& stream);
  StreamList Streams();

  void Push(const AVPacket* packet, int stream);

  // Los paquetes de los `seconds` segundos que terminan en `end` (segundos en
  // la línea de tiempo del pipeline: el instante en que se pidió el clip),
  // empezando en un keyframe de vídeo para poder escribir el MP4 sin
  // recodificar. Vídeo y audio se cortan en el mismo `end`. Bajo el lock solo
  // se copian referencias (~1 ms con 5 min de replay); ordenar va fuera,
  // porque los hilos de vídeo y audio esperan a ese lock en cada paquete.
  std::vector<std::pair<SharedPacket, int>> Snapshot(double seconds, double end);

  // Envía a `live` los paquetes desde el último keyframe y, a partir de ese
  // momento, cada paquete nuevo (todo bajo el mismo lock: no se pierde nada).
  void AttachLive(PacketSink live);
  void DetachLive();

  // Sin lock: los lee la interfaz y el monitor de rendimiento, que van con
  // menos prioridad que los hilos de captura y no deben poder frenarlos.
  size_t Bytes() const { return publishedBytes_; }
  double DurationSeconds() const { return publishedSeconds_; }
  size_t MaxBytes() const { return publishedMaxBytes_; }
  void SetLimits(double seconds, size_t maxBytes);

private:
  struct Entry {
    SharedPacket packet;
    int stream;
    double time; // segundos (pts en la base de tiempo de su pista)
  };
  double TimeOf(const AVPacket* p, int stream) const;
  void Trim();
  void Publish(); // copia los tamaños a los atómicos (con mutex_ tomado)
  // Si entries_[i] entra en un clip que empieza en el keyframe entries_[first]
  // y termina en `end`.
  bool InClip(size_t i, size_t first, double end) const;
  // Orden de escritura: por tiempo y, a igual tiempo, el vídeo primero.
  static bool ClipOrder(const Entry& x, const Entry& y);
  // Paquetes desde el keyframe entries_[first] hasta `end`, en orden de tiempo.
  std::vector<size_t> ClipEntries(size_t first, double end) const;

  std::mutex mutex_;
  StreamList streams_;
  std::deque<Entry> entries_;
  double seconds_ = 30;
  size_t maxBytes_ = 512u << 20;
  size_t bytes_ = 0;
  double lastVideoTime_ = 0;
  PacketSink live_;
  std::atomic<size_t> publishedBytes_{0};
  std::atomic<double> publishedSeconds_{0};
  std::atomic<size_t> publishedMaxBytes_{0};
};

} // namespace spectre
