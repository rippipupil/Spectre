#include "ui/Menu.h"

#include "common/Util.h"
#include "ui/OverlayWindow.h"

#define IMGUI_DEFINE_MATH_OPERATORS
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

// ------------------------------------------------------------------ tema
// Una sola paleta para panel, ajustes y avisos. Los colores pasan por
// ImGui::GetColorU32 para respetar la transparencia de la animación.
namespace col {
constexpr ImU32 Bg = IM_COL32(17, 17, 21, 246);
constexpr ImU32 Surface = IM_COL32(29, 29, 36, 255);
constexpr ImU32 SurfaceHover = IM_COL32(37, 37, 46, 255);
constexpr ImU32 SurfaceActive = IM_COL32(46, 46, 57, 255);
constexpr ImU32 Line = IM_COL32(255, 255, 255, 16);
constexpr ImU32 Text = IM_COL32(236, 236, 241, 255);
constexpr ImU32 Muted = IM_COL32(150, 150, 162, 255);
constexpr ImU32 Faint = IM_COL32(104, 104, 116, 255);
constexpr ImU32 Accent = IM_COL32(139, 92, 246, 255);
constexpr ImU32 AccentHover = IM_COL32(153, 112, 250, 255);
constexpr ImU32 AccentActive = IM_COL32(122, 76, 228, 255);
constexpr ImU32 Red = IM_COL32(229, 72, 77, 255);
constexpr ImU32 RedHover = IM_COL32(239, 92, 96, 255);
constexpr ImU32 RedActive = IM_COL32(204, 58, 63, 255);
constexpr ImU32 Green = IM_COL32(52, 199, 112, 255);
constexpr ImU32 Amber = IM_COL32(232, 170, 60, 255);
} // namespace col

constexpr float kPi = 3.14159265358979f;

// Estado del frame actual (fuentes y escala de la ventana que se dibuja).
float s_ = 1.0f;
ImFont* bold_ = nullptr;
ImFont* big_ = nullptr;

void BeginFrameStyle(OverlayWindow& w) {
  s_ = w.Scale();
  bold_ = w.BoldFont();
  big_ = w.BigFont();
}

ImU32 C(ImU32 color, float alpha = 1.0f) { return ImGui::GetColorU32(color, alpha); }
ImVec4 V(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  auto ch = [&](int shift) {
    float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
    return ImU32(x + (y - x) * t + 0.5f) << shift;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

float SmallSize() { return ImGui::GetFontSize() * 0.84f; }

ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
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
// Dibujados con primitivas para que tengan el mismo trazo a cualquier escala
// y no dependan de fuentes de iconos.
enum class Icon { Replay, Record, Stop, Mic, Camera, Folder, Gear, Close, Back, Play, Monitor, Keyboard, Chart, Power };

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float r, ImU32 color) {
  const float t = std::max(1.4f, r * 0.17f);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
  switch (icon) {
  case Icon::Replay: {
    const float a0 = -kPi * 0.30f, a1 = kPi * 1.38f, rr = 0.78f * r;
    dl->PathArcTo(c, rr, a0, a1, 28);
    dl->PathStroke(color, 0, t);
    ImVec2 p(c.x + std::cos(a1) * rr, c.y + std::sin(a1) * rr);
    ImVec2 tan(-std::sin(a1), std::cos(a1)), nor(std::cos(a1), std::sin(a1));
    dl->AddTriangleFilled(p + tan * (r * 0.42f), p + nor * (r * 0.30f), p - nor * (r * 0.30f), color);
    dl->AddCircleFilled(c, r * 0.20f, color);
    break;
  }
  case Icon::Record:
    dl->AddCircleFilled(c, r * 0.58f, color);
    break;
  case Icon::Stop:
    dl->AddRectFilled(P(-0.48f, -0.48f), P(0.48f, 0.48f), color, r * 0.14f);
    break;
  case Icon::Mic:
    dl->AddRectFilled(P(-0.24f, -0.86f), P(0.24f, 0.18f), color, r * 0.24f);
    dl->PathArcTo(P(0, -0.08f), r * 0.50f, 0.0f, kPi, 14);
    dl->PathStroke(color, 0, t);
    dl->AddLine(P(0, 0.42f), P(0, 0.80f), color, t);
    dl->AddLine(P(-0.30f, 0.80f), P(0.30f, 0.80f), color, t);
    break;
  case Icon::Camera:
    dl->AddRect(P(-0.85f, -0.48f), P(0.32f, 0.48f), color, r * 0.16f, 0, t);
    dl->AddTriangleFilled(P(0.40f, 0.0f), P(0.88f, -0.40f), P(0.88f, 0.40f), color);
    break;
  case Icon::Folder: {
    const ImVec2 pts[] = {P(-0.85f, -0.62f), P(-0.28f, -0.62f), P(-0.12f, -0.44f), P(0.85f, -0.44f),
                          P(0.85f, 0.62f),   P(-0.85f, 0.62f)};
    dl->AddPolyline(pts, 6, color, ImDrawFlags_Closed, t);
    break;
  }
  case Icon::Gear:
    for (int i = 0; i < 8; ++i) {
      float a = kPi * 2.0f * float(i) / 8.0f;
      ImVec2 d(std::cos(a), std::sin(a));
      dl->AddLine(c + d * (r * 0.50f), c + d * (r * 0.86f), color, t * 1.7f);
    }
    dl->AddCircle(c, r * 0.52f, color, 20, t * 1.2f);
    dl->AddCircleFilled(c, r * 0.18f, color);
    break;
  case Icon::Close:
    dl->AddLine(P(-0.45f, -0.45f), P(0.45f, 0.45f), color, t);
    dl->AddLine(P(0.45f, -0.45f), P(-0.45f, 0.45f), color, t);
    break;
  case Icon::Back:
    dl->AddLine(P(0.22f, -0.55f), P(-0.30f, 0.0f), color, t);
    dl->AddLine(P(-0.30f, 0.0f), P(0.22f, 0.55f), color, t);
    break;
  case Icon::Play:
    dl->AddTriangleFilled(P(-0.36f, -0.52f), P(0.52f, 0.0f), P(-0.36f, 0.52f), color);
    break;
  case Icon::Monitor:
    dl->AddRect(P(-0.85f, -0.62f), P(0.85f, 0.38f), color, r * 0.12f, 0, t);
    dl->AddLine(P(0, 0.40f), P(0, 0.72f), color, t);
    dl->AddLine(P(-0.40f, 0.74f), P(0.40f, 0.74f), color, t);
    break;
  case Icon::Keyboard:
    dl->AddRect(P(-0.90f, -0.52f), P(0.90f, 0.52f), color, r * 0.14f, 0, t);
    for (int row = 0; row < 2; ++row)
      for (int k = 0; k < 4; ++k)
        dl->AddRectFilled(P(-0.62f + k * 0.38f, -0.26f + row * 0.30f),
                          P(-0.46f + k * 0.38f, -0.12f + row * 0.30f), color);
    dl->AddLine(P(-0.38f, 0.30f), P(0.38f, 0.30f), color, t);
    break;
  case Icon::Chart:
    dl->AddRectFilled(P(-0.70f, 0.10f), P(-0.36f, 0.70f), color, r * 0.06f);
    dl->AddRectFilled(P(-0.17f, -0.30f), P(0.17f, 0.70f), color, r * 0.06f);
    dl->AddRectFilled(P(0.36f, -0.70f), P(0.70f, 0.70f), color, r * 0.06f);
    break;
  case Icon::Power:
    dl->PathArcTo(c, r * 0.66f, -kPi * 0.30f, kPi * 1.30f, 24);
    dl->PathStroke(color, 0, t);
    dl->AddLine(P(0, -0.86f), P(0, -0.10f), color, t);
    break;
  }
}

// ----------------------------------------------------------------- widgets

void Gap(float px) { ImGui::Dummy(ImVec2(0, px * s_)); }

void SectionLabel(const char* text, const char* link = nullptr, bool* linkClicked = nullptr) {
  Gap(6);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float size = SmallSize();
  dl->AddText(ImGui::GetFont(), size, p, C(col::Faint), text);
  float h = size + 4 * s_;
  if (link) {
    ImVec2 ls = TextSize(ImGui::GetFont(), size, link);
    float x = ImGui::GetContentRegionAvail().x - ls.x;
    ImGui::SetCursorScreenPos(ImVec2(p.x + x, p.y));
    bool clicked = ImGui::InvisibleButton(link, ImVec2(ls.x, h));
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddText(ImGui::GetFont(), size, ImVec2(p.x + x, p.y), C(hov ? col::AccentHover : col::Muted), link);
    if (linkClicked) *linkClicked = clicked;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  } else {
    ImGui::Dummy(ImVec2(0, h));
  }
}

// Tecla dibujada como una "keycap" pequeña alineada a la derecha.
void DrawKeycap(ImDrawList* dl, ImVec2 rightMid, const std::string& key, bool onColor, float alpha) {
  if (key.empty()) return;
  float size = SmallSize() * 0.95f;
  ImVec2 ts = TextSize(ImGui::GetFont(), size, key.c_str());
  ImVec2 pad(7 * s_, 3 * s_);
  ImVec2 b(rightMid.x, rightMid.y + ts.y * 0.5f + pad.y);
  ImVec2 a(b.x - ts.x - pad.x * 2, rightMid.y - ts.y * 0.5f - pad.y);
  ImU32 bg = onColor ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 12);
  ImU32 fg = onColor ? IM_COL32(255, 255, 255, 230) : col::Muted;
  dl->AddRectFilled(a, b, C(bg, alpha), 5 * s_);
  dl->AddText(ImGui::GetFont(), size, a + pad, C(fg, alpha), key.c_str());
}

