// Antes de cualquier cabecera que incluya imgui.h (ui/Theme.h lo hace).
#define IMGUI_DEFINE_MATH_OPERATORS

#include "ui/Menu.h"

#include "common/Util.h"
#include "ui/OverlayWindow.h"
#include "ui/Theme.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#ifndef SPECTRE_VERSION
#define SPECTRE_VERSION "0.1.0"
#endif

namespace spectre {

namespace {

// ------------------------------------------------------------------ estilo
// Reglas del rediseño (docs: investigacion/diseno-interfaz.md): sin esquinas
// redondeadas, sin degradados ni sombras, superficies planas con líneas de
// 1 px, un solo acento por tema y el rojo únicamente para grabar y errores.
// Los colores vienen de ui/Theme.h y pasan por ImGui::GetColorU32 para
// respetar la transparencia de la animación de entrada.

constexpr float kPi = 3.14159265358979f;

// Tamaños de texto en píxeles lógicos (se multiplican por la escala DPI).
constexpr float kBody = 15.0f;  // texto del panel y de los formularios
constexpr float kSmall = 13.0f; // filas de clips, estado, keycaps
constexpr float kHint = 12.0f;  // ayudas y enlaces
constexpr float kLabel = 11.0f; // títulos de sección en mayúsculas
constexpr float kTitle = 22.0f; // título de la página de Ajustes

// Estado del frame actual (fuentes y escala de la ventana que se dibuja).
float s_ = 1.0f;
ImFont* bold_ = nullptr;
ImFont* big_ = nullptr;

void BeginFrameStyle(OverlayWindow& w, const Config& config) {
  s_ = w.Scale();
  bold_ = w.BoldFont();
  big_ = w.BigFont();
  BeginThemeFrame(config);
}

float Px(float logical) { return logical * s_; }

ImU32 C(ImU32 color, float alpha = 1.0f) { return ImGui::GetColorU32(color, alpha); }
ImVec4 V(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  auto ch = [&](int shift) {
    float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
    return ImU32(x + (y - x) * t + 0.5f) << shift;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text) {
  dl->AddText(font, size, pos, color, text);
}

// Texto con espaciado entre letras (marca y títulos de sección). Se dibuja
// glifo a glifo respetando UTF-8; tracking en píxeles.
float TrackedWidth(ImFont* font, float size, const char* text, float tracking) {
  float w = 0;
  int glyphs = 0;
  for (const char* c = text; *c;) {
    const char* next = c + 1;
    while (*next && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
    w += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, next).x;
    ++glyphs;
    c = next;
  }
  return glyphs > 0 ? w + tracking * float(glyphs - 1) : 0.0f;
}

void TrackedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text,
                 float tracking) {
  float x = pos.x;
  for (const char* c = text; *c;) {
    const char* next = c + 1;
    while (*next && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
    dl->AddText(font, size, ImVec2(x, pos.y), color, c, next);
    x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, next).x + tracking;
    c = next;
  }
}

// Recorta el texto con "..." para que quepa en maxWidth (respeta UTF-8).
std::string Ellipsize(ImFont* font, float size, const std::string& text, float maxWidth) {
  if (TextSize(font, size, text.c_str()).x <= maxWidth) return text;
  std::string out = text;
  while (!out.empty()) {
    size_t cut = out.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.erase(cut);
    if (TextSize(font, size, (out + "...").c_str()).x <= maxWidth) break;
  }
  return out + "...";
}

std::string FormatDuration(double seconds) {
  int s = std::max(0, int(seconds));
  char buf[32];
  if (s >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(buf, sizeof(buf), "%02d:%02d", s / 60, s % 60);
  return buf;
}

std::string FormatClipLength(int seconds) {
  char buf[32];
  if (seconds % 60 == 0) snprintf(buf, sizeof(buf), "%d min", seconds / 60);
  else snprintf(buf, sizeof(buf), "%d s", seconds);
  return buf;
}

// ----------------------------------------------------------------- iconos
// Trazo fino y constante a cualquier escala, sin fondos ni círculos detrás.
enum class Icon { Replay, Mic, Camera, Close, Back, Play };

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float r, ImU32 color) {
  const float t = std::max(1.2f, r * 0.17f);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
  switch (icon) {
  case Icon::Replay: {
    const float a0 = -kPi * 0.30f, a1 = kPi * 1.38f, rr = 0.78f * r;
    dl->PathArcTo(c, rr, a0, a1, 28);
    dl->PathStroke(color, 0, t);
    ImVec2 p(c.x + std::cos(a1) * rr, c.y + std::sin(a1) * rr);
    ImVec2 tan(-std::sin(a1), std::cos(a1)), nor(std::cos(a1), std::sin(a1));
    dl->AddTriangleFilled(p + tan * (r * 0.42f), p + nor * (r * 0.30f), p - nor * (r * 0.30f), color);
    break;
  }
  case Icon::Mic:
    dl->AddRect(P(-0.26f, -0.86f), P(0.26f, 0.18f), color, r * 0.26f, 0, t);
    dl->PathArcTo(P(0, -0.08f), r * 0.50f, 0.0f, kPi, 14);
    dl->PathStroke(color, 0, t);
    dl->AddLine(P(0, 0.42f), P(0, 0.80f), color, t);
    dl->AddLine(P(-0.30f, 0.80f), P(0.30f, 0.80f), color, t);
    break;
  case Icon::Camera:
    dl->AddRect(P(-0.85f, -0.48f), P(0.32f, 0.48f), color, 0, 0, t);
    dl->AddLine(P(0.32f, -0.10f), P(0.88f, -0.40f), color, t);
    dl->AddLine(P(0.88f, -0.40f), P(0.88f, 0.40f), color, t);
    dl->AddLine(P(0.88f, 0.40f), P(0.32f, 0.10f), color, t);
    break;
  case Icon::Close:
    dl->AddLine(P(-0.45f, -0.45f), P(0.45f, 0.45f), color, t);
    dl->AddLine(P(0.45f, -0.45f), P(-0.45f, 0.45f), color, t);
    break;
  case Icon::Back:
    dl->AddLine(P(0.22f, -0.55f), P(-0.30f, 0.0f), color, t);
    dl->AddLine(P(-0.30f, 0.0f), P(0.22f, 0.55f), color, t);
    break;
  case Icon::Play: {
    const ImVec2 pts[] = {P(-0.40f, -0.60f), P(0.52f, 0.0f), P(-0.40f, 0.60f)};
    dl->AddPolyline(pts, 3, color, ImDrawFlags_Closed, t);
    break;
  }
  }
}

// Marca de estado: cuadrado en Negro, rombo en Psíquico. Nunca un círculo
// de color: la marca va siempre acompañada del texto que explica el estado.
void DrawMark(ImDrawList* dl, ImVec2 c, float half, ImU32 color, bool filled) {
  if (CurrentTheme().psychic) {
    const float d = half * 1.3f;
    const ImVec2 pts[] = {ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)};
    if (filled) dl->AddConvexPolyFilled(pts, 4, color);
    else dl->AddPolyline(pts, 4, color, ImDrawFlags_Closed, 1.0f * s_);
  } else {
    ImVec2 a(c.x - half, c.y - half), b(c.x + half, c.y + half);
    if (filled) dl->AddRectFilled(a, b, color);
    else dl->AddRect(a, b, color, 0, 0, 1.0f * s_);
  }
}

// ------------------------------------------------------- movimiento del tema
// Animaciones del tema Psíquico. Todas son primitivas de ImGui (unas decenas
// de vértices) y solo se dibujan con el panel abierto y las animaciones
// activadas en Ajustes; con el panel cerrado no existe ni el contexto.

// Sombra psíquica: al cambiar la fila sobre la que está el ratón, la anterior
// se desvanece en 200 ms.
struct HoverTrail {
  ImGuiID current = 0;
  ImVec2 a, b;        // rectángulo actual
  ImVec2 ghostA, ghostB;
  double ghostSince = -10.0;
} trail_;

void TrackHover(ImGuiID id, ImVec2 a, ImVec2 b, bool hovered) {
  if (!hovered) {
    if (trail_.current == id) {
      trail_.ghostA = trail_.a;
      trail_.ghostB = trail_.b;
      trail_.ghostSince = ImGui::GetTime();
      trail_.current = 0;
    }
    return;
  }
  if (trail_.current != id) {
    if (trail_.current != 0) {
      trail_.ghostA = trail_.a;
      trail_.ghostB = trail_.b;
      trail_.ghostSince = ImGui::GetTime();
    }
    trail_.current = id;
  }
  trail_.a = a;
  trail_.b = b;
}

void DrawHoverTrail(ImDrawList* dl) {
  if (!ThemeMotion()) return;
  float t = float(ImGui::GetTime() - trail_.ghostSince) / 0.2f;
  if (t < 0 || t >= 1) return;
  dl->AddRectFilled(trail_.ghostA, trail_.ghostB, C(Colors().accent, 0.12f * (1.0f - t)));
}

// Estela: un segmento de 2 px recorre el borde inferior de un rectángulo
// cada 2,8 s.
void DrawTrace(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color) {
  if (!ThemeMotion()) return;
  const float period = 2.8f, len = Px(34);
  float phase = float(std::fmod(ImGui::GetTime(), double(period))) / period;
  float alpha = phase < 0.1f ? phase / 0.1f : phase > 0.9f ? (1.0f - phase) / 0.1f : 1.0f;
  float x = a.x + (b.x - a.x - len) * phase;
  dl->AddRectFilled(ImVec2(x, b.y - Px(2)), ImVec2(x + len, b.y), C(color, alpha));
}

// Respiración de la marca de estado (2,6 s).
float Breathe() {
  if (!ThemeMotion()) return 1.0f;
  return 0.35f + 0.65f * (0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.0f * kPi / 2.6f));
}

