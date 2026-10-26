#include "ui/EditorUi.h"

#include "app/CrashHandler.h"
#include "app/TextImage.h"
#include "common/Util.h"
#include "ui/AppUi.h"
#include "ui/EditorWidgets.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace spectre {

using namespace ui;

namespace {

constexpr float kToolsWidth = 336.0f;
constexpr float kFooterHeight = 64.0f;
constexpr size_t kMaxUndo = 100;
constexpr double kMinRange = 0.2;  // lo más corto que se puede dejar el recorte
constexpr double kMinPart = 0.1;   // trozo quitado o tramo lento más corto
constexpr int kOverlayBox = 1280;  // el texto de la vista previa en movimiento, como mucho a este tamaño

std::string Title(const MediaItem& item) {
  if (!item.spectre) return item.stem;
  return item.game.empty() ? std::string("Escritorio") : item.game;
}

// 01:02.3
std::string Precise(double seconds) {
  seconds = std::max(0.0, seconds);
  const int tenthsTotal = int(std::floor(seconds * 10.0 + 1e-6));
  const int whole = tenthsTotal / 10, tenths = tenthsTotal % 10;
  char buf[32];
  if (whole >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d.%d", whole / 3600, (whole / 60) % 60, whole % 60, tenths);
  else snprintf(buf, sizeof(buf), "%02d:%02d.%d", whole / 60, whole % 60, tenths);
  return buf;
}

// 2,5 s (coma decimal) o, desde un minuto, 1:05.
std::string Length(double seconds) {
  if (seconds >= 59.95) return FormatDuration(seconds);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f s", std::max(0.0, seconds));
  std::string s = buf;
  for (auto& c : s)
    if (c == '.') c = ',';
  if (s.size() > 4 && s.compare(s.size() - 4, 4, ",0 s") == 0) s.erase(s.size() - 4, 2);
  return s;
}

} // namespace

// Piezas de las herramientas que comparte el editor de fotos (EditorWidgets.h).
namespace editui {

bool TypingText() { return ImGui::GetIO().WantTextInput; }

std::string NameOf(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Título de una herramienta. Con `on`, un interruptor a la derecha y toda la
// fila lo cambia; bold=false para las opciones dentro de una herramienta.
bool SwitchRow(const char* label, bool* on, bool bold) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x, h = Px(28);
  ImGui::PushID(label);
  bool changed = false;
  const ImVec2 q = ImGui::GetCursorScreenPos();
  if (on) {
    if (ImGui::InvisibleButton("row", ImVec2(w, h))) {
      *on = !*on;
      changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    DrawSwitch(dl, ImVec2(q.x + w, q.y + h * 0.5f), *on, ImGui::GetItemID() ^ 0x5157u, 1.0f);
  } else {
    ImGui::Dummy(ImVec2(w, h));
  }
  const float fs = Px(bold ? kBody : kSmall);
  ImFont* font = bold ? Bold() : ImGui::GetFont();
  Text(dl, font, fs, ImVec2(q.x, q.y + (h - fs) * 0.5f), C(bold || (on && *on) ? p.text : p.textMuted), label);
  ImGui::PopID();
  return changed;
}

bool ToolHeader(const char* title, bool* on) { return SwitchRow(title, on, true); }

// Texto pequeño encima de un control.
void Label(const char* text) {
  ImVec2 q = ImGui::GetCursorScreenPos();
  Text(ImGui::GetWindowDrawList(), ImGui::GetFont(), Px(kSmall), q, C(Colors().textMuted), text);
  ImGui::Dummy(ImVec2(0, Px(kSmall) + Px(5)));
}

void Note(const std::string& text) { Help(text.c_str()); }

// Dos botones del mismo ancho en una fila; devuelve 1, 2 o 0.
int TwoButtons(const char* id1, const char* label1, const char* id2, const char* label2) {
  const float half = (ImGui::GetContentRegionAvail().x - Px(8)) * 0.5f;
  int pressed = 0;
  if (GhostButton(id1, label1, half, Tone::Normal, 28)) pressed = 1;
  ImGui::SameLine(0, Px(8));
  if (GhostButton(id2, label2, half, Tone::Normal, 28)) pressed = 2;
  return pressed;
}

// Rejilla de opciones planas (estilos de color, esquinas). Devuelve la pulsada o -1.
int ChoiceGrid(const char* id, const std::vector<std::pair<int, std::string>>& options, int current, int cols) {
  const Palette& p = Colors();
  ImGui::PushID(id);
  const int count = int(options.size());
  const int rows = (count + cols - 1) / cols;
  const float h = Px(30), line = Px(1);
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, cellW = w / float(cols);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h * float(rows)), C(p.surface));
  int clicked = -1;
  for (int i = 0; i < count; ++i) {
    const ImVec2 q(a.x + cellW * float(i % cols), a.y + h * float(i / cols));
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("cell", ImVec2(cellW, h))) clicked = options[size_t(i)].first;
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const bool sel = options[size_t(i)].first == current;
    const ImVec2 b(q.x + cellW, q.y + h);
    if (sel) dl->AddRectFilled(q, b, C(p.accent));
    else if (hov) dl->AddRectFilled(q, b, C(p.surfaceHover));
    if (i % cols != 0) dl->AddRectFilled(q, ImVec2(q.x + line, b.y), C(p.bg));
    if (i >= cols) dl->AddRectFilled(q, ImVec2(b.x, q.y + line), C(p.bg));
    const float ss = Px(kSmall);
    ImFont* font = sel ? Bold() : ImGui::GetFont();
    const std::string& label = options[size_t(i)].second;
    const ImVec2 ts = TextSize(font, ss, label.c_str());
    Text(dl, font, ss, ImVec2(q.x + (cellW - ts.x) * 0.5f, q.y + (h - ts.y) * 0.5f),
         C(sel ? p.onAccent : hov ? p.text : p.textMuted), label.c_str());
  }
  ImGui::SetCursorScreenPos(a);
  ImGui::Dummy(ImVec2(w, h * float(rows)));
  ImGui::PopID();
  return clicked;
}

// Deslizador de -100 a 100 a todo el ancho; clic derecho lo deja en 0.
bool CenteredSlider(const char* id, int* value) {
  ImGui::SetNextItemWidth(-1);
  bool changed = ImGui::SliderInt(id, value, -100, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    changed = *value != 0;
    *value = 0;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
    ImGui::SetTooltip("Clic derecho: volver a 0");
  return changed;
}

// Volumen en % (0 a 200) a todo el ancho; clic derecho lo deja en 100 %.
bool PercentSlider(const char* id, float* value) {
  int pct = int(std::lround(*value * 100.0f));
  ImGui::SetNextItemWidth(-1);
  bool changed = ImGui::SliderInt(id, &pct, 0, 200, "%d %%", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    changed = pct != 100;
    pct = 100;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
    ImGui::SetTooltip("Clic derecho: volver a 100 %%");
  if (changed) *value = float(pct) / 100.0f;
  return changed;
}

// 0xRRGGBB de un color de ImGui (ABGR).
uint32_t Rgb(ImU32 c) { return ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu); }

} // namespace editui

using namespace editui;

namespace {

// Lo que cambia la forma de la imagen: con lo mismo, una vista previa exacta
// anterior sigue sirviendo mientras llega la nueva (color, texto).
bool SameShape(const EditSettings& a, const EditSettings& b) {
  return a.aspect == b.aspect && a.fit == b.fit && a.zoom == b.zoom && a.panX == b.panX && a.panY == b.panY &&
         a.rotate == b.rotate && a.flipH == b.flipH && a.flipV == b.flipV && a.camEdit == b.camEdit &&
         a.camHide == b.camHide && a.camX == b.camX && a.camY == b.camY && a.camW == b.camW &&
         a.camShape == b.camShape && a.camBorder == b.camBorder && a.camBorderColor == b.camBorderColor &&
         a.camMirror == b.camMirror;
}

std::string OverlayKey(const EditSettings& s, const EditGeometry& g) {
  if (s.text.empty() && s.watermark.empty()) return {};
  char tail[96];
  snprintf(tail, sizeof(tail), "|%d|%d|%d|%d|%dx%d", int(s.textPlace), s.textSize, s.textBox ? 1 : 0,
           int(s.watermarkCorner), g.outW, g.outH);
  return s.text + '\x1f' + s.watermark + tail;
}

// Fps para leer: 60, 29,97.
std::string FpsText(double fps) {
  char buf[32];
  if (std::fabs(fps - std::round(fps)) < 0.01) snprintf(buf, sizeof(buf), "%d fps", int(std::lround(fps)));
  else snprintf(buf, sizeof(buf), "%.2f fps", fps);
  std::string s = buf;
  for (auto& c : s)
    if (c == '.') c = ',';
  return s;
}

} // namespace

// ------------------------------------------------------------ hilo de la vista previa

// Análisis del original, vista previa exacta y texto, fuera del hilo de la
// interfaz. De cada tipo solo cuenta la última petición: mientras se arrastra
// un deslizador no se acumulan.
struct EditorUi::Worker {
  enum class Kind { Probe, Preview, Overlays, CamFrame };
  struct Job {
    Kind kind = Kind::Probe;
    uint64_t serial = 0;
    std::string path;
    EditSettings settings;
    EditSource source;
    std::shared_ptr<const std::vector<uint8_t>> bgr;
    int width = 0, height = 0;
    double at = 0;
    int maxW = 0, maxH = 0;
    uint64_t frame = 0;
    std::string key;
  };
  struct Result {
    Kind kind = Kind::Probe;
    uint64_t serial = 0;
    bool ok = false;
    std::string error;
    EditSource source;
    EditSettings settings;
    std::vector<uint8_t> bgra;
    int width = 0, height = 0;
    uint64_t frame = 0;
    std::vector<EditOverlay> overlays;
    std::string key;
    double at = 0;
  };