enum class Tone { Normal, Primary, Danger };

// Botón grande del panel: icono, título, subtítulo y atajo.
bool ActionButton(const char* id, Icon icon, const char* title, const std::string& subtitle, const std::string& key,
                  Tone tone, bool enabled) {
  const float h = 58 * s_;
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  ImGui::BeginDisabled(!enabled);
  bool pressed = ImGui::InvisibleButton(id, size);
  ImGui::EndDisabled();
  bool hov = enabled && ImGui::IsItemHovered();
  bool held = enabled && ImGui::IsItemActive();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float alpha = enabled ? 1.0f : 0.45f;

  ImU32 bg, chip;
  switch (tone) {
  case Tone::Primary:
    bg = held ? col::AccentActive : hov ? col::AccentHover : col::Accent;
    chip = IM_COL32(255, 255, 255, 38);
    break;
  case Tone::Danger:
    bg = held ? col::RedActive : hov ? col::RedHover : col::Red;
    chip = IM_COL32(255, 255, 255, 38);
    break;
  default:
    bg = held ? col::SurfaceActive : hov ? col::SurfaceHover : col::Surface;
    chip = IM_COL32(255, 255, 255, 12);
    break;
  }
  if (!enabled) bg = col::Surface;
  bool onColor = enabled && tone != Tone::Normal;
  dl->AddRectFilled(a, b, C(bg, alpha), 11 * s_);

  ImVec2 ic(a.x + h * 0.5f, (a.y + b.y) * 0.5f);
  dl->AddCircleFilled(ic, h * 0.31f, C(chip, alpha), 32);
  ImU32 iconColor = tone == Tone::Normal && icon == Icon::Record ? col::Red : col::Text;
  DrawIcon(dl, icon, ic, h * 0.17f, C(iconColor, alpha));

  float x = a.x + h * 0.98f;
  float fs = ImGui::GetFontSize(), ss = SmallSize();
  float mid = (a.y + b.y) * 0.5f;
  ImU32 subColor = onColor ? IM_COL32(255, 255, 255, 190) : col::Muted;
  float keyW = key.empty() ? 0 : TextSize(ImGui::GetFont(), ss * 0.95f, key.c_str()).x + 26 * s_;
  float textW = b.x - x - keyW - 8 * s_;
  dl->AddText(bold_, fs, ImVec2(x, mid - fs + 1 * s_), C(col::Text, alpha), title);
  std::string sub = Ellipsize(ImGui::GetFont(), ss, subtitle, textW);
  dl->AddText(ImGui::GetFont(), ss, ImVec2(x, mid + 2 * s_), C(subColor, alpha), sub.c_str());
  DrawKeycap(dl, ImVec2(b.x - 12 * s_, mid), key, onColor, alpha);
  return pressed && enabled;
}

