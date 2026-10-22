#include "ui/Menu.h"

#include "common/Util.h"
#include "ui/OverlayWindow.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace spectre {

using namespace ui;

namespace {

Fonts FontsOf(const OverlayWindow& w) { return Fonts{w.Scale(), w.BoldFont(), w.BigFont(), &w.FontSizes()}; }

bool ClipRow(int index, const ClipFile& clip) {
  const Palette& p = Colors();
  ImGui::PushID(index);
  const float h = RowHeight(30);
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  bool pressed = ImGui::InvisibleButton("clip", size);
  Motion m = Animate();
  bool hov = ImGui::IsItemHovered();
  ImGuiID id = ImGui::GetItemID();
  ImGui::PopID();
  if (hov) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip("Clic para verlo en Spectre, clic derecho para mostrarlo en la carpeta");
  }
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 rowA(a.x - Px(8), a.y), rowB(b.x + Px(8), b.y);
  TrackHover(id, rowA, rowB, hov);
  PaintButton(dl, rowA, rowB, Role::Quiet, m);
  PaintPress(dl, rowA, rowB, Role::Quiet, m);
  float mid = (a.y + b.y) * 0.5f;
  DrawIcon(dl, Icon::Play, ImVec2(a.x + Px(5), mid), Px(5), C(Mix(p.textMuted, p.text, m.hover)));
  float ss = Px(kSmall);
  float timeW = TextSize(ImGui::GetFont(), ss, clip.when.c_str()).x;
  std::string name = Ellipsize(ImGui::GetFont(), ss, clip.name, b.x - a.x - timeW - Px(30));
  Text(dl, ImGui::GetFont(), ss, ImVec2(a.x + Px(20), mid - ss * 0.5f), C(p.text), name.c_str());
  Text(dl, ImGui::GetFont(), ss, ImVec2(b.x - timeW, mid - ss * 0.5f), C(p.textFaint), clip.when.c_str());
  return pressed;
}

// Botón cuadrado con icono, del alto de los botones grandes (captura junto a grabar).
bool SquareButton(const char* id, Icon icon, float size, const char* tooltip) {
  const Palette& p = Colors();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  Motion m = Animate();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ButtonPaint bp = PaintButton(dl, a, b, Role::Normal, m);
  if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (tooltip) ImGui::SetTooltip("%s", tooltip);
  }
  DrawIcon(dl, icon, (bp.a + bp.b) * 0.5f, (bp.b.x - bp.a.x) * 0.2f, C(Mix(p.textMuted, bp.fg, m.hover)));
  PaintPress(dl, a, b, Role::Normal, m);
  return pressed;
}

void PanelStatus(const MenuModel& m) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q = ImGui::GetCursorScreenPos();
  float ss = Px(kSmall);
  ImU32 mark;
  bool filled = true;
  float alpha = 1.0f;
  std::string text;
  if (m.stats.recording) {
    mark = p.danger;
    text = "Grabando " + FormatDuration(m.stats.recordSeconds);
    // La marca late mientras se graba (en los dos temas: es el estado, no adorno).
    alpha = 0.55f + 0.45f * std::cos(float(GetTickCount64() % 1600) / 1600.0f * kPi * 2);
  } else if (m.replayActive) {
    mark = p.accent;
    text = "Replay activo, guarda los últimos " + FormatClipLength(m.config.clipSeconds);
    alpha = AmbientMotion() == Ambient::Cursor ? Blink() : Breathe();
  } else if (m.config.replayEnabled && m.config.onlyInGames) {
    mark = p.textMuted;
    filled = false;
    text = "Replay esperando a un juego";
  } else {
    mark = p.lineStrong;
    text = "Replay apagado";
  }
  float h = ss + Px(2);
  DrawMark(dl, ImVec2(q.x + Px(4), q.y + h * 0.5f), Px(3.5f), C(mark, alpha), filled);
  Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(16), q.y + (h - ss) * 0.5f),
       C(m.stats.recording ? p.text : p.textMuted), text.c_str());
  if (m.gameDetected && !m.gameName.empty()) {
    std::string game = Ellipsize(ImGui::GetFont(), ss, m.gameName, ImGui::GetContentRegionAvail().x - Px(16));
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(16), q.y + h + Px(2)), C(p.textFaint), game.c_str());
    h += ss + Px(4);
  }
  ImGui::Dummy(ImVec2(0, h));
}

