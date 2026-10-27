#include "ui/AppUi.h"

#include "common/Util.h"
#include "ui/Backdrop.h"
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

constexpr float kTopbarHeight = 56.0f; // pestañas arriba (NavLayout::Topbar)
constexpr float kFloatMargin = 14.0f;  // margen de la barra lateral flotante (Suave)
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

// Se puede abrir en el editor de fotos: una imagen que no sea un GIF animado.
bool PhotoEditable(const MediaItem& item) { return item.kind == MediaKind::Screenshot && !IsGif(item); }

// «Para Discord» de varios: los vídeos con imagen que no son ya una copia para Discord.
bool ExportableForDiscord(const MediaItem& item) { return Editable(item) && item.kind != MediaKind::Export; }

// «12 seleccionados», «1 vídeo»
std::string Count(size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Casilla plana de 1 px (tarjetas de la biblioteca y diálogos).
void DrawCheckbox(ImDrawList* dl, ImVec2 c, bool on, bool hover) {
  const Palette& p = Colors();
  ImVec2 b0 = c - ImVec2(Px(8), Px(8)), b1 = c + ImVec2(Px(8), Px(8));
  dl->AddRectFilled(b0, b1, C(on ? p.accent : p.bg, on ? 1.0f : 0.86f));
  dl->AddRect(b0, b1, C(on ? p.accent : hover ? p.text : p.lineStrong), 0, 0, Px(1));
  if (on) {
    ImU32 tick = C(p.onAccent);
    dl->AddLine(c + ImVec2(-Px(4), 0), c + ImVec2(-Px(1), Px(3)), tick, Px(1.5f));
    dl->AddLine(c + ImVec2(-Px(1), Px(3)), c + ImVec2(Px(4), -Px(3)), tick, Px(1.5f));
  }
}

// Casilla con su texto; todo es clicable.
bool CheckRow(const char* id, const char* label, bool* value) {
  const float fs = Px(kSmall);
  ImVec2 ts = TextSize(ImGui::GetFont(), fs, label);
  bool pressed = ImGui::InvisibleButton(id, ImVec2(Px(16) + Px(10) + ts.x, std::max(Px(16), ts.y) + Px(4)));
  bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (pressed) *value = !*value;
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float mid = (a.y + b.y) * 0.5f;
  DrawCheckbox(dl, ImVec2(a.x + Px(8), mid), *value, hov);
  Text(dl, ImGui::GetFont(), fs, ImVec2(a.x + Px(26), mid - ts.y * 0.5f), C(Colors().text), label);
  return pressed;
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

bool LinkButton(const char* id, const char* label) {
  float ls = Px(kHint);
  ImVec2 ts = TextSize(ImGui::GetFont(), ls, label);
  ImVec2 q = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton(id, ImVec2(ts.x, ts.y + Px(4)));
  bool hov = ImGui::IsItemHovered();
  Motion mo = Animate();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Text(dl, ImGui::GetFont(), ls, q, C(Mix(Colors().textMuted, Colors().text, mo.hover)), label);
  // El subrayado crece desde la izquierda.
  if (mo.hover > 0)
    dl->AddRectFilled(ImVec2(q.x, q.y + ts.y + Px(1)), ImVec2(q.x + ts.x * EaseOut(mo.hover), q.y + ts.y + Px(2)),
                      C(Colors().text));
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
  Motion mo = Animate();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (active) ShapeFill(dl, a, b, C(p.surfaceActive));
  else if (mo.hover > 0) ShapeFill(dl, a, b, C(p.surface, mo.hover));
  ShapeStroke(dl, a, b, C(active ? p.accent : Mix(p.line, p.lineStrong, mo.hover)));
  PaintPress(dl, a, b, Role::Normal, mo);
  DrawMark(dl, ImVec2(a.x + Px(9), (a.y + b.y) * 0.5f), Px(2.5f), C(p.accent), true);
  Text(dl, ImGui::GetFont(), fs, ImVec2(a.x + Px(15), (a.y + b.y - ts.y) * 0.5f), C(p.text), label.c_str());
  return pressed;
}

bool TypingText() { return ImGui::GetIO().WantTextInput; }

// Entradas principales de la navegación (barra lateral, raíl o pestañas).
// Ajustes va aparte: en la barra lateral se ven sus apartados uno a uno.
struct NavEntry {
  int id;
  const char* name;
  Icon icon;
  bool selected;
  AppPage page;
};
std::vector<NavEntry> MainEntries(const AppModel& m) {
  return {
      {1, "Inicio", Icon::Home, m.page == AppPage::Home, AppPage::Home},
      {2, "Biblioteca", Icon::Grid, m.page == AppPage::Library || m.page == AppPage::Viewer, AppPage::Library},
      {3, "Editor", Icon::Scissors, m.page == AppPage::Editor, AppPage::Editor},
  };
}

// Estado del replay en pocas palabras (la marca y el texto de la navegación).
struct NavStatus {
  ImU32 mark;
  bool filled = true;
  float alpha = 1.0f;
  std::string text, shortText;
};
NavStatus StatusOf(const AppModel& m) {
  const Palette& p = Colors();
  NavStatus s;
  if (m.stats.recording) {
    s.mark = p.danger;
    s.text = "Grabando " + FormatDuration(m.stats.recordSeconds);
    s.shortText = FormatDuration(m.stats.recordSeconds);
  } else if (m.replayActive) {
    s.mark = p.accent;
    s.text = "Replay activo";
    s.shortText = "Replay";
    s.alpha = AmbientMotion() == Ambient::Cursor ? Blink() : Breathe();
  } else if (m.config.replayEnabled && m.config.onlyInGames) {
    s.mark = p.textMuted;
    s.filled = false;
    s.text = "Esperando a un juego";
    s.shortText = "Espera";
  } else {
    s.mark = p.lineStrong;
    s.text = "Replay apagado";
    s.shortText = "Apagado";
  }
  return s;
}

// Número de página para los títulos de Táctico ("01 // INICIO").
int PageNumber(AppPage page, SettingsPage settings) {
  switch (page) {
  case AppPage::Home: return 1;
  case AppPage::Library: return 2;
  case AppPage::Editor: return 3;
  case AppPage::Settings: return 4 + int(settings);
  default: return 0;
  }
}

} // namespace

// ------------------------------------------------------------------ ciclo de vida

void AppUi::OnOpened(HWND notifyWindow, UINT notifyMsg) {
  worker_.Start(notifyWindow, notifyMsg);
  editor_.Start(notifyWindow, notifyMsg);
  photo_.Start(notifyWindow, notifyMsg);
  frame_ = 0;
  openedAt_ = pageSince_ = GetTickCount64();
}

void AppUi::OnClosed() {
  worker_.Stop(false); // una miniatura lenta (carpeta de red) no retrasa el cierre
  thumbs_.clear();
  durationsAsked_.clear();
  durationFailed_.clear();
  player_.Close();
  editor_.Stop();
  photo_.Stop();
  playerPath_.clear();
  playerFailed_ = false;
  image_.reset();
  imagePath_.clear();
  imagePending_ = imageFailed_ = false;
  scrubbing_ = false;
  trimMode_ = false;
  menuPath_.clear();
  openRename_ = openDelete_ = openBulkDelete_ = false;
  EndSelection();
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
  if (m.page == AppPage::Editor && page != AppPage::Editor) LeaveEditors();
  // Desde la pestaña Editor, Volver lleva a elegir otro.
  if (page == AppPage::Editor && m.page != AppPage::Editor) m.viewBack = AppPage::Editor;
  m.page = page;
}

void AppUi::LeaveEditors() {
  editor_.Leave();
  photo_.Leave();
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
  return selected_.count(path) > 0;
}

void AppUi::ToggleSelected(const std::string& path) {
  if (!selected_.erase(path)) selected_.insert(path);
}

void AppUi::ClickSelect(const std::vector<const MediaItem*>& list, size_t index, bool range) {
  const std::string path = list[index]->path;
  size_t anchor = list.size();
  if (range)
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i]->path == selectAnchor_) anchor = i;
  if (anchor < list.size()) {
    // Mayús+clic: todo el tramo entre el último pulsado y este queda elegido.
    for (size_t i = std::min(anchor, index); i <= std::max(anchor, index); ++i)
      selected_.insert(list[i]->path);
  } else {
    ToggleSelected(path);
  }
  selectAnchor_ = path;
}