  Worker(HWND notify, UINT msg) : notify_(notify), msg_(msg) {
    thread_ = std::thread([this] { Loop(); });
  }
  ~Worker() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  void Post(Job job) {
    {
      std::lock_guard lock(mutex_);
      pending_[int(job.kind)] = std::move(job);
    }
    cv_.notify_one();
  }
  std::vector<Result> Take() {
    std::lock_guard lock(mutex_);
    std::vector<Result> out;
    out.swap(results_);
    return out;
  }

private:
  void Loop() {
    InstallThreadCrashHandler();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || pending_[0] || pending_[1] || pending_[2] || pending_[3]; });
        if (stop_) return;
        for (auto& p : pending_) {
          if (!p) continue;
          job = std::move(*p);
          p.reset();
          break;
        }
      }
      Result r = Run(job);
      {
        std::lock_guard lock(mutex_);
        results_.push_back(std::move(r));
      }
      PostMessageW(notify_, msg_, 0, 0);
    }
  }

  // El fondo de la cámara en el segundo t (BGRA), con el lector abierto de
  // la última vez si es el mismo archivo: al reproducir se lee seguido.
  bool CamBackground(const std::string& path, int track, double t, std::vector<uint8_t>* bgra, int* w, int* h) {
    if (track < 0) return false;
    if (!camReader_.IsOpen() || camReader_.Path() != path || camTrack_ != track) {
      camTrack_ = track;
      if (!camReader_.Open(path, track)) return false;
    }
    return camReader_.ReadBgra(t, bgra, w, h);
  }

  Result Run(const Job& job) {
    Result r;
    r.kind = job.kind;
    r.serial = job.serial;
    try {
      switch (job.kind) {
      case Kind::Probe: r.ok = ProbeEditSource(job.path, &r.source, &r.error); break;
      case Kind::Preview: {
        r.settings = job.settings;
        r.frame = job.frame;
        std::vector<uint8_t> camBg;
        int cw = 0, ch = 0;
        if (job.settings.camEdit && job.source.Webcam() &&
            !CamBackground(job.path, job.source.camTrack, job.at, &camBg, &cw, &ch))
          camBg.clear();
        r.ok = job.bgr && RenderEditPreview(job.settings, job.source, job.bgr->data(), job.width, job.height, job.at,
                                            RasterizeText, job.maxW, job.maxH, &r.bgra, &r.width, &r.height, &r.error,
                                            EditImage{camBg.empty() ? nullptr : camBg.data(), cw, ch});
        break;
      }
      case Kind::CamFrame:
        r.at = job.at;
        r.ok = CamBackground(job.path, job.source.camTrack, job.at, &r.bgra, &r.width, &r.height);
        break;
      case Kind::Overlays: {
        const EditGeometry g = ComputeGeometry(job.settings, job.source);
        const double k = std::min(1.0, double(kOverlayBox) / std::max(1, std::max(g.outW, g.outH)));
        r.width = std::max(2, int(g.outW * k));
        r.height = std::max(2, int(g.outH * k));
        r.overlays = BuildOverlays(job.settings, r.width, r.height, RasterizeText);
        r.key = job.key;
        r.ok = true;
        break;
      }
      }
    } catch (const std::exception& e) {
      r.ok = false;
      r.error = e.what();
      Log("Editor: vista previa: %s", e.what());
    }
    return r;
  }

  HWND notify_;
  UINT msg_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::optional<Job> pending_[4];
  std::vector<Result> results_;
  // Solo en el hilo del trabajo.
  CamBackgroundReader camReader_;
  int camTrack_ = -1;
};

// ------------------------------------------------------------ ciclo de vida

EditorUi::EditorUi() = default;
EditorUi::~EditorUi() { Stop(); }

void EditorUi::Start(HWND notifyWindow, UINT notifyMsg) {
  notify_ = notifyWindow;
  notifyMsg_ = notifyMsg;
}

void EditorUi::Stop() {
  Leave();
  worker_.reset();
  notify_ = nullptr;
  path_.clear();
  s_ = committed_ = EditSettings{};
  undo_.clear();
  redo_.clear();
  probed_ = false;
  probeError_.clear();
  src_ = EditSource{};
}

void EditorUi::Open(const std::string& path) {
  if (path != path_) {
    Leave();
    path_ = path;
    s_ = committed_ = EditSettings{};
    undo_.clear();
    redo_.clear();
    probed_ = false;
    probeError_.clear();
    src_ = EditSource{};
    tab_ = 0;
  }
  active_ = true;
  if (!worker_ && notify_) worker_ = std::make_unique<Worker>(notify_, notifyMsg_);
  if (!probed_ && worker_) {
    Worker::Job job;
    job.kind = Worker::Kind::Probe;
    job.serial = serial_;
    job.path = path_;
    worker_->Post(std::move(job));
  }
}

void EditorUi::Leave() {
  active_ = false;
  player_.Close();
  playerFailed_ = false;
  firstFrame_ = false;
  wantPlay_ = false;
  frozen_ = false;
  lastPos_ = -1;
  scrubbing_ = false;
  drag_ = 0;
  areaMode_ = 0;
  frameBgr_.reset();
  frameSerial_ = ~0ull;
  previewBusy_ = false;
  askedFrame_ = ~0ull;
  exact_.reset();
  exactFrame_ = ~0ull;
  overlays_.clear();
  overlayKey_.clear();
  overlayAsked_.clear();
  camBg_.reset();
  camBusy_ = false;
  camAsked_ = -1;
  // Lo que llegue del hilo para el reproductor de antes ya no vale (el
  // análisis se vuelve a pedir si no había llegado).
  ++serial_;
}

void EditorUi::Pause() {
  wantPlay_ = false;
  frozen_ = false;
  if (player_.Playing()) player_.Pause();
}

void EditorUi::TakeResults(AppWindow& w) {
  if (!worker_) return;
  for (auto& r : worker_->Take()) {
    if (r.serial != serial_) continue;
    switch (r.kind) {
    case Worker::Kind::Probe:
      probed_ = true;
      probeError_ = r.ok ? std::string() : (r.error.empty() ? "No se pudo leer el vídeo" : r.error);
      if (r.ok) src_ = r.source;
      break;
    case Worker::Kind::Preview:
      previewBusy_ = false;
      if (r.ok) {
        exact_ = w.CreateTexture(r.bgra.data(), r.width, r.height);
        exactSettings_ = r.settings;
        exactFrame_ = r.frame;
      } else {
        exact_.reset();
        if (!r.error.empty()) Log("Editor: vista previa: %s", r.error.c_str());
      }
      break;
    case Worker::Kind::CamFrame:
      camBusy_ = false;
      if (r.ok) camBg_ = w.CreateTexture(r.bgra.data(), r.width, r.height);
      break;
    case Worker::Kind::Overlays:
      overlays_.clear();
      overlayKey_ = r.key;
      for (auto& o : r.overlays) {
        // RGBA -> BGRA para la textura.
        for (size_t i = 0; i + 3 < o.rgba.size(); i += 4) std::swap(o.rgba[i], o.rgba[i + 2]);
        LiveOverlay live;
        live.texture = w.CreateTexture(o.rgba.data(), o.width, o.height);
        if (!live.texture) continue;
        live.x = float(o.x) / float(r.width);
        live.y = float(o.y) / float(r.height);
        live.w = float(o.width) / float(r.width);
        live.h = float(o.height) / float(r.height);
        live.until = o.until;
        overlays_.push_back(std::move(live));
      }
      break;
    }
  }
  w.Invalidate();
}

// ------------------------------------------------------------ reproducción

double EditorUi::Duration(const MediaItem& item) const {
  if (player_.Duration() > 0) return player_.Duration();
  if (probed_ && src_.duration > 0) return src_.duration;
  return item.duration;
}

double EditorUi::End(const MediaItem& item) const {
  const double d = Duration(item);
  return s_.end > s_.start ? std::min(s_.end, d) : d;
}

void EditorUi::Seek(double seconds, bool exact) {
  player_.Seek(std::max(0.0, seconds), exact);
  lastPos_ = seconds;
  settleUntil_ = GetTickCount64() + 400; // el fotograma nuevo llega un poco después
}

void EditorUi::SetStart(double t, const MediaItem& item) {
  s_.start = std::clamp(t, 0.0, std::max(0.0, End(item) - kMinRange));
  if (s_.start < 0.01) s_.start = 0;
}

void EditorUi::SetEnd(double t, const MediaItem& item) {
  const double d = Duration(item);
  s_.end = std::clamp(t, std::min(d, s_.start + kMinRange), d);
  if (s_.end >= d - 0.01) s_.end = 0; // hasta el final
}

void EditorUi::TogglePlay(const MediaItem& item) {
  if (frozen_ || player_.Playing()) {
    Pause();
    return;
  }
  const double a = std::clamp(s_.start, 0.0, Duration(item)), b = End(item);
  const double pos = player_.Position();
  if (player_.Ended()) player_.Play(); // si no, Play vuelve al principio del archivo
  if (player_.Ended() || pos >= b - 0.05 || pos < a - 0.01) Seek(a);
  player_.Play();
  wantPlay_ = true;
  lastPos_ = player_.Position();
}

// Mientras se reproduce, el original se ve como saldrá: solo el tramo elegido
// y en bucle, sin el trozo quitado, a la velocidad de cada tramo y parado en
// la imagen congelada. Al revés y bumerán solo se ven al guardar.
void EditorUi::SyncPlayback(const MediaItem& item) {
  if (!player_.IsOpen() || !player_.Ready()) return;
  // Lo que se oirá, hasta el 100 % (el reproductor no amplifica).
  const float vol = volume_ * std::clamp(s_.volume, 0.0f, 1.0f);
  if (std::fabs(player_.Volume() - vol) > 1e-3f) player_.SetVolume(vol);
  const bool mute = muted_ || s_.mute || (src_.SeparateMic() && s_.volume <= 0 && s_.micVolume <= 0);
  if (player_.Muted() != mute) player_.SetMuted(mute);
  const unsigned long long now = GetTickCount64();
  if (frozen_) {
    if (now < frozenUntil_) return;
    frozen_ = false;
    player_.Play();
  }
  const double d = Duration(item);
  const double a = std::clamp(s_.start, 0.0, d), b = End(item);
  const double pos = player_.Position();
  if (wantPlay_ && player_.Ended()) {
    player_.Play();
    Seek(a);
  } else if (player_.Playing() && !scrubbing_ && drag_ == 0) {
    double rate = std::clamp(s_.speed, 0.25, 3.0);
    if (s_.slow && pos >= s_.slowStart && pos < s_.slowEnd) rate *= std::clamp(s_.slowSpeed, 0.25, 1.0);
    player_.SetRate(rate);
    if (pos < a - 0.25 || pos >= b - 0.03) {
      Seek(a);
    } else if (s_.cut && s_.cutEnd > s_.cutStart && pos >= s_.cutStart && pos < s_.cutEnd - 0.05) {
      Seek(s_.cutEnd);
    } else if (s_.freeze && lastPos_ >= 0 && lastPos_ < s_.freezeAt && pos >= s_.freezeAt && pos - lastPos_ < 1.0) {
      player_.Pause();
      frozen_ = true;
      frozenUntil_ = now + (unsigned long long)(std::clamp(s_.freezeSeconds, 0.5, 10.0) * 1000.0);
    }
  }
  lastPos_ = player_.Position();
}

// ------------------------------------------------------------ deshacer

void EditorUi::Commit() {
  if (s_ == committed_ || ImGui::IsAnyItemActive()) return;
  undo_.push_back(committed_);
  if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
  redo_.clear();
  committed_ = s_;
}

void EditorUi::Undo() {
  if (!(s_ == committed_)) { // un cambio a medias cuenta como hecho
    undo_.push_back(committed_);
    committed_ = s_;
    redo_.clear();
  }
  if (undo_.empty()) return;
  redo_.push_back(s_);
  s_ = committed_ = undo_.back();
  undo_.pop_back();
}

void EditorUi::Redo() {
  if (redo_.empty()) return;
  undo_.push_back(s_);
  s_ = committed_ = redo_.back();
  redo_.pop_back();
}

// ------------------------------------------------------------ resumen