// ----------------------------------------------------------------- widgets

void Gap(float px) { ImGui::Dummy(ImVec2(0, Px(px))); }

void Divider(float above = 10, float below = 10) {
  Gap(above);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + 1.0f * s_), C(Colors().line));
  ImGui::Dummy(ImVec2(0, 1.0f * s_));
  Gap(below);
}

void Wordmark(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 color) {
  TrackedText(dl, bold_, Px(kSmall), pos, color, text, Px(3.2f));
}

// Título de sección en mayúsculas espaciadas; opcionalmente un enlace a la
// derecha (p. ej. "Ver todos").
void SectionLabel(const char* text, const char* link = nullptr, bool* linkClicked = nullptr) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float size = Px(kLabel);
  float h = Px(kHint) + Px(4);
  TrackedText(dl, bold_, size, ImVec2(p.x, p.y + (h - size) * 0.5f), C(Colors().textMuted), text, Px(1.5f));
  if (link) {
    float ls = Px(kHint);
    ImVec2 ts = TextSize(ImGui::GetFont(), ls, link);
    float x = ImGui::GetContentRegionAvail().x - ts.x;
    ImGui::SetCursorScreenPos(ImVec2(p.x + x, p.y));
    bool clicked = ImGui::InvisibleButton(link, ImVec2(ts.x, h));
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    Text(dl, ImGui::GetFont(), ls, ImVec2(p.x + x, p.y + (h - ts.y) * 0.5f),
         C(hov ? Colors().text : Colors().textMuted), link);
    if (hov) dl->AddRectFilled(ImVec2(p.x + x, p.y + h - 1.0f * s_), ImVec2(p.x + x + ts.x, p.y + h), C(Colors().text));
    if (linkClicked) *linkClicked = clicked;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  } else {
    ImGui::Dummy(ImVec2(0, h));
  }
}

// Tecla dibujada como texto con un borde de 1 px, alineada a la derecha.
void DrawKeycap(ImDrawList* dl, ImVec2 rightMid, const std::string& key, ImU32 border, ImU32 fg, float alpha) {
  if (key.empty()) return;
  float size = Px(kHint);
  ImVec2 ts = TextSize(ImGui::GetFont(), size, key.c_str());
  ImVec2 pad(Px(7), Px(3));
  ImVec2 b(rightMid.x, rightMid.y + ts.y * 0.5f + pad.y);
  ImVec2 a(b.x - ts.x - pad.x * 2, rightMid.y - ts.y * 0.5f - pad.y);
  dl->AddRect(a, b, C(border, alpha), 0, 0, 1.0f * s_);
  Text(dl, ImGui::GetFont(), size, a + pad, C(fg, alpha), key.c_str());
}

float KeycapWidth(const std::string& key) {
  if (key.empty()) return 0;
  return TextSize(ImGui::GetFont(), Px(kHint), key.c_str()).x + Px(14);
}

enum class Tone { Normal, Primary, Danger };

// Botón grande del panel: título, atajo y, si se pide, una marca a la
// izquierda (el cuadrado rojo de "Grabar pantalla").
bool ActionButton(const char* id, const char* title, const std::string& key, Tone tone, bool enabled,
                  bool recordMark = false) {
  const Palette& p = Colors();
  const float h = Px(44);
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  ImGui::BeginDisabled(!enabled);
  bool pressed = ImGui::InvisibleButton(id, size);
  ImGui::EndDisabled();
  bool hov = enabled && ImGui::IsItemHovered();
  bool held = enabled && ImGui::IsItemActive();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  ImU32 bg, fg, keyBorder;
  switch (tone) {
  case Tone::Primary:
    bg = held ? p.accentActive : hov ? p.accentHover : p.accent;
    fg = p.onAccent;
    keyBorder = Mix(p.onAccent, bg, 0.45f);
    break;
  case Tone::Danger:
    bg = held ? p.dangerActive : hov ? p.dangerHover : p.danger;
    fg = IM_COL32(255, 255, 255, 255);
    keyBorder = IM_COL32(255, 255, 255, 120);
    break;
  default:
    bg = held ? p.surfaceActive : hov ? p.surfaceHover : p.surface;
    fg = p.text;
    keyBorder = p.lineStrong;
    break;
  }
  if (!enabled) {
    bg = p.surface;
    fg = p.textFaint;
    keyBorder = p.line;
  }
  dl->AddRectFilled(a, b, C(bg));
  if (tone == Tone::Primary && enabled) DrawTrace(dl, a, b, p.text);

  float mid = (a.y + b.y) * 0.5f;
  float x = a.x + Px(14);
  if (recordMark) {
    DrawMark(dl, ImVec2(x + Px(4), mid), Px(4), C(enabled ? p.danger : p.textFaint), true);
    x += Px(18);
  }
  float fs = Px(kBody);
  float textW = b.x - x - KeycapWidth(key) - Px(20);
  std::string t = Ellipsize(bold_, fs, title, textW);
  Text(dl, bold_, fs, ImVec2(x, mid - fs * 0.5f), C(fg), t.c_str());
  ImU32 keyFg = tone == Tone::Normal ? (enabled ? p.textMuted : p.textFaint) : fg;
  DrawKeycap(dl, ImVec2(b.x - Px(12), mid), key, keyBorder, keyFg, 1.0f);
  return pressed && enabled;
}

