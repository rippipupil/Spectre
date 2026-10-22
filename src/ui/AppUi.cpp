#include "ui/AppUi.h"

#include "common/Util.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>

#ifndef SPECTRE_VERSION
#define SPECTRE_VERSION "0.1.0"
#endif

namespace spectre {

using namespace ui;

namespace {

constexpr float kSidebarWidth = 224.0f;
constexpr float kFooterHeight = 60.0f;
// Las miniaturas se piden al ancho real de la tarjeta, redondeado hacia arriba
// a un múltiplo de 128 px: con la ventana maximizada una miniatura pequeña se
// estiraba y se veía pixelada.
constexpr int kThumbMin = 256, kThumbMax = 1280, kThumbStep = 128;
constexpr size_t kMaxThumbs = 180;                 // texturas de miniatura vivas
constexpr size_t kThumbBudget = 64ull << 20;       // y como mucho 64 MB de VRAM entre todas
constexpr const char* kTrimHelp = "Se guarda como un clip nuevo y el original no cambia. Como no se recodifica, "
                                  "empieza en el fotograma clave anterior al inicio elegido: hasta 2 s antes.";

std::string Lower(std::string s) {
  for (auto& c : s) c = char(tolower((unsigned char)c));
  return s;
}

std::string FormatPrecise(double seconds) {
  seconds = std::max(0.0, seconds);
  int whole = int(seconds);
  int tenths = int((seconds - double(whole)) * 10.0);
  char buf[32];
  if (whole >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d.%d", whole / 3600, (whole / 60) % 60, whole % 60, tenths);
  else snprintf(buf, sizeof(buf), "%02d:%02d.%d", whole / 60, whole % 60, tenths);
  return buf;
}

std::string Title(const MediaItem& item) {
  if (!item.spectre) return item.stem;
  return item.game.empty() ? std::string("Escritorio") : item.game;
}

std::string KindUpper(MediaKind kind) {
  switch (kind) {
  case MediaKind::Clip: return "CLIP";
  case MediaKind::Recording: return "GRABACIÓN";
  case MediaKind::Trim: return "RECORTE";
  case MediaKind::Export: return "DISCORD";
  case MediaKind::Edit: return "EDITADO";
  case MediaKind::Screenshot: return "CAPTURA";
  }
  return "";
}

bool IsGif(const MediaItem& item) {
  return item.name.size() > 4 && Lower(item.name.substr(item.name.size() - 4)) == ".gif";
}

// Etiqueta de la tarjeta: el tipo, o GIF y AUDIO para lo que sale del editor.
std::string BadgeText(const MediaItem& item) {
  if (IsGif(item)) return "GIF";
  if (item.audioOnly) return "AUDIO";
  return KindUpper(item.kind);
}

std::string KindName(const MediaItem& item) {
  if (IsGif(item)) return "GIF";
  if (item.audioOnly) return "Audio";
  return MediaKindName(item.kind);
}

// Se puede abrir en «Editar»: vídeo con imagen.
bool Editable(const MediaItem& item) { return item.Video() && !item.audioOnly; }

void PageTitle(const char* title, const std::string& subtitle) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q = ImGui::GetCursorScreenPos();
  float ts = Px(kTitle);
  Text(dl, Big(), ts, q, C(Colors().text), title);
  float h = ts + Px(4);
  if (!subtitle.empty()) {
    Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(q.x, q.y + ts + Px(6)), C(Colors().textMuted), subtitle.c_str());
    h += Px(kSmall) + Px(6);
  }
  ImGui::Dummy(ImVec2(0, h));
  Gap(16);
}

// Imagen ajustada al rectángulo; cover=true la recorta para llenarlo.
void DrawImage(ImDrawList* dl, const UiTexture& tex, ImVec2 a, ImVec2 b, bool cover, float alpha = 1.0f) {
  float aw = b.x - a.x, ah = b.y - a.y;
  if (aw <= 0 || ah <= 0 || tex.width <= 0 || tex.height <= 0) return;
  float tw = float(tex.width), th = float(tex.height);
  ImU32 tint = IM_COL32(255, 255, 255, int(255 * std::clamp(alpha, 0.0f, 1.0f)));
  if (cover) {
    float k = std::max(aw / tw, ah / th);
    float uw = aw / (tw * k), uh = ah / (th * k);
    ImVec2 uv0((1.0f - uw) * 0.5f, (1.0f - uh) * 0.5f), uv1(uv0.x + uw, uv0.y + uh);
    AddTextureImage(dl, tex, a, b, uv0, uv1, tint);
  } else {
    float k = std::min(aw / tw, ah / th);
    float w = tw * k, h = th * k;
    ImVec2 p0(a.x + (aw - w) * 0.5f, a.y + (ah - h) * 0.5f);
    AddTextureImage(dl, tex, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
  }
}

// Etiqueta pequeña sobre la miniatura (tipo de archivo, duración).
void Badge(ImDrawList* dl, ImVec2 pos, const std::string& text, bool alignRight) {
  const Palette& p = Colors();
  float fs = Px(kLabel);
  ImVec2 ts = TextSize(Bold(), fs, text.c_str());
  ImVec2 pad(Px(6), Px(3));
  ImVec2 a = alignRight ? ImVec2(pos.x - ts.x - pad.x * 2, pos.y) : pos;
  ImVec2 b(a.x + ts.x + pad.x * 2, a.y + ts.y + pad.y * 2);
  dl->AddRectFilled(a, b, C(p.bg, 0.86f));
  Text(dl, Bold(), fs, a + pad, C(p.text), text.c_str());
}

bool LinkButton(const char* id, const char* label) {
  float ls = Px(kHint);
  ImVec2 ts = TextSize(ImGui::GetFont(), ls, label);
  ImVec2 q = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton(id, ImVec2(ts.x, ts.y + Px(4)));
  bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Text(dl, ImGui::GetFont(), ls, q, C(hov ? Colors().text : Colors().textMuted), label);
  if (hov) dl->AddRectFilled(ImVec2(q.x, q.y + ts.y + Px(1)), ImVec2(q.x + ts.x, q.y + ts.y + Px(2)), C(Colors().text));
  return clicked;
}

// Botón pequeño con borde (marcas del visor).
bool Chip(const char* id, const std::string& label, bool active) {
  const Palette& p = Colors();
  float fs = Px(kSmall);
  ImVec2 ts = TextSize(ImGui::GetFont(), fs, label.c_str());
  ImVec2 size(ts.x + Px(18), Px(26));
  bool pressed = ImGui::InvisibleButton(id, size);
  bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov || active) dl->AddRectFilled(a, b, C(active ? p.surfaceActive : p.surface));
  dl->AddRect(a, b, C(active ? p.accent : hov ? p.lineStrong : p.line), 0, 0, Px(1));
  DrawMark(dl, ImVec2(a.x + Px(9), (a.y + b.y) * 0.5f), Px(2.5f), C(p.accent), true);
  Text(dl, ImGui::GetFont(), fs, ImVec2(a.x + Px(15), (a.y + b.y - ts.y) * 0.5f), C(p.text), label.c_str());
  return pressed;
}

bool TypingText() { return ImGui::GetIO().WantTextInput; }

} // namespace

// ------------------------------------------------------------------ ciclo de vida

void AppUi::OnOpened(HWND notifyWindow, UINT notifyMsg) {
  worker_.Start(notifyWindow, notifyMsg);
  editor_.Start(notifyWindow, notifyMsg);
  frame_ = 0;
  openedAt_ = GetTickCount64();
}

void AppUi::OnClosed() {
  worker_.Stop(false); // una miniatura lenta (carpeta de red) no retrasa el cierre
  thumbs_.clear();
  durationsAsked_.clear();
  durationFailed_.clear();
  player_.Close();
  editor_.Stop();
  playerPath_.clear();
  playerFailed_ = false;
  image_.reset();
  imagePath_.clear();
  imagePending_ = imageFailed_ = false;
  scrubbing_ = false;
  trimMode_ = false;
  menuPath_.clear();
  openRename_ = openDelete_ = false;
}

void AppUi::PauseMedia() {
  if (player_.Playing()) player_.Pause();
  editor_.Pause();
}

void AppUi::LeaveViewer() {
  player_.Close();
  playerPath_.clear();
  playerFailed_ = false;
  image_.reset();
  imagePath_.clear();
  imagePending_ = imageFailed_ = false;
  scrubbing_ = false;
  trimMode_ = false;
  dragHandle_ = 0;
}

void AppUi::Navigate(AppModel& m, AppPage page) {
  if (m.page == AppPage::Viewer && page != AppPage::Viewer) LeaveViewer();
  if (m.page == AppPage::Editor && page != AppPage::Editor) editor_.Leave();
  m.page = page;
}

void AppUi::Edit(AppModel& m, const std::string& path) {
  if (m.page == AppPage::Viewer) LeaveViewer();
  else if (m.page != AppPage::Editor) m.viewBack = m.page; // Volver: al visor y de ahí, aquí
  m.viewPath = path;
  m.editPath = path;
  m.page = AppPage::Editor;
}

void AppUi::View(AppModel& m, const std::string& path, AppPage back) {
  if (m.page == AppPage::Viewer && m.viewPath != path) LeaveViewer();
  m.viewPath = path;
  m.viewBack = back == AppPage::Viewer ? AppPage::Library : back;
  m.page = AppPage::Viewer;
}

const MediaItem* AppUi::Find(const AppModel& m, const std::string& path) const {
  for (const auto& item : m.items)
    if (item.path == path) return &item;
  return nullptr;
}

void AppUi::SaveCurrentFrame(const MediaItem& item, const AppActions& a) {
  std::vector<uint8_t> bgr;
  int width = 0, height = 0;
  // Lo que se ve: si se está reproduciendo, se pausa para que el fotograma
  // guardado sea el de la pantalla.
  if (player_.Playing()) player_.Pause();
  if (!player_.ReadFrame(&bgr, &width, &height)) bgr.clear();
  if (a.saveFrame) a.saveFrame(item, std::move(bgr), width, height);
}

bool AppUi::Selected(const std::string& path) const {
  return std::find(selected_.begin(), selected_.end(), path) != selected_.end();
}

void AppUi::ToggleSelected(const std::string& path) {
  auto it = std::find(selected_.begin(), selected_.end(), path);
  if (it != selected_.end()) selected_.erase(it);
  else selected_.push_back(path);
}

