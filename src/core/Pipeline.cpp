#include "core/Pipeline.h"

#include "app/CrashHandler.h"
#include "common/Util.h"
#include "core/ThreadPolicy.h"

#include <avrt.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>

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

  // Fuente graba a los Hz del monitor (ver SourceFps). La captura tiene que
  // saberlo antes de empezar: limita los frames que entrega a ese ritmo.
  if (IsSourceQuality(settings_.quality)) {
    HMONITOR mon = settings_.target.monitor ? settings_.target.monitor
                                            : MonitorFromWindow(settings_.target.window, MONITOR_DEFAULTTONEAREST);
    SIZE size = TargetSize(settings_.target);
    int hz = MonitorRefreshHz(mon);
    settings_.fps = SourceFps(hz, size.cx, size.cy, settings_.fps);
    Log("Calidad Fuente: el monitor va a %d Hz, se graba a %d fps", hz, settings_.fps);
  }

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
  PerfMonitor::LogEnvironment(settings_.target.monitor
                                  ? settings_.target.monitor
                                  : MonitorFromWindow(settings_.target.window, MONITOR_DEFAULTTONEAREST));

  SIZE src = TargetSize(settings_.target);
  // Conversor y encoder para una salida de w x h (pares; quien llama nunca
  // pide más que la fuente). 1: abiertos; 0: ningún encoder se pudo abrir y
  // el conversor queda libre para otro intento; -1: falló el conversor.
  auto openVideo = [&](int w, int h, bool hardwareOnly) {
    width_ = std::max(w & ~1, 2);
    height_ = std::max(h & ~1, 2);
    if (!converter_.Init(gpu_.Device(), gpu_.Context(), UINT(width_), UINT(height_), UINT(settings_.fps)))
      return -1;
    VideoEncoderSettings es;
    es.width = width_;
    es.height = height_;
    es.fps = settings_.fps;
    plan_ = PlanQuality(settings_.quality, settings_.bitrateKbps, width_, height_, settings_.fps, settings_.codec);
    es.quality = plan_;
    es.codec = settings_.codec;
    es.preferred = settings_.encoder;
    es.hardwareOnly = hardwareOnly;
    if (encoder_.Open(es, gpu_.Device())) return 1;
    GpuLock lock(GpuMutex());
    converter_.Reset();
    return 0;
  };

  int opened = 0;
  if (IsSourceQuality(settings_.quality)) {
    // 2-3. Fuente: siempre la resolución nativa, sea cual sea la elegida en
    // Resolución. Si ningún encoder por GPU la admite (H.264 llega a 4096 de
    // ancho en NVENC y a 4096x2160 en muchas AMD, así que un monitor 5120x1440
    // no entra), se prueba en cajas cada vez más pequeñas antes que codificar
    // en la CPU, que a esa resolución le quitaría el procesador al juego.
    // Si tampoco abre a ningún tamaño con los fps del monitor (el nivel de
    // H.264 que admite la gráfica puede no llegar), se repite a 60. La captura
    // sigue al ritmo de antes: solo cuesta alguna copia de textura de más.
    settings_.outputHeight = 0;
    static const SIZE kBoxes[] = {{LONG_MAX, LONG_MAX}, {4096, 2304}, {2560, 1440}, {1920, 1080}};
    for (;;) {
      LONG lastHeight = LONG_MAX;
      for (const SIZE& box : kBoxes) {
        double fit = std::min({1.0, double(box.cx) / double(src.cx), double(box.cy) / double(src.cy)});
        LONG h = LONG(double(src.cy) * fit);
        if (h >= lastHeight) continue; // esta caja no la hace más pequeña
        lastHeight = h;
        opened = openVideo(int(double(src.cx) * fit), int(h), true);
        if (opened != 0) break;
      }
      if (opened != 0 || settings_.fps <= 60) break;
      Log("Calidad Fuente: ningún encoder por GPU abre a %d fps; se prueba a 60", settings_.fps);
      settings_.fps = 60;
    }
    if (opened > 0)
      Log("Calidad Fuente: %dx%d@%d (la fuente es %ldx%ld)", width_, height_, settings_.fps, src.cx, src.cy);
    // Solo pasa con un encoder por CPU elegido a mano: se respeta, pero la
    // nativa a los Hz del monitor en la CPU le quita el procesador al juego.
    // Una vez por sesión: la captura se reinicia al abrir o cerrar el juego.
    static std::atomic<bool> warnedCpu{false};
    if (opened > 0 && encoder_.Software() && notify_ && !warnedCpu.exchange(true))
      notify_("Fuente con el encoder por CPU elegido en Ajustes puede dar lag: pon Encoder en Automático para usar "
              "la gráfica",
              true);
    if (opened == 0) {
      // Sin encoder por GPU, lo máximo que da el PC sin lag es lo de abajo:
      // Máxima, y a 720p30 si acaba en la CPU.
      Log("Calidad Fuente: ningún encoder por GPU se pudo abrir; se sigue como Máxima");
      settings_.quality = "maxima";
    }
  }
  for (int pass = 0; opened == 0; ++pass) {
    // 2. Tamaño de salida: nunca se escala hacia arriba.
    int h = settings_.outputHeight > 0 ? std::min<int>(settings_.outputHeight, src.cy) : src.cy;
    int w = int(std::lround(double(h) * src.cx / src.cy));
    // 3. Encoder de vídeo.
    opened = openVideo(w, h, false);
    if (opened <= 0) break;

    // Sin encoder por GPU, codificar 1080p60 en la CPU le quita al juego
    // varios núcleos: se baja a 720p30, que cuesta unas 4,5 veces menos. Si
    // el encoder por CPU se eligió a mano, se respeta. La captura sigue a los
    // fps de antes: solo cuesta alguna copia de textura de más.
    if (pass > 0 || !encoder_.Software() || settings_.encoder == encoder_.Name() ||
        (height_ <= 720 && settings_.fps <= 30))
      break;
    Log("Encoder por CPU: se baja de %dx%d@%d a 720p30 como mucho para no quitarle CPU al juego", width_, height_,
        settings_.fps);
    settings_.encoder = encoder_.Name(); // la segunda vuelta no vuelve a probar los de GPU
    settings_.outputHeight = settings_.outputHeight > 0 ? std::min(settings_.outputHeight, 720) : 720;
    settings_.fps = std::min(settings_.fps, 30);
    encoder_.Close();
    {
      GpuLock lock(GpuMutex());
      converter_.Reset();
    }
    opened = 0;
    static std::atomic<bool> warned{false};
    if (notify_ && !warned.exchange(true))
      notify_("La gráfica no pudo codificar el vídeo: se graba a 720p30 por CPU. Actualiza su driver", true);
  }
  if (opened <= 0) {
    if (opened == 0 && notify_) notify_("No hay ningún encoder de vídeo disponible", true);
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
  buffer_->Configure(streams_, settings_.replaySeconds, BufferLimit(settings_.replaySeconds));
  if (!settings_.maxBufferBytes)
    Log("Replay: tope de memoria automático %zu MB para %.0f s", BufferLimit(settings_.replaySeconds) >> 20,
        settings_.replaySeconds);
  acceptPackets_ = true;

  // Lo que hay debajo de la webcam, aparte (solo con encoder por GPU; su
  // encoder se abre la primera vez que se ve la cámara).
  camEditable_ = settings_.cameraEditable;
  if (!encoder_.Software())
    camBackground_.Start(gpu_.Device(), gpu_.Context(), buffer_, encoder_.Name(), plan_, width_, height_,
                         settings_.fps);

  // 5. Cámara (opcional). Se abre en su hilo: aquí no se espera.
  if (settings_.camera) StartCamera(false);

  running_ = true;
  failed_ = false;
  videoThread_ = std::thread([this] {
    InstallThreadCrashHandler();
    try {
      VideoLoop();
    } catch (const std::exception& e) {
      Log("Vídeo: excepción en el hilo de captura: %s", e.what());
      failed_ = true;
    }
  });
  Log("Pipeline iniciado: %dx%d@%d, captura %s, encoder %s", width_, height_, settings_.fps, capture_->Name(),
      encoder_.Name().c_str());
  perf_.Start(gpu_.Adapter(), settings_.fps, [this](PerfMonitor::Sample* sample) {
    {
      std::lock_guard lock(statsMutex_);
      sample->video = window_;
      window_ = {};
    }
    char buf[200];
    snprintf(buf, sizeof(buf), "replay %zu MB de %zu MB (%.0f s), audio codificado %.0f ms por detrás",
             buffer_->Bytes() >> 20, buffer_->MaxBytes() >> 20, buffer_->DurationSeconds(),
             audio_.LatencySeconds() * 1000.0);
    sample->extra = buf;
  });
  return true;
}