// Interruptor animado (la animación se guarda en el storage de ImGui).
void DrawSwitch(ImDrawList* dl, ImVec2 rightMid, bool on, ImGuiID animId, float alpha) {
  float& t = *ImGui::GetStateStorage()->GetFloatRef(animId, on ? 1.0f : 0.0f);
  float target = on ? 1.0f : 0.0f;
  t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
  if (std::fabs(t - target) < 0.01f) t = target;
  const float w = 36 * s_, h = 20 * s_, r = h * 0.5f;
  ImVec2 a(rightMid.x - w, rightMid.y - r), b(rightMid.x, rightMid.y + r);
  dl->AddRectFilled(a, b, C(Mix(col::SurfaceActive, col::Accent, t), alpha), r);
  float knobX = a.x + r + (w - h) * t;
  dl->AddCircleFilled(ImVec2(knobX, rightMid.y), r - 3 * s_, C(IM_COL32(255, 255, 255, 255), alpha), 24);
}

// Fila con icono, texto y un interruptor; toda la fila es clicable.
bool ToggleRow(const char* id, Icon icon, const char* label, const char* detail, bool on) {
  const float h = 40 * s_;
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  bool pressed = ImGui::InvisibleButton(id, size);
  bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov) dl->AddRectFilled(a, b, C(IM_COL32(255, 255, 255, 9)), 9 * s_);
  float mid = (a.y + b.y) * 0.5f;
  DrawIcon(dl, icon, ImVec2(a.x + 18 * s_, mid), 8.5f * s_, C(on ? col::Text : col::Muted));
  float fs = ImGui::GetFontSize();
  dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + 38 * s_, mid - fs * 0.5f), C(col::Text), label);
  if (detail && *detail) {
    float lw = TextSize(ImGui::GetFont(), fs, label).x;
    float ss = SmallSize();
    dl->AddText(ImGui::GetFont(), ss, ImVec2(a.x + 38 * s_ + lw + 8 * s_, mid - ss * 0.5f + 1 * s_), C(col::Faint),
                detail);
  }
  DrawSwitch(dl, ImVec2(b.x - 8 * s_, mid), on, ImGui::GetItemID() ^ 0x5157u, 1.0f);
  return pressed;
}

// Control segmentado (p. ej. duración del replay). Devuelve el valor pulsado o -1.
int Segmented(const char* id, const std::vector<std::pair<int, std::string>>& options, int current) {
  ImGui::PushID(id);
  const float h = 34 * s_, pad = 3 * s_;
  ImVec2 a = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), C(col::Surface), 9 * s_);
  float segW = (w - pad * 2) / float(options.size());
  int clicked = -1;
  for (size_t i = 0; i < options.size(); ++i) {
    ImVec2 p(a.x + pad + segW * float(i), a.y + pad);
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(int(i));
    if (ImGui::InvisibleButton("seg", ImVec2(segW, h - pad * 2))) clicked = options[i].first;
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    bool sel = options[i].first == current;
    if (sel) dl->AddRectFilled(p, ImVec2(p.x + segW, p.y + h - pad * 2), C(col::SurfaceActive), 7 * s_);
    float ss = SmallSize();
    ImVec2 ts = TextSize(sel ? bold_ : ImGui::GetFont(), ss, options[i].second.c_str());
    dl->AddText(sel ? bold_ : ImGui::GetFont(), ss,
                ImVec2(p.x + (segW - ts.x) * 0.5f, p.y + (h - pad * 2 - ts.y) * 0.5f),
                C(sel ? col::Text : hov ? col::Text : col::Muted), options[i].second.c_str());
  }
  ImGui::SetCursorScreenPos(a);
  ImGui::Dummy(ImVec2(w, h));
  ImGui::PopID();
  return clicked;
}

bool ClipRow(int index, const ClipFile& clip) {
  ImGui::PushID(index);
  const float h = 34 * s_;
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  bool pressed = ImGui::InvisibleButton("clip", size);
  bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (hov) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip("Clic para verlo, clic derecho para mostrarlo en la carpeta");
  }
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov) dl->AddRectFilled(a, b, C(IM_COL32(255, 255, 255, 9)), 8 * s_);
  float mid = (a.y + b.y) * 0.5f;
  DrawIcon(dl, Icon::Play, ImVec2(a.x + 16 * s_, mid), 6 * s_, C(hov ? col::AccentHover : col::Muted));
  float ss = SmallSize();
  float timeW = TextSize(ImGui::GetFont(), ss, clip.when.c_str()).x;
  std::string name = Ellipsize(ImGui::GetFont(), ss, clip.name, b.x - a.x - timeW - 54 * s_);
  dl->AddText(ImGui::GetFont(), ss, ImVec2(a.x + 32 * s_, mid - ss * 0.5f), C(col::Text), name.c_str());
  dl->AddText(ImGui::GetFont(), ss, ImVec2(b.x - timeW - 8 * s_, mid - ss * 0.5f), C(col::Faint), clip.when.c_str());
  return pressed;
}

bool IconButton(const char* id, Icon icon, float size, const char* tooltip) {
  bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  bool hov = ImGui::IsItemHovered();
  ImVec2 c = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddCircleFilled(c, size * 0.5f, C(IM_COL32(255, 255, 255, 16)), 24);
    if (tooltip) ImGui::SetTooltip("%s", tooltip);
  }
  DrawIcon(dl, icon, c, size * 0.24f, C(hov ? col::Text : col::Muted));
  return pressed;
}