// Rastro fantasma del tema Psíquico: un segundo contorno aparece 10 px por
// dentro del panel y se funde en 240 ms mientras llega al borde. Va por encima
// del panel y dentro de su rectángulo: el panel es opaco y la ventana recorta
// lo que quede fuera de la zona de clic, así que fuera no se vería. Usa la
// posición y el tamaño del frame anterior.
void DrawOpenGhost(const MenuModel& m, float width) {
  if (!ThemeMotion() || CurrentStyle().toast != ToastLook::Ghost || m.panelHeight <= 0) return;
  float t = float(GetTickCount64() - m.openedAt) / 240.0f;
  if (t >= 1.0f) return;
  t = std::clamp(t, 0.0f, 1.0f);
  float ease = 1.0f - (1.0f - t) * (1.0f - t);
  float inset = Px(10) * (1.0f - ease);
  float top = m.panelTop;
  ImGui::GetForegroundDrawList()->AddRect(ImVec2(inset, top + inset),
                                          ImVec2(width - inset, top + m.panelHeight - inset),
                                          ImGui::GetColorU32(Colors().accent, 0.55f * (1.0f - ease)), 0, 0, Px(1));
}

std::string WithKey(const char* text, const std::string& key) {
  return key.empty() ? std::string(text) : std::string(text) + " (" + key + ")";
}

// Detalles del diseño sobre el marco del panel (dentro de su rectángulo: la
// ventana recorta lo que quede fuera de la zona de clic).
void PanelDecor(ImDrawList* dl, ImVec2 a, ImVec2 b, bool onRight) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  switch (st.toast) {
  case ToastLook::Header:
    // Táctico: una franja del acento en el lado que da al centro de la pantalla.
    if (onRight) dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + Px(2), b.y), C(p.accent));
    else dl->AddRectFilled(ImVec2(b.x - Px(2), a.y), ImVec2(b.x, b.y), C(p.accent));
    break;
  case ToastLook::Terminal:
    DrawBrackets(dl, a + ImVec2(Px(4), Px(4)), b - ImVec2(Px(4), Px(4)), C(p.lineStrong), Px(16));
    break;
  case ToastLook::TopLine:
    dl->AddRectFilled(a, ImVec2(b.x, a.y + Px(2)), C(p.accent));
    break;
  default:
    break;
  }
}

} // namespace