// Interruptor rectangular: apagado, pista gris y mando apagado; encendido,
// pista del acento y mando del color del texto sobre el acento.
void DrawSwitch(ImDrawList* dl, ImVec2 rightMid, bool on, ImGuiID animId, float alpha) {
  const Palette& p = Colors();
  float& t = *ImGui::GetStateStorage()->GetFloatRef(animId, on ? 1.0f : 0.0f);
  float target = on ? 1.0f : 0.0f;
  t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
  if (std::fabs(t - target) < 0.01f) t = target;
  const float w = Px(32), h = Px(16), knob = Px(10), pad = Px(3);
  ImVec2 a(rightMid.x - w, rightMid.y - h * 0.5f), b(rightMid.x, rightMid.y + h * 0.5f);
  dl->AddRectFilled(a, b, C(Mix(p.surfaceActive, p.accent, t), alpha));
  float knobX = a.x + pad + (w - knob - pad * 2) * t;
  dl->AddRectFilled(ImVec2(knobX, a.y + pad), ImVec2(knobX + knob, a.y + pad + knob),
                    C(Mix(p.textMuted, p.onAccent, t), alpha));
}

// Fila con icono, texto y un interruptor; toda la fila es clicable.
bool ToggleRow(const char* id, Icon icon, const char* label, bool on) {
  const Palette& p = Colors();
  const float h = Px(36);
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  bool pressed = ImGui::InvisibleButton(id, size);
  bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 rowA(a.x - Px(8), a.y), rowB(b.x + Px(8), b.y);
  TrackHover(ImGui::GetItemID(), rowA, rowB, hov);
  if (hov) dl->AddRectFilled(rowA, rowB, C(p.surface));
  float mid = (a.y + b.y) * 0.5f;
  DrawIcon(dl, icon, ImVec2(a.x + Px(8), mid), Px(8), C(on ? p.text : p.textMuted));
  float fs = Px(kBody);
  Text(dl, ImGui::GetFont(), fs, ImVec2(a.x + Px(26), mid - fs * 0.5f), C(p.text), label);
  DrawSwitch(dl, ImVec2(b.x, mid), on, ImGui::GetItemID() ^ 0x5157u, 1.0f);
  return pressed;
}

// Control segmentado (duración del replay). Devuelve el valor pulsado o -1.
int Segmented(const char* id, const std::vector<std::pair<int, std::string>>& options, int current) {
  const Palette& p = Colors();
  ImGui::PushID(id);
  const float h = Px(32);
  ImVec2 a = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), C(p.surface));
  float segW = w / float(options.size());
  int clicked = -1;
  for (size_t i = 0; i < options.size(); ++i) {
    ImVec2 q(a.x + segW * float(i), a.y);
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(int(i));
    if (ImGui::InvisibleButton("seg", ImVec2(segW, h))) clicked = options[i].first;
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    bool sel = options[i].first == current;
    if (sel) dl->AddRectFilled(q, ImVec2(q.x + segW, q.y + h), C(p.accent));
    else if (hov) dl->AddRectFilled(q, ImVec2(q.x + segW, q.y + h), C(p.surfaceHover));
    float ss = Px(kSmall);
    ImFont* font = sel ? bold_ : ImGui::GetFont();
    ImVec2 ts = TextSize(font, ss, options[i].second.c_str());
    Text(dl, font, ss, ImVec2(q.x + (segW - ts.x) * 0.5f, q.y + (h - ts.y) * 0.5f),
         C(sel ? p.onAccent : hov ? p.text : p.textMuted), options[i].second.c_str());
  }
  ImGui::SetCursorScreenPos(a);
  ImGui::Dummy(ImVec2(w, h));
  ImGui::PopID();
  return clicked;
}

bool ClipRow(int index, const ClipFile& clip, bool selected = false) {
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
    ImGui::SetTooltip("Clic para verlo, clic derecho para mostrarlo en la carpeta");
  }
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 rowA(a.x - Px(8), a.y), rowB(b.x + Px(8), b.y);
  TrackHover(id, rowA, rowB, hov);
  if (hov || selected) dl->AddRectFilled(rowA, rowB, C(selected ? p.surfaceActive : p.surface));
  if (selected) dl->AddRectFilled(rowA, ImVec2(rowA.x + Px(2), rowB.y), C(p.accent));
  float mid = (a.y + b.y) * 0.5f;
  DrawIcon(dl, Icon::Play, ImVec2(a.x + Px(5), mid), Px(5), C(hov || selected ? p.text : p.textMuted));
  float ss = Px(kSmall);
  float timeW = TextSize(ImGui::GetFont(), ss, clip.when.c_str()).x;
  std::string name = Ellipsize(ImGui::GetFont(), ss, clip.name, b.x - a.x - timeW - Px(30));
  Text(dl, ImGui::GetFont(), ss, ImVec2(a.x + Px(20), mid - ss * 0.5f), C(p.text), name.c_str());
  Text(dl, ImGui::GetFont(), ss, ImVec2(b.x - timeW, mid - ss * 0.5f), C(p.textFaint), clip.when.c_str());
  return pressed;
}

bool IconButton(const char* id, Icon icon, float size, const char* tooltip) {
  const Palette& p = Colors();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  bool hov = ImGui::IsItemHovered();
  ImVec2 c = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), C(p.surface));
    if (tooltip) ImGui::SetTooltip("%s", tooltip);
  }
  DrawIcon(dl, icon, c, size * 0.25f, C(hov ? p.text : p.textMuted));
  return pressed;
}

// Botón con borde de 1 px y fondo transparente (acciones secundarias).
bool GhostButton(const char* id, const char* label, float width, Tone tone = Tone::Normal, float height = 32) {
  const Palette& p = Colors();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(width, Px(height)));
  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 fg = tone == Tone::Danger ? p.danger : p.text;
  if (hov || held) dl->AddRectFilled(a, b, C(held ? p.surfaceActive : p.surface));
  dl->AddRect(a, b, C(tone == Tone::Danger && hov ? p.danger : hov ? p.lineStrong : p.line), 0, 0, 1.0f * s_);
  float fs = Px(kSmall);
  ImVec2 ts = TextSize(ImGui::GetFont(), fs, label);
  Text(dl, ImGui::GetFont(), fs, ImVec2(a.x + (b.x - a.x - ts.x) * 0.5f, a.y + (b.y - a.y - ts.y) * 0.5f), C(fg),
       label);
  return pressed;
}

// Botón relleno con el acento (la acción principal de un formulario).
bool SolidButton(const char* id, const char* label, float width, float height = 32) {
  const Palette& p = Colors();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(width, Px(height)));
  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(held ? p.accentActive : hov ? p.accentHover : p.accent));
  float fs = Px(kSmall);
  ImVec2 ts = TextSize(bold_, fs, label);
  Text(dl, bold_, fs, ImVec2(a.x + (b.x - a.x - ts.x) * 0.5f, a.y + (b.y - a.y - ts.y) * 0.5f), C(p.onAccent),
       label);
  return pressed;
}

// Animación de entrada: 0 -> 1 en ~120 ms, sin rebote.
float OpenProgress(const MenuModel& m) {
  float t = float(GetTickCount64() - m.openedAt) / 120.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t);
}

// --------------------------------------------------------------- panel rápido

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