// Botón secundario con icono (pie del panel, barra de ajustes).
bool SoftButton(const char* id, Icon icon, const char* label, float width, Tone tone = Tone::Normal) {
  const float h = 36 * s_;
  bool pressed = ImGui::InvisibleButton(id, ImVec2(width, h));
  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bg = tone == Tone::Primary ? (held ? col::AccentActive : hov ? col::AccentHover : col::Accent)
                                   : (held ? col::SurfaceActive : hov ? col::SurfaceHover : col::Surface);
  dl->AddRectFilled(a, b, C(bg), 9 * s_);
  float fs = ImGui::GetFontSize() * 0.94f;
  ImVec2 ts = TextSize(ImGui::GetFont(), fs, label);
  float iconW = 18 * s_;
  float x = a.x + (b.x - a.x - ts.x - iconW) * 0.5f;
  float mid = (a.y + b.y) * 0.5f;
  ImU32 fg = tone == Tone::Danger ? col::Red : col::Text;
  DrawIcon(dl, icon, ImVec2(x + 6 * s_, mid), 6.5f * s_, C(tone == Tone::Primary ? col::Text : hov ? fg : col::Muted));
  dl->AddText(ImGui::GetFont(), fs, ImVec2(x + iconW, mid - ts.y * 0.5f), C(fg), label);
  return pressed;
}

void Divider() {
  Gap(4);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), C(col::Line), 1.0f);
  Gap(5);
}

void BrandMark(ImDrawList* dl, ImVec2 c, float r) {
  dl->AddRectFilled(c - ImVec2(r, r), c + ImVec2(r, r), C(col::Accent), r * 0.42f);
  // Fantasma minimalista: cúpula + base ondulada, con el punto de grabación.
  ImVec2 g = c + ImVec2(0, r * 0.06f);
  float gr = r * 0.52f;
  dl->PathArcTo(ImVec2(g.x, g.y - gr * 0.10f), gr, kPi, kPi * 2.0f, 16);
  dl->PathLineTo(ImVec2(g.x + gr, g.y + gr * 0.78f));
  dl->PathLineTo(ImVec2(g.x + gr * 0.5f, g.y + gr * 0.50f));
  dl->PathLineTo(ImVec2(g.x, g.y + gr * 0.78f));
  dl->PathLineTo(ImVec2(g.x - gr * 0.5f, g.y + gr * 0.50f));
  dl->PathLineTo(ImVec2(g.x - gr, g.y + gr * 0.78f));
  dl->PathFillConvex(C(IM_COL32(255, 255, 255, 240)));
  dl->AddCircleFilled(ImVec2(g.x, g.y - gr * 0.12f), gr * 0.26f, C(col::Red), 12);
}

// Animación de entrada: 0 -> 1 en ~160 ms con easing.
float OpenProgress(const MenuModel& m) {
  float t = float(GetTickCount64() - m.openedAt) / 160.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// --------------------------------------------------------------- panel rápido

void PanelStatus(const MenuModel& m) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float ss = SmallSize();
  ImU32 dot;
  std::string text;
  if (m.stats.recording) {
    dot = col::Red;
    text = "Grabando  " + FormatDuration(m.stats.recordSeconds);
  } else if (m.replayActive) {
    dot = col::Green;
    text = "Replay activo, guarda los últimos " + FormatClipLength(m.config.clipSeconds);
  } else if (m.config.replayEnabled && m.config.onlyInGames) {
    dot = col::Amber;
    text = "Replay esperando a un juego";
  } else {
    dot = col::Faint;
    text = "Replay apagado";
  }
  // El punto parpadea suavemente mientras se graba.
  float pulse = m.stats.recording ? 0.55f + 0.45f * std::cos(float(GetTickCount64() % 1600) / 1600.0f * kPi * 2) : 1.0f;
  dl->AddCircleFilled(ImVec2(p.x + 4 * s_, p.y + ss * 0.55f), 3.5f * s_, C(dot, pulse), 12);
  dl->AddText(ImGui::GetFont(), ss, ImVec2(p.x + 14 * s_, p.y), C(col::Muted), text.c_str());
  float h = ss + 2 * s_;
  if (m.gameDetected && !m.gameName.empty()) {
    std::string game = Ellipsize(ImGui::GetFont(), ss, m.gameName, ImGui::GetContentRegionAvail().x - 14 * s_);
    dl->AddText(ImGui::GetFont(), ss, ImVec2(p.x + 14 * s_, p.y + h), C(col::Faint), game.c_str());
    h += ss + 2 * s_;
  }
  ImGui::Dummy(ImVec2(0, h));
}

} // namespace