namespace {
// La cuarta parte de la RAM: tope del replay automático.
double RamQuarter() {
  static const double quarter = [] {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    return GlobalMemoryStatusEx(&mem) ? double(mem.ullTotalPhys) / 4.0 : double(1u << 30);
  }();
  return quarter;
}
} // namespace

double Pipeline::ReplayBytesPerSecond() const {
  // Peor caso: todo el rato al bitrate máximo del preset (en VBR se llega a él
  // en escenas con mucho movimiento), más el audio y un 10 %.
  double kbps = plan_.maxKbps;
  for (size_t i = 1; i < streams_.size(); ++i)
    if (streams_[i].params) kbps += double(streams_[i].params->bit_rate) / 1000.0;
  if (settings_.camera && settings_.cameraEditable && !encoder_.Software())
    kbps += CamBackground::MaxKbps(plan_, width_, height_, settings_.cameraSizePercent);
  return kbps * 125.0 * 1.1;
}

size_t Pipeline::BufferLimit(double seconds) const {
  if (settings_.maxBufferBytes) return settings_.maxBufferBytes;
  // Lo que pide la duración más los segundos de margen del anillo, sin pasar
  // de la cuarta parte de la RAM: con 16 GB caben 5 min en Alta a 1440p; con
  // poca RAM el clip sale más corto antes que hacer que Windows mande el juego
  // al archivo de paginación.
  double bytes = ReplayBytesPerSecond() * (seconds + ReplayBuffer::kExtraSeconds);
  return size_t(std::max(std::min(bytes, RamQuarter()), double(64u << 20)));
}

