#pragma once

#include "encode/Media.h"

#include <deque>
#include <mutex>
#include <vector>

namespace spectre {

// Anillo de paquetes YA codificados (no frames): 60 s a 20 Mbps son ~150 MB
// en lugar de los ~11 GB que ocuparían en NV12 sin comprimir.
class ReplayBuffer {
public:
  void Configure(const StreamList& streams, double seconds, size_t maxBytes);
  void Clear();

  void Push(const AVPacket* packet, int stream);

  // Copia de los paquetes de los últimos `seconds` segundos, empezando en un
  // keyframe de vídeo para poder escribir el MP4 sin recodificar.
  std::vector<std::pair<PacketPtr, int>> Snapshot(double seconds);

  // Envía a `live` los paquetes desde el último keyframe y, a partir de ese
  // momento, cada paquete nuevo (todo bajo el mismo lock: no se pierde nada).
  void AttachLive(PacketSink live);
  void DetachLive();

  size_t Bytes();
  double DurationSeconds();
  void SetSeconds(double seconds);

private:
  struct Entry {
    PacketPtr packet;
    int stream;
    double time; // segundos (pts en la base de tiempo de su pista)
  };
  double TimeOf(const AVPacket* p, int stream) const;
  void Trim();
  // Paquetes del clip que empieza en el keyframe entries_[first], en orden de tiempo.
  std::vector<size_t> ClipEntries(size_t first) const;

  std::mutex mutex_;
  StreamList streams_;
  std::deque<Entry> entries_;
  double seconds_ = 30;
  size_t maxBytes_ = 512u << 20;
  size_t bytes_ = 0;
  double lastVideoTime_ = 0;
  PacketSink live_;
};

} // namespace spectre
