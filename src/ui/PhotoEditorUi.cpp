#include "ui/PhotoEditorUi.h"

#include "app/CrashHandler.h"
#include "app/Screenshot.h"
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
#include <mutex>
#include <optional>
#include <thread>

namespace spectre {

using namespace ui;
using namespace editui;

namespace {

constexpr float kToolsWidth = 336.0f; // como el editor de vídeo
constexpr float kFooterHeight = 64.0f;
constexpr size_t kMaxUndo = 100;
constexpr int kWorkSide = 2048; // la copia de trabajo de la vista previa, como mucho este lado

std::string Title(const MediaItem& item) {
  if (!item.spectre) return item.stem;
  return item.game.empty() ? std::string("Escritorio") : item.game;
}

ImVec2 Lerp(ImVec2 a, ImVec2 b, float tx, float ty) { return ImVec2(a.x + (b.x - a.x) * tx, a.y + (b.y - a.y) * ty); }

float Dist(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

// Distancia de p al segmento ab.
float SegmentDist(ImVec2 p, ImVec2 a, ImVec2 b) {
  const ImVec2 d(b.x - a.x, b.y - a.y);
  const float len2 = d.x * d.x + d.y * d.y;
  const float t = len2 > 0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / len2, 0.0f, 1.0f) : 0.0f;
  return Dist(p, ImVec2(a.x + d.x * t, a.y + d.y * t));
}

ImU32 Opaque(uint32_t rgb) { return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255); }

bool Boxy(PhotoMarkKind k) {
  return k == PhotoMarkKind::Box || k == PhotoMarkKind::Blur || k == PhotoMarkKind::Pixelate;
}

// Colores de las marcas: los de siempre y el del tema. Devuelve el pulsado o -1.
int64_t Swatches(const char* id, uint32_t current) {
  const Palette& p = Colors();
  const uint32_t colors[] = {0xFF3B30, 0xFFCC00, 0x34C759, 0x0A84FF, 0xFFFFFF, 0x000000, Rgb(p.accent)};
  static const char* kNames[] = {"Rojo", "Amarillo", "Verde", "Azul", "Blanco", "Negro", "El del tema"};
  ImGui::PushID(id);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float gap = Px(6), side = std::floor((ImGui::GetContentRegionAvail().x - gap * 6) / 7.0f);
  int64_t picked = -1;
  for (int i = 0; i < 7; ++i) {
    if (i > 0) ImGui::SameLine(0, gap);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("c", ImVec2(side, Px(26)))) picked = colors[i];
    const bool hov = ImGui::IsItemHovered();
    if (hov) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImGui::SetTooltip("%s", kNames[i]);
    }
    ImGui::PopID();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool sel = colors[i] == current;
    dl->AddRectFilled(a, b, C(sel || hov ? p.text : p.line));
    const float inset = Px(sel ? 3.0f : 1.0f);
    dl->AddRectFilled(a + ImVec2(inset, inset), b - ImVec2(inset, inset), Opaque(colors[i]));
  }
  ImGui::PopID();
  return picked;
}

} // namespace

// ------------------------------------------------------------ hilo

// Lee la imagen una vez (y se queda una copia de trabajo de como mucho
// kWorkSide) y hace la vista previa con los últimos ajustes que se pidan.
struct PhotoEditorUi::Worker {
  enum class Kind { Load, Render };
  struct Job {
    Kind kind = Kind::Load;
    uint64_t serial = 0;
    std::string path;
    PhotoSettings settings;
    bool cropView = false;
  };
  struct Result {
    Kind kind = Kind::Load;
    uint64_t serial = 0;
    bool ok = false;
    std::string error;
    int fullW = 0, fullH = 0, baseW = 0, baseH = 0;
    std::vector<uint8_t> bgra;
    int width = 0, height = 0;
    bool cropView = false;
    PhotoMarkBoxes boxes;
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
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || pending_[0] || pending_[1]; });
        if (stop_) break;
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
    if (SUCCEEDED(co)) CoUninitialize();
  }

  Result Run(const Job& job) {
    Result r;
    r.kind = job.kind;
    r.serial = job.serial;
    try {
      if (job.kind == Kind::Load) {
        base_ = PhotoImage{};
        baseSerial_ = job.serial;
        PhotoImage full;
        if (ReadImageFile(job.path, 16384, &full, &r.error)) {
          r.fullW = full.width;
          r.fullH = full.height;
          const int longSide = std::max(full.width, full.height);
          if (longSide > kWorkSide) {
            const double k = double(kWorkSide) / longSide;
            base_ = ScalePhoto(full, std::max(1, int(full.width * k)), std::max(1, int(full.height * k)));
          } else {
            base_ = std::move(full);
          }
          r.baseW = base_.width;
          r.baseH = base_.height;
          r.ok = !base_.Empty();
        }
      } else if (job.serial == baseSerial_ && !base_.Empty()) {
        PhotoImage out;
        PhotoRender options;
        options.cropView = job.cropView;
        r.cropView = job.cropView;
        r.ok = RenderPhoto(job.settings, base_, RasterizeText, options, &out, &r.boxes);
        r.bgra = std::move(out.bgra);
        r.width = out.width;
        r.height = out.height;
      }
    } catch (const std::exception& e) {
      r.ok = false;
      r.error = e.what();
      Log("Editor de fotos: %s", e.what());
    }
    return r;
  }

  HWND notify_;
  UINT msg_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::optional<Job> pending_[2];
  std::vector<Result> results_;
  // Solo en el hilo del trabajo.
  PhotoImage base_;
  uint64_t baseSerial_ = ~0ull;
};

// ------------------------------------------------------------ ciclo de vida

