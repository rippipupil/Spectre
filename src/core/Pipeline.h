#pragma once

#include "audio/AudioPipeline.h"
#include "capture/Camera.h"
#include "capture/CaptureSource.h"
#include "core/Mp4Writer.h"
#include "core/PerfMonitor.h"
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
#include <vector>

namespace spectre {

struct PipelineSettings {
  CaptureTarget target;
  std::string captureApi = "auto"; // auto | wgc | dda
  bool cursor = true;
  int fps = 60;
  int outputHeight = 1080; // 0 = nativa
  std::string quality = "alta"; // baja | media | alta | maxima | personalizada
  int bitrateKbps = 20000;      // solo con quality = personalizada
  std::string codec = "h264";
  std::string encoder = "auto";
  bool gpuPriority = true;
  AudioSettings audio;
  bool camera = false;
  std::string cameraId;
  std::string cameraCorner = "bottom-right";
  int cameraSizePercent = 25;
  double replaySeconds = 30;
  // Tope de RAM del replay; 0 = automático: lo que ocupa la duración pedida
  // al bitrate máximo del preset, sin pasar de la cuarta parte de la RAM.
  size_t maxBufferBytes = 0;
};

struct PipelineStats {
  bool running = false;
  std::string encoder;
  std::string capture;
  std::string gpu;
  int width = 0, height = 0, fps = 0;
  int targetKbps = 0, maxKbps = 0;
  double encodeMs = 0;
  uint64_t framesEncoded = 0;
  uint64_t framesSkipped = 0;
  size_t bufferBytes = 0;
  double bufferSeconds = 0;
  size_t bufferLimitBytes = 0; // tope en uso (el automático ya calculado)
  bool encoderSoftware = false; // codifica en la CPU
  int audioTracks = 0;
  std::string systemAudio; // dispositivo y formato abiertos (vacío = no se captura)
  std::string micAudio;
  bool recording = false;
  double recordSeconds = 0;
  std::string recordPath;
};

// Captura -> NV12 en GPU -> encoder -> replay buffer (+ grabación).
class Pipeline {
public:
  // Mensajes para el usuario (clip guardado, errores...). Se llama desde otros hilos.
  using Notify = std::function<void(const std::string& message, bool error)>;

  ~Pipeline();
  bool Start(const PipelineSettings& settings, Notify notify);
  void Stop();
  bool Running() const { return running_; }
  // La captura se perdió, la GPU se reinició o el encoder dejó de aceptar
  // frames: la app reinicia el pipeline. Tras perder la GPU se esperan unos
  // segundos: si fue una actualización del driver, Windows puede estar aún
  // con el adaptador básico y arrancaríamos con un encoder por software.
  bool CaptureLost() const {
    if (failed_) return GetTickCount64() - failedAt_ > 4000;
    return capture_ && capture_->Lost();
  }
  HWND TargetWindow() const { return settings_.target.window; }

  // Guarda los últimos `seconds` segundos en `path` (asíncrono).
  void SaveClip(const std::string& path, double seconds);
  bool StartRecording(const std::string& path);
  void StopRecording();
  bool Recording() const { return recorder_ && recorder_->Active(); }
  void SetReplaySeconds(double seconds);
  bool MicCaptured() const { return running_ && audio_.HasMic(); }
  void SetMicMuted(bool muted) { audio_.SetMicMuted(muted); }
  // Nivel de pico reciente (0..1) de cada fuente de audio, barato de leer en cada frame.
  void AudioLevels(float* system, float* mic) const {
    if (running_) audio_.Levels(system, mic);
    else {
      if (system) *system = 0;
      if (mic) *mic = 0;
    }
  }
  PipelineStats Stats();

private:
  void VideoLoop();
  bool Render(ID3D11Texture2D* dst, UINT slice, const CapturedFrame& frame);
  RECT CameraRect(UINT camWidth, UINT camHeight) const;
  size_t BufferLimit(double seconds) const;
  void ReleaseRecorder();

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
  std::vector<std::thread> recorderDrains_; // grabaciones cortadas que aún vacían su cola
  StreamList streams_;

  std::atomic<bool> running_{false};
  std::atomic<bool> acceptPackets_{false};
  std::atomic<bool> failed_{false};
  std::atomic<ULONGLONG> failedAt_{0};
  QualityPlan plan_;
  std::thread videoThread_;
  int64_t startQpc_ = 0;
  int width_ = 0, height_ = 0;

  std::mutex statsMutex_;
  double encodeMs_ = 0;
  double frameAgeMs_ = 0; // cuánto lleva capturado cada frame nuevo al codificarlo
  uint64_t framesEncoded_ = 0;
  uint64_t framesSkipped_ = 0;
  VideoCounters window_; // desde la última lectura de perf_
  PerfMonitor perf_;
};

} // namespace spectre
