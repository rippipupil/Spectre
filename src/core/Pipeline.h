#pragma once

#include "audio/AudioPipeline.h"
#include "capture/Camera.h"
#include "capture/CaptureSource.h"
#include "core/Mp4Writer.h"
#include "core/ReplayBuffer.h"
#include "encode/VideoEncoder.h"
#include "gpu/GpuDevice.h"
#include "gpu/VideoConverter.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace spectre {

struct PipelineSettings {
  CaptureTarget target;
  std::string captureApi = "auto"; // auto | wgc | dda
  bool cursor = true;
  int fps = 60;
  int outputHeight = 1080; // 0 = nativa
  int bitrateKbps = 20000;
  std::string codec = "h264";
  std::string encoder = "auto";
  bool gpuPriority = true;
  AudioSettings audio;
  bool camera = false;
  std::string cameraId;
  std::string cameraCorner = "bottom-right";
  int cameraSizePercent = 25;
  double replaySeconds = 30;
  size_t maxBufferBytes = 1024u << 20;
};

struct PipelineStats {
  bool running = false;
  std::string encoder;
  std::string capture;
  std::string gpu;
  int width = 0, height = 0, fps = 0;
  double encodeMs = 0;
  uint64_t framesEncoded = 0;
  uint64_t framesSkipped = 0;
  size_t bufferBytes = 0;
  double bufferSeconds = 0;
  int audioTracks = 0;
  bool recording = false;
  double recordSeconds = 0;
  std::string recordPath;
};

// Captura -> NV12 en GPU -> encoder -> replay buffer (+ grabación).
class Pipeline {
public:
  // Mensajes para el usuario (clip guardado, errores...). Se llama desde otros hilos.
  using Notify = std::function<void(const std::string& message, bool error)>;

  ~Pipeline() { Stop(); }
  bool Start(const PipelineSettings& settings, Notify notify);
  void Stop();
  bool Running() const { return running_; }
  bool CaptureLost() const { return capture_ && capture_->Lost(); }
  HWND TargetWindow() const { return settings_.target.window; }

  // Guarda los últimos `seconds` segundos en `path` (asíncrono).
  void SaveClip(const std::string& path, double seconds);
  bool StartRecording(const std::string& path);
  void StopRecording();
  bool Recording() const { return recorder_ && recorder_->Active(); }
  void SetReplaySeconds(double seconds);
  bool MicCaptured() const { return running_ && audio_.HasMic(); }
  void SetMicMuted(bool muted) { audio_.SetMicMuted(muted); }
  PipelineStats Stats();

private:
  void VideoLoop();
  bool Render(ID3D11Texture2D* dst, UINT slice, const CapturedFrame& frame);
  RECT CameraRect(UINT camWidth, UINT camHeight) const;

  PipelineSettings settings_;
  Notify notify_;
  GpuDevice gpu_;
  std::unique_ptr<CaptureSource> capture_;
  VideoConverter converter_;
  VideoEncoder encoder_;
  AudioPipeline audio_;
  std::unique_ptr<Camera> camera_;
  std::shared_ptr<ReplayBuffer> buffer_;
  std::unique_ptr<Recorder> recorder_;
  StreamList streams_;

  std::atomic<bool> running_{false};
  std::atomic<bool> acceptPackets_{false};
  std::thread videoThread_;
  int64_t startQpc_ = 0;
  int width_ = 0, height_ = 0;

  std::mutex statsMutex_;
  double encodeMs_ = 0;
  uint64_t framesEncoded_ = 0;
  uint64_t framesSkipped_ = 0;
};

} // namespace spectre
