#pragma once

#include "encode/Media.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

// Escribe paquetes ya codificados en un MP4 (remux, sin recodificar). Los
// tiempos se desplazan para que el primer keyframe de vídeo sea t = 0.
class Mp4Writer {
public:
  ~Mp4Writer() { Close(); }
  // fragmented: MP4 fragmentado, un corte de luz no deja el archivo inservible.
  bool Open(const std::string& path, const StreamList& streams, bool fragmented);
  bool Write(const AVPacket* packet, int stream);
  bool Close();
  double WrittenSeconds() const { return writtenSeconds_; }
  // Primer y último instante escrito de cada pista (s, respecto al keyframe
  // inicial) y paquetes descartados, para el log: si el audio empieza tarde
  // respecto al vídeo se ve aquí.
  std::string Summary() const;

private:
  AVFormatContext* fmt_ = nullptr;
  StreamList streams_;
  std::vector<int64_t> offsets_;
  std::vector<int64_t> lastDts_;
  std::vector<int64_t> firstPts_;
  std::vector<int64_t> lastPts_;
  std::vector<int> dropped_;
  bool haveOrigin_ = false;
  bool headerWritten_ = false;
  double writtenSeconds_ = 0;
  AVPacket* tmp_ = nullptr;
};

bool WriteClip(const std::string& path, const StreamList& streams,
               const std::vector<std::pair<PacketPtr, int>>& packets);

// Recorta un MP4 entre start y end (segundos) sin recodificar. El inicio se
// ajusta al keyframe anterior.
bool TrimFile(const std::string& input, const std::string& output, double start, double end, std::string* error);
bool ProbeDuration(const std::string& path, double* seconds);

// Grabación continua: los paquetes se encolan y se escriben en otro hilo de
// baja prioridad para no frenar al encoder.
class Recorder {
public:
  ~Recorder() { Stop(); }
  bool Start(const std::string& path, const StreamList& streams);
  void Push(const AVPacket* packet, int stream);
  void Stop();
  bool Active() const { return running_; }
  const std::string& Path() const { return path_; }
  double Seconds() const { return seconds_; }

private:
  void Run();
  Mp4Writer writer_;
  std::string path_;
  std::atomic<bool> running_{false};
  std::atomic<double> seconds_{0};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::pair<PacketPtr, int>> queue_;
};

} // namespace spectre