// Rastro fantasma del tema Psíquico: el contorno del panel aparece 10 px más
// arriba y se funde en 240 ms. Usa el tamaño del frame anterior.
void DrawOpenGhost(const MenuModel& m, float width) {
  if (!ThemeMotion() || m.panelHeight <= 0) return;
  float t = float(GetTickCount64() - m.openedAt) / 240.0f;
  if (t >= 1.0f) return;
  t = std::clamp(t, 0.0f, 1.0f);
  float ease = 1.0f - (1.0f - t) * (1.0f - t);
  float top = ImGui::GetIO().DisplaySize.y * 0.5f - m.panelHeight * 0.5f - Px(10) * (1.0f - ease);
  ImGui::GetBackgroundDrawList()->AddRect(ImVec2(0, top), ImVec2(width, top + m.panelHeight),
                                          ImGui::GetColorU32(Colors().accent, 0.55f * (1.0f - ease)), 0, 0,
                                          1.0f * s_);
}

} // namespace

void DrawPanel(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  BeginFrameStyle(w, m.config);
  const Palette& p = Colors();
  const float appear = OpenProgress(m);
  const float width = kPanelWidth * s_;

  DrawOpenGhost(m, width);

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(16), Px(16)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8), Px(4)));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V(p.bg));
  ImGui::PushStyleColor(ImGuiCol_Border, V(p.line));
  // Centrado en vertical dentro de la franja; App calcula la zona de clic igual.
  ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
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
  std::vector<std::pair<int, std::string>> lengths;
  for (int sec : {15, 30, 60, 120, 300}) lengths.emplace_back(sec, FormatClipLength(sec));
  if (int v = Segmented("len", lengths, c.clipSeconds); v > 0 && v != c.clipSeconds) a.setClipSeconds(v);

  Gap(6);
  if (m.stats.recording) {
    if (ActionButton("rec", "Detener grabación", c.hotkeys.toggleRecording, Tone::Danger, true)) a.toggleRecording();
  } else {
    if (ActionButton("rec", "Grabar pantalla", c.hotkeys.toggleRecording, Tone::Normal, true, true))
      a.toggleRecording();
  }

  Divider(12, 6);
  SectionLabel("CAPTURA");
  if (ToggleRow("replay", Icon::Replay, "Replay", c.replayEnabled)) a.toggleReplay();
  if (ToggleRow("mic", Icon::Mic, "Micrófono", c.captureMic)) a.toggleMic();
  if (ToggleRow("cam", Icon::Camera, "Cámara", c.cameraEnabled)) a.toggleCamera();

  Divider(6, 6);
  bool viewAll = false;
  SectionLabel("RECIENTES", "Ver todos", &viewAll);
  if (viewAll) a.openPath(m.outputDir);
  if (m.clips.empty()) {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float ss = Px(kSmall);
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x, q.y + Px(6)), C(p.textFaint), "Aún no hay clips ni grabaciones.");
    ImGui::Dummy(ImVec2(0, ss + Px(12)));
  }
  for (int i = 0; i < int(std::min<size_t>(m.clips.size(), 3)); ++i) {
    const auto& clip = m.clips[size_t(i)];
    if (ClipRow(i, clip)) a.openPath(clip.path);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.showInFolder(clip.path);
  }

  Divider(6, 8);
  float half = (ImGui::GetContentRegionAvail().x - Px(8)) * 0.5f;
  if (GhostButton("settings", "Ajustes", half, Tone::Normal, 34)) a.openSettings();
  ImGui::SameLine(0, Px(8));
  if (GhostButton("folder", "Carpeta", half, Tone::Normal, 34)) a.openPath(m.outputDir);

  m.panelWidth = ImGui::GetWindowWidth();
  m.panelHeight = ImGui::GetWindowHeight();
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(5);

  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) a.close();
}

// ------------------------------------------------------------------ ajustes

namespace {

constexpr float kLabelColumn = 230.0f;
constexpr float kControlWidth = 340.0f;

float ControlWidth() { return std::min(ImGui::GetContentRegionAvail().x, Px(kControlWidth)); }

// Fila de formulario: etiqueta (y ayuda) a la izquierda, control a la derecha.
void FieldLabel(const char* label, const char* help = nullptr) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  ImGui::SameLine(Px(kLabelColumn));
  ImGui::SetNextItemWidth(ControlWidth());
}