Pipeline::~Pipeline() {
  Stop();
  // Al salir sí se espera a que terminen de escribirse las grabaciones
  // cortadas (ver ReleaseRecorder), como antes del corte por la cola.
  for (auto& t : recorderDrains_) t.join();
}

void Pipeline::Stop() {
  perf_.Stop();
  running_ = false;
  if (videoThread_.joinable()) videoThread_.join();
  camBackground_.Stop();
  audio_.Stop();
  ReleaseRecorder();
  {
    // Stop no espera a la cámara: si su driver tarda en soltarla, su hilo
    // termina solo (ver Camera).
    std::unique_ptr<Camera> camera;
    {
      GpuLock lock(GpuMutex());
      camera = std::move(camera_);
    }
    if (camera) camera->Stop();
  }
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
  encodeMs_ = frameAgeMs_ = 0;
  window_ = {};
}

void Pipeline::StartCamera(bool userAsked) {
  auto camera = std::make_unique<Camera>();
  UINT camWidth = UINT(std::max(width_ * settings_.cameraSizePercent / 100, 16));
  if (!camera->Start(gpu_.Device(), settings_.cameraId, camWidth, notify_, userAsked)) return;
  GpuLock lock(GpuMutex());
  camera_ = std::move(camera);
  cameraComposited_ = false;
}