PhotoEditorUi::PhotoEditorUi() = default;
PhotoEditorUi::~PhotoEditorUi() { Stop(); }

void PhotoEditorUi::Start(HWND notifyWindow, UINT notifyMsg) {
  notify_ = notifyWindow;
  notifyMsg_ = notifyMsg;
}

void PhotoEditorUi::Stop() {
  Leave();
  worker_.reset();
  notify_ = nullptr;
  // La imagen se vuelve a leer al abrir la ventana otra vez.
  loaded_ = false;
}

void PhotoEditorUi::Open(const std::string& path) {
  if (path != path_) {
    Leave();
    path_ = path;
    s_ = committed_ = PhotoSettings{};
    undo_.clear();
    redo_.clear();
    tab_ = 0;
    selected_ = -1;
    text_.clear();
    loaded_ = false;
  }
  active_ = true;
  if (!worker_ && notify_) worker_ = std::make_unique<Worker>(notify_, notifyMsg_);
  if (!loaded_ && worker_) {
    loadError_.clear();
    Worker::Job job;
    job.kind = Worker::Kind::Load;
    job.serial = serial_;
    job.path = path_;
    worker_->Post(std::move(job));
  }
}

void PhotoEditorUi::Leave() {
  active_ = false;
  loaded_ = false; // la copia de trabajo es del hilo: se vuelve a leer al entrar
  preview_.reset();
  boxes_.clear();
  busy_ = false;
  askedOnce_ = false;
  drag_ = 0;
  ++serial_;
}

void PhotoEditorUi::TakeResults(AppWindow& w) {
  if (!worker_) return;
  for (auto& r : worker_->Take()) {
    if (r.serial != serial_) continue;
    if (r.kind == Worker::Kind::Load) {
      loaded_ = r.ok;
      loadError_ = r.ok ? std::string() : (r.error.empty() ? "No se pudo leer la imagen" : r.error);
      fullW_ = r.fullW;
      fullH_ = r.fullH;
      baseW_ = r.baseW;
      baseH_ = r.baseH;
      askedOnce_ = false;
    } else {
      busy_ = false;
      if (r.ok && r.width > 0 && r.height > 0) {
        preview_ = w.CreateTexture(r.bgra.data(), r.width, r.height);
        previewCrop_ = r.cropView;
        boxes_ = std::move(r.boxes);
      }
    }
  }
  w.Invalidate();
}

// ------------------------------------------------------------ deshacer

void PhotoEditorUi::Commit() {
  if (s_ == committed_ || ImGui::IsAnyItemActive()) return;
  undo_.push_back(committed_);
  if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
  redo_.clear();
  committed_ = s_;
}

void PhotoEditorUi::Undo() {
  if (!(s_ == committed_)) {
    undo_.push_back(committed_);
    committed_ = s_;
    redo_.clear();
  }
  if (undo_.empty()) return;
  redo_.push_back(s_);
  s_ = committed_ = undo_.back();
  undo_.pop_back();
  selected_ = -1; // la marca elegida puede no existir ya
}

void PhotoEditorUi::Redo() {
  if (redo_.empty()) return;
  undo_.push_back(s_);
  s_ = committed_ = redo_.back();
  redo_.pop_back();
  selected_ = -1;
}

std::string PhotoEditorUi::Summary() const {
  if (!loadError_.empty()) return loadError_;
  if (!loaded_) return "Leyendo la imagen...";
  PhotoSettings out = s_;
  if (target_ == 2 && out.size == PhotoSize::Same) out.size = PhotoSize::Large;
  int w = 0, h = 0;
  PhotoOutputSize(out, fullW_, fullH_, &w, &h);
  char buf[160];
  snprintf(buf, sizeof(buf), "Resultado: %dx%d  ·  %s  ·  el original no cambia", w, h,
           target_ == 0 ? "PNG" : target_ == 1 ? "JPG" : "JPG para Discord");
  return buf;
}

PhotoMark& PhotoEditorUi::NewMark(PhotoMarkKind kind) {
  PhotoMark m;
  m.kind = kind;
  m.color = color_;
  m.size = size_;
  if (kind == PhotoMarkKind::Text) m.text = text_.empty() ? std::string("Texto") : text_;
  s_.marks.push_back(std::move(m));
  selected_ = int(s_.marks.size()) - 1;
  return s_.marks.back();
}

// ------------------------------------------------------------ dibujo

PhotoEditorUi::Exit PhotoEditorUi::Draw(AppWindow& w, AppModel& m, const AppActions& a, const MediaItem& item) {
  Exit exit;
  if (selected_ >= int(s_.marks.size())) selected_ = -1;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(28), Px(20)));
  ImGui::BeginChild("photo-editor", ImVec2(0, -Px(kFooterHeight)), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  Header(item, &exit);
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
  const float toolsW = Px(kToolsWidth), gap = Px(24);
  const float leftW = std::max(Px(260), availW - toolsW - gap);
  const ImVec2 va = q, vb(q.x + leftW, q.y + std::max(Px(150), availH - Px(8)));
  Preview(w, va, vb);
  ImGui::SetCursorScreenPos(ImVec2(q.x + leftW + gap, q.y));
  Tools(m, availH);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  Footer(m, a, &exit);
  Keyboard(m, a, &exit);
  Commit();
  if (busy_ || drag_ != 0) w.Animate();
  return exit;
}

