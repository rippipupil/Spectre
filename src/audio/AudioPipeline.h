#pragma once

#include "audio/AudioCapture.h"
#include "encode/Media.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

struct AudioSettings {
  bool system = true;
  bool processOnly = false; // audio de un solo proceso (processId)
  DWORD processId = 0;
  std::string outputDeviceId;
  bool mic = false;
  std::string micDeviceId;
  float systemVolume = 1.0f;
  float micVolume = 1.0f;
  bool separateTracks = true;
};

// Mezcla las fuentes y codifica AAC. Pistas: "Mezcla" (todo) y, si se pide,
// una por fuente para poder ajustarlas al editar.
class AudioPipeline {
public:
  ~AudioPipeline() { Stop(); }

  // firstStream: índice de pista que tendrá la primera pista de audio.
  bool Start(const AudioSettings& settings, int64_t startQpc, int firstStream, PacketSink sink);
  void Stop();

  const StreamList& Streams() const { return streams_; }
  bool HasMic() const { return mic_ != nullptr; }
  bool HasSystem() const { return system_ != nullptr; }
  // Nivel de pico reciente (0..1) de cada fuente, para el medidor de Ajustes.
  void Levels(float* system, float* mic) const {
    if (system) *system = systemLevel_;
    if (mic) *mic = micLevel_;
  }
  // Dispositivo y formato con los que se abrió cada fuente (vacío si no hay).
  std::string SystemDescription() const { return system_ ? system_->Description() : std::string(); }
  std::string MicDescription() const { return mic_ ? mic_->Description() : std::string(); }
  // Silencia el micrófono sin reiniciar la captura (el buffer de replay no se pierde).
  void SetMicMuted(bool muted) { micMuted_ = muted; }
  // Cuánto va el audio codificado por detrás del reloj (normalmente 0,1 s; más
  // si una fuente entrega tarde). Al guardar un clip, esperar esto basta para
  // que llegue el audio de los últimos instantes.
  double LatencySeconds() const { return double(margin_.load()) / kAudioRate; }

private:
  struct Track {
    AVCodecContext* ctx = nullptr;
    unsigned sourceMask = 0; // bit 0 = sistema, bit 1 = micrófono
    int streamIndex = 0;
  };

  void Run();
  void EncodeChunk();
  bool OpenTrack(Track& track, const char* title);

  AudioSettings settings_;
  int64_t startQpc_ = 0;
  PacketSink sink_;
  std::unique_ptr<AudioCapture> system_;
  std::unique_ptr<AudioCapture> mic_;
  std::vector<Track> tracks_;
  StreamList streams_;
  std::atomic<bool> running_{false};
  std::atomic<bool> micMuted_{false};
  std::atomic<float> systemLevel_{0.0f};
  std::atomic<float> micLevel_{0.0f};
  float systemPeak_ = 0, micPeak_ = 0; // máximo desde la última línea del log
  int64_t lastLevelLog_ = 0;
  int64_t startedAt_ = 0;
  std::thread thread_;
  int64_t produced_ = 0;
  std::atomic<int64_t> margin_{0};
  int64_t loggedMargin_ = 0;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::vector<float> sysBuf_, micBuf_;
};

} // namespace spectre