void AppUi::TakeMediaResults(AppWindow& w, AppModel& m, const AppActions& a) {
  editor_.TakeResults(w);
  for (auto& r : worker_.Take()) {
    switch (r.job) {
    case MediaWorker::Job::Thumbnail: {
      Thumb& t = thumbs_[r.path];
      t.pending = false;
      // Si falla la versión grande se queda la pequeña que ya había.
      if (auto tex = r.ok ? w.CreateTexture(r.bgra.data(), r.width, r.height) : nullptr) t.texture = tex;
      t.failed = !t.texture;
      break;
    }
    case MediaWorker::Job::Duration:
      if (!r.ok) {
        durationFailed_.insert(r.path);
        break;
      }
      for (auto& item : m.items)
        if (item.path == r.path) {
          item.duration = r.duration;
          if (a.setDuration) a.setDuration(item, r.duration);
        }
      break;
    case MediaWorker::Job::Image:
      if (r.path != imagePath_) break;
      imagePending_ = false;
      imageFailed_ = !r.ok;
      if (r.ok) image_ = w.CreateTexture(r.bgra.data(), r.width, r.height, true);
      imageFailed_ = !image_;
      break;
    }
  }
  w.Invalidate();
}

UiTexture* AppUi::ThumbFor(const MediaItem& item, float width) {
  Thumb& t = thumbs_[item.path];
  if (t.modified != item.modified) t = Thumb{nullptr, false, false, 0, item.modified, 0};
  t.lastUse = frame_;
  int want = std::clamp((int(std::ceil(width)) + kThumbStep - 1) / kThumbStep * kThumbStep, kThumbMin, kThumbMax);
  // Una más grande solo si la que hay (o la que viene) se quedaría corta; la
  // pequeña se sigue viendo hasta que llega. Se compara con lo pedido, no con
  // lo recibido: Windows puede devolver menos si el vídeo es más pequeño.
  if (!t.pending && !t.failed && (!t.texture || (width > 0 && want > t.requested))) {
    t.pending = true;
    t.requested = want;
    worker_.Request(MediaWorker::Job::Thumbnail, item.path, want, want * 9 / 16);
  }
  return t.texture.get();
}

void AppUi::RequestDuration(const MediaItem& item) {
  if (!item.Video() || item.duration > 0) return;
  auto it = durationsAsked_.find(item.path);
  if (it != durationsAsked_.end() && it->second == item.modified) return;
  durationsAsked_[item.path] = item.modified;
  durationFailed_.erase(item.path);
  worker_.Request(MediaWorker::Job::Duration, item.path);
}

std::vector<const MediaItem*> AppUi::Filtered(const AppModel& m) const {
  std::vector<const MediaItem*> out;
  std::string needle = Lower(search_);
  for (const auto& item : m.items) {
    switch (filter_) {
    case 1:
      if (item.kind != MediaKind::Clip && item.kind != MediaKind::Trim && item.kind != MediaKind::Export &&
          item.kind != MediaKind::Edit)
        continue;
      break;
    case 2: if (item.kind != MediaKind::Recording) continue; break;
    case 3: if (item.kind != MediaKind::Screenshot) continue; break;
    case 4: if (!item.favorite) continue; break;
    default: break;
    }
    if (!game_.empty() && Title(item) != game_) continue;
    if (!needle.empty() && Lower(item.stem).find(needle) == std::string::npos &&
        Lower(item.game).find(needle) == std::string::npos)
      continue;
    out.push_back(&item);
  }
  switch (sort_) {
  case 1: std::reverse(out.begin(), out.end()); break;
  case 2: std::stable_sort(out.begin(), out.end(), [](auto* x, auto* y) { return x->size > y->size; }); break;
  case 3: std::stable_sort(out.begin(), out.end(), [](auto* x, auto* y) { return x->duration > y->duration; }); break;
  default: break;
  }
  return out;
}

// ------------------------------------------------------------------ marco

void AppUi::Draw(AppWindow& w, AppModel& m, const AppActions& a) {
  ++frame_;
  // Navegación desde fuera (el panel, la bandeja): el visor no se queda sonando.
  if (m.page != AppPage::Viewer && (!playerPath_.empty() || !imagePath_.empty())) LeaveViewer();
  if (m.page != AppPage::Editor && editor_.Active()) editor_.Leave();
  // En Ajustes se ve el tema elegido antes de aplicarlo.
  const Config& look = m.page == AppPage::Settings ? m.settings.config : m.config;
  BeginFrame(Fonts{w.Scale(), w.BoldFont(), w.BigFont(), &w.FontSizes()}, look);
  ResetHoverTrail(openedAt_);
  const Palette& p = Colors();
  w.SetTitleColors(p);
  ImGuiIO& io = ImGui::GetIO();
  // En el visor el teclado es del reproductor (espacio, flechas). La
  // navegación de ImGui usa las mismas teclas: el espacio pulsaría además el
  // botón con el foco y la pausa se anularía.
  if (m.page == AppPage::Viewer || m.page == AppPage::Editor) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
  else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V(p.bg));
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##app", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(3);

  Sidebar(w, m, a);
  ImGui::SameLine(0, 0);
  ImGui::BeginGroup();
  switch (m.page) {
  case AppPage::Home: PageHome(w, m, a); break;
  case AppPage::Library: PageLibrary(w, m, a); break;
  case AppPage::Viewer: PageViewer(w, m, a); break;
  case AppPage::Editor: PageEditor(w, m, a); break;
  case AppPage::Settings: PageSettings(w, m, a); break;
  }
  ImGui::EndGroup();
  Dialogs(m, a);
  ImGui::End();
  ImGui::PopStyleColor();
  if (!starToggle_.empty()) {
    if (const MediaItem* item = Find(m, starToggle_)) a.setFavorite(item->path, !item->favorite);
    starToggle_.clear();
  }
  if (!selectToggle_.empty()) {
    ToggleSelected(selectToggle_);
    selectToggle_.clear();
  }

  // Animaciones del tema solo con la ventana delante y a 30 fps: detrás de un
  // juego o en otra ventana no se dibuja nada que no haya cambiado.
  unsigned long long now = GetTickCount64();
  if (ThemeMotion() && w.Focused()) w.AnimateAt(now + 33);
  // El reloj de la grabación, no detrás de un juego (ahí no lo ve nadie).
  if (m.stats.recording && (w.Focused() || !m.gameDetected)) w.AnimateAt(now + 1000 - now % 1000);
  if (m.page == AppPage::Settings && m.settings.update.state == UpdateStatus::State::Checking) w.AnimateAt(now + 50);
  if (m.page == AppPage::Settings && m.settings.update.state == UpdateStatus::State::Downloading) w.AnimateAt(now + 100);
  if (m.page == AppPage::Settings && m.settingsPage == SettingsPage::Audio && w.Focused()) w.AnimateAt(now + 33);
  if (m.page == AppPage::Settings && m.settingsPage == SettingsPage::Performance) w.AnimateAt(now + 1000);
  if (player_.Playing() || scrubbing_) w.Animate();

  // Miniaturas que hace rato que no se ven: fuera, para no acumular VRAM.
  size_t thumbBytes = 0;
  for (const auto& [path, t] : thumbs_)
    if (t.texture) thumbBytes += size_t(t.texture->width) * size_t(t.texture->height) * 4;
  if (thumbs_.size() > kMaxThumbs || thumbBytes > kThumbBudget) {
    std::vector<std::pair<unsigned long long, std::string>> ages;
    for (const auto& [path, t] : thumbs_)
      if (!t.pending && t.lastUse + 2 < frame_) ages.emplace_back(t.lastUse, path);
    std::sort(ages.begin(), ages.end());
    for (size_t i = 0; i < ages.size() && (thumbs_.size() > kMaxThumbs || thumbBytes > kThumbBudget); ++i) {
      auto it = thumbs_.find(ages[i].second);
      if (it->second.texture)
        thumbBytes -= size_t(it->second.texture->width) * size_t(it->second.texture->height) * 4;
      thumbs_.erase(it);
    }
  }
}

void AppUi::Sidebar(AppWindow& w, AppModel& m, const AppActions& a) {
  (void)w;
  (void)a;
  const Palette& p = Colors();
  const float navW = Px(kSidebarWidth);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, V(p.bgAlt));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, Px(20)));
  ImGui::BeginChild("side", ImVec2(navW, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 win = ImGui::GetWindowPos();
  float winH = ImGui::GetWindowHeight();
  dl->AddRectFilled(ImVec2(win.x + navW - Px(1), win.y), ImVec2(win.x + navW, win.y + winH), C(p.line));
  DrawHoverTrail(dl);

  // Marca y estado
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    Wordmark(dl, ImVec2(q.x + Px(20), q.y), "SPECTRE", C(p.text));
    float ss = Px(kSmall);
    float y = q.y + Px(kSmall) + Px(14);
    ImU32 mark;
    bool filled = true;
    float alpha = 1.0f;
    std::string text;
    if (m.stats.recording) {
      mark = p.danger;
      text = "Grabando " + FormatDuration(m.stats.recordSeconds);
    } else if (m.replayActive) {
      mark = p.accent;
      text = "Replay activo";
      alpha = Breathe();
    } else if (m.config.replayEnabled && m.config.onlyInGames) {
      mark = p.textMuted;
      filled = false;
      text = "Esperando a un juego";
    } else {
      mark = p.lineStrong;
      text = "Replay apagado";
    }
    DrawMark(dl, ImVec2(q.x + Px(24), y + ss * 0.5f), Px(3.5f), C(mark, alpha), filled);
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(36), y), C(p.textMuted), text.c_str());
    ImGui::Dummy(ImVec2(0, y + ss - q.y + Px(22)));
  }

  // Filas de 34 px con el hueco normal. Con la ventana baja se juntan (hasta
  // 28 px y sin hueco) para no pisar el pie; si ni así cabe, el pie no se
  // dibuja (la versión y el atajo del panel también están en General).
  const float unit = Px(1);
  const float footerTop = win.y + winH - Px(kHint) * 2 - Px(26);
  const float spacing = ImGui::GetStyle().ItemSpacing.y / unit;
  const float room = (footerTop - Px(14) - ImGui::GetCursorScreenPos().y) / unit - (12 + kLabel + 6);
  const int rows = 2 + int(SettingsPage::Count);
  float pitch = (room + 2 * 34) / float(rows + 2); // filas de 34 y el hueco también en el rótulo
  if (pitch < 34) pitch = room / float(rows);      // sin hueco
  pitch = std::clamp(pitch, 28.0f, 34 + spacing);
  const float rowH = std::min(pitch, 34.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Px(pitch - rowH)));
  if (NavItem(1, "Inicio", m.page == AppPage::Home, rowH)) Navigate(m, AppPage::Home);
  char library[48];
  snprintf(library, sizeof(library), "Biblioteca");
  if (NavItem(2, library, m.page == AppPage::Library || m.page == AppPage::Viewer || m.page == AppPage::Editor, rowH))
    Navigate(m, AppPage::Library);
  NavLabel(m.settings.dirty ? "AJUSTES  (SIN APLICAR)" : "AJUSTES");
  for (int i = 0; i < int(SettingsPage::Count); ++i) {
    auto page = SettingsPage(i);
    if (NavItem(10 + i, SettingsPageName(page), m.page == AppPage::Settings && m.settingsPage == page, rowH)) {
      Navigate(m, AppPage::Settings);
      m.settingsPage = page;
    }
  }
  ImGui::PopStyleVar();

  // Pie: versión y recordatorio del panel
  if (ImGui::GetItemRectMax().y + Px(8) <= footerTop) {
    float ss = Px(kHint);
    std::string hint = (m.config.hotkeys.overlay.empty() ? std::string("El panel") : m.config.hotkeys.overlay) +
                       " abre el panel en el juego";
    ImVec2 q(win.x + Px(20), win.y + winH - ss * 2 - Px(26));
    Text(dl, ImGui::GetFont(), ss, q, C(p.textMuted), Ellipsize(ImGui::GetFont(), ss, hint, navW - Px(32)).c_str());
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x, q.y + ss + Px(6)), C(p.textFaint), "Spectre " SPECTRE_VERSION);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ Inicio