void PhotoEditorUi::Header(const MediaItem& item, Exit* exit) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float rowH = Px(34), avail = ImGui::GetContentRegionAvail().x;
  if (IconButton("back", Icon::Back, rowH, "Volver (Esc)")) exit->back = true;
  const float tx = q.x + rowH + Px(10);
  const float resetW = Px(140);
  const float actionsW = rowH * 2 + Px(4) + Px(10) + resetW;
  const std::string title = "Editar captura  ·  " + Title(item);
  Text(dl, Bold(), Px(kBody) + Px(2), ImVec2(tx, q.y), C(p.text),
       Ellipsize(Bold(), Px(kBody) + Px(2), title, avail - rowH - actionsW - Px(30)).c_str());
  Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(tx, q.y + Px(21)), C(loadError_.empty() ? p.textMuted : p.danger),
       Ellipsize(ImGui::GetFont(), Px(kSmall), Summary(), avail - rowH - actionsW - Px(30)).c_str());

  ImGui::SetCursorScreenPos(ImVec2(q.x + avail - actionsW, q.y));
  ImGui::BeginDisabled(undo_.empty() && s_ == committed_);
  if (IconButton("undo", Icon::Undo, rowH, "Deshacer (Ctrl+Z)")) Undo();
  ImGui::EndDisabled();
  ImGui::SameLine(0, Px(4));
  ImGui::BeginDisabled(redo_.empty());
  if (IconButton("redo", Icon::Redo, rowH, "Rehacer (Ctrl+Y)")) Redo();
  ImGui::EndDisabled();
  ImGui::SameLine(0, Px(10));
  ImGui::BeginDisabled(s_ == PhotoSettings{});
  if (GhostButton("reset", "Empezar de cero", resetW, Tone::Normal, 34)) {
    // Quita todos los cambios; el tamaño de guardado se queda.
    PhotoSettings fresh;
    fresh.size = s_.size;
    s_ = fresh;
    selected_ = -1;
  }
  ImGui::EndDisabled();
  ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(14)));
}

PhotoEditorUi::Layout PhotoEditorUi::ComputeLayout(ImVec2 va, ImVec2 vb) const {
  Layout l;
  l.baseW = baseW_;
  l.baseH = baseH_;
  if (baseW_ <= 0 || baseH_ <= 0) return l;
  int rw = 0, rh = 0;
  PhotoRotatedSize(s_, baseW_, baseH_, &rw, &rh);
  const PhotoRect crop = PhotoCrop(s_, baseW_, baseH_);
  int w = rw, h = rh, border = 0;
  if (tab_ != 0) {
    PhotoSettings plain = s_;
    plain.size = PhotoSize::Same;
    PhotoOutputSize(plain, baseW_, baseH_, &w, &h);
    border = (w - crop.w) / 2;
  }
  const float aw = vb.x - va.x, ah = vb.y - va.y;
  const float k = std::min(aw / float(std::max(1, w)), ah / float(std::max(1, h)));
  const float dw = std::floor(float(w) * k), dh = std::floor(float(h) * k);
  l.a = ImVec2(std::floor(va.x + (aw - dw) * 0.5f), std::floor(va.y + (ah - dh) * 0.5f));
  l.b = ImVec2(l.a.x + dw, l.a.y + dh);
  if (tab_ == 0) {
    l.ca = ImVec2(l.a.x + float(crop.x) * k, l.a.y + float(crop.y) * k);
    l.cb = ImVec2(l.ca.x + float(crop.w) * k, l.ca.y + float(crop.h) * k);
  } else {
    l.ca = ImVec2(l.a.x + float(border) * k, l.a.y + float(border) * k);
    l.cb = ImVec2(l.b.x - float(border) * k, l.b.y - float(border) * k);
  }
  return l;
}

void PhotoEditorUi::Preview(AppWindow& w, ImVec2 va, ImVec2 vb) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(va, vb, C(IM_COL32(0, 0, 0, 255)));
  auto centered = [&](const std::string& text, ImU32 color) {
    const ImVec2 ts = TextSize(ImGui::GetFont(), Px(kBody), text.c_str());
    Text(dl, ImGui::GetFont(), Px(kBody), ImVec2((va.x + vb.x - ts.x) * 0.5f, (va.y + vb.y - ts.y) * 0.5f), color,
         text.c_str());
  };
  if (!loadError_.empty()) {
    centered(loadError_, C(p.danger));
    return;
  }
  if (!loaded_) {
    centered("Leyendo la imagen...", C(p.text));
    return;
  }
  RequestRender(w);
  const Layout l = ComputeLayout(va + ImVec2(Px(8), Px(8)), vb - ImVec2(Px(8), Px(8)));
  if (preview_) AddTextureImage(dl, *preview_, l.a, l.b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
  else centered("Preparando la vista previa...", C(p.textMuted));
  ImGui::SetCursorScreenPos(va);
  ImGui::InvisibleButton("photo-area", vb - va);
  if (tab_ == 0) CropArea(l);
  else MarkArea(l);
}

void PhotoEditorUi::RequestRender(AppWindow& w) {
  if (!worker_ || !loaded_ || busy_) return;
  const bool crop = tab_ == 0;
  // En Recortar se ve la imagen entera: el recorte, las marcas y el marco no cambian nada.
  PhotoSettings key = s_;
  if (crop) {
    key.cropX = key.cropY = 0;
    key.cropW = key.cropH = 1;
    key.aspect = PhotoAspect::Free;
    key.marks.clear();
    key.border = 0;
    key.sharpen = key.vignette = false;
  }
  key.size = PhotoSize::Same;
  if (askedOnce_ && askedCrop_ == crop && asked_ == key) return;
  const unsigned long long now = GetTickCount64();
  if (now - askedAt_ < 30) { // al arrastrar, como mucho una cada 30 ms
    w.AnimateAt(askedAt_ + 30);
    return;
  }
  Worker::Job job;
  job.kind = Worker::Kind::Render;
  job.serial = serial_;
  job.settings = key;
  job.cropView = crop;
  worker_->Post(std::move(job));
  busy_ = true;
  asked_ = key;
  askedCrop_ = crop;
  askedOnce_ = true;
  askedAt_ = now;
}