void DrawPanel(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  BeginFrameStyle(w);
  const float appear = OpenProgress(m);
  const float width = kPanelWidth * s_;

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s_, 14 * s_));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s_);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s_, 6 * s_));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V(col::Bg));
  ImGui::PushStyleColor(ImGuiCol_Border, V(col::Line));
  // Centrado en vertical dentro de la franja; App calcula la zona de clic igual.
  ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
  ImGui::Begin("##panel", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& c = m.config;

  // Cabecera: marca, nombre y cerrar.
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float rowH = 30 * s_;
    BrandMark(dl, ImVec2(p.x + 13 * s_, p.y + rowH * 0.5f), 13 * s_);
    float fs = ImGui::GetFontSize() * 1.08f;
    dl->AddText(bold_, fs, ImVec2(p.x + 34 * s_, p.y + (rowH - fs) * 0.5f), C(col::Text), "Spectre");
    ImGui::SetCursorScreenPos(ImVec2(p.x + ImGui::GetContentRegionAvail().x - rowH, p.y));
    if (IconButton("close", Icon::Close, rowH, "Cerrar (Esc)")) a.close();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 6 * s_));
    PanelStatus(m);
  }
  Gap(4);

  std::string clipSub = m.replayActive ? "Los últimos " + FormatClipLength(c.clipSeconds)
                        : c.replayEnabled ? "El replay arranca con el próximo juego"
                                          : "Activa el replay para poder guardar clips";
  if (ActionButton("clip", Icon::Replay, "Guardar clip", clipSub, c.hotkeys.saveClip, Tone::Primary, m.replayActive))
    a.saveClip();

  SectionLabel("DURACIÓN DEL CLIP");
  std::vector<std::pair<int, std::string>> lengths;
  for (int sec : {15, 30, 60, 120, 300}) lengths.emplace_back(sec, FormatClipLength(sec));
  if (int v = Segmented("len", lengths, c.clipSeconds); v > 0 && v != c.clipSeconds) a.setClipSeconds(v);

  Gap(6);
  if (m.stats.recording) {
    if (ActionButton("rec", Icon::Stop, "Detener grabación", "Grabando " + FormatDuration(m.stats.recordSeconds),
                     c.hotkeys.toggleRecording, Tone::Danger, true))
      a.toggleRecording();
  } else {
    if (ActionButton("rec", Icon::Record, "Grabar pantalla", "Graba hasta que la detengas", c.hotkeys.toggleRecording,
                     Tone::Normal, true))
      a.toggleRecording();
  }

  SectionLabel("CAPTURA");
  if (ToggleRow("replay", Icon::Replay, "Replay", nullptr, c.replayEnabled)) a.toggleReplay();
  if (ToggleRow("mic", Icon::Mic, "Micrófono", nullptr, c.captureMic)) a.toggleMic();
  if (ToggleRow("cam", Icon::Camera, "Cámara", nullptr, c.cameraEnabled)) a.toggleCamera();

  bool viewAll = false;
  SectionLabel("RECIENTES", "Ver todos", &viewAll);
  if (viewAll) a.openPath(m.outputDir);
  if (m.clips.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, V(col::Faint));
    ImGui::SetWindowFontScale(0.84f);
    ImGui::TextUnformatted("  Aún no hay clips ni grabaciones.");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
  }
  for (int i = 0; i < int(std::min<size_t>(m.clips.size(), 3)); ++i) {
    const auto& clip = m.clips[size_t(i)];
    if (ClipRow(i, clip)) a.openPath(clip.path);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.showInFolder(clip.path);
  }

  Divider();
  float half = (ImGui::GetContentRegionAvail().x - 8 * s_) * 0.5f;
  if (SoftButton("settings", Icon::Gear, "Ajustes", half)) a.openSettings();
  ImGui::SameLine(0, 8 * s_);
  if (SoftButton("folder", Icon::Folder, "Carpeta", half)) a.openPath(m.outputDir);

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

// Fila de formulario: etiqueta (y ayuda) a la izquierda, control a la derecha.
void FieldLabel(const char* label, const char* help = nullptr) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  ImGui::SameLine(kLabelColumn * s_);
  ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 340 * s_));
}

void Help(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, V(col::Faint));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, 560 * s_));
  ImGui::TextWrapped("%s", text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

void Heading(const char* text) {
  Gap(10);
  ImGui::PushFont(bold_);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  Gap(2);
}

// Interruptor de ajustes: etiqueta a la izquierda, switch en la columna de controles.
bool SwitchField(const char* label, bool* value, const char* help = nullptr) {
  ImGui::PushID(label);
  float rowH = ImGui::GetFrameHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  ImGui::SameLine(kLabelColumn * s_);
  bool pressed = ImGui::InvisibleButton("switch", ImVec2(40 * s_, rowH));
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (pressed) *value = !*value;
  ImVec2 b = ImGui::GetItemRectMax();
  DrawSwitch(ImGui::GetWindowDrawList(), ImVec2(b.x - 2 * s_, (p.y + b.y) * 0.5f), *value, ImGui::GetItemID() ^ 0x5157u,
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
  FieldLabel(label);
  float w = std::min(ImGui::GetContentRegionAvail().x, 340 * s_);
  float h = 8 * s_;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float rowH = ImGui::GetFrameHeight();
  ImVec2 a(p.x, p.y + (rowH - h) * 0.5f), b(p.x + w, a.y + h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(col::Surface), h * 0.5f);
  if (active) {
    // Escala en dB: -60 dB a la izquierda, 0 dB a la derecha.
    float db = level > 1e-5f ? 20.0f * std::log10(level) : -100.0f;
    float t = std::clamp(1.0f + db / 60.0f, 0.0f, 1.0f);
    ImU32 color = db > -3.0f ? C(col::Red) : db > -12.0f ? C(col::Amber) : C(col::Green);
    if (t > 0) dl->AddRectFilled(a, ImVec2(a.x + w * t, b.y), color, h * 0.5f);
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
  auto& c = m.config;
  Heading("Replay");
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
    ImGui::PushStyleColor(ImGuiCol_Text, V(col::Amber));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, 560 * s_));
    ImGui::TextWrapped("%s", hint);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  } else {
    snprintf(hint, sizeof(hint), "El replay guarda vídeo ya comprimido. Con esta calidad ocupa como mucho unos %s de RAM.",
             FormatBytes(uint64_t(estimate)).c_str());
    Help(hint);
  }

  Heading("Clips recientes");
  ImGui::PushStyleColor(ImGuiCol_Text, V(col::Muted));
  ImGui::TextUnformatted(m.outputDir.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::SmallButton("Abrir carpeta")) a.openPath(m.outputDir);
  ImGui::SameLine();
  if (ImGui::SmallButton("Actualizar")) a.refreshClips();

  ImGui::PushStyleColor(ImGuiCol_ChildBg, V(col::Surface));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * s_);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * s_, 6 * s_));
  if (ImGui::BeginChild("clips", ImVec2(0, 190 * s_), ImGuiChildFlags_AlwaysUseWindowPadding)) {
    if (m.clips.empty()) Help("Todavía no hay clips. Pulsa el atajo de guardar clip durante una partida.");
    for (int i = 0; i < int(m.clips.size()); ++i) {
      const auto& clip = m.clips[size_t(i)];
      if (ClipRow(i, clip)) {
        m.selectedClip = i;
        m.clipDuration = a.probeDuration ? a.probeDuration(clip.path) : 0;
        m.trimStart = 0;
        m.trimEnd = float(m.clipDuration);
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.showInFolder(clip.path);
      if (m.selectedClip == i)
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), C(col::Accent), 8 * s_);
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (m.selectedClip >= 0 && m.selectedClip < int(m.clips.size())) {
    const auto& clip = m.clips[size_t(m.selectedClip)];
    Heading("Recortar");
    if (m.clipDuration <= 0) {
      Help("No se pudo leer la duración de este archivo.");
    } else {
      FieldLabel("Inicio y fin");
      ImGui::DragFloatRange2("##trim", &m.trimStart, &m.trimEnd, 0.1f, 0.0f, float(m.clipDuration), "%.1f s",
                             "%.1f s", ImGuiSliderFlags_AlwaysClamp);
      ImGui::Dummy(ImVec2(kLabelColumn * s_ - ImGui::GetStyle().ItemSpacing.x, 0));
      ImGui::SameLine();
      ImGui::BeginDisabled(m.trimEnd - m.trimStart < 0.5f);
      if (ImGui::Button("Guardar recorte")) a.trim(clip.path, m.trimStart, m.trimEnd);
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Reproducir")) a.openPath(clip.path);
      Help("El recorte no recodifica: es instantáneo y sin pérdida. El inicio se ajusta al fotograma clave anterior "
           "(como mucho 2 segundos antes).");
    }
  }
}

