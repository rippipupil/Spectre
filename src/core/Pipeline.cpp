#include "core/Pipeline.h"

#include "common/Util.h"

#include <avrt.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace spectre {

namespace {

SIZE TargetSize(const CaptureTarget& target) {
  RECT r{};
  if (target.window) {
    if (FAILED(DwmGetWindowAttribute(target.window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
      GetWindowRect(target.window, &r);
  } else {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(target.monitor, &mi);
    r = mi.rcMonitor;
  }
  return {std::max<LONG>(r.right - r.left, 2), std::max<LONG>(r.bottom - r.top, 2)};
}

} // namespace

bool Pipeline::Start(const PipelineSettings& settings, Notify notify) {
  Stop();
  settings_ = settings;
  notify_ = std::move(notify);

  if (!settings_.target.window && !settings_.target.monitor)
    settings_.target.monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);

  // 1. Dispositivo y captura. WGC si puede capturar sin el marco amarillo
  // (o si se eligió a mano); si no, Desktop Duplication, que nunca lo dibuja.
  // WGC con marco queda como último recurso.
  bool wgcAvailable = settings_.captureApi != "dda" && WgcSupported();
  bool preferWgc = wgcAvailable && (settings_.captureApi == "wgc" || WgcBorderless());
  auto tryWgc = [&] {
    if (!gpu_.Create(nullptr)) return false;
    capture_ = CreateWgcCapture();
    return capture_->Start(gpu_.Device(), settings_.target, UINT(settings_.fps), settings_.cursor);
  };
  auto tryDda = [&] {
    HMONITOR mon = settings_.target.monitor ? settings_.target.monitor
                                            : MonitorFromWindow(settings_.target.window, MONITOR_DEFAULTTOPRIMARY);
    auto adapter = GpuDevice::AdapterForMonitor(mon);
    if (!gpu_.Create(adapter.Get())) return false;
    capture_ = CreateDdaCapture();
    CaptureTarget t;
    t.monitor = mon;
    if (!capture_->Start(gpu_.Device(), t, UINT(settings_.fps), settings_.cursor)) return false;
    settings_.target = t;
    return true;
  };
  bool started = preferWgc && tryWgc();
  if (!started && settings_.captureApi != "wgc") {
    capture_.reset();
    started = tryDda();
  }
  if (!started && wgcAvailable && !preferWgc) {
    capture_.reset();
    started = tryWgc();
  }
  if (!started) {
    Log("No se pudo iniciar la captura");
    capture_.reset();
    gpu_.Reset();
    return false;
  }
  if (settings_.gpuPriority) gpu_.BoostGpuPriority();

  // 2. Tamaño de salida: nunca se escala hacia arriba; dimensiones pares.
  SIZE src = TargetSize(settings_.target);
  int h = settings_.outputHeight > 0 ? std::min<int>(settings_.outputHeight, src.cy) : src.cy;
  int w = int(std::lround(double(h) * src.cx / src.cy));
  width_ = std::max(w & ~1, 2);
  height_ = std::max(h & ~1, 2);

  if (!converter_.Init(gpu_.Device(), gpu_.Context(), UINT(width_), UINT(height_), UINT(settings_.fps))) {
    Stop();
    return false;
  }

  // 3. Encoder de vídeo.
  VideoEncoderSettings es;
  es.width = width_;
  es.height = height_;
  es.fps = settings_.fps;
  plan_ = PlanQuality(settings_.quality, settings_.bitrateKbps, width_, height_, settings_.fps, settings_.codec);
  es.quality = plan_;
  es.codec = settings_.codec;
  es.preferred = settings_.encoder;
  if (!encoder_.Open(es, gpu_.Device())) {
    if (notify_) notify_("No hay ningún encoder de vídeo disponible", true);
    Stop();
    return false;
  }

  buffer_ = std::make_shared<ReplayBuffer>();
  startQpc_ = QpcNow();
  auto sink = [this](const AVPacket* pkt, int stream) {
    if (acceptPackets_) buffer_->Push(pkt, stream);
  };

  // 4. Audio (opcional: si falla se graba solo vídeo).
  streams_.clear();
  streams_.push_back(MakeStreamDesc(encoder_.Context(), true, "Vídeo"));
  if (settings_.audio.system || settings_.audio.mic) {
    if (audio_.Start(settings_.audio, startQpc_, 1, sink)) {
      for (const auto& s : audio_.Streams()) streams_.push_back(s);
    } else if (notify_) {
      notify_("No se pudo capturar el audio; se grabará solo vídeo", true);
    }
  }
  buffer_->Configure(streams_, settings_.replaySeconds, settings_.maxBufferBytes);
  acceptPackets_ = true;

  // 5. Cámara (opcional).
  if (settings_.camera) {
    camera_ = std::make_unique<Camera>();
    if (!camera_->Start(gpu_.Device(), settings_.cameraId)) camera_.reset();
  }

  running_ = true;
  failed_ = false;
  videoThread_ = std::thread([this] {
    try {
      VideoLoop();
    } catch (const std::exception& e) {
      Log("Vídeo: excepción en el hilo de captura: %s", e.what());
      failed_ = true;
    }
  });
  Log("Pipeline iniciado: %dx%d@%d, captura %s, encoder %s", width_, height_, settings_.fps, capture_->Name(),
      encoder_.Name().c_str());
  return true;
}

void Pipeline::Stop() {
  running_ = false;
  if (videoThread_.joinable()) videoThread_.join();
  audio_.Stop();
  if (recorder_) {
    if (buffer_) buffer_->DetachLive();
    recorder_->Stop();
    recorder_.reset();
  }
  if (camera_) camera_->Stop();
  camera_.reset();
  if (capture_) capture_->Stop();
  capture_.reset();
  acceptPackets_ = false;
  encoder_.Close();
  {
    GpuLock lock(GpuMutex());
    converter_.Reset();
  }
  buffer_.reset();
  streams_.clear();
  gpu_.Reset();
  framesEncoded_ = framesSkipped_ = 0;
  encodeMs_ = 0;
}

RECT Pipeline::CameraRect(UINT camWidth, UINT camHeight) const {
  LONG w = LONG(width_ * settings_.cameraSizePercent / 100) & ~1L;
  LONG h = LONG(double(w) * camHeight / std::max<UINT>(camWidth, 1)) & ~1L;
  LONG margin = LONG(height_ / 40) & ~1L;
  const auto& c = settings_.cameraCorner;
  LONG x = c.find("left") != std::string::npos ? margin : LONG(width_) - w - margin;
  LONG y = c.find("top") != std::string::npos ? margin : LONG(height_) - h - margin;
  return {x & ~1L, y & ~1L, (x & ~1L) + w, (y & ~1L) + h};
}

bool Pipeline::Render(ID3D11Texture2D* dst, UINT slice, const CapturedFrame& frame) {
  if (!converter_.Convert(frame.texture.Get(), frame.width, frame.height, dst, slice)) return false;
  if (camera_) {
    ComPtr<ID3D11Texture2D> cam;
    UINT cw = 0, ch = 0;
    if (camera_->Latest(&cam, &cw, &ch)) converter_.Overlay(cam.Get(), cw, ch, CameraRect(cw, ch), dst, slice);
  }
  return true;
}

void Pipeline::VideoLoop() {
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

  const int64_t freq = QpcFrequency();
  const int fps = settings_.fps;
  int64_t lastIndex = -1;
  int consecutiveFailures = 0;
  auto sink = [this](const AVPacket* pkt, int stream) {
    if (acceptPackets_) buffer_->Push(pkt, stream);
  };

  while (running_ && !failed_) {
    int64_t now = QpcNow();
    int64_t index = (now - startQpc_) * fps / freq;
    if (index > lastIndex) {
      CapturedFrame frame = capture_->Latest();
      if (frame.texture) {
        int64_t t0 = QpcNow();
        bool ok = encoder_.Encode(
            index, [&](ID3D11Texture2D* dst, UINT slice) { return Render(dst, slice, frame); }, sink);
        double ms = QpcToSeconds(QpcNow() - t0) * 1000.0;
        {
          std::lock_guard lock(statsMutex_);
          encodeMs_ = encodeMs_ * 0.95 + ms * 0.05;
          if (ok) framesEncoded_++;
          if (lastIndex >= 0 && index > lastIndex + 1) framesSkipped_ += uint64_t(index - lastIndex - 1);
        }
        // GPU reiniciada (driver actualizado, TDR) o encoder que ya no acepta
        // nada: mejor reiniciar el pipeline entero que seguir fallando en silencio.
        consecutiveFailures = ok ? 0 : consecutiveFailures + 1;
        HRESULT removed = gpu_.Device() ? gpu_.Device()->GetDeviceRemovedReason() : S_OK;
        if (removed != S_OK || consecutiveFailures > fps * 3) {
          Log("Vídeo: %s (0x%08X), se reinicia la captura",
              removed != S_OK ? "la GPU se reinició" : "el encoder lleva 3 s sin aceptar frames", unsigned(removed));
          failed_ = true;
        }
      }
      lastIndex = index;
    }
    // Dormir hasta el siguiente frame.
    int64_t next = startQpc_ + (lastIndex + 1) * freq / fps;
    int64_t wait100ns = (next - QpcNow()) * 10'000'000 / freq;
    if (wait100ns > 0) {
      LARGE_INTEGER due;
      due.QuadPart = -wait100ns;
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, 100);
      else
        Sleep(DWORD(std::max<int64_t>(wait100ns / 10'000, 1)));
    }
  }
  encoder_.Flush(sink);
  if (timer) CloseHandle(timer);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void Pipeline::SaveClip(const std::string& path, double seconds) {
  if (!running_ || !buffer_) {
    if (notify_) notify_("El replay no está activo", true);
    return;
  }
  auto buffer = buffer_;
  auto streams = streams_;
  auto notify = notify_;
  auto pending = std::make_shared<PendingWrite>(); // al salir se espera a que termine
  std::thread([buffer, streams, notify, path, seconds, pending] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Sleep(300); // deja que llegue el audio de los últimos milisegundos
    bool ok = false;
    try {
      auto packets = buffer->Snapshot(seconds);
      ok = WriteClip(path, streams, packets);
    } catch (const std::exception& e) {
      Log("Clip: excepción al guardar: %s", e.what());
    }
    Log("Clip %s: %s", ok ? "guardado" : "FALLÓ", path.c_str());
    if (notify) {
      auto name = std::filesystem::path(Widen(path)).filename();
      notify(ok ? "Clip guardado: " + Narrow(name.wstring()) : "No se pudo guardar el clip", !ok);
    }
  }).detach();
}

bool Pipeline::StartRecording(const std::string& path) {
  if (!running_ || Recording()) return false;
  recorder_ = std::make_unique<Recorder>();
  if (!recorder_->Start(path, streams_)) {
    recorder_.reset();
    return false;
  }
  Recorder* rec = recorder_.get();
  buffer_->AttachLive([rec](const AVPacket* pkt, int stream) { rec->Push(pkt, stream); });
  return true;
}

void Pipeline::StopRecording() {
  if (!recorder_) return;
  if (buffer_) buffer_->DetachLive();
  recorder_->Stop();
  recorder_.reset();
}

void Pipeline::SetReplaySeconds(double seconds) {
  settings_.replaySeconds = seconds;
  if (buffer_) buffer_->SetSeconds(seconds);
}

PipelineStats Pipeline::Stats() {
  PipelineStats s;
  s.running = running_;
  if (!running_) return s;
  s.encoder = encoder_.Name() + (encoder_.ZeroCopy() ? " (cero copias)" : "");
  s.capture = capture_ ? capture_->Name() : "";
  s.gpu = gpu_.AdapterName();
  s.width = width_;
  s.height = height_;
  s.fps = settings_.fps;
  s.targetKbps = plan_.targetKbps;
  s.maxKbps = plan_.maxKbps;
  s.systemAudio = audio_.SystemDescription();
  s.micAudio = audio_.MicDescription();
  {
    std::lock_guard lock(statsMutex_);
    s.encodeMs = encodeMs_;
    s.framesEncoded = framesEncoded_;
    s.framesSkipped = framesSkipped_;
  }
  if (buffer_) {
    s.bufferBytes = buffer_->Bytes();
    s.bufferSeconds = buffer_->DurationSeconds();
  }
  s.audioTracks = int(streams_.size()) - 1;
  s.recording = Recording();
  if (s.recording) {
    s.recordSeconds = recorder_->Seconds();
    s.recordPath = recorder_->Path();
  }
  return s;
}

} // namespace spectre