// Recortar: el recuadro se mueve desde dentro, cambia de tamaño desde las
// esquinas y, pulsando fuera, se dibuja uno nuevo.
void PhotoEditorUi::CropArea(const Layout& l) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float iw = l.b.x - l.a.x, ih = l.b.y - l.a.y;
  if (iw <= 1 || ih <= 1) return;
  // Fuera del recorte, más oscuro; las líneas de los tercios dentro.
  const ImU32 shade = C(p.bg, 0.62f);
  dl->AddRectFilled(l.a, ImVec2(l.b.x, l.ca.y), shade);
  dl->AddRectFilled(ImVec2(l.a.x, l.cb.y), l.b, shade);
  dl->AddRectFilled(ImVec2(l.a.x, l.ca.y), ImVec2(l.ca.x, l.cb.y), shade);
  dl->AddRectFilled(ImVec2(l.cb.x, l.ca.y), ImVec2(l.b.x, l.cb.y), shade);
  for (int i = 1; i < 3; ++i) {
    const float x = l.ca.x + (l.cb.x - l.ca.x) * float(i) / 3.0f, y = l.ca.y + (l.cb.y - l.ca.y) * float(i) / 3.0f;
    dl->AddLine(ImVec2(x, l.ca.y), ImVec2(x, l.cb.y), IM_COL32(255, 255, 255, 70), Px(1));
    dl->AddLine(ImVec2(l.ca.x, y), ImVec2(l.cb.x, y), IM_COL32(255, 255, 255, 70), Px(1));
  }
  dl->AddRect(l.ca, l.cb, C(p.accent), 0, 0, Px(1));
  const ImVec2 corners[4] = {l.ca, ImVec2(l.cb.x, l.ca.y), ImVec2(l.ca.x, l.cb.y), l.cb};
  for (const ImVec2& c : corners) dl->AddRectFilled(c - ImVec2(Px(5), Px(5)), c + ImVec2(Px(5), Px(5)), C(p.accent));

  int rw = 0, rh = 0;
  PhotoRotatedSize(s_, fullW_, fullH_, &rw, &rh);
  const double ratio = PhotoAspectRatio(s_.aspect, rw, rh);
  // Proporción en fracciones de la imagen (ancho / alto).
  const float fracRatio = ratio > 0 && rw > 0 ? float(ratio * double(rh) / double(rw)) : 0.0f;
  const float minW = std::min(1.0f, 16.0f / float(std::max(1, rw))), minH = std::min(1.0f, 16.0f / float(std::max(1, rh)));
  const ImVec2 mouse = ImGui::GetMousePos();
  const float fx = std::clamp((mouse.x - l.a.x) / iw, 0.0f, 1.0f), fy = std::clamp((mouse.y - l.a.y) / ih, 0.0f, 1.0f);
  const float grip = Px(10);
  int corner = -1;
  for (int i = 0; i < 4; ++i)
    if (std::fabs(mouse.x - corners[i].x) <= grip && std::fabs(mouse.y - corners[i].y) <= grip) corner = i;
  const bool inside = mouse.x >= l.ca.x && mouse.x <= l.cb.x && mouse.y >= l.ca.y && mouse.y <= l.cb.y;

  if (ImGui::IsItemActivated()) {
    dragFrom_ = mouse;
    dragStart_ = s_;
    pressX_ = fx;
    pressY_ = fy;
    if (corner >= 0) drag_ = 2 + corner;
    else if (inside) drag_ = 1;
    else drag_ = 6; // uno nuevo desde aquí (un clic sin arrastrar no cambia nada)
  }
  if (ImGui::IsItemActive() && drag_ != 0) {
    const float dx = (mouse.x - dragFrom_.x) / iw, dy = (mouse.y - dragFrom_.y) / ih;
    const PhotoSettings& o = dragStart_;
    if (drag_ == 1) {
      s_.cropX = std::clamp(o.cropX + dx, 0.0f, 1.0f - o.cropW);
      s_.cropY = std::clamp(o.cropY + dy, 0.0f, 1.0f - o.cropH);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (drag_ == 6) {
      const bool moved = std::fabs(mouse.x - dragFrom_.x) > Px(4) || std::fabs(mouse.y - dragFrom_.y) > Px(4);
      if (moved) {
        const bool left = fx < pressX_, up = fy < pressY_;
        const float maxW = left ? pressX_ : 1.0f - pressX_, maxH = up ? pressY_ : 1.0f - pressY_;
        float w = std::max(std::fabs(fx - pressX_), minW), h = std::max(std::fabs(fy - pressY_), minH);
        if (fracRatio > 0) {
          if (w / fracRatio > h) h = w / fracRatio;
          else w = h * fracRatio;
          const float k = std::min({1.0f, maxW / std::max(1e-4f, w), maxH / std::max(1e-4f, h)});
          w *= k;
          h *= k;
        } else {
          w = std::min(w, maxW);
          h = std::min(h, maxH);
        }
        s_.cropX = left ? pressX_ - w : pressX_;
        s_.cropY = up ? pressY_ - h : pressY_;
        s_.cropW = w;
        s_.cropH = h;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      }
    } else {
      // La esquina contraria se queda quieta.
      const int c = drag_ - 2; // 0 arriba izq., 1 arriba der., 2 abajo izq., 3 abajo der.
      const bool right = c == 1 || c == 3, bottom = c >= 2;
      const float ax = right ? o.cropX : o.cropX + o.cropW, ay = bottom ? o.cropY : o.cropY + o.cropH;
      float w = right ? fx - ax : ax - fx, h = bottom ? fy - ay : ay - fy;
      w = std::max(w, minW);
      h = std::max(h, minH);
      if (fracRatio > 0) {
        // Con proporción: manda el lado que más se ha movido.
        if (w / fracRatio > h) h = w / fracRatio;
        else w = h * fracRatio;
        const float maxW = right ? 1.0f - ax : ax, maxH = bottom ? 1.0f - ay : ay;
        const float k = std::min({1.0f, maxW / std::max(1e-4f, w), maxH / std::max(1e-4f, h)});
        w *= k;
        h *= k;
      } else {
        w = std::min(w, right ? 1.0f - ax : ax);
        h = std::min(h, bottom ? 1.0f - ay : ay);
      }
      s_.cropX = right ? ax : ax - w;
      s_.cropY = bottom ? ay : ay - h;
      s_.cropW = w;
      s_.cropH = h;
      ImGui::SetMouseCursor(c == 0 || c == 3 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    }
  }
  if (ImGui::IsItemDeactivated()) drag_ = 0;
  if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
    if (corner == 0 || corner == 3) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (corner >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    else if (inside) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  }
}

// Marcar: pulsar una marca la elige y la mueve; desde su punta o su esquina
// cambia; en un sitio vacío, arrastrar dibuja una nueva con la herramienta
// elegida (el texto se pone con un clic).
void PhotoEditorUi::MarkArea(const Layout& l) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cw = l.cb.x - l.ca.x, ch = l.cb.y - l.ca.y;
  if (cw <= 1 || ch <= 1) return;
  const bool marking = tab_ == 2;
  const ImVec2 mouse = ImGui::GetMousePos();
  const float fx = std::clamp((mouse.x - l.ca.x) / cw, 0.0f, 1.0f), fy = std::clamp((mouse.y - l.ca.y) / ch, 0.0f, 1.0f);
  auto at = [&](float x, float y) { return Lerp(l.ca, l.cb, x, y); };
  auto boxOf = [&](size_t i, ImVec2* a, ImVec2* b) {
    const PhotoMark& m = s_.marks[i];
    if (m.kind == PhotoMarkKind::Text) {
      if (i < boxes_.size() && boxes_[i][2] > boxes_[i][0]) {
        *a = at(boxes_[i][0], boxes_[i][1]);
        *b = at(boxes_[i][2], boxes_[i][3]);
      } else {
        // Aún sin dibujar: un recuadro aproximado alrededor del centro.
        const ImVec2 c = at(m.x0, m.y0);
        *a = c - ImVec2(Px(40), Px(12));
        *b = c + ImVec2(Px(40), Px(12));
      }
    } else {
      *a = at(std::min(m.x0, m.x1), std::min(m.y0, m.y1));
      *b = at(std::max(m.x0, m.x1), std::max(m.y0, m.y1));
    }
  };
  const float grip = Px(9);
  // Qué hay debajo del ratón: la marca de más arriba y si es su punta (12) o su origen (13).
  int hit = -1, handle = 0;
  if (marking) {
    if (selected_ >= 0) {
      const PhotoMark& m = s_.marks[size_t(selected_)];
      if (m.kind == PhotoMarkKind::Arrow) {
        if (Dist(mouse, at(m.x1, m.y1)) <= grip) handle = 12;
        else if (Dist(mouse, at(m.x0, m.y0)) <= grip) handle = 13;
      } else if (Boxy(m.kind)) {
        ImVec2 a, b;
        boxOf(size_t(selected_), &a, &b);
        if (Dist(mouse, b) <= grip) handle = 12;
      }
      if (handle) hit = selected_;
    }
    for (int i = int(s_.marks.size()) - 1; i >= 0 && hit < 0; --i) {
      const PhotoMark& m = s_.marks[size_t(i)];
      if (m.kind == PhotoMarkKind::Arrow) {
        if (SegmentDist(mouse, at(m.x0, m.y0), at(m.x1, m.y1)) <= std::max(Px(7), grip * 0.8f)) hit = i;
      } else {
        ImVec2 a, b;
        boxOf(size_t(i), &a, &b);
        const float pad = Px(5);
        const bool over = mouse.x >= a.x - pad && mouse.x <= b.x + pad && mouse.y >= a.y - pad && mouse.y <= b.y + pad;
        // Un recuadro es hueco: se coge por el borde, y dentro se puede dibujar otra marca.
        const bool hollow = m.kind == PhotoMarkKind::Box && b.x - a.x > pad * 4 && b.y - a.y > pad * 4 &&
                            mouse.x > a.x + pad && mouse.x < b.x - pad && mouse.y > a.y + pad && mouse.y < b.y - pad;
        if (over && !hollow) hit = i;
      }
    }
  }

  if (marking && ImGui::IsItemActivated()) {
    dragFrom_ = mouse;
    dragStart_ = s_;
    pressX_ = fx;
    pressY_ = fy;
    if (hit >= 0) {
      selected_ = hit;
      drag_ = handle ? handle : 11;
    } else if (tool_ == PhotoMarkKind::Text) {
      PhotoMark& m = NewMark(PhotoMarkKind::Text);
      m.x0 = m.x1 = fx;
      m.y0 = m.y1 = fy;
      dragStart_ = s_;
      drag_ = 11;
    } else {
      selected_ = -1;
      drag_ = 10; // se crea al moverse: un clic en vacío solo quita la selección
    }
  }
  if (marking && ImGui::IsItemActive() && drag_ != 0) {
    const bool moved = std::fabs(mouse.x - dragFrom_.x) > Px(4) || std::fabs(mouse.y - dragFrom_.y) > Px(4);
    if (drag_ == 10 && moved) {
      PhotoMark& m = NewMark(tool_);
      m.x0 = pressX_;
      m.y0 = pressY_;
      dragStart_ = s_;
      drag_ = 12;
    }
    if (selected_ >= 0 && selected_ < int(s_.marks.size()) && selected_ < int(dragStart_.marks.size())) {
      PhotoMark& m = s_.marks[size_t(selected_)];
      const PhotoMark& o = dragStart_.marks[size_t(selected_)];
      if (drag_ == 11 && moved) {
        // Mover sin salirse de la imagen.
        float dx = (mouse.x - dragFrom_.x) / cw, dy = (mouse.y - dragFrom_.y) / ch;
        dx = std::clamp(dx, -std::min(o.x0, o.x1), 1.0f - std::max(o.x0, o.x1));
        dy = std::clamp(dy, -std::min(o.y0, o.y1), 1.0f - std::max(o.y0, o.y1));
        m.x0 = o.x0 + dx;
        m.x1 = o.x1 + dx;
        m.y0 = o.y0 + dy;
        m.y1 = o.y1 + dy;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      } else if (drag_ == 12) {
        m.x1 = fx;
        m.y1 = fy;
        ImGui::SetMouseCursor(m.kind == PhotoMarkKind::Arrow ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeNWSE);
      } else if (drag_ == 13) {
        m.x0 = fx;
        m.y0 = fy;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
    }
  }
  if (ImGui::IsItemDeactivated()) {
    // Lo que salió demasiado pequeño al dibujarlo no vale.
    if (selected_ >= 0 && selected_ < int(s_.marks.size()) && drag_ == 12 &&
        selected_ >= int(committed_.marks.size())) {
      const PhotoMark& m = s_.marks[size_t(selected_)];
      if (std::fabs(m.x1 - m.x0) * cw < Px(6) && std::fabs(m.y1 - m.y0) * ch < Px(6)) {
        s_.marks.erase(s_.marks.begin() + selected_);
        selected_ = -1;
      }
    }
    drag_ = 0;
  }
  if (marking && ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
    if (handle == 12 && selected_ >= 0 && Boxy(s_.marks[size_t(selected_)].kind))
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (handle || hit >= 0) ImGui::SetMouseCursor(handle ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeAll);
    else ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
  }

  // Contornos: la elegida (con sus tiradores) y la de debajo del ratón.
  if (!marking) return;
  for (int i = 0; i < int(s_.marks.size()); ++i) {
    const bool sel = i == selected_;
    if (!sel && i != hit) continue;
    const PhotoMark& m = s_.marks[size_t(i)];
    const ImU32 col = C(p.accent, sel ? 1.0f : 0.6f);
    if (m.kind == PhotoMarkKind::Arrow) {
      const ImVec2 a = at(m.x0, m.y0), b = at(m.x1, m.y1);
      if (sel) {
        dl->AddRect(a - ImVec2(Px(5), Px(5)), a + ImVec2(Px(5), Px(5)), col, 0, 0, Px(1));
        dl->AddRectFilled(b - ImVec2(Px(5), Px(5)), b + ImVec2(Px(5), Px(5)), col);
      } else {
        dl->AddLine(a, b, col, Px(1));
      }
    } else {
      ImVec2 a, b;
      boxOf(size_t(i), &a, &b);
      dl->AddRect(a - ImVec2(Px(2), Px(2)), b + ImVec2(Px(2), Px(2)), col, 0, 0, Px(1));
      if (sel && Boxy(m.kind)) dl->AddRectFilled(b - ImVec2(Px(5), Px(5)), b + ImVec2(Px(5), Px(5)), col);
    }
  }
}

// ------------------------------------------------------------ herramientas

void PhotoEditorUi::Tools(const AppModel& m, float height) {
  const float w = Px(kToolsWidth);
  ImGui::BeginGroup();
  static const std::vector<std::pair<int, std::string>> kTabs = {
      {0, "Recortar"}, {1, "Color"}, {2, "Marcar"}, {3, "Marco"}};
  const int tab = Segmented("photo-tabs", kTabs, tab_, w);
  if (tab >= 0 && tab != tab_) {
    tab_ = tab;
    drag_ = 0;
  }
  Gap(10);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(6));
  ImGui::BeginChild("photo-tools", ImVec2(w, std::max(Px(100), height - Px(42))), ImGuiChildFlags_None,
                    ImGuiWindowFlags_None);
  ImGui::BeginDisabled(!loaded_);
  switch (tab_) {
  case 0: TabCrop(); break;
  case 1: TabColor(); break;
  case 2: TabMarks(m); break;
  default: TabFrame(m); break;
  }
  Gap(16);
  ImGui::EndDisabled();
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::EndGroup();
}

