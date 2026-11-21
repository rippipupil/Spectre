#pragma once

#include "audio/AudioPipeline.h"
#include "capture/Camera.h"
#include "capture/CaptureSource.h"
#include "core/CamBackground.h"
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
  int fps = 60;            // con quality = fuente, los Hz del monitor (ver SourceFps)
  int outputHeight = 1080; // 0 = nativa (con quality = fuente, siempre)
  std::string quality = "alta"; // baja | media | alta | maxima | fuente | personalizada
  int bitrateKbps = 20000;      // solo con quality = personalizada
  std::string codec = "h264";
  std::string encoder = "auto";
  bool gpuPriority = true;
  AudioSettings audio;
  bool camera = false;
  std::string cameraId;
  std::string cameraCorner = "bottom-right";
  int cameraSizePercent = 25;
  // Guardar aparte lo que hay debajo de la webcam para poder moverla o
  // quitarla en el editor (ver CamBackground).
  bool cameraEditable = true;
  double replaySeconds = 30;
  // Tope de RAM del replay; 0 = automático: lo que ocupa la duración pedida
  // al bitrate máximo del preset, sin pasar de la cuarta parte de la RAM.
  size_t maxBufferBytes = 0;
  // Encoders que se colgaron antes (claves de VideoStall::encoderKey): con la
  // misma gráfica y el mismo driver no se vuelven a probar.
  std::vector<std::string> stalledEncoders;
};

// Lo que encontró el vigilante del vídeo (Pipeline::CheckStall).
struct VideoStall {
  std::string reason;     // en palabras, para el log y el aviso; vacío = todo bien
  std::string encoderKey; // "id|gráfica|driver" si la culpa es del encoder
  bool restart = false;   // reiniciar la captura lo arregla
  // Guardar encoderKey en la configuración para no volver a probarlo. No con
  // el encoder propio de la gráfica (NVENC, AMF, QuickSync): colgarse una vez
  // puede ser un reinicio de la GPU, y sin él se grabaría por CPU hasta
  // cambiar de driver. Ese solo se deja de usar en la sesión si se cuelga dos veces.
  bool remember = false;
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
  // La duración de clip más larga que cabe entera en el tope de RAM (el fijo o
  // la cuarta parte de la RAM) al bitrate máximo de ahora: lo que el panel y
  // Ajustes avisan que no cabe.
  double replayFitSeconds = 0;
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
  // Vigilante, una vez por segundo desde la interfaz: el encoder lleva
  // segundos dentro de una llamada sin volver, o recibe imagen y no devuelve
  // vídeo. Sin él, el replay se quedaba solo con audio y cada clip fallaba sin
  // decir por qué (el encoder de Media Foundation de AMD, tras 3 frames). Lo
  // devuelve una vez por arranque; la app reinicia la captura sin ese encoder.
  VideoStall CheckStall();
  // La gráfica tiene encoder propio (NVENC, AMF, QuickSync) pero al arrancar
  // no abrió (AMF puede tardar en estar listo al iniciar Windows) y se graba
  // con otro: se vuelve a probar en otro hilo de vez en cuando. true cuando ya
  // abre: la app reinicia la captura para usarlo. Desde la interfaz.
  bool NativeEncoderReady();
  // El encoder propio que se está volviendo a probar (vacío si ya se usa).
  const std::string& NativeEncoder() const { return native_; }