void AppUi::EndSelection() {
  selected_.clear();
  selectAnchor_.clear();
  selectMode_ = false;
}

std::vector<const MediaItem*> AppUi::SelectedItems(const AppModel& m) const {
  std::vector<const MediaItem*> out;
  if (selected_.empty()) return out;
  for (const auto& item : m.items)
    if (selected_.count(item.path)) out.push_back(&item);
  return out;
}

void AppUi::FavoriteSelection(AppModel& m, const AppActions& a) {
  const auto chosen = SelectedItems(m);
  if (chosen.empty() || !a.setFavorites) return;
  // Si ya lo son todos, se quitan; si no, todos pasan a serlo.
  const bool all = std::all_of(chosen.begin(), chosen.end(), [](const MediaItem* item) { return item->favorite; });
  std::vector<std::string> paths;
  for (const auto* item : chosen) paths.push_back(item->path);
  a.setFavorites(paths, !all);
}

void AppUi::ExportSelection(AppModel& m, const AppActions& a) {
  std::vector<const MediaItem*> videos;
  for (const auto* item : SelectedItems(m))
    if (ExportableForDiscord(*item)) videos.push_back(item);
  if (videos.empty() || !a.exportManyForDiscord) return;
  std::stable_sort(videos.begin(), videos.end(), [](auto* x, auto* y) { return x->modified < y->modified; });
  std::vector<std::string> paths;
  for (const auto* item : videos) paths.push_back(item->path);
  a.exportManyForDiscord(paths);
  EndSelection();
}

void AppUi::JoinSelection(AppModel& m, const AppActions& a) {
  auto chosen = SelectedItems(m);
  if (chosen.size() < 2) return;
  std::stable_sort(chosen.begin(), chosen.end(), [](auto* x, auto* y) { return x->modified < y->modified; });
  std::vector<std::string> paths;
  for (const auto* item : chosen) paths.push_back(item->path);
  a.joinClips(paths);
  EndSelection();
}

void AppUi::CopySelection(AppModel& m, const AppActions& a) {
  std::vector<std::string> paths;
  for (const auto* item : SelectedItems(m)) paths.push_back(item->path);
  if (!paths.empty() && a.copyManyToClipboard) a.copyManyToClipboard(paths);
}

void AppUi::AskDeleteSelection(AppModel& m) {
  bulkDelete_.clear();
  for (const auto* item : SelectedItems(m)) bulkDelete_.push_back(item->path);
  if (bulkDelete_.empty()) return;
  bulkKeepFavorites_ = false;
  openBulkDelete_ = true;
}