void PhotoEditorUi::TabCrop() {
  ToolHeader("Girar");
  // Lo que se ve gira; con un volteo (solo uno), al revés por dentro.
  const int turn = TwoButtons("rot-left", "A la izquierda", "rot-right", "A la derecha");
  if (turn) {
    const bool clockwise = turn == 2;
    const int step = (clockwise == (s_.flipH == s_.flipV)) ? 90 : 270;
    s_.rotate = (((s_.rotate + step) % 360) + 360) % 360;
    const PhotoSettings o = s_;
    // El recorte y las marcas siguen pegados a lo mismo de la imagen.
    if (clockwise) {
      s_.cropX = 1.0f - o.cropY - o.cropH;
      s_.cropY = o.cropX;
    } else {
      s_.cropX = o.cropY;
      s_.cropY = 1.0f - o.cropX - o.cropW;
    }
    s_.cropW = o.cropH;
    s_.cropH = o.cropW;
    for (auto& mk : s_.marks) {
      auto turnPoint = [&](float& x, float& y) {
        const float px = x, py = y;
        x = clockwise ? 1.0f - py : py;
        y = clockwise ? px : 1.0f - px;
      };
      turnPoint(mk.x0, mk.y0);
      turnPoint(mk.x1, mk.y1);
    }
    // El recorte girado tiene la proporción al revés: 16:9 pasa a 9:16.
    if (s_.aspect == PhotoAspect::Wide) s_.aspect = PhotoAspect::Vertical;
    else if (s_.aspect == PhotoAspect::Vertical) s_.aspect = PhotoAspect::Wide;
    else if (s_.aspect == PhotoAspect::Classic || s_.aspect == PhotoAspect::Portrait) s_.aspect = PhotoAspect::Free;
  }
  Gap(6);
  const int flip = TwoButtons("flip-h", "Voltear en horizontal", "flip-v", "Voltear en vertical");
  if (flip == 1) {
    s_.flipH = !s_.flipH;
    s_.cropX = 1.0f - s_.cropX - s_.cropW;
    for (auto& mk : s_.marks) {
      mk.x0 = 1.0f - mk.x0;
      mk.x1 = 1.0f - mk.x1;
    }
  } else if (flip == 2) {
    s_.flipV = !s_.flipV;
    s_.cropY = 1.0f - s_.cropY - s_.cropH;
    for (auto& mk : s_.marks) {
      mk.y0 = 1.0f - mk.y0;
      mk.y1 = 1.0f - mk.y1;
    }
  }
  Gap(18);

  ToolHeader("Recortar");
  static const std::vector<std::pair<int, std::string>> kAspects = {
      {int(PhotoAspect::Free), "Libre"},  {int(PhotoAspect::Original), "Original"},
      {int(PhotoAspect::Wide), "16:9"},   {int(PhotoAspect::Vertical), "9:16"},
      {int(PhotoAspect::Square), "1:1"},  {int(PhotoAspect::Classic), "4:3"},
      {int(PhotoAspect::Portrait), "4:5"}};
  const int aspect = ChoiceGrid("aspects", kAspects, int(s_.aspect), 4);
  if (aspect >= 0) {
    s_.aspect = PhotoAspect(aspect);
    ApplyPhotoAspect(&s_, fullW_, fullH_);
  }
  Gap(8);
  {
    const PhotoRect r = PhotoCrop(s_, fullW_, fullH_);
    char buf[96];
    snprintf(buf, sizeof(buf), "%d x %d px. Arrastra las esquinas, mueve el recuadro o dibuja otro fuera de él.", r.w,
             r.h);
    Note(buf);
  }
  const bool cropped = s_.cropX > 0 || s_.cropY > 0 || s_.cropW < 1 || s_.cropH < 1;
  if (cropped) {
    Gap(8);
    if (GhostButton("crop-reset", "Quitar el recorte", ImGui::GetContentRegionAvail().x, Tone::Normal, 28)) {
      s_.cropX = s_.cropY = 0;
      s_.cropW = s_.cropH = 1;
      s_.aspect = PhotoAspect::Free;
    }
  }
}