void Pipeline::SetCamera(const PipelineSettings& s) {
  if (!running_) return; // al arrancar ya se usan los ajustes nuevos
  const bool reopen = s.camera != settings_.camera ||
                      (s.camera && (s.cameraId != settings_.cameraId || s.cameraSizePercent != settings_.cameraSizePercent));
  if (!reopen && s.cameraCorner == settings_.cameraCorner) return;
  std::unique_ptr<Camera> old;
  {
    GpuLock lock(GpuMutex());
    settings_.camera = s.camera;
    settings_.cameraId = s.cameraId;
    settings_.cameraCorner = s.cameraCorner;
    settings_.cameraSizePercent = s.cameraSizePercent;
    if (reopen) old = std::move(camera_);
  }
  if (reopen || s.camera)
    Log("Cámara: %s con la captura en marcha, sin reiniciarla",
        !reopen ? "cambia de esquina" : s.camera ? (old ? "se vuelve a abrir" : "se activa") : "se desactiva");
  if (old) old->Stop(); // no espera
  if (reopen && s.camera) StartCamera(true);
  // El tope de RAM del replay cuenta la pista del fondo de la cámara
  // (ReplayBytesPerSecond), que depende de si hay cámara y de su tamaño.
  if (reopen && buffer_) buffer_->SetLimits(settings_.replaySeconds, BufferLimit(settings_.replaySeconds));
}

std::string Pipeline::TakeCameraStall() { return running_ && camera_ ? camera_->TakeStall() : std::string(); }

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
    if (camera_->Latest(&cam, &cw, &ch)) {
      int64_t t0 = QpcNow();
      camRect_ = CameraRect(cw, ch);
      bool ok = converter_.Overlay(cam.Get(), cw, ch, camRect_, dst, slice);
      camDrawn_ = ok;
      if (!cameraComposited_) {
        // Una vez por cámara: si algo se atasca al componerla, que se vea en el log.
        cameraComposited_ = true;
        Log("Cámara: primera composición sobre el vídeo %s en %.1f ms (%ux%u)", ok ? "hecha" : "FALLÓ",
            QpcToSeconds(QpcNow() - t0) * 1000.0, cw, ch);
      }
    }
  }
  return true;
}