std::string EditorUi::Summary(const MediaItem& item) const {
  (void)item;
  if (!probed_) return "Leyendo el vídeo...";
  if (!probeError_.empty()) return probeError_;
  const auto segments = BuildSegments(s_, src_);
  std::string out = "Resultado: " + Length(SegmentsLength(segments));
  if (s_.format == EditFormat::Audio) {
    out += "  ·  solo el sonido (M4A)";
  } else {
    const EditGeometry g = ComputeGeometry(s_, src_);
    char size[48];
    snprintf(size, sizeof(size), "  ·  %dx%d  ·  ", g.outW, g.outH);
    out += size + FpsText(g.fps);
    if (s_.format == EditFormat::Gif) out += "  ·  GIF";
  }
  return out + "  ·  el original no cambia";
}

bool EditorUi::CanSave(const MediaItem& item, std::string* why) const {
  (void)item;
  if (!probed_) {
    *why = "Leyendo el vídeo...";
    return false;
  }
  if (!probeError_.empty()) {
    *why = probeError_;
    return false;
  }
  const double length = SegmentsLength(BuildSegments(s_, src_));
  if (length < 0.1) {
    *why = "El resultado quedaría vacío: revisa el recorte y el trozo quitado";
    return false;
  }
  if (s_.format == EditFormat::Gif && length > kMaxGifSeconds + 0.05) {
    *why = "Un GIF puede durar como mucho 30 s y este saldría de " + Length(length) +
           ": recórtalo o súbele la velocidad";
    return false;
  }
  if (const double back = ReversedSeconds(s_, src_); back > kMaxReverseSeconds + 0.05) {
    *why = "«Al revés» y «Bumerán» van con 1 min del original como mucho y aquí hay " + Length(back) +
           ": recorta el tramo";
    return false;
  }
  if (s_.format == EditFormat::Audio && (!src_.audio || s_.mute)) {
    *why = src_.audio ? "Has quitado el sonido: no queda nada que guardar" : "Este vídeo no tiene sonido";
    return false;
  }
  if (s_ == EditSettings{}) {
    *why = "Aún no has cambiado nada";
    return false;
  }
  return true;
}

// ------------------------------------------------------------ página

EditorUi::Exit EditorUi::Draw(AppWindow& w, AppModel& m, const AppActions& a, const MediaItem& item) {
  Exit exit;
  if (!player_.IsOpen() && !playerFailed_) {
    // En pausa al abrir: se ve el primer fotograma del tramo.
    playerFailed_ = !player_.Open(w, path_, false);
    if (!playerFailed_) {
      player_.SetVolume(volume_);
      player_.SetMuted(muted_);
    }
    firstFrame_ = false;
  }
  if (player_.Ready() && !firstFrame_) {
    firstFrame_ = true;
    Seek(std::clamp(s_.start, 0.0, Duration(item)));
  }
  SyncPlayback(item);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(28), Px(20)));
  ImGui::BeginChild("editor", ImVec2(0, -Px(kFooterHeight)), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  Header(item, &exit);
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
  const float toolsW = Px(kToolsWidth), gap = Px(24);
  const float leftW = std::max(Px(260), availW - toolsW - gap);
  const float belowH = Px(34) + Px(4) + Px(46) + Px(4) + Px(kHint) + Px(14);
  const ImVec2 va = q, vb(q.x + leftW, q.y + std::max(Px(150), availH - belowH));
  Preview(w, item, va, vb);
  ImGui::SetCursorScreenPos(ImVec2(va.x, vb.y + Px(12)));
  Transport(item, leftW);
  ImGui::SetCursorScreenPos(ImVec2(q.x + leftW + gap, q.y));
  Tools(m, item, a, availH);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  Footer(m, a, item, &exit);
  Keyboard(a, item, &exit);
  Commit();

  const unsigned long long now = GetTickCount64();
  if (player_.Playing() || scrubbing_ || drag_ != 0 || previewBusy_) w.Animate();
  if (frozen_) w.AnimateAt(frozenUntil_);
  // El primer fotograma o el de después de un salto llega solo: unos frames
  // más hasta que esté.
  if (player_.IsOpen() && (!player_.Ready() || player_.FrameSerial() == 0 || now < settleUntil_))
    w.AnimateAt(now + 30);
  return exit;
}

void EditorUi::Header(const MediaItem& item, Exit* exit) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float rowH = Px(34), avail = ImGui::GetContentRegionAvail().x;
  if (IconButton("back", Icon::Back, rowH, "Volver (Esc)")) exit->back = true;
  const float tx = q.x + rowH + Px(10);
  const float resetW = Px(140);
  const float actionsW = rowH * 2 + Px(4) + Px(10) + resetW;
  const std::string title = "Editar  ·  " + Title(item);
  Text(dl, Bold(), Px(kBody) + Px(2), ImVec2(tx, q.y), C(p.text),
       Ellipsize(Bold(), Px(kBody) + Px(2), title, avail - rowH - actionsW - Px(30)).c_str());
  const std::string summary = Summary(item);
  const bool bad = probed_ && !probeError_.empty();
  Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(tx, q.y + Px(21)), C(bad ? p.danger : p.textMuted),
       Ellipsize(ImGui::GetFont(), Px(kSmall), summary, avail - rowH - actionsW - Px(30)).c_str());

  ImGui::SetCursorScreenPos(ImVec2(q.x + avail - actionsW, q.y));
  ImGui::BeginDisabled(undo_.empty() && s_ == committed_);
  if (IconButton("undo", Icon::Undo, rowH, "Deshacer (Ctrl+Z)")) Undo();
  ImGui::EndDisabled();
  ImGui::SameLine(0, Px(4));
  ImGui::BeginDisabled(redo_.empty());
  if (IconButton("redo", Icon::Redo, rowH, "Rehacer (Ctrl+Y)")) Redo();
  ImGui::EndDisabled();
  ImGui::SameLine(0, Px(10));
  ImGui::BeginDisabled(s_ == EditSettings{});
  if (GhostButton("reset", "Empezar de cero", resetW, Tone::Normal, 34)) {
    // Quita todos los cambios; el formato y la calidad de guardado se quedan.
    EditSettings fresh;
    fresh.format = s_.format;
    fresh.size = s_.size;
    s_ = fresh;
  }
  ImGui::EndDisabled();
  ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(14)));
}

// ------------------------------------------------------------ vista previa