void PhotoEditorUi::TabColor() {
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
  const bool any =
      s_.look != EditLook::None || s_.brightness || s_.contrast || s_.saturation || s_.sharpen || s_.vignette;
  if (any) {
    Gap(8);
    if (GhostButton("color-reset", "Quitar los cambios de color", ImGui::GetContentRegionAvail().x, Tone::Normal, 28)) {
      s_.look = EditLook::None;
      s_.brightness = s_.contrast = s_.saturation = 0;
      s_.sharpen = s_.vignette = false;
    }
  }
}

void PhotoEditorUi::TabMarks(const AppModel& m) {
  (void)m;
  PhotoMark* sel = selected_ >= 0 ? &s_.marks[size_t(selected_)] : nullptr;
  ToolHeader("Herramienta");
  static const std::vector<std::pair<int, std::string>> kTools = {
      {int(PhotoMarkKind::Arrow), "Flecha"},   {int(PhotoMarkKind::Box), "Recuadro"},
      {int(PhotoMarkKind::Text), "Texto"},     {int(PhotoMarkKind::Blur), "Difuminar"},
      {int(PhotoMarkKind::Pixelate), "Pixelar"}};
  const int tool = ChoiceGrid("tools", kTools, int(tool_), 3);
  if (tool >= 0) {
    tool_ = PhotoMarkKind(tool);
    selected_ = -1;
    sel = nullptr;
  }
  Gap(6);
  const PhotoMarkKind kind = sel ? sel->kind : tool_;
  Note(kind == PhotoMarkKind::Text ? "Escribe el texto y haz clic donde va. Luego se arrastra para moverlo."
       : kind == PhotoMarkKind::Arrow ? "Arrastra sobre la imagen desde donde sale hasta donde apunta."
       : kind == PhotoMarkKind::Box   ? "Arrastra sobre la imagen para rodear algo."
                                      : "Arrastra sobre la imagen para tapar un nombre, el chat o una notificación.");
  Gap(14);

  if (kind == PhotoMarkKind::Text) {
    ToolHeader(sel ? "Este texto" : "Texto");
    ImGui::SetNextItemWidth(-1);
    std::string& text = sel ? sel->text : text_;
    ImGui::InputTextWithHint("##mark-text", "Escribe aquí", &text);
    if (text.size() > 200) text.resize(200);
    Gap(8);
    Label("Tamaño");
  } else if (kind == PhotoMarkKind::Arrow || kind == PhotoMarkKind::Box) {
    ToolHeader(sel ? (kind == PhotoMarkKind::Arrow ? "Esta flecha" : "Este recuadro") : "Trazo");
    Label("Grosor");
  }
  if (kind == PhotoMarkKind::Text || kind == PhotoMarkKind::Arrow || kind == PhotoMarkKind::Box) {
    static const std::vector<std::pair<int, std::string>> kStroke = {{0, "Fino"}, {1, "Medio"}, {2, "Grueso"}};
    static const std::vector<std::pair<int, std::string>> kText = {{0, "Pequeño"}, {1, "Mediano"}, {2, "Grande"}};
    const int size = Segmented("mark-size", kind == PhotoMarkKind::Text ? kText : kStroke, sel ? sel->size : size_);
    if (size >= 0) {
      size_ = size;
      if (sel) sel->size = size;
    }
    Gap(8);
    Label("Color");
    const int64_t color = Swatches("mark-color", sel ? sel->color : color_);
    if (color >= 0) {
      color_ = uint32_t(color);
      if (sel) sel->color = uint32_t(color);
    }
    if (kind == PhotoMarkKind::Text) {
      Gap(6);
      bool box = sel ? sel->box : false;
      if (SwitchRow("Con fondo oscuro detrás", &box) && sel) sel->box = box;
    }
    Gap(14);
  }

  if (sel) {
    if (GhostButton("mark-remove", "Quitar esta marca (Supr)", ImGui::GetContentRegionAvail().x, Tone::Danger, 28)) {
      s_.marks.erase(s_.marks.begin() + selected_);
      selected_ = -1;
      sel = nullptr;
    }
    Gap(6);
  }
  if (!s_.marks.empty()) {
    char buf[64];
    snprintf(buf, sizeof(buf), s_.marks.size() == 1 ? "Quitar la marca" : "Quitar las %zu marcas", s_.marks.size());
    if (GhostButton("marks-clear", buf, ImGui::GetContentRegionAvail().x, Tone::Normal, 28)) {
      s_.marks.clear();
      selected_ = -1;
    }
  }
}