void AppUi::StatusCard(AppModel& m, const AppActions& a) {
  const Palette& p = Colors();
  const Config& c = m.config;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q = ImGui::GetCursorScreenPos();
  float width = ImGui::GetContentRegionAvail().x;
  const float pad = Px(20);
  float height = Px(150);
  dl->AddRectFilled(q, ImVec2(q.x + width, q.y + height), C(p.surface));
  dl->AddRectFilled(q, ImVec2(q.x + Px(3), q.y + height), C(m.stats.recording ? p.danger : p.accent));

  std::string status, detail;
  if (m.stats.recording) {
    status = "Grabando " + FormatDuration(m.stats.recordSeconds);
    detail = "Pulsa " + c.hotkeys.toggleRecording + " para detener y guardar la grabación.";
  } else if (m.replayActive) {
    status = "Replay activo, guarda los últimos " + FormatClipLength(c.clipSeconds);
    detail = "Pulsa " + c.hotkeys.saveClip + " en cualquier momento para guardar el clip.";
  } else if (c.replayEnabled && c.onlyInGames) {
    status = "Replay esperando a un juego";
    detail = "Se activa solo al abrir un juego a pantalla completa (Ajustes > Replay).";
  } else {
    status = "Replay apagado";
    detail = "Actívalo para poder guardar lo que acaba de pasar en cualquier momento.";
  }
  if (m.gameDetected && !m.gameName.empty()) detail += "  Juego: " + m.gameName + ".";
  Text(dl, Bold(), Px(kBody) + Px(2), ImVec2(q.x + pad, q.y + pad), C(p.text), status.c_str());
  Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(q.x + pad, q.y + pad + Px(26)), C(p.textMuted),
       Ellipsize(ImGui::GetFont(), Px(kSmall), detail, width - pad * 2).c_str());

  float gap = Px(10);
  float bw = (width - pad * 2 - gap * 2) / 3.0f;
  ImGui::SetCursorScreenPos(ImVec2(q.x + pad, q.y + height - pad - Px(44)));
  ImGui::PushID("status");
  if (ActionButton("clip", "Guardar clip", c.hotkeys.saveClip, Tone::Primary, m.replayActive, false, bw))
    a.saveClip();
  ImGui::SameLine(0, gap);
  if (m.stats.recording) {
    if (ActionButton("rec", "Detener grabación", c.hotkeys.toggleRecording, Tone::Danger, true, false, bw))
      a.toggleRecording();
  } else if (ActionButton("rec", "Grabar pantalla", c.hotkeys.toggleRecording, Tone::Normal, true, true, bw)) {
    a.toggleRecording();
  }
  ImGui::SameLine(0, gap);
  if (ActionButton("shot", "Captura de pantalla", c.hotkeys.screenshot, Tone::Normal, true, false, bw))
    a.screenshot();
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + height));
  ImGui::Dummy(ImVec2(width, 0));
}

namespace {

// Números de la biblioteca para Inicio. Días y horas en hora local, a partir
// de la fecha de cada archivo.
struct LibraryStats {
  int clips = 0, recordings = 0, screenshots = 0, favorites = 0;
  double seconds = 0;
  int unmeasured = 0;
  uint64_t bytes = 0;
  std::string topGame;
  int topGameCount = 0;
  int64_t bestDay = -1, firstDay = -1;
  int bestDayCount = 0;
  int longestStreak = 0;
  int topHour = -1, topHourCount = 0;
};

constexpr int64_t kTicksPerHour = int64_t(36000000000);
constexpr int64_t kTicksPerDay = kTicksPerHour * 24;

int64_t LocalTicks(int64_t modified) {
  FILETIME utc{DWORD(uint64_t(modified) & 0xFFFFFFFFu), DWORD(uint64_t(modified) >> 32)}, local{};
  if (!FileTimeToLocalFileTime(&utc, &local)) return modified;
  return int64_t((uint64_t(local.dwHighDateTime) << 32) | local.dwLowDateTime);
}

// unmeasurable: vídeos cuya duración no se pudo leer; no cuentan como pendientes.
LibraryStats ComputeStats(const std::vector<MediaItem>& items, const std::unordered_set<std::string>& unmeasurable) {
  LibraryStats st;
  std::unordered_map<std::string, int> games;
  std::unordered_map<int64_t, int> days; // día local -> clips
  int hours[24] = {};
  std::vector<int64_t> activeDays;
  for (const auto& item : items) {
    st.bytes += item.size;
    if (item.favorite) ++st.favorites;
    const int64_t local = LocalTicks(item.modified);
    const int64_t day = local / kTicksPerDay;
    if (st.firstDay < 0 || day < st.firstDay) st.firstDay = day;
    activeDays.push_back(day);
    if (item.kind == MediaKind::Screenshot) {
      ++st.screenshots;
      continue;
    }
    // Recortes, uniones, cámara lenta y copias repiten jugadas que ya cuentan.
    if (item.kind != MediaKind::Clip && item.kind != MediaKind::Recording) continue;
    if (item.duration > 0) st.seconds += item.duration;
    else if (!unmeasurable.count(item.path)) ++st.unmeasured;
    if (item.kind == MediaKind::Recording) {
      ++st.recordings;
      continue;
    }
    ++st.clips;
    ++days[day];
    ++hours[(local / kTicksPerHour) % 24];
    if (item.spectre && !item.game.empty()) ++games[item.game];
  }
  for (const auto& [game, n] : games)
    if (n > st.topGameCount || (n == st.topGameCount && game < st.topGame)) {
      st.topGame = game;
      st.topGameCount = n;
    }
  for (const auto& [day, n] : days)
    if (n > st.bestDayCount || (n == st.bestDayCount && day > st.bestDay)) {
      st.bestDay = day;
      st.bestDayCount = n;
    }
  for (int h = 0; h < 24; ++h)
    if (hours[h] > st.topHourCount) {
      st.topHour = h;
      st.topHourCount = hours[h];
    }
  // Racha: días seguidos en los que se guardó algo.
  std::sort(activeDays.begin(), activeDays.end());
  activeDays.erase(std::unique(activeDays.begin(), activeDays.end()), activeDays.end());
  int run = 0;
  for (size_t i = 0; i < activeDays.size(); ++i) {
    run = i > 0 && activeDays[i] == activeDays[i - 1] + 1 ? run + 1 : 1;
    st.longestStreak = std::max(st.longestStreak, run);
  }
  return st;
}

// «21 oct 2026»
std::string DayText(int64_t day) {
  static const char* months[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
  const int64_t ticks = day * kTicksPerDay;
  FILETIME ft{DWORD(uint64_t(ticks) & 0xFFFFFFFFu), DWORD(uint64_t(ticks) >> 32)};
  SYSTEMTIME t{};
  if (!FileTimeToSystemTime(&ft, &t)) return "";
  char buf[32];
  snprintf(buf, sizeof(buf), "%d %s %d", t.wDay, months[(t.wMonth + 11) % 12], t.wYear);
  return buf;
}

// «45 min», «3 h 05 min»
std::string HoursText(double seconds) {
  const int minutes = int(seconds / 60.0 + 0.5);
  if (minutes < 1) return seconds > 0 ? "menos de 1 min" : "0 min";
  if (minutes < 60) return std::to_string(minutes) + " min";
  char buf[32];
  snprintf(buf, sizeof(buf), "%d h %02d min", minutes / 60, minutes % 60);
  return buf;
}

// Casilla plana con un número grande y una línea debajo.
void StatTile(ImVec2 a, float width, float height, const std::string& label, const std::string& value,
              const std::string& detail) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + width, a.y + height), C(p.surface));
  const float pad = Px(14), inner = width - pad * 2;
  ImFont* font = ImGui::GetFont();
  Text(dl, font, Px(kLabel), ImVec2(a.x + pad, a.y + pad), C(p.textMuted),
       Ellipsize(font, Px(kLabel), label, inner).c_str());
  Text(dl, Bold(), Px(kBody) + Px(2), ImVec2(a.x + pad, a.y + pad + Px(kLabel) + Px(8)), C(p.text),
       Ellipsize(Bold(), Px(kBody) + Px(2), value, inner).c_str());
  Text(dl, font, Px(kSmall), ImVec2(a.x + pad, a.y + height - pad - Px(kSmall)), C(p.textFaint),
       Ellipsize(font, Px(kSmall), detail, inner).c_str());
}

} // namespace