void EditorUi::Preview(AppWindow& w, const MediaItem& item, ImVec2 va, ImVec2 vb) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(va);
  ImGui::InvisibleButton("preview", vb - va);
  const bool hov = ImGui::IsItemHovered();
  dl->AddRectFilled(va, vb, C(IM_COL32(0, 0, 0, 255)));

  auto centered = [&](const char* text, float dy) {
    const ImVec2 ts = TextSize(ImGui::GetFont(), Px(kBody), text);
    Text(dl, ImGui::GetFont(), Px(kBody), ImVec2((va.x + vb.x - ts.x) * 0.5f, (va.y + vb.y - ts.y) * 0.5f + dy),
         C(p.text), text);
  };
  if (playerFailed_ || player_.Failed()) {
    const std::string err = playerFailed_ ? std::string(Player::Available() ? "No se pudo abrir el reproductor."
                                                                            : "Este Windows no tiene Media Foundation (Windows N).")
                                          : player_.Error();
    centered(err.c_str(), 0);
    return;
  }
  std::shared_ptr<UiTexture> frame = player_.Frame(w);
  if (!frame) {
    centered("Cargando el vídeo...", 0);
    return;
  }

  // Geometría con el tamaño real del fotograma, como la vista previa exacta.
  EditSource src = src_;
  src.width = frame->width;
  src.height = frame->height;
  EditSettings s = s_;
  if (s.format == EditFormat::Audio) s.format = EditFormat::Video;
  const EditGeometry g = ComputeGeometry(s, src);
  if (g.outW <= 0 || g.outH <= 0 || g.cropW <= 0 || g.cropH <= 0) return;
  const float aw = vb.x - va.x, ah = vb.y - va.y;
  const float k = std::min(aw / float(g.outW), ah / float(g.outH));
  const float dispW = std::floor(float(g.outW) * k), dispH = std::floor(float(g.outH) * k);
  const ImVec2 da(std::floor(va.x + (aw - dispW) * 0.5f), std::floor(va.y + (ah - dispH) * 0.5f));
  const ImVec2 db(da.x + dispW, da.y + dispH);

  // La cámara del clip, en el lienzo (camBox) y en pantalla (ca, cb).
  const bool camLive = s_.camEdit && src_.Webcam();
  const EditCamBox camBox = camLive ? ComputeCamBox(s, src, g) : EditCamBox{};
  const float bkx = dispW / float(std::max(1, g.canvasW)), bky = dispH / float(std::max(1, g.canvasH));
  const ImVec2 ca(da.x + float(camBox.x) * bkx, da.y + float(camBox.y) * bky);
  const ImVec2 cb(ca.x + float(camBox.w) * bkx, ca.y + float(camBox.h) * bky);
  if (camLive) RequestCamFrame();

  const bool still = !player_.Playing() && !scrubbing_ && drag_ == 0;
  const bool touches = EditTouchesPicture(s_);
  const bool exactOk = touches && still && exact_ && exactFrame_ == player_.FrameSerial() && SameShape(exactSettings_, s_);
  if (exactOk) {
    AddTextureImage(dl, *exact_, da, db, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
  } else {
    // En movimiento (o mientras llega la exacta): el fotograma con el giro,
    // el encuadre y el formato, sin filtros.
    const int rot = ((s.rotate % 360) + 360) % 360;
    auto uvOf = [&](double x, double y) {
      double u = x / g.rotatedW, v = y / g.rotatedH;
      if (s.flipH) u = 1 - u;
      if (s.flipV) v = 1 - v;
      switch (rot) {
      case 90: return ImVec2(float(v), float(1 - u));
      case 270: return ImVec2(float(1 - v), float(u));
      case 180: return ImVec2(float(1 - u), float(1 - v));
      default: return ImVec2(float(u), float(v));
      }
    };
    auto quad = [&](ImVec2 p0, ImVec2 p1, double cx, double cy, double cw, double ch, ImU32 tint) {
      dl->AddImageQuad(ImTextureID(frame->Id()), p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y), uvOf(cx, cy),
                       uvOf(cx + cw, cy), uvOf(cx + cw, cy + ch), uvOf(cx, cy + ch), tint);
    };
    const float kx = dispW / float(g.canvasW), ky = dispH / float(g.canvasH);
    const bool fit = g.canvasW != g.cropW || g.canvasH != g.cropH;
    if (fit && s.fit == EditFit::Blur) {
      // Fondo: el vídeo llenando el lienzo, más oscuro (difuminado al guardar
      // y en pausa).
      const double cover = std::max(double(g.canvasW) / g.cropW, double(g.canvasH) / g.cropH);
      const double vw = g.canvasW / cover, vh = g.canvasH / cover;
      quad(da, db, g.cropX + (g.cropW - vw) * 0.5, g.cropY + (g.cropH - vh) * 0.5, vw, vh, IM_COL32(120, 120, 120, 255));
    }
    const ImVec2 fa(da.x + float(g.canvasW - g.fgW) * 0.5f * kx, da.y + float(g.canvasH - g.fgH) * 0.5f * ky);
    const ImVec2 fb(fa.x + float(g.fgW) * kx, fa.y + float(g.fgH) * ky);
    quad(fa, fb, g.cropX, g.cropY, g.cropW, g.cropH, IM_COL32_WHITE);

    // Cámara: lo que había debajo en su sitio de antes y la cámara donde va.
    if (camLive && src_.width > 0 && src_.height > 0) {
      const double cx = double(src_.camX) / src_.width, cy = double(src_.camY) / src_.height;
      const double cw = double(src_.camW) / src_.width, ch = double(src_.camH) / src_.height;
      if (camBg_) {
        // Del fotograma (0..1) a la pantalla: lo contrario de uvOf.
        auto screenOf = [&](double u, double v) {
          double x = u, y = v;
          switch (rot) {
          case 90: x = 1 - v; y = u; break;
          case 270: x = v; y = 1 - u; break;
          case 180: x = 1 - u; y = 1 - v; break;
          default: break;
          }
          if (s.flipH) x = 1 - x;
          if (s.flipV) y = 1 - y;
          x *= g.rotatedW;
          y *= g.rotatedH;
          return ImVec2(fa.x + float((x - g.cropX) / g.cropW) * (fb.x - fa.x),
                        fa.y + float((y - g.cropY) / g.cropH) * (fb.y - fa.y));
        };
        dl->PushClipRect(fa, fb, true);
        dl->AddImageQuad(ImTextureID(camBg_->Id()), screenOf(cx, cy), screenOf(cx + cw, cy),
                         screenOf(cx + cw, cy + ch), screenOf(cx, cy + ch));
        dl->PopClipRect();
      }
      if (camBox.w > 0) {
        // Recorte cuadrado del centro para las formas cuadrada y redonda.
        double u0 = cx, v0 = cy, u1 = cx + cw, v1 = cy + ch;
        if (s_.camShape != EditCamShape::Original) {
          const double side = std::min(src_.camW, src_.camH);
          u0 = (src_.camX + (src_.camW - side) * 0.5) / src_.width;
          v0 = (src_.camY + (src_.camH - side) * 0.5) / src_.height;
          u1 = u0 + side / src_.width;
          v1 = v0 + side / src_.height;
        }
        if (s_.camMirror) std::swap(u0, u1);
        const ImVec2 ua{float(u0), float(v0)}, ub{float(u1), float(v1)};
        const float bk = dispW / float(g.canvasW);
        const float side = std::min(cb.x - ca.x, cb.y - ca.y);
        const float boxSide = float(std::min(camBox.w, camBox.h));
        const float thick = s_.camBorder <= 0 ? 0.0f
                            : s_.camBorder == 1 ? std::max(2.0f, boxSide * 0.025f) * bk
                                                : std::max(4.0f, boxSide * 0.06f) * bk;
        const ImU32 border = IM_COL32((s_.camBorderColor >> 16) & 0xFF, (s_.camBorderColor >> 8) & 0xFF,
                                      s_.camBorderColor & 0xFF, 255);
        if (s_.camShape == EditCamShape::Circle) {
          dl->AddImageRounded(ImTextureID(frame->Id()), ca, cb, ua, ub, IM_COL32_WHITE, side * 0.5f);
          if (thick > 0)
            dl->AddCircle(ImVec2((ca.x + cb.x) * 0.5f, (ca.y + cb.y) * 0.5f), side * 0.5f - thick * 0.5f, border, 0,
                          thick);
        } else {
          dl->AddImage(ImTextureID(frame->Id()), ca, cb, ua, ub);
          if (thick > 0)
            dl->AddRect(ca + ImVec2(thick * 0.5f, thick * 0.5f), cb - ImVec2(thick * 0.5f, thick * 0.5f), border, 0,
                        0, thick);
        }
      }
    }

    // Zona tapada: oscura en movimiento; en pausa se ve el difuminado de verdad.
    if (s_.blur) {
      const ImVec2 za(da.x + s_.blurX * dispW, da.y + s_.blurY * dispH);
      const ImVec2 zb(za.x + s_.blurW * dispW, za.y + s_.blurH * dispH);
      dl->AddRectFilled(za, zb, C(IM_COL32(40, 40, 40, 255), 0.85f));
    }
    // Texto y marca de agua (los dibuja el hilo a su tamaño).
    if (!overlays_.empty() && (!s_.text.empty() || !s_.watermark.empty())) {
      const double t = OutputTimeOf(BuildSegments(s_, src_), player_.Position());
      for (const auto& o : overlays_) {
        if (o.until > 0 && t >= o.until) continue;
        const ImVec2 oa(da.x + o.x * dispW, da.y + o.y * dispH);
        AddTextureImage(dl, *o.texture, oa, ImVec2(oa.x + o.w * dispW, oa.y + o.h * dispH), ImVec2(0, 0), ImVec2(1, 1),
                        IM_COL32_WHITE);
      }
    }
  }

  // Texto de la vista previa: se pide al hilo cuando cambia.
  {
    const std::string key = OverlayKey(s_, g);
    if (key.empty()) {
      overlays_.clear();
      overlayKey_.clear();
      overlayAsked_.clear();
    } else if (key != overlayAsked_ && worker_) {
      Worker::Job job;
      job.kind = Worker::Kind::Overlays;
      job.serial = serial_;
      job.settings = s;
      job.source = src;
      job.key = key;
      worker_->Post(std::move(job));
      overlayAsked_ = key;
    }
  }
  if (touches && still) RequestPreview(w, ImVec2(dispW, dispH));

  // Arrastrar: el recuadro de la zona tapada (mover o, desde la esquina,
  // cambiar su tamaño) o el encuadre si sobra imagen. Un clic sin arrastrar
  // reproduce o pausa.
  const ImVec2 mouse = ImGui::GetMousePos();
  const ImVec2 za(da.x + s_.blurX * dispW, da.y + s_.blurY * dispH);
  const ImVec2 zb(za.x + s_.blurW * dispW, za.y + s_.blurH * dispH);
  const float grip = Px(9);
  const bool overGrip = s_.blur && std::fabs(mouse.x - zb.x) <= grip && std::fabs(mouse.y - zb.y) <= grip;
  const bool overZone = s_.blur && mouse.x >= za.x && mouse.x <= zb.x && mouse.y >= za.y && mouse.y <= zb.y;
  const bool canPan = g.cropW < g.rotatedW - 2 || g.cropH < g.rotatedH - 2;
  const bool camOn = camBox.w > 0;
  const bool overCamGrip = camOn && std::fabs(mouse.x - cb.x) <= grip && std::fabs(mouse.y - cb.y) <= grip;
  const bool overCam = camOn && mouse.x >= ca.x && mouse.x <= cb.x && mouse.y >= ca.y && mouse.y <= cb.y;
  // Con la pestaña Cámara abierta manda la cámara; si no, la zona tapada.
  const bool camFirst = tab_ == 5 || !(overGrip || overZone);
  if (ImGui::IsItemActivated()) {
    dragFrom_ = mouse;
    if (camFirst && overCamGrip) areaMode_ = 6;
    else if (camFirst && overCam) areaMode_ = 5;
    else if (overGrip) areaMode_ = 3;
    else if (overZone) areaMode_ = 2;
    else if (canPan) areaMode_ = 1;
    else areaMode_ = 4;
    if (areaMode_ >= 5) {
      dragStart_[0] = s_.camX;
      dragStart_[1] = s_.camY;
      dragStart_[2] = s_.camW;
    } else {
      dragStart_[0] = areaMode_ == 1 ? s_.panX : s_.blurX;
      dragStart_[1] = areaMode_ == 1 ? s_.panY : s_.blurY;
      dragStart_[2] = s_.blurW;
      dragStart_[3] = s_.blurH;
    }
  }
  const ImVec2 delta(mouse.x - dragFrom_.x, mouse.y - dragFrom_.y);
  const bool moved = std::fabs(delta.x) > Px(4) || std::fabs(delta.y) > Px(4);
  if (ImGui::IsItemActive() && dispW > 0 && dispH > 0) {
    switch (areaMode_) {
    case 1: {
      // Mover la imagen a la derecha enseña lo que había a la izquierda.
      const float fgDispW = dispW * float(g.fgW) / float(g.canvasW), fgDispH = dispH * float(g.fgH) / float(g.canvasH);
      if (moved && g.rotatedW > g.cropW)
        s_.panX = std::clamp(dragStart_[0] - delta.x * float(g.cropW) / fgDispW / float(g.rotatedW - g.cropW), 0.0f, 1.0f);
      if (moved && g.rotatedH > g.cropH)
        s_.panY = std::clamp(dragStart_[1] - delta.y * float(g.cropH) / fgDispH / float(g.rotatedH - g.cropH), 0.0f, 1.0f);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      break;
    }
    case 2:
      s_.blurX = std::clamp(dragStart_[0] + delta.x / dispW, 0.0f, 1.0f - s_.blurW);
      s_.blurY = std::clamp(dragStart_[1] + delta.y / dispH, 0.0f, 1.0f - s_.blurH);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      break;
    case 3:
      s_.blurW = std::clamp(dragStart_[2] + delta.x / dispW, 0.04f, 1.0f - s_.blurX);
      s_.blurH = std::clamp(dragStart_[3] + delta.y / dispH, 0.04f, 1.0f - s_.blurY);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
      break;
    case 5:
      if (camOn) {
        const float fw = float(camBox.w) / float(g.canvasW), fh = float(camBox.h) / float(g.canvasH);
        s_.camX = std::clamp(dragStart_[0] + delta.x / dispW, 0.0f, std::max(0.0f, 1.0f - fw));
        s_.camY = std::clamp(dragStart_[1] + delta.y / dispH, 0.0f, std::max(0.0f, 1.0f - fh));
      }
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      break;
    case 6:
      if (camOn) {
        // Crece desde la esquina de arriba a la izquierda sin salirse.
        const float ratio = float(camBox.h) / float(camBox.w) * float(g.canvasW) / float(g.canvasH);
        const float maxW = std::min(1.0f - s_.camX, (1.0f - s_.camY) / std::max(0.01f, ratio));
        const float grow = std::max(delta.x / dispW, delta.y / dispH / std::max(0.01f, ratio));
        s_.camW = std::clamp(dragStart_[2] + grow, 0.05f, std::max(0.05f, maxW));
      }
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
      break;
    default: break;
    }
  }
  if (ImGui::IsItemDeactivated()) {
    if (areaMode_ == 4 && !moved) TogglePlay(item);
    areaMode_ = 0;
  }
  if (hov && !ImGui::IsItemActive()) {
    if ((camFirst && overCamGrip) || overGrip) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if ((camFirst && overCam) || overZone || canPan) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (canPan && !overZone && !overGrip && !overCam) ImGui::SetTooltip("Arrastra para elegir qué parte se ve");
  }

  // Borde de la imagen final y del recuadro de la zona tapada.
  if (g.outW != src.width || g.outH != src.height || EditTouchesPicture(s_))
    dl->AddRect(da - ImVec2(1, 1), db + ImVec2(1, 1), C(p.line), 0, 0, Px(1));
  if (s_.blur && (tab_ == 3 || overZone || areaMode_ == 2 || areaMode_ == 3)) {
    dl->AddRect(za, zb, C(p.accent), 0, 0, Px(1));
    dl->AddRectFilled(zb - ImVec2(Px(5), Px(5)), zb + ImVec2(Px(5), Px(5)), C(p.accent));
  }
  if (camOn && (tab_ == 5 || (hov && overCam) || areaMode_ >= 5)) {
    dl->AddRect(ca, cb, C(p.accent), 0, 0, Px(1));
    dl->AddRectFilled(cb - ImVec2(Px(5), Px(5)), cb + ImVec2(Px(5), Px(5)), C(p.accent));
  }

  // Un aviso arriba a la izquierda cuando lo que se ve no es lo que saldrá.
  const char* note = nullptr;
  if (frozen_) note = "Imagen congelada";
  else if (s_.format == EditFormat::Audio) note = "Solo audio: se guarda sin imagen";
  else if (s_.direction == EditDirection::Reverse) note = "Al revés: se ve al guardar";
  else if (s_.direction == EditDirection::Boomerang) note = "Bumerán: la vuelta se ve al guardar";
  if (note) {
    const float fs = Px(kSmall);
    const ImVec2 ts = TextSize(ImGui::GetFont(), fs, note);
    const ImVec2 na(va.x + Px(10), va.y + Px(10));
    dl->AddRectFilled(na, na + ts + ImVec2(Px(16), Px(10)), C(p.bg, 0.86f));
    Text(dl, ImGui::GetFont(), fs, na + ImVec2(Px(8), Px(5)), C(p.text), note);
  }
}