void PageVideo(MenuModel& m) {
  auto& c = m.config;
  Heading("Qué capturar");
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

  Heading("Calidad");
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

  Heading("Codificación");
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
  Heading("Sonido del PC");
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

  Heading("Micrófono");
  m.dirty |= SwitchField("Grabar el micrófono", &c.captureMic);
  ImGui::BeginDisabled(!c.captureMic);
  FieldLabel("Dispositivo de entrada");
  m.dirty |= DeviceCombo("##in", c.micDeviceId, m.inputs);
  LevelMeter("Nivel", m.micLevel, m.stats.running && !m.stats.micAudio.empty(), m.stats.micAudio);
  FieldLabel("Volumen");
  m.dirty |= ImGui::SliderFloat("##micvol", &c.micVolume, 0.0f, 2.0f, "x%.2f");
  ImGui::EndDisabled();

  Heading("Pistas");
  m.dirty |= SwitchField("Pistas separadas", &c.separateTracks,
                         "Además de la mezcla, guarda el PC y el micrófono en pistas aparte para editarlas por "
                         "separado.");
}

void PageCamera(MenuModel& m) {
  auto& c = m.config;
  Heading("Cámara");
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
  Heading("Atajos de teclado");
  Help("Escribe la combinación, por ejemplo Alt+Z, Ctrl+Shift+F10 o Alt+Num5. Déjalo vacío para desactivarlo.");
  Gap(4);
  auto field = [&](const char* label, const char* id, std::string& value) {
    FieldLabel(label);
    ImGui::SetNextItemWidth(200 * s_);
    if (ImGui::InputText(id, &value)) m.dirty = true;
    UINT mods = 0, vk = 0;
    if (!value.empty() && !ParseHotkey(value, &mods, &vk)) {
      ImGui::SameLine();
      ImGui::TextColored(V(col::Red), "No es una combinación válida");
    }
  };
  field("Abrir el panel", "##hk1", h.overlay);
  field("Guardar clip", "##hk2", h.saveClip);
  field("Grabar o detener", "##hk3", h.toggleRecording);
  field("Activar o pausar el replay", "##hk4", h.toggleReplay);
  if (!m.hotkeyError.empty()) {
    Gap(4);
    ImGui::PushStyleColor(ImGuiCol_Text, V(col::Red));
    ImGui::TextWrapped("%s", m.hotkeyError.c_str());
    ImGui::PopStyleColor();
  }
}

// Estado de la actualización: punto de color (o ruedita si está trabajando) y texto.
void UpdateStatusLine(ImU32 color, bool busy, const std::string& text, bool bold = false) {
  float rowH = ImGui::GetFrameHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 c(p.x + 6 * s_, p.y + rowH * 0.5f);
  if (busy) {
    float t = float(ImGui::GetTime()) * 5.0f;
    dl->PathArcTo(c, 5.5f * s_, t, t + kPi * 1.4f, 16);
    dl->PathStroke(C(color), 0, 1.8f * s_);
  } else {
    dl->AddCircleFilled(c, 4 * s_, C(color), 16);
  }
  ImFont* font = bold ? bold_ : ImGui::GetFont();
  float fs = ImGui::GetFontSize();
  float wrap = std::min(ImGui::GetContentRegionAvail().x, 560 * s_) - 20 * s_;
  ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, wrap, text.c_str());
  dl->AddText(font, fs, ImVec2(p.x + 20 * s_, p.y + (rowH - fs) * 0.5f), C(col::Text), text.c_str(), nullptr, wrap);
  ImGui::Dummy(ImVec2(0, std::max(rowH, ts.y + (rowH - fs))));
}