void AppUi::StatsSection(const AppModel& m) {
  SectionLabel("TUS NÚMEROS");
  Gap(6);
  if (m.items.empty()) {
    Help("Cuando guardes clips verás aquí cuánto has grabado, tu juego más clipeado y tu mejor racha.");
    return;
  }
  const LibraryStats st = ComputeStats(m.items, durationFailed_);
  const auto plural = [](int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
  };
  struct Tile {
    std::string label, value, detail;
  };
  std::vector<Tile> tiles;
  tiles.push_back({"CLIPS", std::to_string(st.clips),
                   plural(st.recordings, "grabación", "grabaciones") + " y " + plural(st.screenshots, "captura", "capturas")});
  tiles.push_back({"TIEMPO GUARDADO", HoursText(st.seconds),
                   st.unmeasured > 0 ? "midiendo " + plural(st.unmeasured, "vídeo", "vídeos") + "..."
                                     : "entre clips y grabaciones"});
  tiles.push_back({"JUEGO CON MÁS CLIPS", st.topGame.empty() ? std::string("Ninguno aún") : st.topGame,
                   st.topGame.empty() ? std::string("se ve al clipear un juego") : plural(st.topGameCount, "clip", "clips")});
  tiles.push_back({"MEJOR RACHA", plural(st.longestStreak, "día", "días"), "seguidos guardando algo"});
  tiles.push_back({"DÍA CON MÁS CLIPS", st.bestDay >= 0 ? DayText(st.bestDay) : std::string("Ninguno aún"),
                   st.bestDay >= 0 ? plural(st.bestDayCount, "clip", "clips") : std::string("")});
  char hour[32] = "Sin datos";
  if (st.topHour >= 0) snprintf(hour, sizeof(hour), "%02d:00 a %02d:00", st.topHour, (st.topHour + 1) % 24);
  tiles.push_back({"HORA FAVORITA", hour, st.topHour >= 0 ? "cuando más clips guardas" : ""});
  tiles.push_back({"FAVORITOS", std::to_string(st.favorites), "el límite de espacio no los borra"});
  tiles.push_back({"OCUPA", FormatBytes(st.bytes),
                   st.firstDay >= 0 ? "desde el " + DayText(st.firstDay) : std::string("")});

  const float gap = Px(10);
  const float avail = ImGui::GetContentRegionAvail().x;
  const int cols = avail > Px(760) ? 4 : 2;
  const float tileW = std::floor((avail - gap * float(cols - 1)) / float(cols));
  const float tileH = Px(92);
  ImVec2 q = ImGui::GetCursorScreenPos();
  const int rows = int((tiles.size() + size_t(cols) - 1) / size_t(cols));
  for (size_t i = 0; i < tiles.size(); ++i) {
    const int col = int(i) % cols, row = int(i) / cols;
    ImVec2 a(q.x + float(col) * (tileW + gap), q.y + float(row) * (tileH + gap));
    StatTile(a, tileW, tileH, tiles[i].label, tiles[i].value, tiles[i].detail);
  }
  ImGui::Dummy(ImVec2(avail, float(rows) * tileH + float(rows - 1) * gap));
}

void AppUi::PageHome(AppWindow& w, AppModel& m, const AppActions& a) {
  // Las estadísticas suman la duración de los clips y las grabaciones: las que
  // faltan se piden antes que las miniaturas de las tarjetas, que así van
  // delante en la cola (lo último pedido sale primero). Cada una se mide una
  // vez y se guarda.
  for (const auto& item : m.items)
    if ((item.kind == MediaKind::Clip || item.kind == MediaKind::Recording) && item.duration <= 0)
      RequestDuration(item);
  const Palette& p = Colors();
  const Config& c = m.config;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(32), Px(26)));
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  DrawHoverTrail(ImGui::GetWindowDrawList());
  PageTitle("Inicio", "Todo lo que hace falta durante la partida, y lo último que has guardado.");
  StatusCard(m, a);
  Gap(22);

  // Duración y fuentes, en dos columnas.
  float avail = ImGui::GetContentRegionAvail().x;
  float colW = (avail - Px(32)) * 0.5f;
  ImGui::BeginGroup();
  ImGui::PushItemWidth(colW);
  SectionLabel("DURACIÓN DEL CLIP");
  Gap(4);
  if (int v = ClipLengthPicker("len", c.clipSeconds, m.stats.replayFitSeconds, colW); v > 0 && v != c.clipSeconds)
    a.setClipSeconds(v);
  Gap(8);
  {
    char hint[300];
    // Una duración que no cabe en la RAM del replay se avisa en el color de aviso.
    const double fit = m.stats.replayFitSeconds;
    const bool tight = m.replayActive && fit > 0 && double(c.clipSeconds) > fit + 0.5;
    if (tight)
      snprintf(hint, sizeof(hint), "Con esta calidad, %s no caben enteros en la memoria del replay: en escenas de "
                                   "mucho movimiento el clip saldría más corto. Caben unos %s.",
               FormatClipLength(c.clipSeconds).c_str(), FormatClipLength(std::max(5, int(fit) / 5 * 5)).c_str());
    else
      snprintf(hint, sizeof(hint), "Al guardar un clip se guardan los últimos %s. Cambiarla no borra lo que ya hay "
                                   "en el replay.",
               FormatClipLength(c.clipSeconds).c_str());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colW);
    ImGui::PushStyleColor(ImGuiCol_Text, V(tight ? p.warning : p.textFaint));
    ImGui::TextWrapped("%s", hint);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
  }
  ImGui::PopItemWidth();
  ImGui::EndGroup();
  ImGui::SameLine(0, Px(32));
  ImGui::BeginGroup();
  ImGui::BeginChild("toggles", ImVec2(colW, Px(4) + Px(kHint) + Px(4) + Px(36) * 3), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
  DrawHoverTrail(ImGui::GetWindowDrawList());
  SectionLabel("CAPTURA");
  ImGui::Indent(Px(8));
  if (ToggleRow("replay", Icon::Replay, "Replay", c.replayEnabled)) a.toggleReplay();
  if (ToggleRow("mic", Icon::Mic, "Micrófono", c.captureMic)) a.toggleMic();
  if (ToggleRow("cam", Icon::Camera, "Cámara", c.cameraEnabled)) a.toggleCamera();
  ImGui::Unindent(Px(8));
  ImGui::EndChild();
  ImGui::EndGroup();

  Gap(22);
  bool viewAll = false;
  SectionLabel("LO ÚLTIMO", "Ver la biblioteca", &viewAll);
  if (viewAll) Navigate(m, AppPage::Library);
  Gap(6);
  if (m.items.empty()) {
    std::string text = "Aún no hay nada guardado. Durante una partida, pulsa " + c.hotkeys.saveClip +
                       " para guardar los últimos " + FormatClipLength(c.clipSeconds) + ", o " + c.hotkeys.overlay +
                       " para abrir el panel sin salir del juego.";
    Help(text.c_str());
  } else {
    const int count = 4;
    float gap = Px(14);
    float cardW = (ImGui::GetContentRegionAvail().x - gap * float(count - 1)) / float(count);
    for (int i = 0; i < count && i < int(m.items.size()); ++i) {
      if (i > 0) ImGui::SameLine(0, gap);
      ImGui::PushID(i);
      bool menu = false;
      const MediaItem& item = m.items[size_t(i)];
      if (Card(w, item, cardW, &menu)) View(m, item.path, AppPage::Home);
      if (menu) {
        menuPath_ = item.path;
        ImGui::OpenPopup("item-menu");
      }
      ImGui::PopID();
    }
  }
  ItemMenu(m, a);

  Gap(24);
  StatsSection(m);

  Gap(24);
  SectionLabel("CONSEJOS");
  Gap(2);
  std::string tips = c.hotkeys.overlay + " abre el panel encima del juego sin quitarle la pantalla completa. " +
                     (c.hotkeys.marker.empty() ? std::string()
                                               : c.hotkeys.marker + " marca un momento para encontrarlo luego en el clip. ") +
                     (c.hotkeys.shortClip.empty()
                          ? std::string()
                          : c.hotkeys.shortClip + " guarda un clip corto, solo los últimos " +
                                FormatClipLength(std::min(c.shortClipSeconds, c.clipSeconds)) + ". ") +
                     "Con la estrella guardas un clip como favorito: el límite de espacio nunca lo borra. En la "
                     "biblioteca, Ctrl+clic elige varios clips para unirlos.";
  Help(tips.c_str());
  if (m.freeBytes > 0 && m.freeBytes < (uint64_t(2) << 30)) {
    Gap(8);
    std::string warn = "Queda poco espacio en el disco de los clips (" + FormatBytes(m.freeBytes) +
                       "). Libera espacio o pon un límite en Ajustes > Almacenamiento.";
    Warning(warn.c_str());
  }
  Gap(12);
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ biblioteca

bool AppUi::Card(AppWindow& w, const MediaItem& item, float width, bool* contextMenu, int selection) {
  (void)w;
  const Palette& p = Colors();
  const float thumbH = std::floor(width * 9.0f / 16.0f);
  const float textH = Px(50);
  ImVec2 size(width, thumbH + textH);
  bool pressed = ImGui::InvisibleButton("card", size);
  bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) *contextMenu = true;
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!ImGui::IsRectVisible(a, b)) return false;
  ImVec2 ta = a, tb(b.x, a.y + thumbH);
  dl->AddRectFilled(ta, tb, C(p.surface));
  if (UiTexture* tex = ThumbFor(item, width)) {
    DrawImage(dl, *tex, ta, tb, true);
  } else {
    DrawIcon(dl, item.Video() ? Icon::Play : Icon::Shot, (ta + tb) * 0.5f, Px(12), C(p.textFaint));
  }
  RequestDuration(item);
  if (hov) dl->AddRect(a, b, C(p.lineStrong), 0, 0, Px(1));
  Badge(dl, ImVec2(ta.x + Px(8), ta.y + Px(8)), BadgeText(item), false);
  if (item.Video() && item.duration > 0)
    Badge(dl, ImVec2(tb.x - Px(8), tb.y - Px(8) - Px(kLabel) - Px(6)), FormatDuration(item.duration), true);

  // Estrella de favorito: siempre si lo es, al pasar el ratón si no.
  ImVec2 starC(tb.x - Px(18), ta.y + Px(18));
  bool overStar = hov && std::fabs(ImGui::GetMousePos().x - starC.x) < Px(14) &&
                  std::fabs(ImGui::GetMousePos().y - starC.y) < Px(14);
  if (item.favorite || hov) {
    dl->AddRectFilled(starC - ImVec2(Px(13), Px(13)), starC + ImVec2(Px(13), Px(13)), C(p.bg, 0.86f));
    DrawIcon(dl, item.favorite ? Icon::StarFilled : Icon::Star, starC, Px(8),
             C(item.favorite ? p.accent : overStar ? p.text : p.textMuted));
  }
  if (overStar) ImGui::SetTooltip(item.favorite ? "Quitar de favoritos" : "Favorito: el límite de espacio no lo borra");

  // Casilla para elegir vídeos y unirlos: al pasar el ratón y, mientras haya
  // alguno elegido, en todas las tarjetas.
  bool overCheck = false;
  if (selection >= 0) {
    ImVec2 boxC(ta.x + Px(18), tb.y - Px(18));
    overCheck = hov && std::fabs(ImGui::GetMousePos().x - boxC.x) < Px(14) &&
                std::fabs(ImGui::GetMousePos().y - boxC.y) < Px(14);
    if (selection > 0 || hov || !selected_.empty()) {
      ImVec2 b0 = boxC - ImVec2(Px(8), Px(8)), b1 = boxC + ImVec2(Px(8), Px(8));
      dl->AddRectFilled(b0, b1, C(selection > 0 ? p.accent : p.bg, selection > 0 ? 1.0f : 0.86f));
      dl->AddRect(b0, b1, C(selection > 0 ? p.accent : overCheck ? p.text : p.lineStrong), 0, 0, Px(1));
      if (selection > 0) {
        ImU32 tick = C(p.onAccent);
        dl->AddLine(boxC + ImVec2(-Px(4), 0), boxC + ImVec2(-Px(1), Px(3)), tick, Px(1.5f));
        dl->AddLine(boxC + ImVec2(-Px(1), Px(3)), boxC + ImVec2(Px(4), -Px(3)), tick, Px(1.5f));
      }
    }
    if (selection > 0) dl->AddRect(a, b, C(p.accent), 0, 0, Px(1));
    if (overCheck) ImGui::SetTooltip(selection > 0 ? "Quitar de la selección" : "Elegir para unir con otros clips");
  }

  float fs = Px(kBody);
  std::string title = Ellipsize(Bold(), fs, Title(item), width - Px(4));
  Text(dl, Bold(), fs, ImVec2(a.x, tb.y + Px(10)), C(p.text), title.c_str());
  std::string sub = item.date + "  ·  " + FormatBytes(item.size);
  Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(a.x, tb.y + Px(30)), C(p.textFaint),
       Ellipsize(ImGui::GetFont(), Px(kSmall), sub, width - Px(4)).c_str());
  if (pressed && overStar) {
    // El clic en la estrella no abre el visor; se aplica al final del frame.
    starToggle_ = item.path;
    return false;
  }
  if (pressed && overCheck) {
    selectToggle_ = item.path;
    return false;
  }
  return pressed;
}