void EditorUi::RequestPreview(AppWindow& w, ImVec2 area) {
  if (!worker_ || !probed_ || !probeError_.empty() || previewBusy_ || !player_.Ready()) return;
  const uint64_t serial = player_.FrameSerial();
  if (serial == 0) return; // aún no hay fotograma
  if (askedFrame_ == serial && askedSettings_ == s_) return;
  // Como mucho una cada 60 ms mientras se arrastra un deslizador.
  const unsigned long long now = GetTickCount64();
  if (now - askedAt_ < 60) {
    w.AnimateAt(askedAt_ + 60);
    return;
  }
  // El fotograma se lee de la GPU una vez por fotograma en pausa, no por cambio.
  if (frameSerial_ != serial || !frameBgr_) {
    auto bgr = std::make_shared<std::vector<uint8_t>>();
    int fw = 0, fh = 0;
    if (!player_.ReadFrame(bgr.get(), &fw, &fh)) return;
    frameBgr_ = std::move(bgr);
    frameW_ = fw;
    frameH_ = fh;
    frameSerial_ = serial;
  }
  Worker::Job job;
  job.kind = Worker::Kind::Preview;
  job.serial = serial_;
  job.path = path_;
  job.settings = s_;
  job.source = src_;
  job.bgr = frameBgr_;
  job.width = frameW_;
  job.height = frameH_;
  job.at = player_.Position();
  job.maxW = std::max(16, int(area.x));
  job.maxH = std::max(16, int(area.y));
  job.frame = serial;
  worker_->Post(std::move(job));
  previewBusy_ = true;
  askedFrame_ = serial;
  askedSettings_ = s_;
  askedAt_ = now;
}

// Lo que hay debajo de la cámara en el momento que se ve, para la vista previa
// en movimiento: una petición cada vez, la siguiente al llegar la anterior.
void EditorUi::RequestCamFrame() {
  if (!worker_ || camBusy_ || !player_.Ready()) return;
  const double t = player_.Position();
  const double step = src_.fps > 1 ? 1.0 / src_.fps : 1.0 / 30.0;
  if (camBg_ && std::fabs(t - camAsked_) < step * 0.5) return;
  Worker::Job job;
  job.kind = Worker::Kind::CamFrame;
  job.serial = serial_;
  job.path = path_;
  job.source = src_;
  job.at = t;
  worker_->Post(std::move(job));
  camBusy_ = true;
  camAsked_ = t;
}

// ------------------------------------------------------------ reproducción y línea de tiempo

void EditorUi::Transport(const MediaItem& item, float width) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rowH = Px(34);
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const bool playing = player_.Playing() || frozen_;
  ImGui::BeginDisabled(playerFailed_ || !player_.Ready());
  if (IconButton("play", playing ? Icon::Pause : Icon::Play, rowH,
                 playing ? "Pausa (espacio)" : "Reproducir como saldrá (espacio)"))
    TogglePlay(item);
  ImGui::EndDisabled();
  const double d = Duration(item);
  const std::string time = Precise(player_.Position()) + " / " + Precise(d);
  const float fs = Px(kSmall);
  Text(dl, ImGui::GetFont(), fs, ImVec2(q.x + rowH + Px(10), q.y + (rowH - fs) * 0.5f), C(p.textMuted), time.c_str());
  // A la derecha: dónde cae en el resultado y el sonido de la vista previa.
  if (probed_ && probeError_.empty()) {
    const auto segments = BuildSegments(s_, src_);
    const double at = OutputTimeOf(segments, player_.Position());
    const std::string out = at >= 0 ? "En el resultado: " + Precise(at) + " de " + Precise(SegmentsLength(segments))
                                    : "Este momento no sale en el resultado";
    const ImVec2 ts = TextSize(ImGui::GetFont(), fs, out.c_str());
    Text(dl, ImGui::GetFont(), fs, ImVec2(q.x + width - rowH - Px(12) - ts.x, q.y + (rowH - fs) * 0.5f),
         C(at >= 0 ? p.textFaint : p.textMuted), out.c_str());
  }
  ImGui::SetCursorScreenPos(ImVec2(q.x + width - rowH, q.y));
  if (IconButton("mute", muted_ ? Icon::Muted : Icon::Volume, rowH,
                 muted_ ? "Activar el sonido de la vista previa (M)" : "Silenciar la vista previa (M)"))
    muted_ = !muted_;
  ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(4)));
  Timeline(item, width);
  // Una sola línea, recortada al ancho de la columna (al lado están las herramientas).
  const ImVec2 hq = ImGui::GetCursorScreenPos() + ImVec2(0, Px(4));
  const std::string hint = Ellipsize(ImGui::GetFont(), Px(kHint),
                                     "I y O marcan el inicio y el fin  ·  Mayús+flechas, de fotograma en fotograma  ·  "
                                     "Ctrl+Z deshace  ·  en pausa la imagen se ve tal cual saldrá",
                                     width);
  Text(dl, ImGui::GetFont(), Px(kHint), hq, C(p.textFaint), hint.c_str());
  ImGui::Dummy(ImVec2(width, Px(kHint) + Px(4)));
}

void EditorUi::Timeline(const MediaItem& item, float width) {
  const Palette& p = Colors();
  const double d = Duration(item);
  const float h = Px(46);
  ImGui::InvisibleButton("timeline", ImVec2(width, h));
  const bool hov = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float x0 = a.x + Px(6), x1 = b.x - Px(6);
  const float mid = a.y + Px(24);
  auto xOf = [&](double t) { return d > 0 ? x0 + float(std::clamp(t / d, 0.0, 1.0)) * (x1 - x0) : x0; };
  auto tOf = [&](float x) { return d > 0 ? std::clamp(double((x - x0) / (x1 - x0)) * d, 0.0, d) : 0.0; };
  const float mx = ImGui::GetMousePos().x;
  const double ra = std::clamp(s_.start, 0.0, d), rb = End(item);

  // Asas: 1 inicio, 2 fin, 3-4 trozo quitado, 5-6 cámara lenta, 7 congelar.
  struct Handle {
    int id;
    double t;
  };
  std::vector<Handle> handles = {{1, ra}, {2, rb}};
  if (s_.cut) {
    handles.push_back({3, s_.cutStart});
    handles.push_back({4, s_.cutEnd});
  }
  if (s_.slow) {
    handles.push_back({5, s_.slowStart});
    handles.push_back({6, s_.slowEnd});
  }
  if (s_.freeze) handles.push_back({7, s_.freezeAt});
  auto nearest = [&] {
    int best = 0;
    float bestDist = Px(8);
    for (const auto& hd : handles) {
      const float dist = std::fabs(mx - xOf(hd.t));
      if (dist <= bestDist) {
        bestDist = dist;
        best = hd.id;
      }
    }
    return best;
  };
  const int over = hov && !active ? nearest() : 0;

  if (ImGui::IsItemActivated() && d > 0) {
    drag_ = nearest();
    wasPlaying_ = player_.Playing() || frozen_;
    frozen_ = false;
    if (player_.Playing()) player_.Pause();
    scrubbing_ = drag_ == 0;
  }
  if (active && d > 0) {
    const double t = tOf(mx);
    switch (drag_) {
    case 0:
      if (std::fabs(t - player_.Position()) > 0.04) Seek(t, false);
      break;
    case 1:
      SetStart(t, item);
      Seek(s_.start, false);
      break;
    case 2:
      SetEnd(t, item);
      Seek(End(item), false);
      break;
    case 3:
      s_.cutStart = std::clamp(t, ra, std::max(ra, s_.cutEnd - kMinPart));
      Seek(s_.cutStart, false);
      break;
    case 4:
      s_.cutEnd = std::clamp(t, std::min(rb, s_.cutStart + kMinPart), rb);
      Seek(s_.cutEnd, false);
      break;
    case 5:
      s_.slowStart = std::clamp(t, 0.0, std::max(0.0, s_.slowEnd - kMinPart));
      Seek(s_.slowStart, false);
      break;
    case 6:
      s_.slowEnd = std::clamp(t, std::min(d, s_.slowStart + kMinPart), d);
      Seek(s_.slowEnd, false);
      break;
    case 7:
      s_.freezeAt = std::clamp(t, ra, rb);
      Seek(s_.freezeAt, false);
      break;
    default: break;
    }
  }
  if (ImGui::IsItemDeactivated()) {
    // Al soltar, el fotograma exacto. Tras mover un asa se queda en pausa
    // enseñando ese fotograma; tras arrastrar el cabezal sigue como estaba.
    Seek(drag_ == 0 ? tOf(mx) : player_.Position(), true);
    if (drag_ == 0 && wasPlaying_) {
      player_.Play();
      wantPlay_ = true;
    } else if (drag_ != 0) {
      wantPlay_ = false;
    }
    scrubbing_ = false;
    drag_ = 0;
  }

  // Pista: apagada fuera del recorte; el trozo quitado, un hueco con borde.
  dl->AddRectFilled(ImVec2(x0, mid - Px(2)), ImVec2(x1, mid + Px(2)), C(p.surfaceActive));
  dl->AddRectFilled(ImVec2(xOf(ra), mid - Px(2)), ImVec2(xOf(rb), mid + Px(2)), C(p.accent, 0.6f));
  if (s_.cut && s_.cutEnd > s_.cutStart) {
    const ImVec2 ca(xOf(s_.cutStart), mid - Px(6)), cb(xOf(s_.cutEnd), mid + Px(6));
    dl->AddRectFilled(ca, cb, C(p.bg));
    dl->AddRect(ca, cb, C(p.lineStrong), 0, 0, Px(1));
  }
  // Cámara lenta: una línea debajo de la pista.
  if (s_.slow && s_.slowEnd > s_.slowStart)
    dl->AddRectFilled(ImVec2(xOf(s_.slowStart), mid + Px(10)), ImVec2(xOf(s_.slowEnd), mid + Px(12)), C(p.textMuted));
  for (double t : item.markers) DrawMark(dl, ImVec2(xOf(t), a.y + Px(5)), Px(3), C(p.accent), true);
  if (s_.freeze) DrawMark(dl, ImVec2(xOf(s_.freezeAt), mid - Px(13)), Px(3.5f), C(p.text), false);

  auto handle = [&](int id, double t, float top, float bottom, ImU32 color) {
    const float x = xOf(t);
    const bool hot = over == id || drag_ == id;
    dl->AddRectFilled(ImVec2(x - Px(2), mid - top), ImVec2(x + Px(2), mid + bottom), C(hot ? p.text : color));
  };
  handle(1, ra, Px(12), Px(12), p.accent);
  handle(2, rb, Px(12), Px(12), p.accent);
  if (s_.cut) {
    handle(3, s_.cutStart, Px(8), Px(8), p.lineStrong);
    handle(4, s_.cutEnd, Px(8), Px(8), p.lineStrong);
  }
  if (s_.slow) {
    handle(5, s_.slowStart, -Px(6), Px(15), p.textMuted);
    handle(6, s_.slowEnd, -Px(6), Px(15), p.textMuted);
  }
  const float px = xOf(player_.Position());
  dl->AddRectFilled(ImVec2(px - Px(1), mid - Px(17)), ImVec2(px + Px(1), mid + Px(17)), C(p.text));

  if (over) {
    static const char* const kNames[] = {"", "Inicio", "Fin", "Quitar desde", "Quitar hasta", "Lenta desde",
                                         "Lenta hasta", "Congelar en"};
    double t = 0;
    for (const auto& hd : handles)
      if (hd.id == over) t = hd.t;
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    ImGui::SetTooltip("%s %s", kNames[over], Precise(t).c_str());
  } else if (hov && d > 0 && !active) {
    const float x = std::clamp(mx, x0, x1);
    dl->AddRectFilled(ImVec2(x, mid - Px(6)), ImVec2(x + Px(1), mid + Px(6)), C(p.textMuted));
    ImGui::SetTooltip("%s", Precise(tOf(mx)).c_str());
  }
}