void Pipeline::VideoLoop() {
  // Con encoder por GPU el hilo apenas usa CPU y la prioridad multimedia le
  // hace despertar puntual en cada ranura. Con encoder por CPU es al revés:
  // codifica aquí mismo, y con prioridad multimedia (por encima de cualquier
  // hilo del juego) le quitaría un núcleo entero. Entonces el juego va primero.
  DWORD taskIndex = 0;
  HANDLE mmcss = nullptr;
  if (encoder_.Software()) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  } else {
    mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  }

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

  const int64_t freq = QpcFrequency();
  const int fps = settings_.fps;
  const double period = double(freq) / fps; // ticks de QPC por ranura
  // Fase de la rejilla (R18). Con el monitor a los mismos Hz que la salida
  // llega un frame por ranura y, si la rejilla cae justo donde llegan, unos
  // pocos ms de ruido hacen que una ranura vea aún el frame anterior (sale
  // repetido) y la siguiente se salte uno: el clip va a tirones. Mientras la
  // captura entrega un frame por ranura, la rejilla se corre poco a poco para
  // muestrear a mitad de camino entre dos llegadas. Puede ir de -1/2 a 3/2
  // periodos (como mucho 17 ms de desfase con el audio a 60 fps); al salirse
  // vuelve un periodo, que es el frame repetido o saltado que de todas formas
  // exige un monitor a 59,94 Hz. Simulado en investigacion/simulacion-fase.py.
  double phase = 0;
  std::array<uint8_t, 30> arrivals{}; // frames nuevos en cada una de las últimas ranuras
  int arrivalsSum = 0;
  size_t arrivalsPos = 0, arrivalsCount = 0;
  int64_t lastIndex = -1;
  uint64_t lastSequence = 0;
  int consecutiveFailures = 0;
  auto sink = [this](const AVPacket* pkt, int stream) {
    if (acceptPackets_) buffer_->Push(pkt, stream);
  };

  while (running_ && !failed_) {
    int64_t now = QpcNow();
    double rel = double(now - startQpc_) - phase;
    int64_t index = rel < 0 ? -1 : int64_t(rel / period);
    if (index > lastIndex) {
      double lateMs = (rel - double(index) * period) * 1000.0 / double(freq);
      CapturedFrame frame = capture_->Latest();
      if (frame.texture) {
        int64_t t0 = QpcNow();
        // El pts es el instante de la ranura, no el de la imagen: un frame
        // que llegó hace X ms se muestra X ms tarde respecto al audio. Se mide
        // solo con frames nuevos (uno repetido en una escena quieta no cuenta).
        bool fresh = frame.sequence != lastSequence;
        uint64_t arrived = lastSequence ? frame.sequence - lastSequence : 1;
        lastSequence = frame.sequence;
        double ageMs = QpcToSeconds(t0 - frame.qpc) * 1000.0;
        camDrawn_ = false;
        bool ok = encoder_.Encode(
            index, [&](ID3D11Texture2D* dst, UINT slice) { return Render(dst, slice, frame); }, sink);
        if (ok) camBackground_.Frame(index, frame, camDrawn_ && camEditable_ ? &camRect_ : nullptr);
        double ms = QpcToSeconds(QpcNow() - t0) * 1000.0;

        uint8_t count = uint8_t(std::min<uint64_t>(arrived, 3));
        arrivalsSum += int(count) - int(arrivals[arrivalsPos]);
        arrivals[arrivalsPos] = count;
        arrivalsPos = (arrivalsPos + 1) % arrivals.size();
        arrivalsCount = std::min(arrivalsCount + 1, arrivals.size());
        bool inPhase = arrivalsCount == arrivals.size() && std::abs(arrivalsSum - int(arrivals.size())) <= 1;
        if (inPhase && fresh && arrived == 1) {
          phase -= 0.1 * (double(t0 - frame.qpc) - period / 2);
          if (phase < -period / 2) phase += period;
          else if (phase >= 1.5 * period) phase -= period;
        }

        {
          std::lock_guard lock(statsMutex_);
          encodeMs_ = encodeMs_ * 0.95 + ms * 0.05;
          if (fresh && ageMs >= 0 && ageMs < 1000) frameAgeMs_ = frameAgeMs_ * 0.95 + ageMs * 0.05;
          if (ok) framesEncoded_++;
          window_.slots++;
          if (ok) window_.encoded++;
          window_.AddEncode(ms);
          if (fresh) window_.captured += arrived;
          else window_.repeated++;
          if (inPhase) window_.inPhase++;
          window_.lateMaxMs = std::max(window_.lateMaxMs, lateMs);
          if (lastIndex >= 0 && index > lastIndex + 1) {
            framesSkipped_ += uint64_t(index - lastIndex - 1);
            window_.skipped += uint64_t(index - lastIndex - 1);
          }
        }
        // GPU reiniciada (driver actualizado, TDR) o encoder que ya no acepta
        // nada: mejor reiniciar el pipeline entero que seguir fallando en silencio.
        consecutiveFailures = ok ? 0 : consecutiveFailures + 1;
        HRESULT removed = gpu_.Device() ? gpu_.Device()->GetDeviceRemovedReason() : S_OK;
        if (removed != S_OK || consecutiveFailures > fps * 3) {
          Log("Vídeo: %s (0x%08X), se reinicia la captura en unos segundos",
              removed != S_OK ? "la GPU se reinició" : "el encoder lleva 3 s sin aceptar frames", unsigned(removed));
          failedAt_ = GetTickCount64();
          failed_ = true;
        }
      }
      lastIndex = index;
    }
    // Dormir hasta la siguiente ranura.
    int64_t next = startQpc_ + int64_t(phase + double(lastIndex + 1) * period);
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
  auto notify = notify_;
  // El clip termina en el instante de la tecla, el mismo para vídeo y audio.
  double end = QpcToSeconds(QpcNow() - startQpc_);
  double age;
  {
    std::lock_guard lock(statsMutex_);
    age = frameAgeMs_;
  }
  Log("Clip: %.0f s pedidos, el frame de vídeo llega al encoder con %.1f ms de media", seconds, age);
  // El audio se codifica con un margen (100 ms, hasta 1 s si una fuente llega
  // tarde): se espera ese margen y un poco más para que haya audio hasta `end`.
  double latency = streams_.size() > 1 ? audio_.LatencySeconds() : 0.0;
  DWORD waitMs = DWORD(std::clamp(latency + 0.15, 0.15, 1.5) * 1000.0);
  auto pending = std::make_shared<PendingWrite>(); // al salir se espera a que termine
  std::thread([buffer, notify, path, seconds, end, pending, waitMs] {
    InstallThreadCrashHandler();
    Sleep(waitMs);
    bool ok = false;
    try {
      // La copia de la lista bloquea el anillo, al que entregan sus paquetes
      // los hilos de vídeo y audio: se hace con la prioridad normal del hilo,
      // no con la baja de la escritura, para que el juego no pueda dejarlo sin
      // turno con el lock cogido. Son referencias, ~1 ms con 5 min de replay.
      int64_t t0 = QpcNow();
      auto packets = buffer->Snapshot(seconds, end);
      auto streams = buffer->Streams(); // con las del fondo de la cámara añadidas por el camino
      double copyMs = QpcToSeconds(QpcNow() - t0) * 1000.0;
      // Escribir sí puede esperar: prioridad baja y, dentro de WriteClip, E/S
      // y memoria de fondo.
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      SetThreadEcoQoS(true);
      Log("Clip: %zu paquetes tomados del anillo en %.1f ms", packets.size(), copyMs);
      t0 = QpcNow();
      ok = WriteClip(path, streams, packets);
      Log("Clip: escrito en %.0f ms", QpcToSeconds(QpcNow() - t0) * 1000.0);
      // Los paquetes que el anillo soltó mientras se escribía se liberan aquí,
      // y liberar toma el lock del heap, que el hilo de vídeo necesita en cada
      // frame: con prioridad normal, por lo mismo que la copia.
      SetThreadEcoQoS(false);
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
      packets.clear();
    } catch (const std::exception& e) {
      Log("Clip: excepción al guardar: %s", e.what());
    }
    Log("Clip %s: %s", ok ? "guardado" : "FALLÓ", path.c_str());
    if (notify) {
      auto name = std::filesystem::path(Widen(path)).filename();
      notify((ok ? "Clip guardado: " : "No se pudo guardar el clip: ") + Narrow(name.wstring()), !ok);
    }
  }).detach();
}