void AppUi::ItemMenu(AppModel& m, const AppActions& a) {
  if (!ImGui::BeginPopup("item-menu")) return;
  const MediaItem* item = Find(m, menuPath_);
  if (!item) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  std::string path = item->path;
  if (ImGui::MenuItem("Abrir")) View(m, path, m.page);
  if (Editable(*item) && ImGui::MenuItem("Editar...")) Edit(m, path);
  if (ImGui::MenuItem("Abrir con Windows")) a.openPath(path);
  if (ImGui::MenuItem("Mostrar en la carpeta")) a.showInFolder(path);
  if (ImGui::MenuItem("Copiar para pegar en Discord")) a.copyToClipboard(path);
  if (Editable(*item)) {
    if (m.exportPercent >= 0 && m.exportPath == path) {
      std::string label = "Cancelar la exportación (" + std::to_string(m.exportPercent) + " %)";
      if (ImGui::MenuItem(label.c_str())) a.cancelExport();
    } else if (ImGui::MenuItem("Exportar para Discord (menos de 20 MB)", nullptr, false, m.exportPercent < 0)) {
      a.exportForDiscord(path);
    }
    if (ImGui::MenuItem("Unir con otros clips...")) {
      if (!Selected(path)) selected_.push_back(path);
      Navigate(m, AppPage::Library);
    }
    if (ImGui::BeginMenu("Cámara lenta")) {
      if (ImGui::MenuItem("A mitad de velocidad")) a.slowMotion(path, 2);
      if (ImGui::MenuItem("A un cuarto de velocidad")) a.slowMotion(path, 4);
      ImGui::EndMenu();
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(item->favorite ? "Quitar de favoritos" : "Añadir a favoritos")) a.setFavorite(path, !item->favorite);
  if (ImGui::MenuItem("Renombrar...")) {
    renamePath_ = path;
    renameText_ = item->stem;
    renameError_.clear();
    openRename_ = true;
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Mover a la papelera...")) {
    deletePath_ = path;
    openDelete_ = true;
  }
  ImGui::EndPopup();
}

void AppUi::Dialogs(AppModel& m, const AppActions& a) {
  const Palette& p = Colors();
  ImGuiIO& io = ImGui::GetIO();
  if (openRename_) {
    ImGui::OpenPopup("Renombrar");
    openRename_ = false;
  }
  if (openDelete_) {
    ImGui::OpenPopup("Mover a la papelera");
    openDelete_ = false;
    deleteHasBin_ = HasRecycleBin(deletePath_);
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(24), Px(20)));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, V(p.bg));
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, V(IM_COL32(0, 0, 0, 140)));
  ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(Px(460), 0));
  if (ImGui::BeginPopupModal("Renombrar", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings)) {
    Text(ImGui::GetWindowDrawList(), Bold(), Px(kBody) + Px(2), ImGui::GetCursorScreenPos(), C(p.text), "Renombrar");
    Gap(30);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##name", &renameText_, ImGuiInputTextFlags_EnterReturnsTrue);
    Gap(4);
    if (!renameError_.empty()) ErrorText(renameError_.c_str());
    else Help("La extensión no cambia. Favoritos y marcas se conservan.");
    Gap(14);
    float bw = Px(130);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw * 2 - Px(8));
    bool cancel = GhostButton("cancel", "Cancelar", bw) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::SameLine(0, Px(8));
    bool ok = SolidButton("ok", "Renombrar", bw) || enter;
    if (cancel) {
      ImGui::CloseCurrentPopup();
    } else if (ok) {
      // El reproductor suelta el archivo antes: abierto, Windows no deja
      // renombrarlo. Después lo vuelve a abrir con el nombre nuevo (o el viejo).
      if (m.viewPath == renamePath_) LeaveViewer();
      std::string newPath, error;
      if (a.rename(renamePath_, renameText_, &newPath, &error)) {
        if (m.viewPath == renamePath_) m.viewPath = newPath;
        ImGui::CloseCurrentPopup();
      } else {
        renameError_ = error;
      }
    }
    ImGui::EndPopup();
  }
  ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(Px(460), 0));
  if (ImGui::BeginPopupModal("Mover a la papelera", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings)) {
    const MediaItem* item = Find(m, deletePath_);
    std::string name = item ? item->name : deletePath_;
    Text(ImGui::GetWindowDrawList(), Bold(), Px(kBody) + Px(2), ImGui::GetCursorScreenPos(), C(p.text),
         deleteHasBin_ ? "¿Mover a la papelera?" : "¿Borrar para siempre?");
    Gap(30);
    ImGui::PushTextWrapPos(0);
    ImGui::TextWrapped("%s", name.c_str());
    ImGui::PopTextWrapPos();
    Gap(4);
    Help(deleteHasBin_ ? "Podrás recuperarlo desde la papelera de reciclaje de Windows."
                       : "Esta unidad no tiene papelera (memoria USB o carpeta de red): Windows te pedirá "
                         "confirmarlo y no se podrá recuperar.");
    Gap(14);
    float bw = Px(150);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw * 2 - Px(8));
    bool cancel = GhostButton("cancel", "Cancelar", bw) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::SameLine(0, Px(8));
    // Intro no cuenta en el frame en que se abre: si no, el Intro que eligió
    // «Mover a la papelera...» en el menú con el teclado lo confirmaría sin verlo.
    bool ok = GhostButton("ok", deleteHasBin_ ? "Mover a la papelera" : "Borrar para siempre", bw, Tone::Danger) ||
              (!ImGui::IsWindowAppearing() && ImGui::IsKeyPressed(ImGuiKey_Enter, false));
    if (cancel) {
      ImGui::CloseCurrentPopup();
    } else if (ok) {
      // El visor suelta el archivo antes: si no, Windows no deja moverlo.
      bool viewing = m.viewPath == deletePath_ && m.page == AppPage::Viewer;
      std::string next;
      if (viewing) {
        auto list = Filtered(m);
        for (size_t i = 0; i < list.size(); ++i)
          if (list[i]->path == deletePath_) next = i + 1 < list.size() ? list[i + 1]->path : i > 0 ? list[i - 1]->path : "";
        LeaveViewer();
      }
      a.recycle(deletePath_);
      if (viewing) {
        if (next.empty()) Navigate(m, m.viewBack);
        else m.viewPath = next;
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}

void AppUi::PageLibrary(AppWindow& w, AppModel& m, const AppActions& a) {
  const Palette& p = Colors();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(32), Px(26)));
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  auto list = Filtered(m);
  uint64_t bytes = 0;
  for (const auto* item : list) bytes += item->size;
  char subtitle[160];
  if (m.items.empty()) snprintf(subtitle, sizeof(subtitle), "%s", m.outputDir.c_str());
  else
    snprintf(subtitle, sizeof(subtitle), "%zu de %zu  ·  %s  ·  %s", list.size(), m.items.size(),
             FormatBytes(bytes).c_str(), m.outputDir.c_str());
  PageTitle("Biblioteca", subtitle);

  // Barra de filtros
  {
    static const std::vector<std::pair<int, std::string>> filters = {
        {0, "Todo"}, {1, "Clips"}, {2, "Grabaciones"}, {3, "Capturas"}, {4, "Favoritos"}};
    float avail = ImGui::GetContentRegionAvail().x;
    float segW = std::min(Px(470), avail * 0.5f);
    if (int v = Segmented("filter", filters, filter_, segW); v >= 0) filter_ = v;
    ImGui::SameLine(0, Px(12));
    // Juegos que aparecen en la biblioteca
    std::vector<std::string> games;
    for (const auto& item : m.items) {
      std::string g = Title(item);
      if (item.spectre && std::find(games.begin(), games.end(), g) == games.end()) games.push_back(g);
    }
    std::sort(games.begin(), games.end());
    ImGui::SetNextItemWidth(Px(170));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (Px(32) - ImGui::GetFrameHeight()) * 0.5f);
    if (ImGui::BeginCombo("##game", game_.empty() ? "Todos los juegos" : game_.c_str())) {
      if (ImGui::Selectable("Todos los juegos", game_.empty())) game_.clear();
      for (const auto& g : games)
        if (ImGui::Selectable(g.c_str(), g == game_)) game_ = g;
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, Px(8));
    ImGui::SetNextItemWidth(Px(150));
    ChoiceCombo<int>("##sort", sort_, {{0, "Más recientes"}, {1, "Más antiguos"}, {2, "Más grandes"}, {3, "Más largos"}});
    ImGui::SameLine(0, Px(8));
    // Un vídeo cualquiera de los que se ven, para recordar partidas.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - Px(28)) * 0.5f);
    if (GhostButton("random", "Al azar", Px(84), Tone::Normal, 28)) {
      std::vector<const MediaItem*> videos;
      for (const auto* item : list)
        if (item->Video() && item->path != m.viewPath) videos.push_back(item);
      if (videos.empty())
        for (const auto* item : list) videos.push_back(item);
      if (!videos.empty()) {
        randomSeed_ = randomSeed_ * 1664525u + 1013904223u + unsigned(GetTickCount64());
        View(m, videos[(randomSeed_ >> 8) % videos.size()]->path, AppPage::Library);
      }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Abre un vídeo cualquiera de los que se ven");
    ImGui::SameLine(0, Px(8));
    float searchW = std::max(Px(120), ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(searchW);
    ImGui::InputTextWithHint("##search", "Buscar", &search_);
  }
  Gap(16);

  // Vídeos elegidos para unir: los que ya no están (borrados, renombrados) salen.
  selected_.erase(std::remove_if(selected_.begin(), selected_.end(),
                                 [&](const std::string& path) {
                                   const MediaItem* item = Find(m, path);
                                   return !item || !item->Video();
                                 }),
                  selected_.end());
  if (!selected_.empty()) {
    const Palette& pal = Colors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x - Px(12), barH = Px(48);
    dl->AddRectFilled(q, ImVec2(q.x + barW, q.y + barH), C(pal.surface));
    dl->AddRectFilled(q, ImVec2(q.x + Px(3), q.y + barH), C(pal.accent));
    const size_t n = selected_.size();
    std::string text = n == 1 ? std::string("1 vídeo elegido. Elige otro con la casilla o Ctrl+clic.")
                              : std::to_string(n) + " vídeos elegidos. Se unen del más antiguo al más reciente.";
    const float bw = Px(150);
    Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(q.x + Px(16), q.y + (barH - Px(kSmall)) * 0.5f), C(pal.text),
         Ellipsize(ImGui::GetFont(), Px(kSmall), text, barW - bw * 2 - Px(48)).c_str());
    ImGui::SetCursorScreenPos(ImVec2(q.x + barW - bw * 2 - Px(8) - Px(10), q.y + (barH - Px(30)) * 0.5f));
    if (GhostButton("unselect", "Quitar selección", bw, Tone::Normal, 30)) selected_.clear();
    ImGui::SameLine(0, Px(8));
    ImGui::BeginDisabled(n < 2);
    std::string join = n < 2 ? std::string("Unir") : "Unir " + std::to_string(n) + " vídeos";
    if (SolidButton("join", join.c_str(), bw, 30)) {
      std::vector<const MediaItem*> chosen;
      for (const auto& path : selected_)
        if (const MediaItem* item = Find(m, path)) chosen.push_back(item);
      std::stable_sort(chosen.begin(), chosen.end(), [](auto* x, auto* y) { return x->modified < y->modified; });
      std::vector<std::string> paths;
      for (const auto* item : chosen) paths.push_back(item->path);
      a.joinClips(paths);
      selected_.clear();
    }
    ImGui::EndDisabled();
    if (n >= 2 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Sin recodificar: solo vídeos grabados con la misma calidad, resolución y audio. El "
                        "resultado es un vídeo nuevo; los originales no cambian.");
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + barH));
    Gap(12);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
        !TypingText())
      selected_.clear();
  }

  if (m.items.empty()) {
    Help(m.itemsLoaded ? "La biblioteca está vacía. Los clips, grabaciones y capturas que guardes aparecerán aquí."
                       : "Leyendo la carpeta...");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return;
  }
  if (list.empty()) {
    Help("Nada coincide con los filtros.");
    Gap(6);
    if (GhostButton("clear", "Quitar filtros", Px(140))) {
      filter_ = 0;
      game_.clear();
      search_.clear();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return;
  }

  // Rejilla: tantas columnas como quepan de ~260 px.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("grid", ImVec2(0, 0), ImGuiChildFlags_None);
  DrawHoverTrail(ImGui::GetWindowDrawList());
  const float gap = Px(16);
  float avail = ImGui::GetContentRegionAvail().x - Px(12);
  int cols = std::max(1, int((avail + gap) / (Px(250) + gap)));
  float cardW = std::floor((avail - gap * float(cols - 1)) / float(cols));
  float rowH = std::floor(cardW * 9.0f / 16.0f) + Px(50) + gap;
  int rows = int((list.size() + size_t(cols) - 1) / size_t(cols));
  ImGuiListClipper clipper;
  clipper.Begin(rows, rowH);
  while (clipper.Step()) {
    for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
      for (int c = 0; c < cols; ++c) {
        size_t i = size_t(r) * size_t(cols) + size_t(c);
        if (i >= list.size()) break;
        if (c > 0) ImGui::SameLine(0, gap);
        ImGui::PushID(int(i));
        bool menu = false;
        const MediaItem& item = *list[i];
        const int selection = item.Video() ? (Selected(item.path) ? 1 : 0) : -1;
        if (Card(w, item, cardW, &menu, selection)) {
          // Con algo elegido (o con Ctrl), el clic elige en vez de abrir.
          if (selection >= 0 && (!selected_.empty() || ImGui::GetIO().KeyCtrl)) ToggleSelected(item.path);
          else View(m, item.path, AppPage::Library);
        }
        if (menu) {
          menuPath_ = list[i]->path;
          ImGui::OpenPopup("item-menu");
        }
        ImGui::PopID();
      }
      ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    }
  }
  ItemMenu(m, a);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  (void)p;
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ visor