void UpdatesSection(MenuModel& m, const MenuActions& a) {
  using S = UpdateStatus::State;
  const UpdateStatus& u = m.update;
  Heading("Actualizaciones");
  ImGui::PushStyleColor(ImGuiCol_Text, V(col::Muted));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Versión instalada");
  ImGui::PopStyleColor();
  ImGui::SameLine(kLabelColumn * s_);
  ImGui::TextUnformatted(SPECTRE_VERSION);
  Gap(2);

  const float bw = 230 * s_;
  char buf[256];
  switch (u.state) {
  case S::Idle:
    Help("Spectre solo se conecta a GitHub cuando pulsas el botón: no comprueba nada en segundo plano.");
    Gap(4);
    if (SoftButton("update-check", Icon::Replay, "Buscar actualizaciones", bw)) a.checkUpdates();
    break;
  case S::Checking:
    UpdateStatusLine(col::Muted, true, "Buscando actualizaciones...");
    break;
  case S::UpToDate:
    snprintf(buf, sizeof(buf), "Tienes la última versión (%s).", SPECTRE_VERSION);
    UpdateStatusLine(col::Green, false, buf);
    Gap(4);
    if (SoftButton("update-check", Icon::Replay, "Volver a buscar", bw)) a.checkUpdates();
    break;
  case S::Available:
    snprintf(buf, sizeof(buf), "Versión nueva disponible: %s", u.latest.c_str());
    UpdateStatusLine(col::Accent, false, buf, true);
    if (u.portable || u.unverified) {
      if (u.portable)
        Help("Esta es la copia portable de Spectre: descarga el zip nuevo y reemplaza la carpeta. Si usas el "
             "instalador, Spectre se actualiza solo desde aquí.");
      else
        Help("GitHub no ha publicado la huella de este instalador, así que Spectre no puede comprobarlo y no lo "
             "instala solo. Descárgalo desde la página de la versión.");
      Gap(4);
      if (SoftButton("update-page", Icon::Folder, "Abrir página de descarga", bw, Tone::Primary))
        a.openPath(Updater::ReleasePage());
    } else {
      std::string help = "Se descarga el instalador";
      if (u.sizeBytes > 0) help += " (" + FormatBytes(u.sizeBytes) + ")";
      help += ", Spectre se cierra, se actualiza conservando tus ajustes y se vuelve a abrir solo.";
      if (m.stats.recording) help += " Si sigues grabando, se instalará cuando detengas la grabación.";
      Help(help.c_str());
      Gap(4);
      if (SoftButton("update-install", Icon::Play, "Actualizar ahora", bw, Tone::Primary)) a.installUpdate();
    }
    break;
  case S::Downloading: {
    int pct = int(std::clamp(u.downloaded, 0.0, 1.0) * 100.0 + 0.5);
    snprintf(buf, sizeof(buf), "Descargando la versión %s... %d %%", u.latest.c_str(), pct);
    UpdateStatusLine(col::Accent, true, buf);
    float w = std::min(ImGui::GetContentRegionAvail().x, 340 * s_), h = 6 * s_;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), C(col::Surface), h * 0.5f);
    if (pct > 0) dl->AddRectFilled(p, ImVec2(p.x + w * float(pct) / 100.0f, p.y + h), C(col::Accent), h * 0.5f);
    ImGui::Dummy(ImVec2(w, h));
    Gap(8);
    if (SoftButton("update-cancel", Icon::Close, "Cancelar", 150 * s_)) a.cancelUpdate();
    break;
  }
  case S::Launching:
    if (m.stats.recording)
      UpdateStatusLine(col::Accent, false, "Actualización lista: se instalará cuando detengas la grabación.");
    else
      UpdateStatusLine(col::Accent, true, "Abriendo el instalador. Spectre se cerrará y volverá a abrirse al terminar.");
    break;
  case S::Error:
    UpdateStatusLine(col::Red, false, u.message);
    Gap(4);
    if (SoftButton("update-check", Icon::Replay, "Reintentar", bw)) a.checkUpdates();
    break;
  }
}

void PageGeneral(MenuModel& m, const MenuActions& a) {
  auto& c = m.config;
  Heading("Archivos");
  FieldLabel("Carpeta de destino");
  if (ImGui::InputTextWithHint("##dir", m.outputDir.c_str(), &c.outputDir)) m.dirty = true;
  Help("Déjalo vacío para usar Vídeos\\Spectre.");

  Heading("Inicio");
  m.dirty |= SwitchField("Iniciar con Windows", &c.autostart, "Spectre arranca minimizado en la bandeja.");
  m.dirty |= SwitchField("Mostrar avisos", &c.showNotifications,
                         "Avisos breves al guardar un clip o empezar a grabar. Los errores se muestran siempre.");

  Heading("Memoria");
  FieldLabel("Límite del replay");
  m.dirty |= ImGui::SliderInt("##mem", &c.maxBufferMB, 64, 4096, "%d MB");
  Help("Tope de RAM para el replay. Si la duración y la calidad elegidas no caben, se guardan menos segundos.");

  UpdatesSection(m, a);

  Heading("Spectre");
  Help("Cerrar el panel no cierra Spectre: sigue en la bandeja del sistema, junto al reloj.");
  Gap(4);
  if (SoftButton("quit", Icon::Power, "Salir de Spectre", 200 * s_, Tone::Danger)) a.quit();
}

void PagePerformance(MenuModel& m) {
  const auto& s = m.stats;
  auto row = [&](const char* label, const std::string& value) {
    ImGui::PushStyleColor(ImGuiCol_Text, V(col::Muted));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(kLabelColumn * s_);
    ImGui::TextUnformatted(value.c_str());
  };
  char buf[160];
  Heading("Consumo de Spectre");
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

  Heading("Captura");
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

struct Page {
  const char* name;
  Icon icon;
};
constexpr Page kPages[] = {{"Clips", Icon::Replay},  {"Vídeo", Icon::Monitor},   {"Audio", Icon::Mic},
                           {"Cámara", Icon::Camera}, {"Atajos", Icon::Keyboard}, {"General", Icon::Gear},
                           {"Rendimiento", Icon::Chart}};

bool NavItem(int index, const Page& page, bool selected) {
  ImGui::PushID(index);
  const float h = 38 * s_;
  bool pressed = ImGui::InvisibleButton("nav", ImVec2(ImGui::GetContentRegionAvail().x, h));
  bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (selected) {
    dl->AddRectFilled(a, b, C(col::SurfaceActive), 9 * s_);
    dl->AddRectFilled(ImVec2(a.x, a.y + 10 * s_), ImVec2(a.x + 3 * s_, b.y - 10 * s_), C(col::Accent), 2 * s_);
  } else if (hov) {
    dl->AddRectFilled(a, b, C(IM_COL32(255, 255, 255, 9)), 9 * s_);
  }
  float mid = (a.y + b.y) * 0.5f;
  DrawIcon(dl, page.icon, ImVec2(a.x + 22 * s_, mid), 8 * s_, C(selected ? col::Text : col::Muted));
  float fs = ImGui::GetFontSize();
  dl->AddText(selected ? bold_ : ImGui::GetFont(), fs, ImVec2(a.x + 42 * s_, mid - fs * 0.5f),
              C(selected ? col::Text : col::Muted), page.name);
  return pressed;
}

} // namespace

