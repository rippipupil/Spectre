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

Fonts FontsOf(const OverlayWindow& w) { return Fonts{w.Scale(), w.BoldFont(), w.BigFont()}; }

bool ClipRow(int index, const ClipFile& clip) {
  const Palette& p = Colors();
  ImGui::PushID(index);
  const float h = Px(30);
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  bool pressed = ImGui::InvisibleButton("clip", size);
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
  if (hov) dl->AddRectFilled(rowA, rowB, C(p.surface));
  float mid = (a.y + b.y) * 0.5f;
  DrawIcon(dl, Icon::Play, ImVec2(a.x + Px(5), mid), Px(5), C(hov ? p.text : p.textMuted));
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
  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(held ? p.surfaceActive : hov ? p.surfaceHover : p.surface));
  if (hov) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (tooltip) ImGui::SetTooltip("%s", tooltip);
  }
  DrawIcon(dl, icon, (a + b) * 0.5f, size * 0.2f, C(hov ? p.text : p.textMuted));
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
    alpha = Breathe();
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
// lo que quede fuera de la zona de clic, así que fuera no se vería. Usa el
// tamaño del frame anterior.
void DrawOpenGhost(const MenuModel& m, float width) {
  if (!ThemeMotion() || m.panelHeight <= 0) return;
  float t = float(GetTickCount64() - m.openedAt) / 240.0f;
  if (t >= 1.0f) return;
  t = std::clamp(t, 0.0f, 1.0f);
  float ease = 1.0f - (1.0f - t) * (1.0f - t);
  float inset = Px(10) * (1.0f - ease);
  float top = ImGui::GetIO().DisplaySize.y * 0.5f - m.panelHeight * 0.5f;
  ImGui::GetForegroundDrawList()->AddRect(ImVec2(inset, top + inset),
                                          ImVec2(width - inset, top + m.panelHeight - inset),
                                          ImGui::GetColorU32(Colors().accent, 0.55f * (1.0f - ease)), 0, 0, Px(1));
}

std::string WithKey(const char* text, const std::string& key) {
  return key.empty() ? std::string(text) : std::string(text) + " (" + key + ")";
}

} // namespace

void DrawPanel(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  BeginFrame(FontsOf(w), m.config);
  // La sombra de fila no sobrevive a una reapertura: el reloj de ImGui vuelve
  // a cero con cada contexto nuevo y un resto antiguo parpadearía.
  ResetHoverTrail(m.openedAt);
  const Palette& p = Colors();
  const float appear = OpenProgress(m.openedAt);
  const float width = Px(kPanelWidth);

  DrawOpenGhost(m, width);

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(16), Px(16)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8), Px(4)));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V(p.bg));
  ImGui::PushStyleColor(ImGuiCol_Border, V(p.line));
  // Centrado en vertical dentro de la franja; App calcula la zona de clic igual.
  // Si no cabe (monitor pequeño con escala alta), la rueda del ratón lo desplaza.
  ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, ImGui::GetIO().DisplaySize.y));
  ImGui::Begin("##panel", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& c = m.config;
  DrawHoverTrail(dl);

  // Cabecera: marca, cerrar y estado.
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float rowH = Px(28);
    Wordmark(dl, ImVec2(q.x, q.y + (rowH - Px(kSmall)) * 0.5f), "SPECTRE", C(p.text));
    ImGui::SetCursorScreenPos(ImVec2(q.x + ImGui::GetContentRegionAvail().x - rowH, q.y));
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
    const float bh = Px(44), gap = Px(6);
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
      ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, wrap, text);
      ImVec2 end(q.x + w, q.y + ts.y + pad * 2);
      // Como los avisos: plano, borde de 1 px y una barra a la izquierda.
      dl->AddRectFilled(q, end, C(p.surface));
      dl->AddRect(q + ImVec2(0.5f, 0.5f), end - ImVec2(0.5f, 0.5f), C(p.line), 0, 0, Px(1));
      dl->AddRectFilled(q, ImVec2(q.x + bar, end.y), C(p.warning));
      dl->AddText(ImGui::GetFont(), fs, ImVec2(q.x + bar + pad, q.y + pad), C(p.text), text, nullptr, wrap);
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
  for (int i = 0; i < int(std::min<size_t>(m.clips.size(), 3)); ++i) {
    const auto& clip = m.clips[size_t(i)];
    if (ClipRow(i, clip)) a.openClip(clip.path);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.showInFolder(clip.path);
  }

  Divider(6, 8);
  float half = (ImGui::GetContentRegionAvail().x - Px(8)) * 0.5f;
  if (GhostButton("app", "Abrir Spectre", half, Tone::Normal, 34)) a.openApp();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Biblioteca, reproductor y todos los ajustes");
  ImGui::SameLine(0, Px(8));
  if (GhostButton("folder", "Carpeta", half, Tone::Normal, 34)) a.openPath(m.outputDir);

  m.panelWidth = ImGui::GetWindowWidth();
  m.panelHeight = ImGui::GetWindowHeight();
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(5);

  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) a.close();
}

void DrawToast(OverlayWindow& w, const Config& config, const std::string& text, bool error, float alpha) {
  BeginFrame(FontsOf(w), config);
  const Palette& p = Colors();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::Begin("toast", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 a(0, 0), b(io.DisplaySize.x, io.DisplaySize.y);
  // Rectángulo plano con borde de 1 px y una barra de 3 px a la izquierda:
  // del acento al guardar o grabar, roja si es un error.
  dl->AddRectFilled(a, b, C(p.bg));
  dl->AddRect(a + ImVec2(0.5f, 0.5f), b - ImVec2(0.5f, 0.5f), C(p.line), 0, 0, Px(1));
  dl->AddRectFilled(a, ImVec2(a.x + Px(3), b.y), C(error ? p.danger : p.accent));
  if (!error) DrawTrace(dl, a, b, p.accent);
  float x = a.x + Px(18);
  float wrap = b.x - x - Px(16);
  float fs = Px(kBody);
  ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, wrap, text.c_str());
  dl->AddText(ImGui::GetFont(), fs, ImVec2(x, (a.y + b.y - ts.y) * 0.5f), C(p.text), text.c_str(), nullptr, wrap);
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

} // namespace spectre