void AppUi::Timeline(AppWindow& w, const MediaItem& item, float width) {
  (void)w;
  const Palette& p = Colors();
  double duration = player_.Duration() > 0 ? player_.Duration() : item.duration;
  const float h = Px(34);
  ImGui::InvisibleButton("timeline", ImVec2(width, h));
  bool hov = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float mid = (a.y + b.y) * 0.5f + Px(4);
  auto xOf = [&](double t) { return duration > 0 ? a.x + float(t / duration) * (b.x - a.x) : a.x; };
  auto tOf = [&](float x) { return duration > 0 ? std::clamp(double((x - a.x) / (b.x - a.x)) * duration, 0.0, duration) : 0.0; };
  float mx = ImGui::GetMousePos().x;

  if (ImGui::IsItemActivated() && duration > 0) {
    dragHandle_ = 0;
    if (trimMode_) {
      if (std::fabs(mx - xOf(trimStart_)) < Px(8)) dragHandle_ = 1;
      else if (std::fabs(mx - xOf(trimEnd_)) < Px(8)) dragHandle_ = 2;
    }
    if (dragHandle_ == 0) {
      scrubbing_ = true;
      wasPlaying_ = player_.Playing();
      player_.Pause();
    }
  }
  if (active && duration > 0) {
    double t = tOf(mx);
    if (dragHandle_ == 1) trimStart_ = std::min(t, trimEnd_ - 0.5);
    else if (dragHandle_ == 2) trimEnd_ = std::max(t, trimStart_ + 0.5);
    else if (std::fabs(t - player_.Position()) > 0.04) player_.Seek(t, false);
  }
  if (ImGui::IsItemDeactivated()) {
    if (scrubbing_) {
      player_.Seek(tOf(mx), true);
      if (wasPlaying_) player_.Play();
    }
    scrubbing_ = false;
    dragHandle_ = 0;
  }
  trimStart_ = std::clamp(trimStart_, 0.0, std::max(0.0, duration));
  trimEnd_ = std::clamp(trimEnd_, 0.0, std::max(0.0, duration));

  // Pista, parte reproducida y cabezal
  double pos = player_.Position();
  dl->AddRectFilled(ImVec2(a.x, mid - Px(2)), ImVec2(b.x, mid + Px(2)), C(p.surfaceActive));
  if (trimMode_ && duration > 0) {
    dl->AddRectFilled(ImVec2(xOf(trimStart_), mid - Px(2)), ImVec2(xOf(trimEnd_), mid + Px(2)), C(p.accent, 0.45f));
    for (int k = 0; k < 2; ++k) {
      float x = xOf(k == 0 ? trimStart_ : trimEnd_);
      bool atHandle = (hov || dragHandle_ == k + 1) && std::fabs(mx - x) < Px(8);
      dl->AddRectFilled(ImVec2(x - Px(2), mid - Px(11)), ImVec2(x + Px(2), mid + Px(11)),
                        C(atHandle || dragHandle_ == k + 1 ? p.text : p.accent));
    }
    if (hov && (std::fabs(mx - xOf(trimStart_)) < Px(8) || std::fabs(mx - xOf(trimEnd_)) < Px(8)))
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  } else {
    dl->AddRectFilled(ImVec2(a.x, mid - Px(2)), ImVec2(xOf(pos), mid + Px(2)), C(p.accent));
  }
  // Marcas: rombos o cuadrados del acento encima de la pista.
  for (double t : item.markers) {
    float x = xOf(t);
    DrawMark(dl, ImVec2(x, a.y + Px(7)), Px(3), C(p.accent), true);
    if (hov && std::fabs(mx - x) < Px(6)) ImGui::SetTooltip("Marca en %s", FormatDuration(t).c_str());
  }
  float px = xOf(pos);
  dl->AddRectFilled(ImVec2(px - Px(1), mid - Px(8)), ImVec2(px + Px(1), mid + Px(8)), C(p.text));
  if (hov && duration > 0 && !active) {
    float x = std::clamp(mx, a.x, b.x);
    dl->AddRectFilled(ImVec2(x, mid - Px(5)), ImVec2(x + Px(1), mid + Px(5)), C(p.textMuted));
    ImGui::SetTooltip("%s", FormatDuration(tOf(mx)).c_str());
  }
}