// ------------------------------------------------------------ herramientas

void EditorUi::Tools(const AppModel& m, const MediaItem& item, const AppActions& a, float height) {
  const float w = Px(kToolsWidth);
  ImGui::BeginGroup();
  static const std::vector<std::pair<int, std::string>> kTabs = {
      {0, "Tiempo"}, {1, "Imagen"}, {2, "Color"}, {3, "Texto"}, {4, "Audio"}, {5, "Cámara"}};
  const int tab = Segmented("tabs", kTabs, tab_, w);
  if (tab >= 0) tab_ = tab;
  Gap(10);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(6));
  ImGui::BeginChild("tools", ImVec2(w, std::max(Px(100), height - Px(42))), ImGuiChildFlags_None,
                    ImGuiWindowFlags_None);
  ImGui::BeginDisabled(!probed_ || !probeError_.empty());
  switch (tab_) {
  case 0: TabTime(item, a); break;
  case 1: TabPicture(); break;
  case 2: TabColor(); break;
  case 3: TabText(); break;
  case 4: TabAudio(); break;
  default: TabCamera(m); break;
  }
  Gap(16);
  ImGui::EndDisabled();
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::EndGroup();
}

void EditorUi::TabTime(const MediaItem& item, const AppActions& a) {
  const double d = Duration(item), pos = player_.Position();
  const double ra = std::clamp(s_.start, 0.0, d), rb = End(item);

  ToolHeader("Recortar");
  Note("De " + Precise(ra) + " a " + Precise(rb) + " (" + Length(rb - ra) + "). También con las asas de la barra.");
  Gap(4);
  switch (TwoButtons("in", "Empieza aquí (I)", "out", "Acaba aquí (O)")) {
  case 1: SetStart(pos, item); break;
  case 2: SetEnd(pos, item); break;
  default: break;
  }
  Gap(18);

  if (ToolHeader("Quitar un trozo del medio", &s_.cut) && s_.cut &&
      !(s_.cutEnd > s_.cutStart && s_.cutStart >= ra && s_.cutEnd <= rb)) {
    // Un tercio del recorte alrededor de donde se está.
    const double third = (rb - ra) / 3.0;
    s_.cutStart = std::clamp(pos - third * 0.5, ra, rb - third);
    s_.cutEnd = s_.cutStart + third;
  }
  if (s_.cut) {
    Note("Se salta de " + Precise(s_.cutStart) + " a " + Precise(s_.cutEnd) + " (" + Length(s_.cutEnd - s_.cutStart) +
         ").");
    Gap(4);
    switch (TwoButtons("cut-in", "Desde aquí", "cut-out", "Hasta aquí")) {
    case 1: s_.cutStart = std::clamp(pos, ra, std::max(ra, s_.cutEnd - kMinPart)); break;
    case 2: s_.cutEnd = std::clamp(pos, std::min(rb, s_.cutStart + kMinPart), rb); break;
    default: break;
    }
  }
  Gap(18);

  ToolHeader("Velocidad");
  static const std::vector<std::pair<int, std::string>> kSpeeds = {
      {25, "0,25×"}, {50, "0,5×"}, {75, "0,75×"}, {100, "1×"}, {150, "1,5×"}, {200, "2×"}, {300, "3×"}};
  const int speed = Segmented("speed", kSpeeds, int(std::lround(s_.speed * 100)));
  if (speed > 0) s_.speed = speed / 100.0;
  if (s_.speed != 1.0) {
    Gap(4);
    Note("El sonido va a la misma velocidad sin cambiar de tono.");
  }
  Gap(18);

  if (ToolHeader("Cámara lenta en un tramo", &s_.slow) && s_.slow &&
      !(s_.slowEnd > s_.slowStart && s_.slowStart >= ra && s_.slowEnd <= rb)) {
    s_.slowStart = std::clamp(pos - 1.0, ra, std::max(ra, rb - 2.0));
    s_.slowEnd = std::min(rb, s_.slowStart + 2.0);
  }
  if (s_.slow) {
    static const std::vector<std::pair<int, std::string>> kSlow = {{50, "A la mitad"}, {25, "A un cuarto"}};
    const int slow = Segmented("slow-speed", kSlow, int(std::lround(s_.slowSpeed * 100)));
    if (slow > 0) s_.slowSpeed = slow / 100.0;
    Gap(6);
    Note("De " + Precise(s_.slowStart) + " a " + Precise(s_.slowEnd) + ": la jugada a cámara lenta y el resto normal.");
    Gap(4);
    switch (TwoButtons("slow-in", "Desde aquí", "slow-out", "Hasta aquí")) {
    case 1: s_.slowStart = std::clamp(pos, 0.0, std::max(0.0, s_.slowEnd - kMinPart)); break;
    case 2: s_.slowEnd = std::clamp(pos, std::min(d, s_.slowStart + kMinPart), d); break;
    default: break;
    }
  }
  Gap(18);

  if (ToolHeader("Congelar la imagen", &s_.freeze) && s_.freeze) s_.freezeAt = std::clamp(pos, ra, rb);
  if (s_.freeze) {
    static const std::vector<std::pair<int, std::string>> kHold = {{1, "1 s"}, {2, "2 s"}, {3, "3 s"}, {5, "5 s"}};
    const int hold = Segmented("hold", kHold, int(std::lround(s_.freezeSeconds)));
    if (hold > 0) s_.freezeSeconds = hold;
    Gap(6);
    Note("La imagen se para en " + Precise(s_.freezeAt) + " y luego sigue.");
    Gap(4);
    if (GhostButton("freeze-here", "Congelar aquí", ImGui::GetContentRegionAvail().x, Tone::Normal, 28))
      s_.freezeAt = std::clamp(pos, ra, rb);
  }
  Gap(18);

  ToolHeader("Sentido");
  static const std::vector<std::pair<int, std::string>> kDirections = {
      {int(EditDirection::Forward), "Normal"}, {int(EditDirection::Reverse), "Al revés"},
      {int(EditDirection::Boomerang), "Bumerán"}};
  const int dir = Segmented("direction", kDirections, int(s_.direction));
  if (dir >= 0) s_.direction = EditDirection(dir);
  if (s_.direction == EditDirection::Boomerang) {
    Gap(4);
    Note("Hacia delante y luego hacia atrás, sin la imagen congelada.");
  }
  if (s_.direction != EditDirection::Forward && probed_ && ReversedSeconds(s_, src_) > kMaxReverseSeconds + 0.05) {
    Gap(4);
    Note("Con más de 1 min no se puede: recorta el tramo.");
  }
  Gap(18);

  ToolHeader("Repetir");
  static const std::vector<std::pair<int, std::string>> kRepeat = {{1, "Una vez"}, {2, "2 veces"}, {3, "3 veces"}};
  const int repeat = Segmented("repeat", kRepeat, s_.repeat);
  if (repeat > 0) s_.repeat = repeat;
  Gap(18);

  ToolHeader("Dividir en dos");
  Note("Guarda dos vídeos al momento, hasta aquí y desde aquí, sin recodificar: la segunda parte empieza en el "
       "fotograma clave anterior (hasta 2 s antes). No usa los cambios de arriba.");
  Gap(4);
  ImGui::BeginDisabled(pos < 0.5 || pos > d - 0.5);
  if (GhostButton("split", ("Dividir en " + Precise(pos)).c_str(), ImGui::GetContentRegionAvail().x, Tone::Normal, 28)) {
    Pause();
    if (a.split) a.split(path_, pos);
  }
  ImGui::EndDisabled();
}