void Help(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, V(Colors().textFaint));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, Px(560)));
  ImGui::TextWrapped("%s", text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

void Warning(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, V(Colors().warning));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, Px(560)));
  ImGui::TextWrapped("%s", text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

// Título de sección: mayúsculas espaciadas y una línea hasta el borde.
void Heading(const char* text) {
  Gap(14);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q = ImGui::GetCursorScreenPos();
  float size = Px(kLabel), h = Px(kHint) + Px(2);
  float w = TrackedWidth(bold_, size, text, Px(1.5f));
  TrackedText(dl, bold_, size, ImVec2(q.x, q.y + (h - size) * 0.5f), C(Colors().textMuted), text, Px(1.5f));
  float avail = ImGui::GetContentRegionAvail().x;
  if (avail - w > Px(24))
    dl->AddRectFilled(ImVec2(q.x + w + Px(12), q.y + h * 0.5f), ImVec2(q.x + avail, q.y + h * 0.5f + 1.0f * s_),
                      C(Colors().line));
  ImGui::Dummy(ImVec2(0, h));
  Gap(6);
}

// Interruptor de ajustes: etiqueta a la izquierda, switch en la columna de controles.
bool SwitchField(const char* label, bool* value, const char* help = nullptr) {
  ImGui::PushID(label);
  float rowH = ImGui::GetFrameHeight();
  ImVec2 q = ImGui::GetCursorScreenPos();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  ImGui::SameLine(Px(kLabelColumn));
  bool pressed = ImGui::InvisibleButton("switch", ImVec2(Px(32), rowH));
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (pressed) *value = !*value;
  ImVec2 b = ImGui::GetItemRectMax();
  DrawSwitch(ImGui::GetWindowDrawList(), ImVec2(b.x, (q.y + b.y) * 0.5f), *value, ImGui::GetItemID() ^ 0x5157u,
             ImGui::GetStyle().Alpha);
  ImGui::PopID();
  return pressed;
}

bool DeviceCombo(const char* id, std::string& value, const std::vector<AudioDevice>& devices) {
  std::string current = "Predeterminado del sistema";
  for (const auto& d : devices)
    if (d.id == value) current = d.name;
  bool changed = false;
  if (ImGui::BeginCombo(id, current.c_str())) {
    if (ImGui::Selectable("Predeterminado del sistema", value.empty())) {
      value.clear();
      changed = true;
    }
    for (const auto& d : devices) {
      if (ImGui::Selectable(d.name.c_str(), d.id == value)) {
        value = d.id;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

template <typename T>
bool ChoiceCombo(const char* id, T& value, const std::vector<std::pair<T, const char*>>& options) {
  const char* current = options.empty() ? "" : options.front().second;
  for (const auto& [v, name] : options)
    if (v == value) current = name;
  bool changed = false;
  if (ImGui::BeginCombo(id, current)) {
    for (const auto& [v, name] : options) {
      if (ImGui::Selectable(name, v == value)) {
        value = v;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Bitrate máximo que usará la configuración en edición (para las estimaciones).
int EstimatedKbps(const MenuModel& m) {
  const auto& c = m.config;
  int height = c.outputHeight > 0 ? c.outputHeight : (m.stats.running && m.stats.height > 0 ? m.stats.height : 1080);
  int width = height * 16 / 9;
  if (m.stats.running && m.stats.height > 0) width = height * m.stats.width / m.stats.height;
  return PlanQuality(c.quality, c.bitrateKbps, width, height, c.fps, c.codec).maxKbps;
}

// Medidor de nivel: barra en la columna de controles, con el dispositivo y
// formato reales debajo. Si la barra no se mueve con el juego sonando, se
// está escuchando el dispositivo equivocado.
void LevelMeter(const char* label, float level, bool active, const std::string& description) {
  const Palette& p = Colors();
  FieldLabel(label);
  float w = ControlWidth();
  float h = Px(6);
  ImVec2 q = ImGui::GetCursorScreenPos();
  float rowH = ImGui::GetFrameHeight();
  ImVec2 a(q.x, q.y + (rowH - h) * 0.5f), b(q.x + w, a.y + h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(p.surface));
  if (active) {
    // Escala en dB: -60 dB a la izquierda, 0 dB a la derecha.
    float db = level > 1e-5f ? 20.0f * std::log10(level) : -100.0f;
    float t = std::clamp(1.0f + db / 60.0f, 0.0f, 1.0f);
    ImU32 color = db > -3.0f ? C(p.danger) : db > -12.0f ? C(p.warning) : C(p.accent);
    if (t > 0) dl->AddRectFilled(a, ImVec2(a.x + w * t, b.y), color);
  }
  ImGui::Dummy(ImVec2(w, rowH));
  if (!active) {
    Help("La captura está parada o esta fuente no se pudo abrir: no hay nivel que mostrar. Mira spectre.log.");
  } else {
    std::string text = "Capturando: " + description;
    Help(text.c_str());
  }
}

void PageClips(MenuModel& m, const MenuActions& a) {
  const Palette& p = Colors();
  auto& c = m.config;
  Heading("REPLAY");
  m.dirty |= SwitchField("Replay activado", &c.replayEnabled,
                         "Guarda continuamente los últimos segundos para poder crear un clip en cualquier momento.");
  m.dirty |= SwitchField("Solo con un juego abierto", &c.onlyInGames,
                         "El replay se pausa si no hay ninguna app a pantalla completa.");
  FieldLabel("Duración del clip");
  m.dirty |= ChoiceCombo<int>("##len", c.clipSeconds,
                              {{15, "15 segundos"}, {30, "30 segundos"}, {60, "1 minuto"},
                               {120, "2 minutos"}, {300, "5 minutos"}});
  double estimate = double(EstimatedKbps(m)) * 1000.0 / 8.0 * c.clipSeconds * 1.1;
  double limit = double(c.maxBufferMB) * 1024.0 * 1024.0;
  char hint[320];
  if (estimate > limit) {
    snprintf(hint, sizeof(hint),
             "Con esta calidad y duración el replay puede necesitar hasta %s de RAM, más que el tope de %d MB de "
             "Ajustes > General. En escenas de mucho movimiento el clip saldría más corto: sube el tope o baja la calidad.",
             FormatBytes(uint64_t(estimate)).c_str(), c.maxBufferMB);
    Warning(hint);
  } else {
    snprintf(hint, sizeof(hint), "El replay guarda vídeo ya comprimido. Con esta calidad ocupa como mucho unos %s de RAM.",
             FormatBytes(uint64_t(estimate)).c_str());
    Help(hint);
  }

  Heading("CLIPS RECIENTES");
  ImGui::PushStyleColor(ImGuiCol_Text, V(p.textMuted));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(m.outputDir.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine(0, Px(12));
  if (GhostButton("open-dir", "Abrir carpeta", Px(110), Tone::Normal, 26)) a.openPath(m.outputDir);
  ImGui::SameLine(0, Px(6));
  if (GhostButton("refresh", "Actualizar", Px(90), Tone::Normal, 26)) a.refreshClips();

  Gap(4);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, V(p.bgAlt));
  ImGui::PushStyleColor(ImGuiCol_Border, V(p.line));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(10), Px(6)));
  if (ImGui::BeginChild("clips", ImVec2(0, Px(190)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) {
    if (m.clips.empty()) Help("Todavía no hay clips. Pulsa el atajo de guardar clip durante una partida.");
    for (int i = 0; i < int(m.clips.size()); ++i) {
      const auto& clip = m.clips[size_t(i)];
      if (ClipRow(i, clip, m.selectedClip == i)) {
        m.selectedClip = i;
        m.clipDuration = a.probeDuration ? a.probeDuration(clip.path) : 0;
        m.trimStart = 0;
        m.trimEnd = float(m.clipDuration);
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.showInFolder(clip.path);
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);

  if (m.selectedClip >= 0 && m.selectedClip < int(m.clips.size())) {
    const auto& clip = m.clips[size_t(m.selectedClip)];
    Heading("RECORTAR");
    if (m.clipDuration <= 0) {
      Help("No se pudo leer la duración de este archivo.");
    } else {
      FieldLabel("Inicio y fin");
      ImGui::DragFloatRange2("##trim", &m.trimStart, &m.trimEnd, 0.1f, 0.0f, float(m.clipDuration), "%.1f s",
                             "%.1f s", ImGuiSliderFlags_AlwaysClamp);
      ImGui::Dummy(ImVec2(Px(kLabelColumn) - ImGui::GetStyle().ItemSpacing.x, 0));
      ImGui::SameLine();
      ImGui::BeginDisabled(m.trimEnd - m.trimStart < 0.5f);
      if (SolidButton("trim", "Guardar recorte", Px(140))) a.trim(clip.path, m.trimStart, m.trimEnd);
      ImGui::EndDisabled();
      ImGui::SameLine(0, Px(6));
      if (GhostButton("play", "Reproducir", Px(110))) a.openPath(clip.path);
      Help("El recorte no recodifica: es instantáneo y sin pérdida. El inicio se ajusta al fotograma clave anterior "
           "(como mucho 2 segundos antes).");
    }
  }
}

void PageVideo(MenuModel& m) {
  auto& c = m.config;
  Heading("QUÉ CAPTURAR");
  FieldLabel("Fuente");
  m.dirty |= ChoiceCombo<std::string>("##src", c.captureMode,
                                      {{"monitor", "Pantalla completa"}, {"game", "Ventana del juego"}});
  if (c.captureMode == "monitor") {
    std::vector<std::pair<int, const char*>> mons;
    for (size_t i = 0; i < m.monitors.size(); ++i) mons.emplace_back(int(i), m.monitors[i].name.c_str());
    FieldLabel("Monitor");
    m.dirty |= ChoiceCombo<int>("##mon", c.monitorIndex, mons);
  } else {
    Help("Captura la ventana que esté a pantalla completa en primer plano. Si no hay ninguna, se captura el monitor.");
  }
  m.dirty |= SwitchField("Mostrar el cursor", &c.captureCursor);

  Heading("CALIDAD");
  FieldLabel("Resolución");
  m.dirty |= ChoiceCombo<int>("##res", c.outputHeight,
                              {{0, "Nativa"}, {2160, "2160p (4K)"}, {1440, "1440p"}, {1080, "1080p"},
                               {720, "720p"}, {480, "480p"}});
  FieldLabel("Fotogramas por segundo");
  m.dirty |= ChoiceCombo<int>("##fps", c.fps, {{30, "30 fps"}, {60, "60 fps"}, {120, "120 fps"}});
  FieldLabel("Calidad");
  m.dirty |= ChoiceCombo<std::string>("##quality", c.quality, QualityOptions());
  if (c.quality == "personalizada") {
    FieldLabel("Bitrate");
    int mbps = c.bitrateKbps / 1000;
    if (ImGui::SliderInt("##br", &mbps, 2, 100, "%d Mbps")) {
      c.bitrateKbps = mbps * 1000;
      m.dirty = true;
    }
    Help("Bitrate fijo. Para 1080p a 60 fps, 30 Mbps se ve muy bien; más bitrate usa más RAM y disco.");
  } else {
    char hint[240];
    snprintf(hint, sizeof(hint),
             "El encoder apunta a una calidad constante y usa hasta %d Mbps en las escenas de más movimiento "
             "(para la resolución y los fps elegidos). Alta es el equilibrio recomendado; Máxima es lo que usa "
             "ShadowPlay en Alta.",
             EstimatedKbps(m) / 1000);
    Help(hint);
  }

  Heading("CODIFICACIÓN");
  FieldLabel("Códec");
  if (ChoiceCombo<std::string>("##codec", c.codec,
                               {{"h264", "H.264, se reproduce en todas partes"},
                                {"hevc", "HEVC, ocupa menos"},
                                {"av1", "AV1, para RTX 40, RX 7000 o Arc"}})) {
    c.encoder = "auto";
    m.encoders = CandidateEncoders(c.codec);
    m.dirty = true;
  }
  std::vector<std::pair<std::string, const char*>> encs = {{"auto", "Automático"}};
  for (const auto& e : m.encoders) encs.emplace_back(e.name, e.label.c_str());
  FieldLabel("Encoder");
  m.dirty |= ChoiceCombo<std::string>("##enc", c.encoder, encs);
  FieldLabel("Método de captura");
  m.dirty |= ChoiceCombo<std::string>("##api", c.captureApi,
                                      {{"auto", "Automático"},
                                       {"wgc", "Windows Graphics Capture"},
                                       {"dda", "Desktop Duplication"}});
  m.dirty |= SwitchField("Prioridad alta en la GPU", &c.gpuPriority,
                         "Evita saltos en el vídeo cuando el juego usa la GPU al 100 %.");
}

void PageAudio(MenuModel& m) {
  auto& c = m.config;
  Heading("SONIDO DEL PC");
  m.dirty |= SwitchField("Grabar el sonido del PC", &c.captureSystemAudio);
  ImGui::BeginDisabled(!c.captureSystemAudio);
  FieldLabel("Dispositivo de salida");
  m.dirty |= DeviceCombo("##out", c.outputDeviceId, m.outputs);
  LevelMeter("Nivel", m.systemLevel, m.stats.running && !m.stats.systemAudio.empty(), m.stats.systemAudio);
  m.dirty |= SwitchField("Solo el audio del juego", &c.gameAudioOnly,
                         "Deja fuera Discord, la música y el resto de apps. Necesita Windows 10 2004 o posterior y un "
                         "juego a pantalla completa.");
  FieldLabel("Volumen");
  m.dirty |= ImGui::SliderFloat("##sysvol", &c.systemVolume, 0.0f, 2.0f, "x%.2f");
  ImGui::EndDisabled();

  Heading("MICRÓFONO");
  m.dirty |= SwitchField("Grabar el micrófono", &c.captureMic);
  ImGui::BeginDisabled(!c.captureMic);
  FieldLabel("Dispositivo de entrada");
  m.dirty |= DeviceCombo("##in", c.micDeviceId, m.inputs);
  LevelMeter("Nivel", m.micLevel, m.stats.running && !m.stats.micAudio.empty(), m.stats.micAudio);
  FieldLabel("Volumen");
  m.dirty |= ImGui::SliderFloat("##micvol", &c.micVolume, 0.0f, 2.0f, "x%.2f");
  ImGui::EndDisabled();

  Heading("PISTAS");
  m.dirty |= SwitchField("Pistas separadas", &c.separateTracks,
                         "Además de la mezcla, guarda el PC y el micrófono en pistas aparte para editarlas por "
                         "separado.");
}

void PageCamera(MenuModel& m) {
  auto& c = m.config;
  Heading("CÁMARA");
  m.dirty |= SwitchField("Incluir la webcam", &c.cameraEnabled);
  ImGui::BeginDisabled(!c.cameraEnabled);
  std::vector<std::pair<std::string, const char*>> cams = {{"", "La primera que encuentre"}};
  for (const auto& cam : m.cameras) cams.emplace_back(cam.id, cam.name.c_str());
  FieldLabel("Dispositivo");
  m.dirty |= ChoiceCombo<std::string>("##cam", c.cameraId, cams);
  FieldLabel("Posición");
  m.dirty |= ChoiceCombo<std::string>("##corner", c.cameraCorner,
                                      {{"top-left", "Arriba a la izquierda"},
                                       {"top-right", "Arriba a la derecha"},
                                       {"bottom-left", "Abajo a la izquierda"},
                                       {"bottom-right", "Abajo a la derecha"}});
  FieldLabel("Tamaño");
  m.dirty |= ImGui::SliderInt("##camsize", &c.cameraSizePercent, 10, 50, "%d %% del ancho");
  ImGui::EndDisabled();
  Help("La cámara se compone en la GPU. Viene apagada porque es lo que más CPU añade.");
}

void PageHotkeys(MenuModel& m) {
  auto& h = m.config.hotkeys;
  Heading("ATAJOS DE TECLADO");
  Help("Escribe la combinación, por ejemplo Alt+Z, Ctrl+Shift+F10 o Alt+Num5. Déjalo vacío para desactivarlo.");
  Gap(4);
  auto field = [&](const char* label, const char* id, std::string& value) {
    FieldLabel(label);
    ImGui::SetNextItemWidth(Px(200));
    if (ImGui::InputText(id, &value)) m.dirty = true;
    UINT mods = 0, vk = 0;
    if (!value.empty() && !ParseHotkey(value, &mods, &vk)) {
      ImGui::SameLine();
      ImGui::TextColored(V(Colors().danger), "No es una combinación válida");
    }
  };
  field("Abrir el panel", "##hk1", h.overlay);
  field("Guardar clip", "##hk2", h.saveClip);
  field("Grabar o detener", "##hk3", h.toggleRecording);
  field("Activar o pausar el replay", "##hk4", h.toggleReplay);
  if (!m.hotkeyError.empty()) {
    Gap(4);
    ImGui::PushStyleColor(ImGuiCol_Text, V(Colors().danger));
    ImGui::TextWrapped("%s", m.hotkeyError.c_str());
    ImGui::PopStyleColor();
  }
}

// Tarjeta del selector de temas: se pinta con los colores del tema que
// representa, con su nombre y tres muestras (fondo, superficie y acento).
bool ThemeCard(const Theme& t, bool selected) {
  const Palette& p = Colors();
  ImGui::PushID(t.id);
  bool pressed = ImGui::InvisibleButton("theme", ImVec2(Px(170), Px(52)));
  bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(t.colors.bg));
  dl->AddRect(a, b, C(selected ? p.accent : hov ? p.lineStrong : p.line), 0, 0, 1.0f * s_);
  float mid = (a.y + b.y) * 0.5f, fs = Px(kSmall);
  Text(dl, selected ? bold_ : ImGui::GetFont(), fs, ImVec2(a.x + Px(12), mid - fs * 0.5f),
       C(selected ? t.colors.text : t.colors.textMuted), t.name);
  const float sw = Px(12), gap = Px(3);
  float x = b.x - Px(12) - sw * 3 - gap * 2;
  ImVec2 s0(x, mid - sw * 0.5f);
  dl->AddRectFilled(s0, s0 + ImVec2(sw, sw), C(t.colors.bg));
  dl->AddRect(s0, s0 + ImVec2(sw, sw), C(t.colors.lineStrong), 0, 0, 1.0f * s_);
  ImVec2 s1 = s0 + ImVec2(sw + gap, 0);
  dl->AddRectFilled(s1, s1 + ImVec2(sw, sw), C(t.colors.surface));
  ImVec2 s2 = s1 + ImVec2(sw + gap, 0);
  dl->AddRectFilled(s2, s2 + ImVec2(sw, sw), C(t.colors.accent));
  return pressed;
}

void AppearanceSection(MenuModel& m) {
  auto& c = m.config;
  Heading("APARIENCIA");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Tema");
  ImGui::SameLine(Px(kLabelColumn));
  bool first = true;
  for (const Theme* t : Themes()) {
    if (!first) ImGui::SameLine(0, Px(8));
    first = false;
    if (ThemeCard(*t, c.theme == t->id) && c.theme != t->id) {
      c.theme = t->id;
      m.dirty = true;
    }
  }
  Gap(2);
  m.dirty |= SwitchField("Animaciones del tema", &c.themeMotion,
                         "Movimiento propio del tema Psíquico: rastro al abrir el panel, estela en el botón "
                         "principal y marca de estado que respira. El tema Negro no tiene animaciones.");
  Help("Los cambios se ven al momento; pulsa Aplicar para conservarlos.");
}

// Estado de la actualización: marca de color (o ruedita si está trabajando) y texto.
void UpdateStatusLine(ImU32 color, bool busy, const std::string& text, bool bold = false) {
  float rowH = ImGui::GetFrameHeight();
  ImVec2 q = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 c(q.x + Px(6), q.y + rowH * 0.5f);
  if (busy) {
    float t = float(ImGui::GetTime()) * 5.0f;
    dl->PathArcTo(c, Px(5), t, t + kPi * 1.4f, 16);
    dl->PathStroke(C(color), 0, 1.5f * s_);
  } else {
    DrawMark(dl, c, Px(3.5f), C(color), true);
  }
  ImFont* font = bold ? bold_ : ImGui::GetFont();
  float fs = ImGui::GetFontSize();
  float wrap = std::min(ImGui::GetContentRegionAvail().x, Px(560)) - Px(20);
  ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, wrap, text.c_str());
  dl->AddText(font, fs, ImVec2(q.x + Px(20), q.y + (rowH - fs) * 0.5f), C(Colors().text), text.c_str(), nullptr,
              wrap);
  ImGui::Dummy(ImVec2(0, std::max(rowH, ts.y + (rowH - fs))));
}

void UpdatesSection(MenuModel& m, const MenuActions& a) {
  using S = UpdateStatus::State;
  const Palette& p = Colors();
  const UpdateStatus& u = m.update;
  Heading("ACTUALIZACIONES");
  ImGui::PushStyleColor(ImGuiCol_Text, V(p.textMuted));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Versión instalada");
  ImGui::PopStyleColor();
  ImGui::SameLine(Px(kLabelColumn));
  ImGui::TextUnformatted(SPECTRE_VERSION);
  Gap(2);

  const float bw = Px(210);
  char buf[256];
  switch (u.state) {
  case S::Idle:
    Help("Spectre solo se conecta a GitHub cuando pulsas el botón: no comprueba nada en segundo plano.");
    Gap(4);
    if (GhostButton("update-check", "Buscar actualizaciones", bw)) a.checkUpdates();
    break;
  case S::Checking:
    UpdateStatusLine(p.textMuted, true, "Buscando actualizaciones...");
    break;
  case S::UpToDate:
    snprintf(buf, sizeof(buf), "Tienes la última versión (%s).", SPECTRE_VERSION);
    UpdateStatusLine(p.accent, false, buf);
    Gap(4);
    if (GhostButton("update-check", "Volver a buscar", bw)) a.checkUpdates();
    break;
  case S::Available:
    snprintf(buf, sizeof(buf), "Versión nueva disponible: %s", u.latest.c_str());
    UpdateStatusLine(p.accent, false, buf, true);
    if (u.portable || u.unverified) {
      if (u.portable)
        Help("Esta es la copia portable de Spectre: descarga el zip nuevo y reemplaza la carpeta. Si usas el "
             "instalador, Spectre se actualiza solo desde aquí.");
      else
        Help("GitHub no ha publicado la huella de este instalador, así que Spectre no puede comprobarlo y no lo "
             "instala solo. Descárgalo desde la página de la versión.");
      Gap(4);
      if (SolidButton("update-page", "Abrir página de descarga", bw)) a.openPath(Updater::ReleasePage());
    } else {
      std::string help = "Se descarga el instalador";
      if (u.sizeBytes > 0) help += " (" + FormatBytes(u.sizeBytes) + ")";
      help += ", Spectre se cierra, se actualiza conservando tus ajustes y se vuelve a abrir solo.";
      if (m.stats.recording) help += " Si sigues grabando, se instalará cuando detengas la grabación.";
      Help(help.c_str());
      Gap(4);
      if (SolidButton("update-install", "Actualizar ahora", bw)) a.installUpdate();
    }
    break;
  case S::Downloading: {
    int pct = int(std::clamp(u.downloaded, 0.0, 1.0) * 100.0 + 0.5);
    snprintf(buf, sizeof(buf), "Descargando la versión %s... %d %%", u.latest.c_str(), pct);
    UpdateStatusLine(p.accent, true, buf);
    float w = ControlWidth(), h = Px(4);
    ImVec2 q = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + h), C(p.surface));
    if (pct > 0) dl->AddRectFilled(q, ImVec2(q.x + w * float(pct) / 100.0f, q.y + h), C(p.accent));
    ImGui::Dummy(ImVec2(w, h));
    Gap(8);
    if (GhostButton("update-cancel", "Cancelar", Px(120))) a.cancelUpdate();
    break;
  }
  case S::Launching:
    if (m.stats.recording)
      UpdateStatusLine(p.accent, false, "Actualización lista: se instalará cuando detengas la grabación.");
    else
      UpdateStatusLine(p.accent, true, "Abriendo el instalador. Spectre se cerrará y volverá a abrirse al terminar.");
    break;
  case S::Error:
    UpdateStatusLine(p.danger, false, u.message);
    Gap(4);
    if (GhostButton("update-check", "Reintentar", bw)) a.checkUpdates();
    break;
  }
}

void PageGeneral(MenuModel& m, const MenuActions& a) {
  auto& c = m.config;
  AppearanceSection(m);

  Heading("ARCHIVOS");
  FieldLabel("Carpeta de destino");
  if (ImGui::InputTextWithHint("##dir", m.outputDir.c_str(), &c.outputDir)) m.dirty = true;
  Help("Déjalo vacío para usar Vídeos\\Spectre.");

  Heading("INICIO");
  m.dirty |= SwitchField("Iniciar con Windows", &c.autostart, "Spectre arranca minimizado en la bandeja.");
  m.dirty |= SwitchField("Mostrar avisos", &c.showNotifications,
                         "Avisos breves al guardar un clip o empezar a grabar. Los errores se muestran siempre.");

  Heading("MEMORIA");
  FieldLabel("Límite del replay");
  m.dirty |= ImGui::SliderInt("##mem", &c.maxBufferMB, 64, 4096, "%d MB");
  Help("Tope de RAM para el replay. Si la duración y la calidad elegidas no caben, se guardan menos segundos.");

  UpdatesSection(m, a);

  Heading("SPECTRE");
  Help("Cerrar el panel no cierra Spectre: sigue en la bandeja del sistema, junto al reloj.");
  Gap(4);
  if (GhostButton("quit", "Salir de Spectre", Px(170), Tone::Danger)) a.quit();
}

void PagePerformance(MenuModel& m) {
  const auto& s = m.stats;
  auto row = [&](const char* label, const std::string& value) {
    ImGui::PushStyleColor(ImGuiCol_Text, V(Colors().textMuted));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(Px(kLabelColumn));
    ImGui::TextUnformatted(value.c_str());
  };
  char buf[160];
  Heading("CONSUMO DE SPECTRE");
  snprintf(buf, sizeof(buf), "%.1f %%", m.usage.cpuPercent);
  row("CPU", buf);
  row("RAM", FormatBytes(m.usage.workingSet) + " (privada " + FormatBytes(m.usage.privateBytes) + ")");
  if (!s.running) {
    Gap(4);
    Help("La captura está parada, así que Spectre no usa la GPU ni el encoder.");
    return;
  }
  snprintf(buf, sizeof(buf), "%s, %.0f s", FormatBytes(s.bufferBytes).c_str(), s.bufferSeconds);
  row("Replay en memoria", buf);

  Heading("CAPTURA");
  row("GPU", s.gpu);
  row("Método", s.capture);
  row("Encoder", s.encoder);
  snprintf(buf, sizeof(buf), "%d x %d a %d fps", s.width, s.height, s.fps);
  row("Vídeo", buf);
  snprintf(buf, sizeof(buf), "hasta %d Mbps", s.maxKbps / 1000);
  row("Bitrate", buf);
  snprintf(buf, sizeof(buf), "%.2f ms", s.encodeMs);
  row("Tiempo por fotograma", buf);
  snprintf(buf, sizeof(buf), "%llu codificados, %llu descartados", (unsigned long long)s.framesEncoded,
           (unsigned long long)s.framesSkipped);
  row("Fotogramas", buf);
  snprintf(buf, sizeof(buf), "%d", s.audioTracks);
  row("Pistas de audio", buf);
}

constexpr const char* kPages[] = {"Clips", "Vídeo", "Audio", "Cámara", "Atajos", "General", "Rendimiento"};

bool NavItem(int index, const char* name, bool selected) {
  const Palette& p = Colors();
  ImGui::PushID(index);
  const float h = Px(36);
  bool pressed = ImGui::InvisibleButton("nav", ImVec2(ImGui::GetContentRegionAvail().x, h));
  bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (selected) {
    dl->AddRectFilled(a, b, C(p.surface));
    dl->AddRectFilled(a, ImVec2(a.x + Px(2), b.y), C(p.accent));
  } else if (hov) {
    dl->AddRectFilled(a, b, C(p.surface, 0.6f));
  }
  float mid = (a.y + b.y) * 0.5f;
  float fs = Px(kBody);
  Text(dl, selected ? bold_ : ImGui::GetFont(), fs, ImVec2(a.x + Px(16), mid - fs * 0.5f),
       C(selected ? p.text : p.textMuted), name);
  return pressed;
}

} // namespace

void DrawSettings(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  BeginFrameStyle(w, m.config);
  const Palette& p = Colors();
  ImGuiIO& io = ImGui::GetIO();
  const float appear = OpenProgress(m);
  // Sin velo oscuro detrás: Pablo quiere ver el juego tal cual con la ventana encima.

  ImVec2 size(std::min(io.DisplaySize.x - Px(48), Px(920)), std::min(io.DisplaySize.y - Px(48), Px(660)));
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V(p.bg));
  ImGui::PushStyleColor(ImGuiCol_Border, V(p.line));
  ImGui::SetNextWindowPos(io.DisplaySize * 0.5f + ImVec2(0, (1.0f - appear) * Px(8)), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("Ajustes", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar(); // WindowPadding: los hijos usan el suyo

  // Columna de navegación
  const float navW = Px(200);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, V(p.bgAlt));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, Px(18)));
  ImGui::BeginChild("nav", ImVec2(navW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    dl->AddRectFilled(ImVec2(winPos.x + navW - 1.0f * s_, winPos.y), ImVec2(winPos.x + navW, winPos.y + ImGui::GetWindowHeight()),
                      C(p.line));
    ImVec2 q = ImGui::GetCursorScreenPos();
    float rowH = Px(28);
    ImGui::SetCursorScreenPos(ImVec2(q.x + Px(12), q.y));
    if (IconButton("back", Icon::Back, rowH, "Volver al panel")) a.backToPanel();
    Wordmark(dl, ImVec2(q.x + Px(12) + rowH + Px(6), q.y + (rowH - Px(kSmall)) * 0.5f), "AJUSTES", C(p.text));
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(18)));
  }
  for (int i = 0; i < int(std::size(kPages)); ++i)
    if (NavItem(i, kPages[i], m.page == i)) m.page = i;
  {
    float ss = Px(kHint);
    ImVec2 q(ImGui::GetWindowPos().x + Px(16), ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ss - Px(16));
    Text(ImGui::GetWindowDrawList(), ImGui::GetFont(), ss, q, C(p.textFaint), "Spectre " SPECTRE_VERSION);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  ImGui::SameLine(0, 0);
  ImGui::BeginGroup();
  // Cabecera de la página
  const float footerH = Px(56);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(28), Px(22)));
  ImGui::BeginChild("page", ImVec2(0, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q = ImGui::GetCursorScreenPos();
    float closeSize = Px(28);
    float ts = Px(kTitle);
    Text(dl, big_, ts, ImVec2(q.x, q.y + (closeSize - ts) * 0.5f), C(p.text), kPages[m.page]);
    ImGui::SetCursorScreenPos(ImVec2(q.x + ImGui::GetContentRegionAvail().x - closeSize, q.y));
    if (IconButton("close", Icon::Close, closeSize, "Cerrar (Esc)")) a.close();
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + closeSize + Px(2)));
  }
  switch (m.page) {
  case 0: PageClips(m, a); break;
  case 1: PageVideo(m); break;
  case 2: PageAudio(m); break;
  case 3: PageCamera(m); break;
  case 4: PageHotkeys(m); break;
  case 5: PageGeneral(m, a); break;
  default: PagePerformance(m); break;
  }
  Gap(12);
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // Pie: aplicar / descartar
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float wAvail = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(q, ImVec2(q.x + wAvail, q.y + 1.0f * s_), C(p.line));
    float bw = Px(120);
    float y = q.y + (footerH - Px(32)) * 0.5f;
    if (m.dirty) {
      float ss = Px(kHint);
      Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(28), q.y + (footerH - ss) * 0.5f), C(p.textMuted),
           "Hay cambios sin aplicar");
      ImGui::SetCursorScreenPos(ImVec2(q.x + wAvail - bw * 2 - Px(8) - Px(28), y));
      if (GhostButton("discard", "Descartar", bw)) a.backToPanel();
      ImGui::SameLine(0, Px(8));
      if (SolidButton("apply", "Aplicar", bw)) {
        a.apply(m.config);
        m.dirty = false;
      }
    } else {
      float ss = Px(kHint);
      std::string hint = "Esc o " + m.config.hotkeys.overlay + " para cerrar";
      Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(28), q.y + (footerH - ss) * 0.5f), C(p.textFaint), hint.c_str());
    }
    ImGui::SetCursorScreenPos(q);
    ImGui::Dummy(ImVec2(wAvail, footerH));
  }
  ImGui::EndGroup();

  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);

  // Esc en un campo de texto o un desplegable solo cierra eso. ImGui los
  // desactiva en este mismo frame, por eso se mira también el anterior.
  bool busy = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !busy && !m.busyLastFrame) a.close();
  m.busyLastFrame = busy;
}

void DrawToast(OverlayWindow& w, const Config& config, const std::string& text, bool error, float alpha) {
  BeginFrameStyle(w, config);
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
  dl->AddRect(a + ImVec2(0.5f, 0.5f), b - ImVec2(0.5f, 0.5f), C(p.line), 0, 0, 1.0f * s_);
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