void AppUi::PageViewer(AppWindow& w, AppModel& m, const AppActions& a) {
  const Palette& p = Colors();
  const MediaItem* found = Find(m, m.viewPath);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(28), Px(20)));
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  if (!found) {
    if (GhostButton("back", "Volver", Px(110))) Navigate(m, m.viewBack);
    Gap(10);
    Help(m.itemsLoaded ? "Este archivo ya no está en la carpeta." : "Leyendo la carpeta...");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return;
  }
  const MediaItem item = *found; // copia: la lista puede cambiar en este frame (renombrar, borrar)
  const bool video = item.Video();
  const bool editable = Editable(item); // vídeo con imagen (no el M4A de «Solo audio»)

  // Abre el archivo en el reproductor o pide la imagen.
  if (video && playerPath_ != item.path) {
    player_.Close();
    playerPath_ = item.path;
    playerFailed_ = !player_.Open(w, item.path, true);
    trimMode_ = false;
    trimStart_ = 0;
    trimEnd_ = item.duration;
    if (!playerFailed_) {
      // El volumen del visor se mantiene de un clip a otro.
      player_.SetVolume(volume_);
      player_.SetMuted(muted_);
    }
  }
  if (!video && imagePath_ != item.path) {
    image_.reset();
    imagePath_ = item.path;
    imagePending_ = true;
    imageFailed_ = false;
    worker_.Request(MediaWorker::Job::Image, item.path);
  }

  // Cabecera: volver, título y acciones
  ImDrawList* dl = ImGui::GetWindowDrawList();
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float rowH = Px(34);
    if (IconButton("back", Icon::Back, rowH, "Volver (Esc)")) {
      Navigate(m, m.viewBack);
      ImGui::EndChild();
      ImGui::PopStyleVar();
      return;
    }
    float tx = q.x + rowH + Px(10);
    std::string title = KindName(item);
    title += "  ·  " + Title(item);
    Text(dl, Bold(), Px(kBody) + Px(2), ImVec2(tx, q.y), C(p.text), title.c_str());
    std::string sub = item.date + "  ·  " + FormatBytes(item.size);
    if (video && player_.VideoWidth() > 0) {
      char res[48];
      snprintf(res, sizeof(res), "  ·  %dx%d", player_.VideoWidth(), player_.VideoHeight());
      sub += res;
    } else if (!video && image_) {
      char res[48];
      snprintf(res, sizeof(res), "  ·  %dx%d", image_->width, image_->height);
      sub += res;
    }
    sub += "  ·  " + item.name;
    const bool exporting = m.exportPercent >= 0;
    const bool exportingThis = exporting && m.exportPath == item.path;
    if (exportingThis) sub = "Exportando para Discord: " + std::to_string(m.exportPercent) + " %";
    const int actions = editable ? 8 : 6;
    float actionsW = rowH * float(actions) + Px(4) * float(actions - 1);
    Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(tx, q.y + Px(21)), C(exportingThis ? p.accent : p.textMuted),
         Ellipsize(ImGui::GetFont(), Px(kSmall), sub, ImGui::GetContentRegionAvail().x - rowH - actionsW - Px(30)).c_str());
    ImGui::SetCursorScreenPos(ImVec2(q.x + ImGui::GetContentRegionAvail().x - actionsW, q.y));
    if (IconButton("fav", item.favorite ? Icon::StarFilled : Icon::Star, rowH,
                   item.favorite ? "Quitar de favoritos (F)" : "Favorito: el límite de espacio no lo borra (F)",
                   item.favorite))
      a.setFavorite(item.path, !item.favorite);
    ImGui::SameLine(0, Px(4));
    if (IconButton("rename", Icon::Pencil, rowH, "Renombrar")) {
      renamePath_ = item.path;
      renameText_ = item.stem;
      renameError_.clear();
      openRename_ = true;
    }
    ImGui::SameLine(0, Px(4));
    if (IconButton("copy", Icon::Copy, rowH, "Copiar el archivo para pegarlo en Discord o WhatsApp"))
      a.copyToClipboard(item.path);
    ImGui::SameLine(0, Px(4));
    if (editable) {
      if (IconButton("edit", Icon::Sliders, rowH,
                     "Editar (E): recortar, velocidad, vertical para TikTok, texto, color, sonido... El original no cambia.")) {
        Edit(m, item.path);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
      }
      ImGui::SameLine(0, Px(4));
      if (exportingThis) {
        std::string tip = "Exportando para Discord: " + std::to_string(m.exportPercent) + " %. Clic para cancelar.";
        if (IconButton("export", Icon::Share, rowH, tip.c_str(), true)) a.cancelExport();
      } else if (IconButton("export", Icon::Share, rowH,
                            exporting ? "Ya se está exportando otro vídeo"
                                      : "Exportar para Discord: una copia de menos de 20 MB que se ve en el chat. "
                                        "El original no cambia.")) {
        a.exportForDiscord(item.path); // si hay otra en marcha, el aviso lo dice
      }
      ImGui::SameLine(0, Px(4));
    }
    if (IconButton("folder", Icon::Folder, rowH, "Mostrar en la carpeta")) a.showInFolder(item.path);
    ImGui::SameLine(0, Px(4));
    if (IconButton("external", Icon::External, rowH, "Abrir con Windows")) {
      player_.Pause();
      a.openPath(item.path);
    }
    ImGui::SameLine(0, Px(4));
    if (IconButton("trash", Icon::Trash, rowH, "Mover a la papelera (Supr)")) {
      deletePath_ = item.path;
      openDelete_ = true;
    }
    if (exportingThis) {
      // 1 px a lo ancho, con la parte hecha en el acento.
      float x0 = q.x, x1 = q.x + ImGui::GetContentRegionAvail().x, y = q.y + rowH + Px(7);
      float done = x0 + (x1 - x0) * float(m.exportPercent) / 100.0f;
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + Px(1)), C(p.line));
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(done, y + Px(1)), C(p.accent));
    }
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(14)));
  }

  // Zona del vídeo o la imagen, con flechas para pasar al anterior y al siguiente.
  auto list = Filtered(m);
  int index = -1;
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i]->path == item.path) index = int(i);
  // El texto del recorte va en una línea aparte (Help: hasta 560 px de ancho).
  const float trimHelpH =
      trimMode_ ? ImGui::CalcTextSize(kTrimHelp, nullptr, false, std::min(ImGui::GetContentRegionAvail().x, Px(560))).y +
                      Px(2) + ImGui::GetStyle().ItemSpacing.y * 2
                : 0.0f;
  const float controlsH =
      video ? Px(trimMode_ ? 128.0f : 92.0f) + trimHelpH + (item.markers.empty() ? 0.0f : Px(36)) : Px(10);
  float areaW = ImGui::GetContentRegionAvail().x;
  float areaH = std::max(Px(160), ImGui::GetContentRegionAvail().y - controlsH);
  ImVec2 va = ImGui::GetCursorScreenPos(), vb(va.x + areaW, va.y + areaH);
  bool areaClicked = ImGui::InvisibleButton("media", ImVec2(areaW, areaH));
  bool areaHovered = ImGui::IsItemHovered();
  dl->AddRectFilled(va, vb, C(IM_COL32(0, 0, 0, 255)));
  if (video) {
    std::shared_ptr<UiTexture> frame = playerFailed_ ? nullptr : player_.Frame(w);
    if (frame) DrawImage(dl, *frame, va, vb, false);
    else if (UiTexture* thumb = ThumbFor(item)) DrawImage(dl, *thumb, va, vb, false, 0.5f);
    else if (item.audioOnly) DrawIcon(dl, Icon::Volume, (va + vb) * 0.5f, Px(28), C(p.textFaint));
    if (playerFailed_ || player_.Failed()) {
      std::string err = playerFailed_ ? std::string(Player::Available() ? "No se pudo abrir el reproductor."
                                                                          : "Este Windows no tiene Media Foundation (Windows N).")
                                      : player_.Error();
      ImVec2 ts = TextSize(ImGui::GetFont(), Px(kBody), err.c_str());
      Text(dl, ImGui::GetFont(), Px(kBody), ImVec2((va.x + vb.x - ts.x) * 0.5f, (va.y + vb.y) * 0.5f - Px(30)),
           C(p.text), err.c_str());
      ImGui::SetCursorScreenPos(ImVec2((va.x + vb.x) * 0.5f - Px(110), (va.y + vb.y) * 0.5f));
      if (SolidButton("ext-play", "Abrir con Windows", Px(220))) a.openPath(item.path);
    } else if (areaClicked) {
      if (player_.Playing()) player_.Pause();
      else player_.Play();
    }
    if (!player_.Playing() && !playerFailed_ && !player_.Failed() && player_.Ready() && !scrubbing_) {
      ImVec2 c = (va + vb) * 0.5f;
      dl->AddRectFilled(c - ImVec2(Px(28), Px(28)), c + ImVec2(Px(28), Px(28)), C(p.bg, 0.75f));
      DrawIcon(dl, Icon::Play, c + ImVec2(Px(2), 0), Px(13), C(p.text));
    }
  } else {
    if (image_) DrawImage(dl, *image_, va, vb, false);
    else if (UiTexture* thumb = ThumbFor(item)) DrawImage(dl, *thumb, va, vb, false, 0.6f);
    if (imageFailed_) {
      const char* err = "No se pudo abrir la imagen.";
      ImVec2 ts = TextSize(ImGui::GetFont(), Px(kBody), err);
      Text(dl, ImGui::GetFont(), Px(kBody), ImVec2((va.x + vb.x - ts.x) * 0.5f, (va.y + vb.y - ts.y) * 0.5f),
           C(p.text), err);
    }
    if (areaClicked) a.openPath(item.path);
    if (areaHovered) ImGui::SetTooltip("Clic para abrirla con Windows");
  }
  // Anterior / siguiente
  auto navArrow = [&](const char* id, Icon icon, ImVec2 c, int target) {
    if (target < 0 || target >= int(list.size())) return;
    ImGui::SetCursorScreenPos(c - ImVec2(Px(18), Px(18)));
    bool clicked = ImGui::InvisibleButton(id, ImVec2(Px(36), Px(36)));
    bool hov = ImGui::IsItemHovered();
    if (!hov && !areaHovered) return;
    dl->AddRectFilled(c - ImVec2(Px(18), Px(18)), c + ImVec2(Px(18), Px(18)), C(p.bg, hov ? 0.95f : 0.7f));
    DrawIcon(dl, icon, c, Px(9), C(hov ? p.text : p.textMuted));
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (clicked) View(m, list[size_t(target)]->path, m.viewBack);
  };
  navArrow("prev", Icon::Prev, ImVec2(va.x + Px(30), (va.y + vb.y) * 0.5f), index - 1);
  navArrow("next", Icon::Next, ImVec2(vb.x - Px(30), (va.y + vb.y) * 0.5f), index + 1);
  ImGui::SetCursorScreenPos(ImVec2(va.x, vb.y + Px(12)));

  // Controles del vídeo
  if (video && !playerFailed_) {
    const float rowH = Px(34);
    double duration = player_.Duration() > 0 ? player_.Duration() : item.duration;
    if (trimEnd_ <= 0 && duration > 0) trimEnd_ = duration;
    if (IconButton("play", player_.Playing() ? Icon::Pause : Icon::Play, rowH,
                   player_.Playing() ? "Pausa (espacio)" : "Reproducir (espacio)")) {
      if (player_.Playing()) player_.Pause();
      else player_.Play();
    }
    ImGui::SameLine(0, Px(10));
    std::string time = FormatDuration(player_.Position()) + " / " + FormatDuration(duration);
    float timeW = TextSize(ImGui::GetFont(), Px(kSmall), "00:00:00 / 00:00:00").x;
    ImVec2 tq = ImGui::GetCursorScreenPos();
    Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(tq.x, tq.y + (rowH - Px(kSmall)) * 0.5f), C(p.textMuted),
         time.c_str());
    ImGui::Dummy(ImVec2(timeW, rowH));
    ImGui::SameLine(0, Px(12));
    float volumeW = Px(110);
    float tlW = ImGui::GetContentRegionAvail().x - volumeW - rowH - Px(24) - (editable ? (rowH + Px(8)) * 2 : 0.0f);
    Timeline(w, item, std::max(Px(100), tlW));
    ImGui::SameLine(0, Px(12));
    if (editable) {
      if (IconButton("trim", Icon::Scissors, rowH, "Recortar (sin recodificar)", trimMode_)) {
        trimMode_ = !trimMode_;
        if (trimMode_ && (trimEnd_ <= trimStart_ || trimEnd_ <= 0)) {
          trimStart_ = 0;
          trimEnd_ = duration;
        }
      }
      ImGui::SameLine(0, Px(8));
      std::string frameTip = std::string("Guardar este fotograma como captura (S), en ") +
                             (m.config.screenshotFormat == "jpg" ? "JPG" : "PNG") + " y junto al vídeo";
      if (IconButton("frame", Icon::Shot, rowH, frameTip.c_str())) SaveCurrentFrame(item, a);
      ImGui::SameLine(0, Px(8));
    }
    if (IconButton("mute", player_.Muted() || player_.Volume() <= 0 ? Icon::Muted : Icon::Volume, rowH,
                   player_.Muted() ? "Activar el sonido (M)" : "Silenciar (M)")) {
      muted_ = !player_.Muted();
      player_.SetMuted(muted_);
    }
    ImGui::SameLine(0, Px(4));
    float vol = player_.Volume();
    ImGui::SetNextItemWidth(volumeW);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetFrameHeight()) * 0.5f);
    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "")) {
      volume_ = vol;
      player_.SetVolume(vol);
      if (muted_ && vol > 0) {
        muted_ = false;
        player_.SetMuted(false);
      }
    }

    if (trimMode_) {
      Gap(8);
      ImGui::AlignTextToFramePadding();
      std::string range = "Recorte: " + FormatPrecise(trimStart_) + " a " + FormatPrecise(trimEnd_) + "  (" +
                          FormatDuration(trimEnd_ - trimStart_) + ")";
      ImGui::TextUnformatted(range.c_str());
      ImGui::SameLine(0, Px(16));
      if (GhostButton("in", "Inicio aquí (I)", Px(130), Tone::Normal, 28))
        trimStart_ = std::min(player_.Position(), trimEnd_ - 0.5);
      ImGui::SameLine(0, Px(6));
      if (GhostButton("out", "Fin aquí (O)", Px(120), Tone::Normal, 28))
        trimEnd_ = std::max(player_.Position(), trimStart_ + 0.5);
      ImGui::SameLine(0, Px(6));
      ImGui::BeginDisabled(trimEnd_ - trimStart_ < 0.5);
      if (SolidButton("save-trim", "Guardar recorte", Px(150), 28)) {
        a.trim(item.path, trimStart_, trimEnd_);
        trimMode_ = false;
      }
      ImGui::EndDisabled();
      Gap(2);
      Help(kTrimHelp);
    }
    if (!item.markers.empty()) {
      Gap(8);
      ImGui::AlignTextToFramePadding();
      ImGui::PushStyleColor(ImGuiCol_Text, V(p.textMuted));
      ImGui::TextUnformatted("Marcas");
      ImGui::PopStyleColor();
      for (size_t k = 0; k < item.markers.size() && k < 24; ++k) {
        ImGui::SameLine(0, Px(8));
        ImGui::PushID(int(k));
        double t = item.markers[k];
        bool current = std::fabs(player_.Position() - t) < 0.5;
        if (Chip("marker", FormatDuration(t), current)) player_.Seek(std::max(0.0, t - 2.0));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ir a 2 s antes de la marca");
        ImGui::PopID();
      }
    }
  }

  // Teclado del visor (sin campo de texto ni ventana emergente activos).
  if (!TypingText() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    double duration = player_.Duration();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      Navigate(m, m.viewBack);
    } else if (video && !playerFailed_) {
      if (ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        if (player_.Playing()) player_.Pause();
        else player_.Play();
      }
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) player_.Seek(player_.Position() + 5.0);
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) player_.Seek(player_.Position() - 5.0);
      if (ImGui::IsKeyPressed(ImGuiKey_L)) player_.Seek(player_.Position() + 10.0);
      if (ImGui::IsKeyPressed(ImGuiKey_J)) player_.Seek(player_.Position() - 10.0);
      if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) player_.Seek(0);
      if (ImGui::IsKeyPressed(ImGuiKey_End, false) && duration > 0) player_.Seek(duration - 0.1);
      if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        muted_ = !player_.Muted();
        player_.SetMuted(muted_);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_I, false) && editable) {
        trimMode_ = true;
        if (trimEnd_ <= 0) trimEnd_ = duration;
        trimStart_ = std::min(player_.Position(), trimEnd_ - 0.5);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_O, false) && editable) {
        trimMode_ = true;
        trimEnd_ = std::max(player_.Position(), trimStart_ + 0.5);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_S, false) && editable) SaveCurrentFrame(item, a);
      if (ImGui::IsKeyPressed(ImGuiKey_E, false) && editable) {
        Edit(m, item.path);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) a.setFavorite(item.path, !item.favorite);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
      deletePath_ = item.path;
      openDelete_ = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false) && index + 1 < int(list.size()))
      View(m, list[size_t(index + 1)]->path, m.viewBack);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false) && index > 0) View(m, list[size_t(index - 1)]->path, m.viewBack);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ editar