void DrawSettings(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  BeginFrameStyle(w);
  ImGuiIO& io = ImGui::GetIO();
  const float appear = OpenProgress(m);
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, int(140 * appear)));

  ImVec2 size(std::min(io.DisplaySize.x - 48 * s_, 920 * s_), std::min(io.DisplaySize.y - 48 * s_, 660 * s_));
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s_);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V(col::Bg));
  ImGui::PushStyleColor(ImGuiCol_Border, V(col::Line));
  ImGui::SetNextWindowPos(io.DisplaySize * 0.5f + ImVec2(0, (1.0f - appear) * 12 * s_), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("Ajustes", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar(); // WindowPadding: los hijos usan el suyo

  // Barra lateral
  const float navW = 210 * s_;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, V(IM_COL32(255, 255, 255, 5)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s_, 14 * s_));
  ImGui::BeginChild("nav", ImVec2(navW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float rowH = 30 * s_;
    ImGui::SetCursorScreenPos(p);
    if (IconButton("back", Icon::Back, rowH, "Volver al panel")) a.backToPanel();
    float fs = ImGui::GetFontSize() * 1.08f;
    ImGui::GetWindowDrawList()->AddText(bold_, fs, ImVec2(p.x + rowH + 6 * s_, p.y + (rowH - fs) * 0.5f),
                                        C(col::Text), "Ajustes");
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 14 * s_));
  }
  for (int i = 0; i < int(std::size(kPages)); ++i)
    if (NavItem(i, kPages[i], m.page == i)) m.page = i;
  {
    float ss = SmallSize();
    ImVec2 p(ImGui::GetWindowPos().x + 14 * s_, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ss - 16 * s_);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ss, p, C(col::Faint), "Spectre " SPECTRE_VERSION);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  ImGui::SameLine(0, 0);
  ImGui::BeginGroup();
  // Cabecera de la página
  const float footerH = 60 * s_;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * s_, 18 * s_));
  ImGui::BeginChild("page", ImVec2(0, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(big_);
    ImGui::TextUnformatted(kPages[m.page].name);
    ImGui::PopFont();
    float closeSize = 30 * s_;
    ImGui::SetCursorScreenPos(ImVec2(p.x + ImGui::GetContentRegionAvail().x - closeSize, p.y));
    if (IconButton("close", Icon::Close, closeSize, "Cerrar (Esc)")) a.close();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::max(closeSize, big_->FontSize) + 4 * s_));
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
    ImVec2 p = ImGui::GetCursorScreenPos();
    float wAvail = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(p, ImVec2(p.x + wAvail, p.y), C(col::Line));
    float bw = 150 * s_;
    float y = p.y + (footerH - 36 * s_) * 0.5f;
    if (m.dirty) {
      float ss = SmallSize();
      dl->AddText(ImGui::GetFont(), ss, ImVec2(p.x + 28 * s_, p.y + (footerH - ss) * 0.5f), C(col::Muted),
                  "Hay cambios sin aplicar");
      ImGui::SetCursorScreenPos(ImVec2(p.x + wAvail - bw * 2 - 8 * s_ - 24 * s_, y));
      if (SoftButton("discard", Icon::Close, "Descartar", bw)) a.backToPanel();
      ImGui::SameLine(0, 8 * s_);
      if (SoftButton("apply", Icon::Play, "Aplicar", bw, Tone::Primary)) {
        a.apply(m.config);
        m.dirty = false;
      }
    } else {
      float ss = SmallSize();
      std::string hint = "Esc o " + m.config.hotkeys.overlay + " para cerrar";
      dl->AddText(ImGui::GetFont(), ss, ImVec2(p.x + 28 * s_, p.y + (footerH - ss) * 0.5f), C(col::Faint),
                  hint.c_str());
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(wAvail, footerH));
  }
  ImGui::EndGroup();

  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);

  // Esc en un campo de texto o un desplegable solo cierra eso. ImGui los
  // desactiva en este mismo frame, por eso se mira también el anterior.
  bool busy = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !busy && !m.busyLastFrame) a.close();
  m.busyLastFrame = busy;
}

void DrawToast(OverlayWindow& w, const std::string& text, bool error, float alpha) {
  BeginFrameStyle(w);
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
  ImVec2 a(1, 1), b(io.DisplaySize.x - 1, io.DisplaySize.y - 1);
  dl->AddRectFilled(a, b, C(col::Bg), 14 * s_);
  dl->AddRect(a, b, C(col::Line), 14 * s_);
  ImVec2 mark(a.x + 30 * s_, (a.y + b.y) * 0.5f);
  if (error) {
    dl->AddCircleFilled(mark, 12 * s_, C(col::Red), 24);
    dl->AddRectFilled(mark + ImVec2(-1.5f * s_, -6.5f * s_), mark + ImVec2(1.5f * s_, 2.0f * s_), C(col::Text));
    dl->AddCircleFilled(mark + ImVec2(0, 5.5f * s_), 1.8f * s_, C(col::Text));
  } else {
    BrandMark(dl, mark, 12 * s_);
  }
  float x = a.x + 54 * s_;
  float wrap = b.x - x - 16 * s_;
  float fs = ImGui::GetFontSize() * 0.95f;
  ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, wrap, text.c_str());
  dl->AddText(ImGui::GetFont(), fs, ImVec2(x, (a.y + b.y - ts.y) * 0.5f), C(col::Text), text.c_str(), nullptr, wrap);
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

} // namespace spectre
