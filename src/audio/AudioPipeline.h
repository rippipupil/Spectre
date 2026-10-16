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
  std::thread thread_;
  int64_t produced_ = 0;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::vector<float> sysBuf_, micBuf_;
};

} // namespace spectre