void AppUi::PageEditor(AppWindow& w, AppModel& m, const AppActions& a) {
  const MediaItem* found = Find(m, m.editPath);
  if (!found) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(28), Px(20)));
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (GhostButton("back", "Volver", Px(110))) Navigate(m, m.viewBack);
    Gap(10);
    Help(m.itemsLoaded ? "Este archivo ya no está en la carpeta." : "Leyendo la carpeta...");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return;
  }
  const MediaItem item = *found; // copia: la lista puede cambiar en este frame
  if (!editor_.Active() || editor_.Path() != item.path) editor_.Open(item.path);
  const EditorUi::Exit exit = editor_.Draw(w, m, a, item);
  if (!exit.view.empty()) {
    editor_.Leave();
    View(m, exit.view, m.viewBack);
  } else if (exit.back) {
    editor_.Leave();
    m.viewPath = item.path;
    m.page = AppPage::Viewer;
  }
}

// ------------------------------------------------------------------ ajustes

void AppUi::PageSettings(AppWindow& w, AppModel& m, const AppActions& a) {
  (void)w;
  const Palette& p = Colors();
  if (!m.settings.devicesLoaded && a.loadDevices) a.loadDevices();
  const float footerH = Px(kFooterHeight);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(32), Px(26)));
  ImGui::BeginChild("page", ImVec2(0, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
  DrawHoverTrail(ImGui::GetWindowDrawList());
  PageTitle(SettingsPageName(m.settingsPage), "");
  DrawSettingsPage(m.settingsPage, m.settings, a.settings);
  Gap(16);
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // Pie: aplicar o descartar
  ImVec2 q = ImGui::GetCursorScreenPos();
  float wAvail = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(q, ImVec2(q.x + wAvail, q.y + Px(1)), C(p.line));
  float bw = Px(130);
  float y = q.y + (footerH - Px(32)) * 0.5f;
  float ss = Px(kHint);
  if (m.settings.dirty) {
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(32), q.y + (footerH - ss) * 0.5f), C(p.textMuted),
         "Hay cambios sin aplicar");
    ImGui::SetCursorScreenPos(ImVec2(q.x + wAvail - bw * 2 - Px(8) - Px(32), y));
    if (GhostButton("discard", "Descartar", bw)) a.discard();
    ImGui::SameLine(0, Px(8));
    if (SolidButton("apply", "Aplicar", bw)) a.apply();
  } else {
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(32), q.y + (footerH - ss) * 0.5f), C(p.textFaint),
         "Los cambios se guardan al pulsar Aplicar.");
  }
  ImGui::SetCursorScreenPos(q);
  ImGui::Dummy(ImVec2(wAvail, footerH));
}

} // namespace spectre