bool Pipeline::StartRecording(const std::string& path) {
  if (!running_ || Recording()) return false;
  if (recorder_) {
    // La anterior se cortó sola porque el disco no daba abasto. Su hilo vacía
    // la cola y cierra el archivo; esperarle aquí congelaría la interfaz.
    if (!recorder_->Finished()) {
      if (notify_) notify_("Aún se está guardando la grabación anterior en el disco", true);
      return false;
    }
    buffer_->DetachLive();
    recorder_->Stop();
    recorder_.reset();
  }
  recorder_ = std::make_unique<Recorder>();
  auto notify = notify_;
  recorder_->onOverflow = [notify] {
    if (notify)
      notify("Grabación detenida: el disco no daba abasto. Se guardó hasta ese momento; prueba otro disco o menos "
             "calidad",
             true);
  };
  // Vídeo, audio y, si se ve la cámara, su fondo de ahora (ver CamBackground).
  // Las pistas del fondo de antes no: una pista que no recibe nada haría que
  // el muxer retuviera lo demás. Una que empiece a mitad no entra.
  const StreamList all = buffer_->Streams();
  StreamList streams(all.begin(), all.begin() + std::ptrdiff_t(std::min(all.size(), streams_.size())));
  std::vector<int> map(all.size(), -1);
  for (size_t i = 0; i < streams.size(); ++i) map[i] = int(i);
  const int cam = camBackground_.ActiveStream();
  if (cam >= int(streams.size()) && cam < int(all.size())) {
    map[size_t(cam)] = int(streams.size());
    streams.push_back(all[size_t(cam)]);
  }
  if (!recorder_->Start(path, streams)) {
    recorder_.reset();
    return false;
  }
  Recorder* rec = recorder_.get();
  buffer_->AttachLive([rec, map](const AVPacket* pkt, int stream) {
    if (stream >= 0 && size_t(stream) < map.size() && map[size_t(stream)] >= 0) rec->Push(pkt, map[size_t(stream)]);
  });
  return true;
}