void EditorUi::TabPicture() {
  ToolHeader("Formato");
  static const std::vector<std::pair<int, std::string>> kAspects = {{int(EditAspect::Original), "Original"},
                                                                    {int(EditAspect::Wide), "16:9"},
                                                                    {int(EditAspect::Vertical), "9:16"},
                                                                    {int(EditAspect::Square), "1:1"},
                                                                    {int(EditAspect::Portrait), "4:5"}};
  const int aspect = Segmented("aspect", kAspects, int(s_.aspect));
  if (aspect >= 0) s_.aspect = EditAspect(aspect);
  Gap(6);
  Note("9:16 para TikTok, Shorts y Reels; 1:1 y 4:5 para Instagram.");
  if (s_.aspect != EditAspect::Original) {
    Gap(10);
    Label("Lo que no cabe");
    static const std::vector<std::pair<int, std::string>> kFits = {
        {int(EditFit::Crop), "Recortar"}, {int(EditFit::Blur), "Fondo borroso"}, {int(EditFit::Bars), "Barras"}};
    const int fit = Segmented("fit", kFits, int(s_.fit));
    if (fit >= 0) s_.fit = EditFit(fit);
    Gap(6);
    Note(s_.fit == EditFit::Crop ? "Se queda con una parte: arrastra la imagen para elegir cuál."
         : s_.fit == EditFit::Blur ? "Todo el vídeo, con el mismo vídeo borroso de fondo."
                                   : "Todo el vídeo, con barras negras.");
  }
  Gap(18);

  ToolHeader("Zoom");
  ImGui::SetNextItemWidth(-1);
  ImGui::SliderFloat("##zoom", &s_.zoom, 1.0f, 3.0f, "%.1f×", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) s_.zoom = 1.0f;
  if (s_.zoom > 1.001f) {
    Gap(6);
    Note("Arrastra la imagen para elegir qué parte se ve.");
  }
  Gap(18);

  ToolHeader("Girar y voltear");
  switch (TwoButtons("rot-left", "Girar a la izquierda", "rot-right", "Girar a la derecha")) {
  case 1: s_.rotate = (s_.rotate + 270) % 360; break;
  case 2: s_.rotate = (s_.rotate + 90) % 360; break;
  default: break;
  }
  Gap(4);
  SwitchRow("Espejo (izquierda y derecha)", &s_.flipH);
  SwitchRow("Boca abajo", &s_.flipV);
  Gap(18);

  // Resolución y fps: solo lo que baja del original (subir no gana nada).
  EditSettings native = s_;
  native.shortSide = 0;
  if (native.format == EditFormat::Gif) native.format = EditFormat::Video;
  const EditGeometry g = ComputeGeometry(native, src_);
  const int shortSide = std::min(g.outW, g.outH);
  std::vector<std::pair<int, std::string>> sizes = {{0, "Original"}};
  for (int side : {1080, 720, 480})
    if (side < shortSide - 8) sizes.push_back({side, std::to_string(side) + "p"});
  ToolHeader("Resolución");
  if (s_.format == EditFormat::Gif) {
    Note("Los GIF se guardan a 480p como mucho.");
  } else if (sizes.size() > 1) {
    // Una elegida antes del 9:16 o del zoom que ya no se ofrece no amplía
    // nada (ComputeGeometry solo baja): se ve como «Original».
    int current = 0;
    for (const auto& option : sizes)
      if (option.first == s_.shortSide) current = option.first;
    const int size = Segmented("short-side", sizes, current);
    if (size >= 0) s_.shortSide = size;
    Gap(6);
    char buf[96];
    snprintf(buf, sizeof(buf), "Original: %dx%d. Menos resolución, archivo más ligero.", g.outW, g.outH);
    Note(buf);
  } else {
    Note("Ya es " + std::to_string(g.outW) + "x" + std::to_string(g.outH) + ": no se puede bajar más.");
  }
  Gap(18);

  ToolHeader("Fotogramas por segundo");
  std::vector<std::pair<int, std::string>> rates = {{0, "Originales"}};
  for (int fps : {60, 30})
    if (fps < src_.fps - 1) rates.push_back({fps, std::to_string(fps)});
  if (s_.format == EditFormat::Gif) {
    Note("Los GIF se guardan a 15 fps.");
  } else if (rates.size() > 1) {
    const int fps = Segmented("fps", rates, s_.fps);
    if (fps >= 0) s_.fps = fps;
    Gap(6);
    Note("El vídeo va a " + FpsText(src_.fps) + ". Con menos, pesa menos.");
  } else {
    Note("El vídeo va a " + FpsText(src_.fps) + ".");
  }
}

void EditorUi::TabColor() {
  ToolHeader("Estilo");
  static const std::vector<std::pair<int, std::string>> kLooks = {
      {int(EditLook::None), "Ninguno"},     {int(EditLook::Vivid), "Vivo"},  {int(EditLook::Mono), "Blanco y negro"},
      {int(EditLook::Sepia), "Sepia"},      {int(EditLook::Warm), "Cálido"}, {int(EditLook::Cold), "Frío"},
      {int(EditLook::Faded), "Desvaído"}};
  const int look = ChoiceGrid("looks", kLooks, int(s_.look), 3);
  if (look >= 0) s_.look = EditLook(look);
  Gap(18);

  ToolHeader("Ajustes");
  Label("Brillo");
  CenteredSlider("##brightness", &s_.brightness);
  Gap(8);
  Label("Contraste");
  CenteredSlider("##contrast", &s_.contrast);
  Gap(8);
  Label("Saturación");
  CenteredSlider("##saturation", &s_.saturation);
  Gap(18);

  ToolHeader("Efectos");
  SwitchRow("Más nitidez", &s_.sharpen);
  SwitchRow("Viñeta (bordes más oscuros)", &s_.vignette);
  SwitchRow("Aparecer desde negro", &s_.fadeIn);
  SwitchRow("Fundido a negro al final", &s_.fadeOut);
  Gap(10);
  Note("Pausa el vídeo para ver el color tal cual saldrá.");
  const bool any = s_.look != EditLook::None || s_.brightness || s_.contrast || s_.saturation || s_.sharpen ||
                   s_.vignette || s_.fadeIn || s_.fadeOut;
  if (any) {
    Gap(8);
    if (GhostButton("color-reset", "Quitar los cambios de color", ImGui::GetContentRegionAvail().x, Tone::Normal, 28)) {
      s_.look = EditLook::None;
      s_.brightness = s_.contrast = s_.saturation = 0;
      s_.sharpen = s_.vignette = s_.fadeIn = s_.fadeOut = false;
    }
  }
}

void EditorUi::TabText() {
  ToolHeader("Texto");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##text", "Escribe un título o un comentario", &s_.text);
  if (s_.text.size() > 200) s_.text.resize(200);
  if (!s_.text.empty()) {
    Gap(10);
    Label("Dónde");
    static const std::vector<std::pair<int, std::string>> kPlaces = {
        {int(EditPlace::Top), "Arriba"}, {int(EditPlace::Middle), "Centro"}, {int(EditPlace::Bottom), "Abajo"}};
    const int place = Segmented("place", kPlaces, int(s_.textPlace));
    if (place >= 0) s_.textPlace = EditPlace(place);
    Gap(8);
    Label("Tamaño");
    static const std::vector<std::pair<int, std::string>> kSizes = {{0, "Pequeño"}, {1, "Mediano"}, {2, "Grande"}};
    const int size = Segmented("text-size", kSizes, s_.textSize);
    if (size >= 0) s_.textSize = size;
    Gap(8);
    Label("Cuánto se ve");
    static const std::vector<std::pair<int, std::string>> kSeconds = {{0, "Todo"}, {3, "3 s"}, {5, "5 s"}, {10, "10 s"}};
    const int secs = Segmented("text-seconds", kSeconds, int(std::lround(s_.textSeconds)));
    if (secs >= 0) s_.textSeconds = secs;
    Gap(6);
    SwitchRow("Con fondo oscuro detrás", &s_.textBox);
  }
  Gap(18);

  ToolHeader("Marca de agua");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##watermark", "Tu nombre o tu canal", &s_.watermark);
  if (s_.watermark.size() > 60) s_.watermark.resize(60);
  if (!s_.watermark.empty()) {
    Gap(10);
    static const std::vector<std::pair<int, std::string>> kCorners = {
        {int(EditCorner::TopLeft), "Arriba a la izquierda"},
        {int(EditCorner::TopRight), "Arriba a la derecha"},
        {int(EditCorner::BottomLeft), "Abajo a la izquierda"},
        {int(EditCorner::BottomRight), "Abajo a la derecha"}};
    const int corner = ChoiceGrid("corner", kCorners, int(s_.watermarkCorner), 2);
    if (corner >= 0) s_.watermarkCorner = EditCorner(corner);
  }
  Gap(18);

  ToolHeader("Tapar una zona", &s_.blur);
  Note("Para nombres, el chat o una notificación. Arrastra el recuadro sobre la imagen; desde la esquina cambia de "
       "tamaño.");
  if (s_.blur) {
    Gap(8);
    static const std::vector<std::pair<int, std::string>> kModes = {{0, "Difuminar"}, {1, "Pixelar"}};
    const int mode = Segmented("blur-mode", kModes, s_.pixelate ? 1 : 0);
    if (mode >= 0) s_.pixelate = mode == 1;
  }
}

void EditorUi::TabAudio() {
  if (!probed_) {
    Note("Leyendo el vídeo...");
    return;
  }
  if (!src_.audio) {
    ToolHeader("Sonido");
    Note("Este vídeo no tiene sonido.");
    return;
  }
  ToolHeader("Quitar el sonido", &s_.mute);
  if (s_.mute) return;
  Gap(14);
  ToolHeader("Volumen");
  Label(src_.SeparateMic() ? "Juego y PC" : "Todo el sonido");
  PercentSlider("##volume", &s_.volume);
  if (src_.SeparateMic()) {
    Gap(8);
    Label("Micrófono");
    PercentSlider("##mic", &s_.micVolume);
  } else {
    Gap(6);
    Note("El micrófono va mezclado con el juego en este clip. Con «Pistas separadas» en Ajustes > Audio, los clips "
         "nuevos dejan ajustarlos por separado.");
  }
  Gap(18);
  ToolHeader("Mejorar");
  SwitchRow("Igualar el volumen", &s_.normalize);
  SwitchRow(src_.SeparateMic() ? "Quitar el ruido del micrófono" : "Quitar el ruido de fondo", &s_.denoise);
  SwitchRow("Entrada suave", &s_.audioFadeIn);
  SwitchRow("Salida suave", &s_.audioFadeOut);
  Gap(10);
  Note("Igualar sube lo que se oye bajo y baja los gritos. La vista previa suena hasta el 100 %; subir de ahí, "
       "igualar y quitar ruido se oyen al guardar.");
}