void AppUi::TakeMediaResults(AppWindow& w, AppModel& m, const AppActions& a) {
  editor_.TakeResults(w);
  photo_.TakeResults(w);
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
  if (m.page != AppPage::Editor && photo_.Active()) photo_.Leave();
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

  // Navegación del diseño: barra lateral (flotante en Suave), raíl de iconos
  // o pestañas arriba. La página ocupa lo que queda, con el fondo ilustrado
  // del tema detrás (en el visor y el editor, más tenue: no compite con el vídeo).
  const Style& st = CurrentStyle();
  ImVec2 content(0, 0);
  switch (st.nav) {
  case NavLayout::Sidebar: content.x = Px(st.navWidth) + (st.floatingNav ? Px(kFloatMargin) : 0.0f); break;
  case NavLayout::Rail: content.x = Px(st.navWidth); break;
  case NavLayout::Topbar: content.y = Px(kTopbarHeight); break;
  }
  if (BackdropVisible())
    DrawBackdrop(ImGui::GetWindowDrawList(), content, io.DisplaySize,
                 m.page == AppPage::Viewer || m.page == AppPage::Editor ? 0.45f : 1.0f);
  switch (st.nav) {
  case NavLayout::Sidebar: Sidebar(w, m, a); break;
  case NavLayout::Rail: NavRail(m); break;
  case NavLayout::Topbar: NavTopbar(m); break;
  }
  ImGui::SetCursorScreenPos(content);
  // Al cambiar de página (o de sección de Ajustes), la nueva entra con la
  // animación del diseño: se desliza, sube, tiembla o se descubre de arriba abajo.
  const int shown = int(m.page) * 64 + (m.page == AppPage::Settings ? int(m.settingsPage) : 0);
  if (shown != shownPage_) {
    if (shownPage_ >= 0) pageSince_ = GetTickCount64();
    shownPage_ = shown;
  }
  const Entrance enter = EnterMotion(pageSince_, true);
  const ImVec2 pageMin = ImGui::GetCursorScreenPos();
  if (!enter.done) {
    ImGui::SetCursorScreenPos(pageMin + enter.offset);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(enter.alpha, 0.0f, 1.0f));
  }
  ImGui::BeginGroup();
  switch (m.page) {
  case AppPage::Home: PageHome(w, m, a); break;
  case AppPage::Library: PageLibrary(w, m, a); break;
  case AppPage::Viewer: PageViewer(w, m, a); break;
  case AppPage::Editor: PageEditor(w, m, a); break;
  case AppPage::Settings: PageSettings(w, m, a); break;
  }
  ImGui::EndGroup();
  if (!enter.done) {
    ImGui::PopStyleVar();
    // Terminal: lo que queda por debajo de la línea aún no se ve.
    if (enter.reveal < 1.0f) {
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      const float y = pageMin.y + (io.DisplaySize.y - pageMin.y) * enter.reveal;
      fg->AddRectFilled(ImVec2(pageMin.x, y), io.DisplaySize, C(p.bg));
      fg->AddRectFilled(ImVec2(pageMin.x, y - Px(1)), ImVec2(io.DisplaySize.x, y + Px(1)), C(p.accent));
    }
  }
  Dialogs(m, a);
  ImGui::End();
  ImGui::PopStyleColor();
  if (!starToggle_.empty()) {
    if (const MediaItem* item = Find(m, starToggle_)) a.setFavorite(item->path, !item->favorite);
    starToggle_.clear();
  }

  // La ventana se dibuja cuando algo cambia. El movimiento de fondo del diseño
  // solo con la ventana delante (detrás de un juego no lo ve nadie): el rastro
  // de Psíquico a 30 fps, el cursor de Terminal al parpadear. Pasar por
  // encima, pulsar o entrar en una página piden frames solo mientras duran.
  unsigned long long now = GetTickCount64();
  if (w.Focused() && AmbientMotion() == Ambient::Ghost) w.AnimateAt(now + 33);
  if (w.Focused() && AmbientMotion() == Ambient::Cursor) w.AnimateAt(now + 500 - now % 500 + 1);
  // Fondo que se mueve (lluvia, nieve, estrellas...): a 25 fps, también solo delante.
  if (w.Focused() && BackdropMoves()) w.AnimateAt(now + 40);
  if (FrameRequested()) w.Animate();
  // El reloj de la grabación, no detrás de un juego (ahí no lo ve nadie).
  if (m.stats.recording && (w.Focused() || !m.gameDetected)) w.AnimateAt(now + 1000 - now % 1000);
  if (m.page == AppPage::Settings && m.settings.update.state == UpdateStatus::State::Checking) w.AnimateAt(now + 50);
  if (m.page == AppPage::Settings && m.settings.update.state == UpdateStatus::State::Downloading) w.AnimateAt(now + 100);
  if (m.page == AppPage::Settings && m.settingsPage == SettingsPage::Audio && w.Focused()) w.AnimateAt(now + 33);
  if (m.page == AppPage::Settings && m.settingsPage == SettingsPage::Performance) w.AnimateAt(now + 1000);
  // Ajustes > Voz: «Lo último que oyó... hace 5 s».
  if (m.page == AppPage::Settings && m.settingsPage == SettingsPage::Voice && m.settings.voice.heardAt)
    w.AnimateAt(now + 1000);
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
  const Style& st = CurrentStyle();
  const float navW = Px(st.navWidth);
  // Suave: una tarjeta separada del borde de la ventana.
  const float margin = st.floatingNav ? Px(kFloatMargin) : 0.0f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(origin + ImVec2(margin, margin));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, Px(20)));
  ImGui::BeginChild("side", ImVec2(navW, ImGui::GetIO().DisplaySize.y - margin * 2),
                    ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 win = ImGui::GetWindowPos();
  float winH = ImGui::GetWindowHeight();
  NavChrome(dl, win, ImVec2(win.x + navW, win.y + winH), false);
  DrawHoverTrail(dl);

  // Marca y estado
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    Wordmark(dl, ImVec2(q.x + Px(20), q.y), "SPECTRE", C(p.text));
    float ss = Px(kSmall);
    float y = q.y + Px(kSmall) + Px(14);
    const NavStatus s = StatusOf(m);
    DrawMark(dl, ImVec2(q.x + Px(24), y + ss * 0.5f), Px(3.5f), C(s.mark, s.alpha), s.filled);
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(36), y), C(p.textMuted), s.text.c_str());
    ImGui::Dummy(ImVec2(0, y + ss - q.y + Px(22)));
  }

  // Filas de 34 px con el hueco normal. Con la ventana baja se juntan (hasta
  // 28 px y sin hueco) para no pisar el pie; si ni así cabe, el pie no se
  // dibuja (la versión y el atajo del panel también están en General).
  // Terminal las pone pegadas: es un árbol y las ramas tienen que unirse.
  const bool tree = st.title == TitleLook::Prompt;
  const float unit = Px(1);
  const float footerTop = win.y + winH - Px(kHint) * 2 - Px(26);
  const float spacing = tree ? 0.0f : ImGui::GetStyle().ItemSpacing.y / unit;
  const float room = (footerTop - Px(14) - ImGui::GetCursorScreenPos().y) / unit - (12 + kLabel + 6);
  const std::vector<NavEntry> entries = MainEntries(m);
  const int rows = int(entries.size()) + int(SettingsPage::Count);
  float pitch = (room + 2 * 34) / float(rows + 2); // filas de 34 y el hueco también en el rótulo
  if (pitch < 34) pitch = room / float(rows);      // sin hueco
  pitch = std::clamp(pitch, 28.0f, 34 + spacing);
  const float rowH = tree ? pitch : std::min(pitch, 34.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Px(pitch - rowH)));
  for (const NavEntry& e : entries)
    if (NavItem(e.id, e.name, e.selected, rowH)) Navigate(m, e.page);
  NavLabel(m.settings.dirty ? "AJUSTES  (SIN APLICAR)" : "AJUSTES");
  for (int i = 0; i < int(SettingsPage::Count); ++i) {
    auto page = SettingsPage(i);
    if (NavItem(10 + i, SettingsPageName(page), m.page == AppPage::Settings && m.settingsPage == page, rowH,
                tree ? 1 : 0, i + 1 == int(SettingsPage::Count))) {
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

void AppUi::NavRail(AppModel& m) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const float navW = Px(st.navWidth);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, Px(18)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, Px(4)));
  ImGui::BeginChild("rail", ImVec2(navW, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 win = ImGui::GetWindowPos();
  const float winH = ImGui::GetWindowHeight();
  NavChrome(dl, win, ImVec2(win.x + navW, win.y + winH), false);
  DrawHoverTrail(dl);

  // La "S" de la marca arriba y, debajo, una raya hasta las entradas.
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float mono = Px(34);
  Monogram(dl, ImVec2(win.x + navW * 0.5f, q.y + mono * 0.5f), mono);
  const float ruleY = q.y + mono + Px(16);
  dl->AddRectFilled(ImVec2(win.x + Px(22), ruleY), ImVec2(win.x + navW - Px(22), ruleY + LineW()), C(p.line));
  ImGui::Dummy(ImVec2(0, mono + Px(30)));

  const float rowH = Px(64);
  for (const NavEntry& e : MainEntries(m))
    if (RailItem(e.id, e.icon, e.name, e.selected, rowH)) Navigate(m, e.page);
  if (RailItem(9, Icon::Gear, "Ajustes", m.page == AppPage::Settings, rowH)) Navigate(m, AppPage::Settings);
  // Cambios sin aplicar: un punto del acento en la esquina del engranaje.
  if (m.settings.dirty) {
    const ImVec2 b = ImGui::GetItemRectMax();
    DrawMark(dl, ImVec2(b.x - Px(22), ImGui::GetItemRectMin().y + Px(12)), Px(3), C(p.accent), true);
  }

  // Estado abajo: la marca y una palabra.
  const NavStatus s = StatusOf(m);
  const float ls = Px(kLabel);
  const float y = win.y + winH - Px(22) - ls;
  if (y > ImGui::GetItemRectMax().y + Px(24)) {
    DrawMark(dl, ImVec2(win.x + navW * 0.5f, y - Px(12)), Px(4), C(s.mark, s.alpha), s.filled);
    std::string text = Ellipsize(ImGui::GetFont(), ls, s.shortText, navW - Px(10));
    const float tw = TextSize(ImGui::GetFont(), ls, text.c_str()).x;
    Text(dl, ImGui::GetFont(), ls, ImVec2(win.x + (navW - tw) * 0.5f, y), C(p.textMuted), text.c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

void AppUi::NavTopbar(AppModel& m) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const float barH = Px(kTopbarHeight);
  const float width = ImGui::GetIO().DisplaySize.x;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("topbar", ImVec2(width, barH), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 win = ImGui::GetWindowPos();
  NavChrome(dl, win, ImVec2(win.x + width, win.y + barH), true);
  DrawHoverTrail(dl);
  const float side = Px(st.pad * 0.75f);
  const float mid = win.y + barH * 0.5f;

  // Marca a la izquierda, con la letra de títulos del diseño.
  float x = win.x + side;
  switch (st.title) {
  case TitleLook::Masthead: {
    // Papel: el nombre con serifa, como la cabecera de un periódico.
    const float fs = Px(kTitle);
    ImFont* df = Display(fs);
    Text(dl, df, fs, ImVec2(x, mid - fs * 0.55f), C(p.text), "Spectre");
    x += TextSize(df, fs, "Spectre").x;
    break;
  }
  case TitleLook::Chroma: {
    // Neón: el nombre con las dos copias desplazadas del título.
    const float fs = Px(kBody + 2), tr = Px(4);
    ImFont* df = Display(fs);
    const ImVec2 pos(x, mid - fs * 0.5f);
    TrackedText(dl, df, fs, pos - ImVec2(Px(1.5f), 0), C(p.danger, 0.55f), "SPECTRE", tr);
    TrackedText(dl, df, fs, pos + ImVec2(Px(1.5f), 0), C(p.accent, 0.8f), "SPECTRE", tr);
    TrackedText(dl, df, fs, pos, C(p.text), "SPECTRE", tr);
    x += TrackedWidth(df, fs, "SPECTRE", tr);
    break;
  }
  default: {
    const float mono = Px(28);
    Monogram(dl, ImVec2(x + mono * 0.5f, mid), mono);
    x += mono + Px(12);
    Wordmark(dl, ImVec2(x, mid - Px(kSmall) * 0.5f), "SPECTRE", C(p.text));
    x += Px(10) + TrackedWidth(Bold(), Px(kSmall), "SPECTRE", Px(3.2f));
    break;
  }
  }

  // Pestañas
  ImGui::SetCursorScreenPos(ImVec2(x + Px(28), win.y));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(2), 0));
  for (const NavEntry& e : MainEntries(m)) {
    if (TabItem(e.id, e.name, e.selected, barH)) Navigate(m, e.page);
    ImGui::SameLine();
  }
  const char* settings = m.settings.dirty ? "Ajustes *" : "Ajustes";
  if (TabItem(9, settings, m.page == AppPage::Settings, barH)) Navigate(m, AppPage::Settings);
  const float tabsEnd = ImGui::GetItemRectMax().x;
  ImGui::PopStyleVar();

  // Estado a la derecha, si cabe.
  const NavStatus s = StatusOf(m);
  const float ss = Px(kSmall);
  const bool upper = st.title == TitleLook::Index || st.title == TitleLook::Chroma;
  const std::string text = upper ? Upper(s.text) : s.text;
  const float tw = upper ? TrackedWidth(ImGui::GetFont(), ss, text.c_str(), Px(1)) : TextSize(ImGui::GetFont(), ss, text.c_str()).x;
  const float tx = win.x + width - side - tw;
  if (tx - Px(20) > tabsEnd + Px(24)) {
    DrawMark(dl, ImVec2(tx - Px(12), mid), Px(3.5f), C(s.mark, s.alpha), s.filled);
    if (upper) TrackedText(dl, ImGui::GetFont(), ss, ImVec2(tx, mid - ss * 0.5f), C(p.textMuted), text.c_str(), Px(1));
    else Text(dl, ImGui::GetFont(), ss, ImVec2(tx, mid - ss * 0.5f), C(p.textMuted), text.c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

// Apartados de Ajustes en una columna dentro de la página (con raíl o
// pestañas; la barra lateral ya los tiene).
void AppUi::SettingsNav(AppModel& m) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const float colW = Px(196);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(10), Px(st.pad * 0.75f)));
  ImGui::BeginChild("subnav", ImVec2(colW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 win = ImGui::GetWindowPos();
  dl->AddRectFilled(ImVec2(win.x + colW - LineW(), win.y), ImVec2(win.x + colW, win.y + ImGui::GetWindowHeight()),
                    C(p.line));
  DrawHoverTrail(dl);
  const bool tree = st.title == TitleLook::Prompt;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, tree ? 0.0f : Px(2)));
  NavLabel(m.settings.dirty ? "AJUSTES  (SIN APLICAR)" : "AJUSTES");
  for (int i = 0; i < int(SettingsPage::Count); ++i) {
    auto page = SettingsPage(i);
    if (NavItem(10 + i, SettingsPageName(page), m.settingsPage == page, 34, tree ? 1 : 0,
                i + 1 == int(SettingsPage::Count)))
      m.settingsPage = page;
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ Inicio

void AppUi::StatusCard(AppModel& m, const AppActions& a) {
  const Palette& p = Colors();
  const Config& c = m.config;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q = ImGui::GetCursorScreenPos();
  float width = ImGui::GetContentRegionAvail().x;
  const float pad = Px(20);
  const float headH = HeadlineHeight();
  float height = pad * 2 + headH + Px(8) + Px(kSmall) + Px(20) + ActionHeight();
  CardFrame(dl, q, ImVec2(q.x + width, q.y + height), m.stats.recording ? p.danger : p.accent);

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
  // El estado como titular, con la letra de títulos del diseño.
  Headline(dl, ImVec2(q.x + pad + Px(4), q.y + pad), status, C(p.text), width - pad * 2 - Px(4));
  Text(dl, ImGui::GetFont(), Px(kSmall), ImVec2(q.x + pad + Px(4), q.y + pad + headH + Px(8)), C(p.textMuted),
       Ellipsize(ImGui::GetFont(), Px(kSmall), detail, width - pad * 2 - Px(4)).c_str());

  float gap = Px(10);
  float bw = (width - pad * 2 - gap * 2) / 3.0f;
  ImGui::SetCursorScreenPos(ImVec2(q.x + pad, q.y + height - pad - ActionHeight()));
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
  // La casilla con la forma de tarjeta del diseño, el rótulo con su estilo y
  // el número con la letra de títulos.
  TileFrame(dl, a, ImVec2(a.x + width, a.y + height));
  const float pad = Px(14), inner = width - pad * 2;
  ImFont* font = ImGui::GetFont();
  DrawLabel(dl, ImVec2(a.x + pad, a.y + pad), label.c_str(), Px(kLabel), C(p.textMuted));
  Headline(dl, ImVec2(a.x + pad, a.y + pad + Px(kLabel) + Px(8)), value, C(p.text), inner);
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

  const float gap = Px(CurrentStyle().gap * 0.75f);
  const float avail = ImGui::GetContentRegionAvail().x;
  const int cols = avail > Px(760) ? 4 : 2;
  const float tileW = std::floor((avail - gap * float(cols - 1)) / float(cols));
  const float tileH = Px(28 + kLabel) + HeadlineHeight() + Px(kSmall + 20);
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
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(CurrentStyle().pad), Px(CurrentStyle().pad * 0.8f)));
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  DrawHoverTrail(ImGui::GetWindowDrawList());
  PageTitle("Inicio", "Todo lo que hace falta durante la partida, y lo último que has guardado.", 1);
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
  // El rótulo y tres filas con su separación (la cámara se quedaba cortada).
  const float spacingY = ImGui::GetStyle().ItemSpacing.y;
  ImGui::BeginChild("toggles", ImVec2(colW, Px(kHint) + Px(4) + (RowHeight(36) + spacingY) * 3 + Px(4)),
                    ImGuiChildFlags_None,
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
    float gap = Px(CurrentStyle().gap);
    float cardW = (ImGui::GetContentRegionAvail().x - gap * float(count - 1)) / float(count);
    bool openMenu = false;
    for (int i = 0; i < count && i < int(m.items.size()); ++i) {
      if (i > 0) ImGui::SameLine(0, gap);
      ImGui::PushID(i);
      bool menu = false;
      const MediaItem& item = m.items[size_t(i)];
      if (Card(w, item, cardW, &menu, -1, i + 1)) View(m, item.path, AppPage::Home);
      if (menu) {
        menuPath_ = item.path;
        openMenu = true;
      }
      ImGui::PopID();
    }
    // Fuera del PushID de la tarjeta: ItemMenu busca el menú con el id de aquí.
    if (openMenu) ImGui::OpenPopup("item-menu");
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

bool AppUi::Card(AppWindow& w, const MediaItem& item, float width, bool* contextMenu, int selection, int number) {
  (void)w;
  const Palette& p = Colors();
  // Dónde van la miniatura y el pie según la tarjeta del diseño (Widgets MediaCard).
  const MediaCard card = MediaCardRects(ImGui::GetCursorScreenPos(), width);
  bool pressed = ImGui::InvisibleButton("card", card.b - card.a);
  bool hov = ImGui::IsItemHovered();
  Motion mo = Animate();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) *contextMenu = true;
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!ImGui::IsRectVisible(a, b)) return false;
  const ImVec2 ta = card.thumbA, tb = card.thumbB;
  MediaCardBack(dl, card, mo.hover, selection > 0);
  if (UiTexture* tex = ThumbFor(item, tb.x - ta.x)) {
    DrawImage(dl, *tex, ta, tb, true);
  } else {
    DrawIcon(dl, item.Video() ? Icon::Play : Icon::Shot, (ta + tb) * 0.5f, Px(12), C(p.textFaint));
  }
  RequestDuration(item);
  MediaCardFront(dl, card, mo.hover, selection > 0);
  Tag(dl, ImVec2(ta.x + Px(8), ta.y + Px(8)), BadgeText(item), false);
  if (item.Video() && item.duration > 0)
    Tag(dl, ImVec2(tb.x - Px(8), tb.y - Px(8) - Px(kLabel) - Px(6)), FormatDuration(item.duration), true);

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

  // Casilla para elegir varios (borrar, favoritos, Discord, unir): al pasar
  // el ratón y, mientras se está eligiendo, en todas las tarjetas.
  bool overCheck = false;
  if (selection >= 0) {
    ImVec2 boxC(ta.x + Px(18), tb.y - Px(18));
    overCheck = hov && std::fabs(ImGui::GetMousePos().x - boxC.x) < Px(14) &&
                std::fabs(ImGui::GetMousePos().y - boxC.y) < Px(14);
    if (selection > 0 || hov || Selecting()) DrawCheckbox(dl, boxC, selection > 0, overCheck);
    if (selection > 0) dl->AddRect(a, b, C(p.accent), 0, 0, std::max(Px(1), LineW()));
    if (overCheck)
      ImGui::SetTooltip(selection > 0 ? "Quitar de la selección"
                                      : "Seleccionar (también Ctrl+clic, y Mayús+clic para un tramo)");
  }

  MediaCardCaption(dl, card, Title(item), item.date + "  ·  " + FormatBytes(item.size), mo.hover, number);
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
  if ((Editable(*item) || PhotoEditable(*item)) && ImGui::MenuItem("Editar...")) Edit(m, path);
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
      selected_.insert(path);
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

void AppUi::SelectionMenu(AppModel& m, const AppActions& a) {
  if (!ImGui::BeginPopup("selection-menu")) return;
  const auto chosen = SelectedItems(m);
  const size_t n = chosen.size();
  if (n == 0) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  size_t videos = 0, favorites = 0, exportable = 0;
  for (const auto* item : chosen) {
    videos += item->Video() ? 1 : 0;
    favorites += item->favorite ? 1 : 0;
    exportable += ExportableForDiscord(*item) ? 1 : 0;
  }
  const std::string count = std::to_string(n);
  if (ImGui::MenuItem(favorites == n ? "Quitar de favoritos" : "Añadir a favoritos")) FavoriteSelection(m, a);
  // Discord acepta 10 archivos por mensaje: con más, solo se ofrece copiarlos.
  if (ImGui::MenuItem(("Copiar " + count + (n > 10 ? " al portapapeles" : " para pegar en Discord")).c_str()))
    CopySelection(m, a);
  if (exportable > 0) {
    std::string label = "Exportar " + Count(exportable, "vídeo", "vídeos") + " para Discord (menos de 20 MB)";
    if (ImGui::MenuItem(label.c_str())) ExportSelection(m, a);
  }
  if (n >= 2 && videos == n && ImGui::MenuItem(("Unir " + count + " vídeos").c_str())) JoinSelection(m, a);
  ImGui::Separator();
  if (ImGui::MenuItem(("Mover " + count + " a la papelera...").c_str())) AskDeleteSelection(m);
  ImGui::Separator();
  if (ImGui::MenuItem("Quitar selección")) EndSelection();
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
  if (openBulkDelete_) {
    ImGui::OpenPopup("Mover varios a la papelera");
    openBulkDelete_ = false;
    // Una vez al abrir, por carpeta: todo suele estar en la misma unidad.
    std::unordered_map<std::string, bool> binByFolder;
    bulkNoBin_ = 0;
    for (const auto& path : bulkDelete_) {
      const std::string folder = path.substr(0, path.find_last_of("\\/") + 1);
      auto it = binByFolder.find(folder);
      if (it == binByFolder.end()) it = binByFolder.emplace(folder, HasRecycleBin(path)).first;
      if (!it->second) ++bulkNoBin_;
    }
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
  ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(Px(480), 0));
  if (ImGui::BeginPopupModal("Mover varios a la papelera", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings)) {
    // Lo que se va: lo elegido que sigue en la biblioteca, sin los favoritos si se dejan fuera.
    std::unordered_set<std::string> asked(bulkDelete_.begin(), bulkDelete_.end());
    std::vector<const MediaItem*> items;
    size_t favorites = 0;
    uint64_t bytes = 0;
    for (const auto& item : m.items) {
      if (!asked.count(item.path)) continue;
      if (item.favorite) {
        ++favorites;
        if (bulkKeepFavorites_) continue;
      }
      items.push_back(&item);
      bytes += item.size;
    }
    const size_t n = items.size();
    const bool allNoBin = bulkNoBin_ > 0 && bulkNoBin_ >= bulkDelete_.size();
    std::string title = n == 0 ? std::string("No queda nada que borrar")
                        : allNoBin ? "¿Borrar " + Count(n, "archivo", "archivos") + " para siempre?"
                                   : "¿Mover " + Count(n, "archivo", "archivos") + " a la papelera?";
    Text(ImGui::GetWindowDrawList(), Bold(), Px(kBody) + Px(2), ImGui::GetCursorScreenPos(), C(p.text), title.c_str());
    Gap(30);
    if (n > 0) {
      // Los primeros nombres, para saber qué se va sin tener que contarlo.
      std::string names = FormatBytes(bytes) + "  ·  ";
      for (size_t i = 0; i < n && i < 3; ++i) names += (i > 0 ? ", " : "") + items[i]->name;
      if (n > 3) names += " y " + std::to_string(n - 3) + " más";
      ImGui::PushTextWrapPos(0);
      ImGui::TextWrapped("%s", names.c_str());
      ImGui::PopTextWrapPos();
      Gap(6);
    }
    if (favorites > 0) {
      std::string note = bulkKeepFavorites_ ? (favorites == 1 ? std::string("El favorito se queda.")
                                                              : "Los " + std::to_string(favorites) + " favoritos se quedan.")
                                            : "Incluye " + Count(favorites, "favorito", "favoritos") + ".";
      Warning(note.c_str());
      Gap(4);
      CheckRow("keep-favorites", "Dejar fuera los favoritos", &bulkKeepFavorites_);
      Gap(6);
    }
    if (bulkNoBin_ == 0)
      Help("Podrás recuperarlos desde la papelera de reciclaje de Windows.");
    else if (allNoBin)
      Help("Esta unidad no tiene papelera (memoria USB o carpeta de red): Windows te pedirá confirmarlo y no se "
           "podrán recuperar.");
    else
      Help((Count(bulkNoBin_, "está", "están") + " en una unidad sin papelera (memoria USB o carpeta de red): "
                                                  "Windows te pedirá confirmarlo y esos no se podrán recuperar.")
               .c_str());
    Gap(14);
    const std::string okLabel = n == 0 ? std::string("Borrar")
                                : allNoBin ? "Borrar " + std::to_string(n) + " para siempre"
                                           : "Mover " + std::to_string(n) + " a la papelera";
    const float cw = Px(130);
    const float ow = std::max(Px(170), TextSize(ImGui::GetFont(), Px(kSmall), okLabel.c_str()).x + Px(32));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cw - ow - Px(8));
    bool cancel = GhostButton("cancel", "Cancelar", cw) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::SameLine(0, Px(8));
    ImGui::BeginDisabled(n == 0);
    // Intro no cuenta en el frame en que se abre (el Intro que eligió la
    // opción del menú con el teclado lo confirmaría sin verlo).
    bool ok = GhostButton("ok", okLabel.c_str(), ow, Tone::Danger) ||
              (n > 0 && !ImGui::IsWindowAppearing() && ImGui::IsKeyPressed(ImGuiKey_Enter, false));
    ImGui::EndDisabled();
    if (cancel) {
      bulkDelete_.clear();
      ImGui::CloseCurrentPopup();
    } else if (ok) {
      std::vector<std::string> paths;
      for (const auto* item : items) paths.push_back(item->path);
      if (a.recycleMany) a.recycleMany(paths);
      bulkDelete_.clear();
      EndSelection();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}

void AppUi::SelectionBar(AppModel& m, const AppActions& a, const std::vector<const MediaItem*>& list) {
  const Palette& p = Colors();
  const auto chosen = SelectedItems(m);
  const size_t n = chosen.size();
  size_t shown = 0;
  for (const auto* item : list)
    if (Selected(item->path)) ++shown;
  const bool allShown = !list.empty() && shown == list.size();
  uint64_t bytes = 0;
  size_t videos = 0, favorites = 0, exportable = 0;
  for (const auto* item : chosen) {
    bytes += item->size;
    videos += item->Video() ? 1 : 0;
    favorites += item->favorite ? 1 : 0;
    exportable += ExportableForDiscord(*item) ? 1 : 0;
  }
  std::string text;
  if (n == 0) {
    text = "Elige con la casilla o con un clic. Mayús+clic elige un tramo entero.";
  } else {
    text = Count(n, "seleccionado", "seleccionados") + "  ·  " + FormatBytes(bytes);
    // Los que quedan fuera de los filtros también cuentan: que se sepa.
    if (n > shown) text += "  ·  " + Count(n - shown, "no se ve", "no se ven") + " con estos filtros";
  }

  struct Action {
    const char* id;
    std::string label;
    Tone tone;
    bool enabled;
    std::string tip;
  };
  const Action actions[] = {
      {"sel-all", allShown ? std::string("Ninguno") : "Todos (" + std::to_string(list.size()) + ")", Tone::Normal,
       !list.empty(),
       allShown ? "Quita de la selección lo que se ve" : "Selecciona todo lo que se ve con estos filtros (Ctrl+A)"},
      {"sel-fav", n > 0 && favorites == n ? "Quitar favorito" : "Favorito", Tone::Normal, n > 0,
       "Los favoritos nunca los borra el límite de espacio"},
      {"sel-discord", "Para Discord", Tone::Normal, exportable > 0,
       exportable > 0
           ? "Una copia de menos de 20 MB de " + Count(exportable, "vídeo", "vídeos") +
                 ", uno detrás de otro. Al acabar se copian para pegarlos en Discord."
           : std::string("Elige algún vídeo: las capturas, los audios y las copias para Discord no se exportan")},
      {"sel-join", "Unir", Tone::Normal, n >= 2 && videos == n,
       n >= 2 && videos == n ? "Del más antiguo al más reciente, sin recodificar: solo vídeos grabados con la misma "
                               "calidad, resolución y audio. El resultado es un vídeo nuevo; los originales no cambian."
                             : "Elige dos vídeos o más (sin capturas) para unirlos"},
      {"sel-delete", "Borrar", Tone::Danger, n > 0, "A la papelera de reciclaje (Supr)"},
  };
  constexpr int kActions = int(sizeof(actions) / sizeof(actions[0]));

  // Una fila si cabe con el texto; si no (ventana estrecha), el texto arriba y los botones debajo.
  const float fs = Px(kSmall), gap = Px(8), pad = Px(16), closeW = Px(30);
  float widths[kActions];
  float total = closeW;
  for (int i = 0; i < kActions; ++i) {
    widths[i] = std::max(Px(64), TextSize(ImGui::GetFont(), fs, actions[i].label.c_str()).x + Px(26));
    total += widths[i] + gap;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float barW = ImGui::GetContentRegionAvail().x - Px(12);
  const bool oneRow = barW - pad - Px(10) - total >= Px(170);
  const float topH = oneRow ? Px(48) : Px(40);
  const float barH = oneRow ? topH : topH + Px(30) + Px(12);
  // El marco de las tarjetas del diseño, con el acento a la izquierda.
  TileFrame(dl, q, ImVec2(q.x + barW, q.y + barH), 0.4f);
  dl->AddRectFilled(q, ImVec2(q.x + Px(3), q.y + barH), C(p.accent));
  const float textW = oneRow ? barW - pad - Px(10) - total - Px(12) : barW - pad - Px(10) - closeW - Px(12);
  Text(dl, ImGui::GetFont(), fs, ImVec2(q.x + pad, q.y + (topH - fs) * 0.5f), C(n > 0 ? p.text : p.textMuted),
       Ellipsize(ImGui::GetFont(), fs, text, textW).c_str());

  if (oneRow) ImGui::SetCursorScreenPos(ImVec2(q.x + barW - Px(10) - total, q.y + (barH - Px(30)) * 0.5f));
  else ImGui::SetCursorScreenPos(ImVec2(q.x + pad, q.y + topH));
  int pressed = -1;
  for (int i = 0; i < kActions; ++i) {
    if (i > 0) ImGui::SameLine(0, gap);
    ImGui::BeginDisabled(!actions[i].enabled);
    if (GhostButton(actions[i].id, actions[i].label.c_str(), widths[i], actions[i].tone, 30)) pressed = i;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", actions[i].tip.c_str());
  }
  if (oneRow) ImGui::SameLine(0, gap);
  else ImGui::SetCursorScreenPos(ImVec2(q.x + barW - Px(10) - closeW, q.y + (topH - closeW) * 0.5f));
  const bool close = IconButton("sel-close", Icon::Close, closeW, "Dejar de seleccionar (Esc)");
  ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + barH));
  Gap(12);

  switch (pressed) {
  case 0:
    if (allShown) {
      for (const auto* item : list) selected_.erase(item->path);
      selectMode_ = true; // la barra se queda para elegir otros
    } else {
      for (const auto* item : list) selected_.insert(item->path);
    }
    break;
  case 1: FavoriteSelection(m, a); break;
  case 2: ExportSelection(m, a); break;
  case 3: JoinSelection(m, a); break;
  case 4: AskDeleteSelection(m); break;
  default: break;
  }
  if (close) EndSelection();
}

void AppUi::PageLibrary(AppWindow& w, AppModel& m, const AppActions& a) {
  const Palette& p = Colors();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(CurrentStyle().pad), Px(CurrentStyle().pad * 0.8f)));
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
  const ImVec2 titlePos = ImGui::GetCursorScreenPos();
  const float titleW = ImGui::GetContentRegionAvail().x - Px(12);
  PageTitle("Biblioteca", subtitle, 2);
  if (!m.items.empty()) {
    // «Seleccionar», a la derecha del título: el clic elige en vez de abrir.
    const ImVec2 back = ImGui::GetCursorScreenPos();
    const bool selecting = Selecting();
    const char* label = selecting ? "Cancelar" : "Seleccionar";
    const float bw = std::max(Px(110), TextSize(ImGui::GetFont(), Px(kSmall), label).x + Px(28));
    ImGui::SetCursorScreenPos(ImVec2(titlePos.x + titleW - bw, titlePos.y + PageTitleActionY(28)));
    if (GhostButton("select", label, bw, Tone::Normal, 28)) {
      if (selecting) EndSelection();
      else selectMode_ = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(selecting ? "Deja de seleccionar (Esc)"
                                  : "Elige varios para borrarlos, marcarlos como favoritos, exportarlos o unirlos. "
                                    "También con Ctrl+clic, Mayús+clic o Ctrl+A");
    ImGui::SetCursorScreenPos(back);
  }

  // Barra de filtros
  {
    static const std::vector<std::pair<int, std::string>> filters = {
        {0, "Todo"}, {1, "Clips"}, {2, "Grabaciones"}, {3, "Capturas"}, {4, "Favoritos"}};
    float avail = ImGui::GetContentRegionAvail().x;
    // Juego, orden, «Al azar» y sus huecos; la búsqueda se lleva lo que sobre.
    // Si le quedan menos de 160 px el filtro se estrecha, y lo que no quepa
    // (ventana pequeña o letras anchas) baja a otra línea en vez de salirse.
    const float fixedW = Px(12) + Px(170) + Px(8) + Px(150) + Px(8) + Px(84) + Px(8);
    float segW = std::min(Px(470), avail * 0.5f);
    if (avail - segW - fixedW < Px(160)) segW = std::max(Px(400), avail - fixedW - Px(160));
    segW = std::min(segW, avail);
    const float rowRight = ImGui::GetCursorScreenPos().x + avail;
    const auto next = [&](float width, float gap) {
      if (ImGui::GetItemRectMax().x + gap + width <= rowRight) ImGui::SameLine(0, gap);
      else Gap(4);
    };
    if (int v = Segmented("filter", filters, filter_, segW); v >= 0) filter_ = v;
    next(Px(170), Px(12));
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
    next(Px(150), Px(8));
    ImGui::SetNextItemWidth(Px(150));
    ChoiceCombo<int>("##sort", sort_, {{0, "Más recientes"}, {1, "Más antiguos"}, {2, "Más grandes"}, {3, "Más largos"}});
    next(Px(84), Px(8));
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
    next(Px(120), Px(8));
    float searchW = std::max(Px(120), ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(searchW);
    ImGui::InputTextWithHint("##search", "Buscar", &search_);
  }
  Gap(16);

  // Los elegidos que ya no están (borrados, renombrados) salen de la selección:
  // una pasada por la biblioteca, sin buscar cada uno en toda la lista.
  if (!selected_.empty()) {
    const auto chosen = SelectedItems(m);
    if (chosen.size() < selected_.size()) {
      std::unordered_set<std::string> still;
      for (const auto* item : chosen) still.insert(item->path);
      selected_ = std::move(still);
    }
  }
  ImGuiIO& io = ImGui::GetIO();
  if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) && !TypingText()) {
    // Ctrl+A elige todo lo que se ve; Supr lo elegido va a la papelera; Esc deja de elegir.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
      for (const auto* item : list) selected_.insert(item->path);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !selected_.empty()) AskDeleteSelection(m);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && Selecting()) EndSelection();
  }
  if (Selecting()) SelectionBar(m, a, list);
  if (m.exportBatchTotal > 0) {
    // «Para Discord» de varios: por cuál va, con una barra de toda la tanda.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const float lineW = ImGui::GetContentRegionAvail().x - Px(12);
    const int current = std::min(m.exportBatchDone + 1, m.exportBatchTotal);
    std::string text = "Exportando para Discord " + std::to_string(current) + " de " + std::to_string(m.exportBatchTotal);
    if (m.exportPercent >= 0) text += "  ·  " + std::to_string(m.exportPercent) + " %";
    Text(dl, ImGui::GetFont(), Px(kSmall), q, C(p.textMuted), text.c_str());
    const char* cancel = "Cancelar";
    ImGui::SetCursorScreenPos(ImVec2(q.x + lineW - TextSize(ImGui::GetFont(), Px(kHint), cancel).x, q.y));
    if (LinkButton("cancel-batch", cancel) && a.cancelExport) a.cancelExport();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cancela este y los que esperan; los ya hechos se quedan");
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + Px(kSmall) + Px(8)));
    const float fraction =
        (float(m.exportBatchDone) + float(std::max(0, m.exportPercent)) / 100.0f) / float(m.exportBatchTotal);
    Bar(ImVec2(lineW, Px(2)), fraction, p.accent);
    Gap(12);
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
  const float gap = Px(CurrentStyle().gap);
  // Hueco a la derecha para la barra de desplazamiento (y el contorno de fuera de Neón).
  float avail = ImGui::GetContentRegionAvail().x - Px(12);
  int cols = std::max(1, int((avail + gap) / (Px(250) + gap)));
  float cardW = std::floor((avail - gap * float(cols - 1)) / float(cols));
  float rowH = MediaCardHeight(cardW) + gap;
  int rows = int((list.size() + size_t(cols) - 1) / size_t(cols));
  const bool selecting = Selecting();
  int clicked = -1;
  bool openItemMenu = false, openSelectionMenu = false;
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
        const int selection = Selected(item.path) ? 1 : 0;
        if (Card(w, item, cardW, &menu, selection, int(i) + 1)) {
          // Eligiendo (o con Ctrl o Mayús), el clic elige en vez de abrir.
          if (selecting || io.KeyCtrl || io.KeyShift) clicked = int(i);
          else View(m, item.path, AppPage::Library);
        }
        if (menu) {
          // Sobre uno de varios elegidos, el menú es el de toda la selección.
          if (selection > 0 && selected_.size() > 1) {
            openSelectionMenu = true;
          } else {
            menuPath_ = item.path;
            openItemMenu = true;
          }
        }
        ImGui::PopID();
      }
      ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    }
  }
  // Fuera del PushID de la tarjeta: ItemMenu y SelectionMenu buscan el menú con el id de aquí.
  if (openItemMenu) ImGui::OpenPopup("item-menu");
  if (openSelectionMenu) ImGui::OpenPopup("selection-menu");
  if (!selectToggle_.empty()) {
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i]->path == selectToggle_) clicked = int(i);
    selectToggle_.clear();
  }
  if (clicked >= 0) ClickSelect(list, size_t(clicked), io.KeyShift);
  ItemMenu(m, a);
  SelectionMenu(m, a);
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
  const bool photoEditable = PhotoEditable(item);

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
    const int actions = editable ? 8 : photoEditable ? 7 : 6;
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
    if (photoEditable) {
      if (IconButton("edit", Icon::Sliders, rowH,
                     "Editar (E): recortar, girar, color, flechas, texto, tapar un nombre... El original no cambia.")) {
        Edit(m, item.path);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
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
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && photoEditable) {
      Edit(m, item.path);
      ImGui::EndChild();
      ImGui::PopStyleVar();
      return;
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
  if (m.editPath.empty()) {
    EditorPicker(w, m);
    return;
  }
  const MediaItem* found = Find(m, m.editPath);
  if (!found || (!Editable(*found) && !PhotoEditable(*found))) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(28), Px(20)));
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (GhostButton("back", "Volver", Px(110))) {
      if (m.viewBack == AppPage::Editor) m.editPath.clear();
      else Navigate(m, m.viewBack);
    }
    Gap(10);
    Help(m.itemsLoaded ? "Este archivo ya no está en la carpeta." : "Leyendo la carpeta...");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return;
  }
  const MediaItem item = *found; // copia: la lista puede cambiar en este frame
  lastEdit_ = item.path;
  std::string view;
  bool back = false;
  if (item.Video()) {
    if (photo_.Active()) photo_.Leave();
    if (!editor_.Active() || editor_.Path() != item.path) editor_.Open(item.path);
    const EditorUi::Exit exit = editor_.Draw(w, m, a, item);
    view = exit.view;
    back = exit.back;
  } else {
    if (editor_.Active()) editor_.Leave();
    if (!photo_.Active() || photo_.Path() != item.path) photo_.Open(item.path);
    const PhotoEditorUi::Exit exit = photo_.Draw(w, m, a, item);
    view = exit.view;
    back = exit.back;
  }
  // Abierto desde la pestaña Editor: Volver (y Volver desde la copia que se
  // abre en el visor) lleva a elegir otro.
  const bool fromTab = m.viewBack == AppPage::Editor;
  if (!view.empty()) {
    LeaveEditors();
    if (fromTab) m.editPath.clear();
    View(m, view, m.viewBack);
  } else if (back) {
    LeaveEditors();
    if (fromTab) {
      m.editPath.clear();
    } else {
      m.viewPath = item.path;
      m.page = AppPage::Viewer;
    }
  }
}