  // Guarda los últimos `seconds` segundos en `path` (asíncrono). endQpc: dónde
  // acaba el clip (una orden de voz acaba donde se empezó a decir); 0 = ahora.
  void SaveClip(const std::string& path, double seconds, int64_t endQpc = 0);
  bool StartRecording(const std::string& path);
  void StopRecording();
  bool Recording() const { return recorder_ && recorder_->Active(); }
  void SetReplaySeconds(double seconds);
  // «Poder editar la cámara después», con la captura en marcha.
  void SetCameraEditable(bool editable);
  // Enciende, apaga o cambia la webcam con la captura en marcha: ni reinicia
  // el pipeline (el replay sigue) ni espera a que la cámara abra (ver Camera).
  // Solo cuentan los campos camera*. Desde el hilo de la interfaz.
  void SetCamera(const PipelineSettings& settings);
  // Un aviso (una vez) si la webcam lleva un rato sin dar imagen; vacío si no.
  std::string TakeCameraStall();
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
  struct VideoRun; // lo que comparte el hilo de vídeo con Stop y el vigilante (ver Pipeline.cpp)
  void VideoLoop(const std::shared_ptr<VideoRun>& run, VideoEncoder* encoder);
  // Espera al hilo de vídeo; si está colgado dentro del encoder, lo deja aparte.
  void JoinVideoThread();
  // Por qué no está llegando vídeo al anillo (vacío si llega), tras
  // `ticks` comprobaciones seguidas del vigilante (una por segundo).
  VideoStall Diagnose(int ticks) const;
  bool Render(ID3D11Texture2D* dst, UINT slice, const CapturedFrame& frame);
  RECT CameraRect(UINT camWidth, UINT camHeight) const;
  void StartCamera(bool userAsked);
  size_t BufferLimit(double seconds) const;
  // Bytes por segundo del replay en el peor caso (todo al bitrate máximo, con audio y un 10 %).
  double ReplayBytesPerSecond() const;
  void ReleaseRecorder();

  PipelineSettings settings_;
  Notify notify_;
  GpuDevice gpu_;
  std::unique_ptr<CaptureSource> capture_;
  VideoConverter converter_;
  // En un puntero: si se cuelga dentro de FFmpeg, se queda con su hilo y aquí
  // se pone uno nuevo (ver JoinVideoThread).
  std::unique_ptr<VideoEncoder> encoder_ = std::make_unique<VideoEncoder>();
  AudioPipeline audio_;
  // Se cambia con GpuMutex tomado, igual que settings_.camera*: Render los lee
  // dentro del GpuMutex del encoder.
  std::unique_ptr<Camera> camera_;
  bool cameraComposited_ = false; // ya se registró en el log la primera composición
  CamBackground camBackground_;
  std::atomic<bool> camEditable_{true};
  // Dónde pintó Render la cámara en el fotograma que se acaba de codificar
  // (solo el hilo de vídeo).
  RECT camRect_{};
  bool camDrawn_ = false;
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
  std::shared_ptr<VideoRun> run_;
  // Vigilante (solo la interfaz): contadores de la comprobación anterior.
  struct Watch {
    uint64_t encodes = 0, videoPackets = 0;
    int stuck = 0;  // comprobaciones seguidas dentro de la misma llamada al encoder
    int silent = 0; // seguidas con frames enviados y ningún paquete de vídeo de vuelta
    int idle = 0;   // seguidas sin que el hilo de vídeo envíe nada (habiendo imagen)
    int ticks = 0;  // desde el arranque
    bool reported = false; // ya se pidió reiniciar
    bool noted = false;    // ya se apuntó en el log algo que reiniciar no arregla
  } watch_;
  std::string gpuKey_;   // "|gráfica|driver" para las claves de VideoStall
  std::string nativeId_; // encoder propio de esta gráfica (abra o no)
  std::vector<std::string> sessionExcluded_; // colgados en esta sesión (ver VideoStall::remember)
  std::vector<std::string> stalledOnce_;     // propios de la gráfica que se colgaron una vez
  std::string native_;   // encoder propio de la gráfica que no abrió al arrancar
  std::shared_ptr<std::atomic<int>> probe_; // prueba en curso: -1, 0 no abre, 1 abre
  size_t probes_ = 0;    // pruebas hechas (no vuelve a 0 al reiniciar: ver NativeEncoderReady)
  ULONGLONG startedAt_ = 0;
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