void EditorUi::TabCamera(const AppModel& m) {
  if (!probed_) {
    Note("Leyendo el vídeo...");
    return;
  }
  if (!src_.Webcam()) {
    ToolHeader("Cámara");
    Note("Este clip no guardó la cámara aparte: va pegada en la imagen y, si se moviera o se quitara, debajo "
         "quedaría un hueco.");
    Gap(8);
    Note("Lo que sí se puede: taparla con un difuminado o dejarla fuera con el formato o el zoom (pestaña Imagen).");
    Gap(8);
    if (GhostButton("cam-cover", "Tapar donde suele ir la cámara", ImGui::GetContentRegionAvail().x, Tone::Normal,
                    28) &&
        src_.width > 0 && src_.height > 0) {
      // Donde la pone la captura con los ajustes de ahora (Pipeline::CameraRect):
      // su ancho en % del vídeo, 16:9 y a 1/40 del alto del borde.
      const Config& c = m.config;
      const float pct = std::clamp(c.cameraSizePercent, 10, 50) / 100.0f;
      const float camH = pct * float(src_.width) * 9.0f / 16.0f / float(src_.height);
      const float mx = float(src_.height) / 40.0f / float(src_.width), my = 1.0f / 40.0f;
      const bool left = c.cameraCorner.find("left") != std::string::npos;
      const bool top = c.cameraCorner.find("top") != std::string::npos;
      s_.blur = true;
      s_.pixelate = false;
      s_.blurW = std::min(1.0f, pct + 0.02f);
      s_.blurH = std::min(1.0f, camH + 0.02f);
      s_.blurX = std::clamp(left ? mx - 0.01f : 1.0f - pct - mx - 0.01f, 0.0f, 1.0f - s_.blurW);
      s_.blurY = std::clamp(top ? my - 0.01f : 1.0f - camH - my - 0.01f, 0.0f, 1.0f - s_.blurH);
      tab_ = 3; // allí se ve el recuadro y se ajusta
    }
    Gap(14);
    Note("Los clips nuevos con la cámara se pueden editar aquí si está activado Ajustes > Cámara > «Poder editar la "
         "cámara después».");
    return;
  }

  if (ToolHeader("Cambiar la cámara", &s_.camEdit) && s_.camEdit) DefaultCamPlacement(&s_, src_);
  Note(s_.camEdit ? "Arrástrala sobre la imagen para moverla; desde la esquina cambia de tamaño."
                  : "Este clip guardó aparte lo que hay debajo de la cámara: se puede mover, cambiar de tamaño o "
                    "quitar sin dejar hueco.");
  if (!s_.camEdit) return;
  Gap(10);
  SwitchRow("Quitarla del vídeo", &s_.camHide);
  if (s_.camHide) return;
  Gap(14);

  // Dónde: seis sitios con el mismo margen en los cuatro bordes.
  ToolHeader("Dónde");
  {
    const EditGeometry g = ComputeGeometry(s_, src_);
    const EditCamBox box = ComputeCamBox(s_, src_, g);
    if (box.w > 0 && g.canvasW > 0 && g.canvasH > 0) {
      const float bw = float(box.w) / float(g.canvasW), bh = float(box.h) / float(g.canvasH);
      const float mx = 0.03f, my = 0.03f * float(g.canvasW) / float(g.canvasH);
      const float xs[3] = {mx, (1.0f - bw) * 0.5f, 1.0f - bw - mx};
      const float ys[2] = {my, 1.0f - bh - my};
      int current = -1;
      for (int i = 0; i < 6; ++i)
        if (std::fabs(s_.camX - xs[i % 3]) < 0.004f && std::fabs(s_.camY - ys[i / 3]) < 0.004f) current = i;
      static const std::vector<std::pair<int, std::string>> kPlaces = {
          {0, "Arriba izq."}, {1, "Arriba"}, {2, "Arriba der."}, {3, "Abajo izq."}, {4, "Abajo"}, {5, "Abajo der."}};
      const int place = ChoiceGrid("cam-place", kPlaces, current, 3);
      if (place >= 0) {
        s_.camX = std::max(0.0f, xs[place % 3]);
        s_.camY = std::max(0.0f, ys[place / 3]);
      }
    }
  }
  Gap(14);

  ToolHeader("Tamaño");
  {
    int pct = int(std::lround(s_.camW * 100.0f));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##cam-size", &pct, 8, 100, "%d %% del ancho", ImGuiSliderFlags_AlwaysClamp))
      s_.camW = float(pct) / 100.0f;
  }
  Gap(14);

  ToolHeader("Forma");
  static const std::vector<std::pair<int, std::string>> kShapes = {
      {int(EditCamShape::Original), "Como se grabó"}, {int(EditCamShape::Square), "Cuadrada"},
      {int(EditCamShape::Circle), "Redonda"}};
  const int shape = Segmented("cam-shape", kShapes, int(s_.camShape));
  if (shape >= 0) s_.camShape = EditCamShape(shape);
  Gap(14);

  ToolHeader("Borde");
  static const std::vector<std::pair<int, std::string>> kBorders = {{0, "Sin borde"}, {1, "Fino"}, {2, "Grueso"}};
  const int border = Segmented("cam-border", kBorders, s_.camBorder);
  if (border >= 0) s_.camBorder = border;
  if (s_.camBorder > 0) {
    Gap(8);
    const uint32_t accent = Rgb(Colors().accent);
    const int color = s_.camBorderColor == 0xFFFFFF ? 0 : s_.camBorderColor == 0x000000 ? 1 : 2;
    static const std::vector<std::pair<int, std::string>> kColors = {{0, "Blanco"}, {1, "Negro"}, {2, "Del tema"}};
    const int picked = Segmented("cam-border-color", kColors, color);
    if (picked == 0) s_.camBorderColor = 0xFFFFFF;
    else if (picked == 1) s_.camBorderColor = 0x000000;
    else if (picked == 2) s_.camBorderColor = accent == 0xFFFFFF || accent == 0 ? 0x808080 : accent;
  }
  Gap(14);
  SwitchRow("Espejo (izquierda y derecha)", &s_.camMirror);
  Gap(10);
  Note("En pausa se ve tal cual saldrá, con los colores de la pestaña Color.");
}

// ------------------------------------------------------------ guardar

void EditorUi::Footer(AppModel& m, const AppActions& a, const MediaItem& item, Exit* exit) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float wAvail = ImGui::GetContentRegionAvail().x, footerH = Px(kFooterHeight);
  dl->AddRectFilled(q, ImVec2(q.x + wAvail, q.y + Px(1)), C(p.line));
  const float y = q.y + (footerH - Px(32)) * 0.5f;
  const float fs = Px(kSmall);
  const bool roomy = wAvail >= Px(1000);
  float x = q.x + Px(28);
  auto label = [&](const char* text) {
    if (!roomy) return;
    const ImVec2 ts = TextSize(ImGui::GetFont(), fs, text);
    Text(dl, ImGui::GetFont(), fs, ImVec2(x, q.y + (footerH - fs) * 0.5f), C(p.textMuted), text);
    x += ts.x + Px(10);
  };

  label("Guardar como");
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  static const std::vector<std::pair<int, std::string>> kFormats = {
      {int(EditFormat::Video), "Vídeo"}, {int(EditFormat::Gif), "GIF"}, {int(EditFormat::Audio), "Solo audio"}};
  const int format = Segmented("format", kFormats, int(s_.format), Px(228));
  if (format >= 0) s_.format = EditFormat(format);
  x += Px(228) + Px(18);
  if (s_.format == EditFormat::Video) {
    label("Calidad");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    static const std::vector<std::pair<int, std::string>> kSizes = {
        {int(EditSize::Same), "Igual"}, {int(EditSize::Light), "Más ligero"}, {int(EditSize::Discord), "Para Discord"}};
    const int size = Segmented("size", kSizes, int(s_.size), Px(288));
    if (size >= 0) s_.size = EditSize(size);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("Igual: la calidad del original. Más ligero: la mitad de bitrate. Para Discord: además, una "
                        "copia de menos de 20 MB que se ve en el chat.");
    x += Px(288) + Px(18);
  } else {
    const char* text = s_.format == EditFormat::Gif ? "GIF a 15 fps, 480p como mucho, hasta 30 s"
                                                    : "M4A (AAC a 192 kbps)";
    Text(dl, ImGui::GetFont(), fs, ImVec2(x, q.y + (footerH - fs) * 0.5f), C(p.textFaint), text);
    x += TextSize(ImGui::GetFont(), fs, text).x + Px(18);
  }

  // A la derecha: guardar, o el progreso de lo que se está guardando.
  const float right = q.x + wAvail - Px(28);
  const bool busy = m.editPercent >= 0;
  const bool mine = busy && m.editRenderPath == path_;
  if (mine) {
    const float bw = Px(120), barW = Px(150);
    ImGui::SetCursorScreenPos(ImVec2(right - bw, y));
    if (GhostButton("cancel", "Cancelar", bw)) a.cancelEdit();
    const std::string text = "Guardando " + std::to_string(m.editPercent) + " %";
    const float tx = right - bw - Px(16) - barW;
    Text(dl, ImGui::GetFont(), fs, ImVec2(tx, y + Px(2)), C(p.text), text.c_str());
    const float by = y + Px(22);
    dl->AddRectFilled(ImVec2(tx, by), ImVec2(tx + barW, by + Px(3)), C(p.surfaceActive));
    dl->AddRectFilled(ImVec2(tx, by), ImVec2(tx + barW * float(m.editPercent) / 100.0f, by + Px(3)), C(p.accent));
  } else {
    std::string why;
    const bool can = CanSave(item, &why) && !busy;
    if (busy) why = "Ya se está guardando otra edición: espera a que termine";
    const float bw = Px(150);
    ImGui::SetCursorScreenPos(ImVec2(right - bw, y));
    ImGui::BeginDisabled(!can);
    if (SolidButton("save", "Guardar copia", bw) && can) {
      Pause();
      a.saveEdit(path_, s_);
    }
    ImGui::EndDisabled();
    if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
    // La última copia de este vídeo, para verla.
    if (m.editSavedFrom == path_ && !m.editSaved.empty() && right - bw - Px(20) - x > Px(170)) {
      const std::string name = Ellipsize(ImGui::GetFont(), Px(kHint), "Ver " + NameOf(m.editSaved),
                                         std::min(Px(260), right - bw - Px(20) - x));
      const ImVec2 ts = TextSize(ImGui::GetFont(), Px(kHint), name.c_str());
      ImGui::SetCursorScreenPos(ImVec2(right - bw - Px(18) - ts.x, q.y + (footerH - ts.y) * 0.5f));
      if (ImGui::InvisibleButton("view-saved", ts + ImVec2(0, Px(4)))) exit->view = m.editSaved;
      const bool hov = ImGui::IsItemHovered();
      if (hov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Abrir la copia guardada en el visor");
      }
      const ImVec2 tq = ImGui::GetItemRectMin();
      Text(dl, ImGui::GetFont(), Px(kHint), tq, C(hov ? p.text : p.textMuted), name.c_str());
      if (hov) dl->AddRectFilled(ImVec2(tq.x, tq.y + ts.y + Px(1)), ImVec2(tq.x + ts.x, tq.y + ts.y + Px(2)), C(p.text));
    }
  }
  ImGui::SetCursorScreenPos(q);
  ImGui::Dummy(ImVec2(wAvail, footerH));
}

void EditorUi::Keyboard(const AppActions& a, const MediaItem& item, Exit* exit) {
  if (TypingText() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) ||
      !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    return;
  const ImGuiIO& io = ImGui::GetIO();
  const double d = Duration(item), pos = player_.Position();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) exit->back = true;
  if (io.KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z, true)) {
      if (io.KeyShift) Redo();
      else Undo();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y, true)) Redo();
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
      std::string why;
      if (CanSave(item, &why) && a.saveEdit) {
        Pause();
        a.saveEdit(path_, s_);
      }
    }
    return;
  }
  if (playerFailed_ || !player_.Ready()) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_K, false)) TogglePlay(item);
  // Flechas: un segundo; con Mayús, un fotograma.
  const double step = io.KeyShift ? 1.0 / std::max(1.0, src_.fps) : 1.0;
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) Seek(std::min(d, pos + step));
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) Seek(std::max(0.0, pos - step));
  if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) Seek(std::clamp(s_.start, 0.0, d));
  if (ImGui::IsKeyPressed(ImGuiKey_End, false)) Seek(std::max(0.0, End(item) - 0.05));
  if (ImGui::IsKeyPressed(ImGuiKey_I, false)) SetStart(pos, item);
  if (ImGui::IsKeyPressed(ImGuiKey_O, false)) SetEnd(pos, item);
  if (ImGui::IsKeyPressed(ImGuiKey_M, false)) muted_ = !muted_;
}

} // namespace spectre