void Pipeline::StopRecording() { ReleaseRecorder(); }

void Pipeline::ReleaseRecorder() {
  if (!recorder_) return;
  if (buffer_) buffer_->DetachLive();
  if (recorder_->Overflowed() && !recorder_->Finished()) {
    // Se cortó porque el disco no daba abasto: su hilo aún vacía hasta 256 MB
    // de cola a la velocidad de ese disco (a 5 MB/s, casi un minuto) y cierra
    // el archivo él solo. Esperarle aquí congelaría la interfaz, que es quien
    // para el pipeline al cambiar un ajuste o al cerrarse el juego: se le
    // espera en otro hilo, contado como escritura pendiente para que una
    // actualización no se instale antes de que acabe.
    recorderDrains_.emplace_back(
        [rec = std::move(recorder_), pending = std::make_shared<PendingWrite>()]() mutable {
          InstallThreadCrashHandler();
          rec->Stop();
          rec.reset();
          pending.reset();
        });
    return;
  }
  recorder_->Stop();
  recorder_.reset();
}

void Pipeline::SetCameraEditable(bool editable) {
  if (settings_.cameraEditable == editable) return;
  settings_.cameraEditable = editable;
  camEditable_ = editable;
  // El tope de RAM del replay cuenta la pista del fondo (ReplayBytesPerSecond).
  if (buffer_) buffer_->SetLimits(settings_.replaySeconds, BufferLimit(settings_.replaySeconds));
}

void Pipeline::SetReplaySeconds(double seconds) {
  settings_.replaySeconds = seconds;
  if (buffer_) buffer_->SetLimits(seconds, BufferLimit(seconds));
}

PipelineStats Pipeline::Stats() {
  PipelineStats s;
  s.running = running_;
  if (!running_) return s;
  s.encoder = encoder_.Label() + (encoder_.ZeroCopy() ? " (cero copias)" : "");
  s.encoderSoftware = encoder_.Software();
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
    s.bufferLimitBytes = buffer_->MaxBytes();
  }
  const double perSecond = ReplayBytesPerSecond();
  const double cap = settings_.maxBufferBytes ? double(settings_.maxBufferBytes) : RamQuarter();
  if (perSecond > 0) s.replayFitSeconds = std::max(0.0, cap / perSecond - ReplayBuffer::kExtraSeconds);
  s.audioTracks = int(streams_.size()) - 1;
  s.recording = Recording();
  if (s.recording) {
    s.recordSeconds = recorder_->Seconds();
    s.recordPath = recorder_->Path();
  }
  return s;
}

} // namespace spectre