void PhotoEditorUi::TabFrame(const AppModel& m) {
  (void)m;
  ToolHeader("Marco");
  Note("Un borde liso alrededor de la imagen.");
  Gap(8);
  static const std::vector<std::pair<int, std::string>> kBorders = {{0, "Sin marco"}, {1, "Fino"}, {2, "Grueso"}};
  const int border = Segmented("border", kBorders, s_.border);
  if (border >= 0) s_.border = border;
  if (s_.border > 0) {
    Gap(10);
    Label("Color");
    const int64_t color = Swatches("border-color", s_.borderColor);
    if (color >= 0) s_.borderColor = uint32_t(color);
  }
}

// ------------------------------------------------------------ guardar

void PhotoEditorUi::Save(AppModel& m, const AppActions& a) {
  if (!loaded_ || !m.photoSavePath.empty() || !a.savePhoto) return;
  m.photoSavePath = path_; // hasta que App diga que acabó
  a.savePhoto(path_, s_, target_);
}

void PhotoEditorUi::Footer(AppModel& m, const AppActions& a, Exit* exit) {
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
  static const std::vector<std::pair<int, std::string>> kTargets = {{0, "PNG"}, {1, "JPG"}, {2, "Para Discord"}};
  const int target = Segmented("target", kTargets, target_, Px(260));
  if (target >= 0) target_ = target;
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("PNG: sin perder nada. JPG: ocupa mucho menos. Para Discord: JPG de como mucho 1920 px, "
                      "copiado para pegarlo con Ctrl+V.");
  x += Px(260) + Px(18);
  label("Tamaño");
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  static const std::vector<std::pair<int, std::string>> kSizes = {{int(PhotoSize::Same), "Igual"},
                                                                  {int(PhotoSize::Large), "1920"},
                                                                  {int(PhotoSize::Medium), "1280"},
                                                                  {int(PhotoSize::Half), "La mitad"}};
  const int size = Segmented("size", kSizes, int(s_.size), Px(270));
  if (size >= 0) s_.size = PhotoSize(size);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("1920 y 1280: el lado largo como mucho de esos píxeles (nunca se agranda).");
  x += Px(270) + Px(18);

  const float right = q.x + wAvail - Px(28);
  const bool busy = !m.photoSavePath.empty();
  const float bw = Px(150);
  ImGui::SetCursorScreenPos(ImVec2(right - bw, y));
  if (busy) {
    const char* text = "Guardando...";
    const ImVec2 ts = TextSize(ImGui::GetFont(), fs, text);
    Text(dl, ImGui::GetFont(), fs, ImVec2(right - ts.x, q.y + (footerH - fs) * 0.5f), C(p.text), text);
  } else {
    const bool can = loaded_ && (PhotoTouches(s_) || target_ != 0);
    ImGui::BeginDisabled(!can);
    if (SolidButton("save", target_ == 2 ? "Guardar y copiar" : "Guardar copia", bw) && can) Save(m, a);
    ImGui::EndDisabled();
    if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", loaded_ ? "Aún no hay nada que cambiar" : "Leyendo la imagen...");
    // La última copia de esta imagen, para verla.
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

void PhotoEditorUi::Keyboard(AppModel& m, const AppActions& a, Exit* exit) {
  if (TypingText() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) ||
      !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    return;
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (selected_ >= 0) selected_ = -1; // primero se suelta la marca elegida
    else exit->back = true;
  }
  if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) &&
      selected_ >= 0 && selected_ < int(s_.marks.size()) && drag_ == 0) {
    s_.marks.erase(s_.marks.begin() + selected_);
    selected_ = -1;
  }
  if (!io.KeyCtrl) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Z, true)) {
    if (io.KeyShift) Redo();
    else Undo();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Y, true)) Redo();
  if (ImGui::IsKeyPressed(ImGuiKey_S, false) && loaded_ && (PhotoTouches(s_) || target_ != 0)) Save(m, a);
}

} // namespace spectre