void DrawPanel(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  BeginFrame(FontsOf(w), m.config);
  // La sombra de fila no sobrevive a una reapertura: el reloj de ImGui vuelve
  // a cero con cada contexto nuevo y un resto antiguo parpadearía.
  ResetHoverTrail(m.openedAt);
  const Palette& p = Colors();
  const float width = Px(kPanelWidth);
  const bool lateral = IsLateralPanel(m.config);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  // Entra desde su lado de la pantalla, con el movimiento del diseño.
  const bool onRight = (lateral ? m.config.panelLateralX : m.config.panelCompactX) > 0.5f;
  const Entrance enter = EnterMotion(m.openedAt, onRight);
  const float appear = ThemeMotion() ? enter.alpha : OpenProgress(m.openedAt);
  bool startDrag = false;

  DrawOpenGhost(m, width);

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(16), Px(16)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  // Suave va sin borde; Alto contraste, con 2 px.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                      CurrentStyle().divider == DividerStyle::Space ? 0.0f : CurrentStyle().line);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8), Px(4)));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V(p.bg));
  ImGui::PushStyleColor(ImGuiCol_Border, V(p.line));
  // La ventana es una franja del ancho del panel (App::MenuRect). Lateral: el
  // panel la llena de arriba abajo. Compacto: del alto de su contenido, a la
  // altura guardada (0 arriba, 1 abajo; 0,5 es el centrado de antes). App
  // calcula la zona de clic con panelTop. Si no cabe (monitor pequeño con
  // escala alta), la rueda del ratón lo desplaza.
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
  if (lateral) {
    ImGui::SetNextWindowPos(enter.offset);
    ImGui::SetNextWindowSize(display);
  } else {
    const float fy = m.config.panelCompactY;
    ImGui::SetNextWindowPos(ImVec2(0, display.y * fy) + enter.offset, ImGuiCond_Always, ImVec2(0, fy));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, display.y));
    flags |= ImGuiWindowFlags_AlwaysAutoResize;
  }
  ImGui::Begin("##panel", nullptr, flags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& c = m.config;
  DrawHoverTrail(dl);

  // Cabecera: marca, cerrar y estado. La fila de la marca es el asa para
  // mover el panel (también vale cualquier hueco sin controles).
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float rowH = Px(28);
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("move", ImVec2(avail - rowH - Px(4), rowH));
    if (ImGui::IsItemActivated()) startDrag = true;
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (!m.dragging && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
      ImGui::SetTooltip("Arrastra para mover el panel");
    Wordmark(dl, ImVec2(q.x, q.y + (rowH - Px(kSmall)) * 0.5f), "SPECTRE", C(p.text));
    ImGui::SetCursorScreenPos(ImVec2(q.x + avail - rowH, q.y));
    if (IconButton("close", Icon::Close, rowH, "Cerrar (Esc)")) a.close();
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(6)));
    PanelStatus(m);
  }
  Divider(12, 12);

  if (ActionButton("clip", "Guardar clip", c.hotkeys.saveClip, Tone::Primary, m.replayActive)) a.saveClip();

  Gap(10);
  SectionLabel("DURACIÓN DEL CLIP");
  Gap(2);
  if (int v = ClipLengthPicker("len", c.clipSeconds, m.stats.replayFitSeconds); v > 0 && v != c.clipSeconds)
    a.setClipSeconds(v);

  Gap(6);
  {
    const float bh = ActionHeight(), gap = Px(6);
    float recW = ImGui::GetContentRegionAvail().x - bh - gap;
    if (m.stats.recording) {
      if (ActionButton("rec", "Detener grabación", c.hotkeys.toggleRecording, Tone::Danger, true, false, recW))
        a.toggleRecording();
    } else {
      if (ActionButton("rec", "Grabar pantalla", c.hotkeys.toggleRecording, Tone::Normal, true, true, recW))
        a.toggleRecording();
    }
    ImGui::SameLine(0, gap);
    std::string tip = WithKey("Captura de pantalla", c.hotkeys.screenshot);
    if (SquareButton("shot", Icon::Shot, bh, tip.c_str())) a.screenshot();
  }

  Divider(12, 6);
  SectionLabel("CAPTURA");
  if (ToggleRow("replay", Icon::Replay, "Replay", c.replayEnabled)) a.toggleReplay();
  if (ToggleRow("mic", Icon::Mic, "Micrófono", c.captureMic)) a.toggleMic();
  if (ToggleRow("cam", Icon::Camera, "Cámara", c.cameraEnabled)) a.toggleCamera();

  Divider(6, 8);
  SectionLabel("CALIDAD");
  Gap(2);
  {
    // Cambiarla reinicia la captura (el aviso lo dice). Personalizada se elige en la app.
    static const std::vector<std::pair<int, std::string>> qualities = {
        {1, "Baja"}, {2, "Media"}, {3, "Alta"}, {4, "Máx."}, {5, "Fuente"}};
    static const char* ids[] = {"", "baja", "media", "alta", "maxima", "fuente"};
    constexpr int kSource = 5;
    int current = 0;
    for (int i = 1; i <= kSource; ++i)
      if (c.quality == ids[i]) current = i;
    if (int v = Segmented("quality", qualities, current); v > 0) {
      // Fuente gasta mucho más que el resto: antes de activarla, el aviso de
      // debajo lo explica. Cualquier otro segmento lo descarta.
      m.confirmSource = v == kSource && current != kSource;
      if (!m.confirmSource && v != current) a.setQuality(ids[v]);
    }
    if (m.confirmSource) {
      Gap(8);
      const char* text = "Fuente graba a la resolución nativa y a los Hz del monitor, con el bitrate más alto "
                         "que da tu gráfica. Gasta mucha más RAM, GPU y disco: si el juego va a tirones, vuelve "
                         "a Alta.";
      const float w = ImGui::GetContentRegionAvail().x;
      const float pad = Px(10), bar = Px(3), fs = Px(kSmall);
      const float wrap = w - bar - pad * 2;
      ImVec2 q = ImGui::GetCursorScreenPos();
      ImFont* font = Sized(ImGui::GetFont(), fs);
      ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, wrap, text);
      ImVec2 end(q.x + w, q.y + ts.y + pad * 2);
      // Una caja del diseño con una barra a la izquierda.
      CardFrame(dl, q, end, p.warning);
      dl->AddText(font, fs, ImVec2(q.x + bar + pad, q.y + pad), C(p.text), text, nullptr, wrap);
      ImGui::Dummy(ImVec2(w, end.y - q.y));
      Gap(6);
      float half = (w - Px(8)) * 0.5f;
      if (GhostButton("source-cancel", "Cancelar", half)) m.confirmSource = false;
      ImGui::SameLine(0, Px(8));
      if (SolidButton("source-ok", "Activar Fuente", half)) {
        m.confirmSource = false;
        a.setQuality(ids[kSource]);
      }
    }
  }

  Divider(12, 6);
  bool viewAll = false;
  SectionLabel("RECIENTES", "Ver todos", &viewAll);
  if (viewAll) a.openLibrary();
  if (m.clips.empty()) {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float ss = Px(kSmall);
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x, q.y + Px(6)), C(p.textFaint), "Aún no hay clips ni grabaciones.");
    ImGui::Dummy(ImVec2(0, ss + Px(12)));
  }
  // Lateral: el pie va pegado abajo, como en una barra lateral, y los recientes
  // llenan el hueco que queda (al menos tres, como el compacto).
  const float spacing = ImGui::GetStyle().ItemSpacing.y;
  const float footerTop = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y -
                          Px(34) - (Px(6) + Px(1) + Px(8) + spacing * 3); // el separador de encima
  size_t shown = std::min<size_t>(m.clips.size(), 3);
  if (lateral) {
    float room = footerTop - ImGui::GetCursorScreenPos().y;
    shown = std::min(m.clips.size(), std::max(shown, size_t(std::max(0.0f, room / (RowHeight(30) + spacing)))));
  }
  for (int i = 0; i < int(shown); ++i) {
    const auto& clip = m.clips[size_t(i)];
    if (ClipRow(i, clip)) a.openClip(clip.path);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.showInFolder(clip.path);
  }
  if (lateral && ImGui::GetCursorScreenPos().y < footerTop)
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, footerTop));

  Divider(6, 8);
  float half = (ImGui::GetContentRegionAvail().x - Px(8)) * 0.5f;
  if (GhostButton("app", "Abrir Spectre", half, Tone::Normal, 34)) a.openApp();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Biblioteca, reproductor y todos los ajustes");
  ImGui::SameLine(0, Px(8));
  if (GhostButton("folder", "Carpeta", half, Tone::Normal, 34)) a.openPath(m.outputDir);

  // Un hueco del panel también sirve de asa: el clic no cae en ningún control.
  if (!m.dragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
      !ImGui::IsAnyItemHovered())
    startDrag = true;
  if (startDrag && !m.dragging) {
    m.dragging = true;
    m.dragFromX = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].x;
    m.dragFromY = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].y;
  } else if (m.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    m.dragging = false;
  }
  if (m.dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

  m.panelWidth = ImGui::GetWindowWidth();
  m.panelHeight = ImGui::GetWindowHeight();
  m.panelTop = ImGui::GetWindowPos().y;
  {
    const ImVec2 wa = ImGui::GetWindowPos(), wb = wa + ImGui::GetWindowSize();
    PanelDecor(dl, wa, wb, onRight);
    // Terminal: una línea barre el panel de arriba abajo y lo va descubriendo.
    if (enter.reveal < 1.0f) {
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      const float y = wa.y + (wb.y - wa.y) * enter.reveal;
      fg->AddRectFilled(ImVec2(wa.x, y), wb, ImGui::GetColorU32(p.bg));
      fg->AddRectFilled(ImVec2(wa.x, y - Px(1)), ImVec2(wb.x, y + Px(1)), ImGui::GetColorU32(p.accent));
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(5);

  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) a.close();
}

} // namespace spectre
