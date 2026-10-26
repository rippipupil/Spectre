#pragma once

#include "encode/Media.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

// Escribe paquetes ya codificados en un MP4 (remux, sin recodificar). Los
// tiempos se desplazan para que el primer keyframe de vídeo sea t = 0. La
// pista 0 es el vídeo; otra de vídeo (el fondo de la webcam) no sale por
// defecto en los reproductores y empieza en su primer keyframe.
class Mp4Writer {
public:
  ~Mp4Writer() { Close(); }
  // fragmented: MP4 fragmentado, un corte de luz no deja el archivo inservible.
  // reserveMoov > 0 (solo sin fragmentar): bytes reservados al principio para
  // el índice (moov), que se escribe ahí al cerrar. Sin reserva se usa
  // +faststart, que al cerrar vuelve a leer y reescribir el archivo entero
  // para mover el índice delante.
  bool Open(const std::string& path, const StreamList& streams, bool fragmented, int64_t reserveMoov = 0);
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
  std::vector<char> keySeen_; // pistas de vídeo además de la 0: ya empezaron en un keyframe
  bool haveOrigin_ = false;
  bool headerWritten_ = false;
  double writtenSeconds_ = 0;
  AVPacket* tmp_ = nullptr;
};

// Escribe el clip de una pasada: el índice va en un hueco reservado al
// principio, calculado con el número de paquetes. Si aun así no cupiera, se
// reescribe con +faststart. Las pistas sin ningún paquete en el clip (un
// fondo de la webcam de antes de moverla) no se escriben.
bool WriteClip(const std::string& path, const StreamList& streams,
               const std::vector<std::pair<SharedPacket, int>>& packets);

// Recorta un MP4 entre start y end (segundos) sin recodificar. El inicio se
// ajusta al keyframe anterior.
bool TrimFile(const std::string& input, const std::string& output, double start, double end, std::string* error);
bool ProbeDuration(const std::string& path, double* seconds);

// Grabación continua: los paquetes se encolan y se escriben en otro hilo de
// baja prioridad para no frenar al encoder. Mientras la cola está al día,
// ese hilo va en modo de fondo (E/S y memoria de baja prioridad); si el disco
// se queda atrás, recupera su prioridad normal para alcanzar. Si aun así la
// cola llega a kMaxQueueBytes, la grabación se cierra sola antes que dejar
// que crezca sin límite en la RAM.
class Recorder {
public:
  static constexpr size_t kMaxQueueBytes = 256u << 20;

  ~Recorder() { Stop(); }
  bool Start(const std::string& path, const StreamList& streams);
  // No admite dos llamadas a la vez: el anillo (ReplayBuffer) las hace con su
  // lock cogido. Nunca bloquea al que llama.
  void Push(const AVPacket* packet, int stream);
  void Stop();
  bool Active() const { return running_ && !overflowed_; }
  // El disco no daba abasto y se cortó la grabación (el archivo vale hasta ahí).
  bool Overflowed() const { return overflowed_; }
  // El hilo de escritura terminó: tras cortarse, el archivo ya está cerrado.
  bool Finished() const { return finished_; }
  const std::string& Path() const { return path_; }
  double Seconds() const { return seconds_; }
  // Lo más que ha llegado a acumular la cola (para el log de rendimiento).
  size_t PeakQueueBytes() const { return peakQueueBytes_; }

  // Se llama una vez al cortar la grabación por la cola, desde el hilo que
  // entrega los paquetes: solo para avisar.
  std::function<void()> onOverflow;

private:
  void Run();
  Mp4Writer writer_;
  std::string path_;
  std::atomic<bool> running_{false};
  std::atomic<bool> overflowed_{false};
  std::atomic<bool> finished_{false};
  std::atomic<double> seconds_{0};
  std::atomic<size_t> peakQueueBytes_{0};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::pair<PacketPtr, int>> queue_;
  std::vector<std::pair<PacketPtr, int>> staged_; // solo los productores (ver Push)
  std::atomic<size_t> pendingBytes_{0};           // encolado y aún sin escribir
};

} // namespace spectre