void AppUi::EditorPicker(AppWindow& w, AppModel& m) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(CurrentStyle().pad), Px(CurrentStyle().pad * 0.8f)));
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  PageTitle("Editor", "Elige un vídeo o una captura para editarla. Se guarda una copia: el original no cambia.",
            PageNumber(AppPage::Editor, m.settingsPage));

  // Vídeos o capturas, y buscar por nombre o juego.
  std::vector<const MediaItem*> list;
  const std::string needle = Lower(pickSearch_);
  for (const auto& item : m.items) {
    if (pickFilter_ == 0 ? !Editable(item) : !PhotoEditable(item)) continue;
    if (!needle.empty() && Lower(item.name).find(needle) == std::string::npos &&
        Lower(Title(item)).find(needle) == std::string::npos)
      continue;
    list.push_back(&item);
  }
  {
    static const std::vector<std::pair<int, std::string>> filters = {{0, "Vídeos"}, {1, "Capturas"}};
    const float segW = std::min(Px(240), ImGui::GetContentRegionAvail().x * 0.4f);
    if (int v = Segmented("pick-filter", filters, pickFilter_, segW); v >= 0) pickFilter_ = v;
    ImGui::SameLine(0, Px(12));
    // Lo último que se editó, para seguir donde se dejó (los cambios siguen ahí).
    const MediaItem* last = lastEdit_.empty() ? nullptr : Find(m, lastEdit_);
    if (last && (Editable(*last) || PhotoEditable(*last))) {
      const std::string label = Ellipsize(ImGui::GetFont(), Px(kSmall), "Seguir con " + last->name, Px(260));
      const float bw = TextSize(ImGui::GetFont(), Px(kSmall), label.c_str()).x + Px(28);
      if (GhostButton("resume", label.c_str(), bw)) {
        m.viewBack = AppPage::Editor;
        Edit(m, last->path);
      }
      ImGui::SameLine(0, Px(12));
    }
    ImGui::SetNextItemWidth(std::max(Px(120), ImGui::GetContentRegionAvail().x - Px(12)));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (Px(32) - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::InputTextWithHint("##pick-search", "Buscar", &pickSearch_);
  }
  Gap(16);

  if (list.empty()) {
    if (!m.itemsLoaded) Help("Leyendo la carpeta...");
    else if (!needle.empty()) Help("Nada coincide con la búsqueda.");
    else if (pickFilter_ == 0) Help("Aún no hay vídeos. Los clips y grabaciones que guardes aparecerán aquí.");
    else {
      const std::string key = m.config.hotkeys.screenshot.empty() ? "la tecla de captura" : m.config.hotkeys.screenshot;
      Help(("Aún no hay capturas. Las de " + key + " y los fotogramas que guardes aparecerán aquí.").c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return;
  }

  // Rejilla como la de la biblioteca: un clic abre el editor.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("pick-grid", ImVec2(0, 0), ImGuiChildFlags_None);
  DrawHoverTrail(ImGui::GetWindowDrawList());
  const float gap = Px(CurrentStyle().gap);
  const float avail = ImGui::GetContentRegionAvail().x - Px(12);
  const int cols = std::max(1, int((avail + gap) / (Px(250) + gap)));
  const float cardW = std::floor((avail - gap * float(cols - 1)) / float(cols));
  const float rowH = MediaCardHeight(cardW) + gap;
  const int rows = int((list.size() + size_t(cols) - 1) / size_t(cols));
  std::string open;
  ImGuiListClipper clipper;
  clipper.Begin(rows, rowH);
  while (clipper.Step()) {
    for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
      for (int c = 0; c < cols; ++c) {
        const size_t i = size_t(r) * size_t(cols) + size_t(c);
        if (i >= list.size()) break;
        if (c > 0) ImGui::SameLine(0, gap);
        ImGui::PushID(int(i));
        bool menu = false; // aquí no hay menú: se elige con un clic
        if (Card(w, *list[i], cardW, &menu, -1, int(i) + 1)) open = list[i]->path;
        ImGui::PopID();
      }
      ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  if (!open.empty()) {
    m.viewBack = AppPage::Editor;
    Edit(m, open);
  }
}

// ------------------------------------------------------------------ ajustes

void AppUi::PageSettings(AppWindow& w, AppModel& m, const AppActions& a) {
  (void)w;
  const Palette& p = Colors();
  if (!m.settings.devicesLoaded && a.loadDevices) a.loadDevices();
  const float footerH = Px(kFooterHeight);
  const Style& st = CurrentStyle();
  // Con raíl o pestañas, los apartados van en una columna a la izquierda.
  const bool column = st.nav != NavLayout::Sidebar;
  if (column) {
    SettingsNav(m);
    ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(st.pad), Px(st.pad * 0.8f)));
  ImGui::BeginChild("page", ImVec2(0, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
  DrawHoverTrail(ImGui::GetWindowDrawList());
  PageTitle(SettingsPageName(m.settingsPage), "", PageNumber(AppPage::Settings, m.settingsPage), "Ajustes");
  DrawSettingsPage(m.settingsPage, m.settings, a.settings);
  Gap(16);
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // Pie: aplicar o descartar (tapa el fondo ilustrado del tema).
  ImVec2 q = ImGui::GetCursorScreenPos();
  float wAvail = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(q, ImVec2(q.x + wAvail, q.y + footerH), C(p.bg));
  dl->AddRectFilled(q, ImVec2(q.x + wAvail, q.y + LineW()), C(p.line));
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
  if (column) ImGui::EndGroup();
}

} // namespace spectre
