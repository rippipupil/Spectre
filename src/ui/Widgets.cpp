#include "ui/Widgets.h"

#include "app/Config.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace spectre::ui {

namespace {

// Estado del frame actual (fuentes y escala de la ventana que se dibuja).
float s_ = 1.0f;
ImFont* bold_ = nullptr;
ImFont* big_ = nullptr;
const FontSet* sizes_ = nullptr;

// Sombra psíquica de cada contexto de ImGui (panel y ventana de la app pueden
// estar abiertos a la vez y sus relojes son distintos).
struct HoverTrail {
  ImGuiContext* context = nullptr;
  ImGuiID current = 0;
  ImVec2 a, b; // rectángulo actual
  ImVec2 ghostA, ghostB;
  double ghostSince = -10.0;
  unsigned long long openedAt = 0; // apertura a la que pertenece
};
HoverTrail trails_[3];

HoverTrail& Trail() {
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  for (auto& t : trails_)
    if (t.context == ctx) return t;
  // Un contexto nuevo ocupa el hueco de uno que ya no existe (o el primero).
  for (auto& t : trails_)
    if (!t.context) {
      t = HoverTrail{};
      t.context = ctx;
      return t;
    }
  trails_[0] = HoverTrail{};
  trails_[0].context = ctx;
  return trails_[0];
}

} // namespace

void BeginFrame(const Fonts& fonts, const Config& config) {
  s_ = fonts.scale;
  bold_ = fonts.bold ? fonts.bold : ImGui::GetFont();
  big_ = fonts.big ? fonts.big : ImGui::GetFont();
  sizes_ = fonts.sizes;
  BeginThemeFrame(config);
}

float Px(float logical) { return logical * s_; }
ImFont* Bold() { return bold_; }
ImFont* Big() { return big_; }
ImFont* Sized(ImFont* font, float size) { return sizes_ ? sizes_->ForSize(font, size) : font; }

ImU32 C(ImU32 color, float alpha) { return ImGui::GetColorU32(color, alpha); }
ImVec4 V(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  auto ch = [&](int shift) {
    float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
    return ImU32(x + (y - x) * t + 0.5f) << shift;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return Sized(font, size)->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text) {
  dl->AddText(Sized(font, size), size, pos, color, text);
}

float TrackedWidth(ImFont* font, float size, const char* text, float tracking) {
  font = Sized(font, size);
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
  font = Sized(font, size);
  float x = pos.x;
  for (const char* c = text; *c;) {
    const char* next = c + 1;
    while (*next && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
    dl->AddText(font, size, ImVec2(x, pos.y), color, c, next);
    x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, next).x + tracking;
    c = next;
  }
}

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
  if (seconds < 60) snprintf(buf, sizeof(buf), "%d s", seconds);
  else if (seconds % 60 == 0) snprintf(buf, sizeof(buf), "%d min", seconds / 60);
  else snprintf(buf, sizeof(buf), "%d min %d s", seconds / 60, seconds % 60);
  return buf;
}

// ----------------------------------------------------------------- iconos

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float r, ImU32 color) {
  const float t = std::max(1.2f, r * 0.17f);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
  auto line = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), color, t); };
  auto poly = [&](std::initializer_list<ImVec2> pts, bool closed) {
    std::vector<ImVec2> v;
    for (const ImVec2& p : pts) v.push_back(P(p.x, p.y));
    dl->AddPolyline(v.data(), int(v.size()), color, closed ? ImDrawFlags_Closed : 0, t);
  };
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
    line(0, 0.42f, 0, 0.80f);
    line(-0.30f, 0.80f, 0.30f, 0.80f);
    break;
  case Icon::Camera:
    dl->AddRect(P(-0.85f, -0.48f), P(0.32f, 0.48f), color, 0, 0, t);
    line(0.32f, -0.10f, 0.88f, -0.40f);
    line(0.88f, -0.40f, 0.88f, 0.40f);
    line(0.88f, 0.40f, 0.32f, 0.10f);
    break;
  case Icon::Close:
    line(-0.45f, -0.45f, 0.45f, 0.45f);
    line(0.45f, -0.45f, -0.45f, 0.45f);
    break;
  case Icon::Back:
  case Icon::Prev:
    line(0.22f, -0.55f, -0.30f, 0.0f);
    line(-0.30f, 0.0f, 0.22f, 0.55f);
    break;
  case Icon::Next:
    line(-0.22f, -0.55f, 0.30f, 0.0f);
    line(0.30f, 0.0f, -0.22f, 0.55f);
    break;
  case Icon::Play:
    poly({{-0.40f, -0.60f}, {0.52f, 0.0f}, {-0.40f, 0.60f}}, true);
    break;
  case Icon::Pause:
    line(-0.28f, -0.58f, -0.28f, 0.58f);
    line(0.28f, -0.58f, 0.28f, 0.58f);
    break;
  case Icon::Folder:
    poly({{-0.85f, -0.58f}, {-0.25f, -0.58f}, {-0.08f, -0.38f}, {0.85f, -0.38f}, {0.85f, 0.62f}, {-0.85f, 0.62f}},
         true);
    break;
  case Icon::Trash:
    line(-0.72f, -0.52f, 0.72f, -0.52f);
    poly({{-0.24f, -0.52f}, {-0.24f, -0.76f}, {0.24f, -0.76f}, {0.24f, -0.52f}}, false);
    poly({{-0.55f, -0.52f}, {-0.44f, 0.80f}, {0.44f, 0.80f}, {0.55f, -0.52f}}, false);
    line(-0.15f, -0.22f, -0.15f, 0.52f);
    line(0.15f, -0.22f, 0.15f, 0.52f);
    break;
  case Icon::Star:
  case Icon::StarFilled: {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
      float a = -kPi * 0.5f + float(i) * kPi / 5.0f;
      float rr = (i % 2 == 0 ? 0.88f : 0.38f) * r;
      pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    if (icon == Icon::StarFilled) {
      // Abanico desde el centro sin antialiasing (evita las juntas entre
      // triángulos); el contorno de encima suaviza el borde.
      ImDrawListFlags flags = dl->Flags;
      dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
      for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], color);
      dl->Flags = flags;
    }
    dl->AddPolyline(pts, 10, color, ImDrawFlags_Closed, t);
    break;
  }
  case Icon::Scissors:
    dl->AddCircle(P(-0.46f, 0.50f), r * 0.24f, color, 12, t);
    dl->AddCircle(P(0.46f, 0.50f), r * 0.24f, color, 12, t);
    line(-0.32f, 0.31f, 0.42f, -0.80f);
    line(0.32f, 0.31f, -0.42f, -0.80f);
    break;
  case Icon::Copy:
    poly({{-0.20f, -0.45f}, {-0.20f, -0.78f}, {0.70f, -0.78f}, {0.70f, 0.30f}, {0.40f, 0.30f}}, false);
    dl->AddRect(P(-0.70f, -0.45f), P(0.40f, 0.78f), color, 0, 0, t);
    break;
  case Icon::Pencil:
    poly({{-0.72f, 0.72f}, {-0.58f, 0.30f}, {0.38f, -0.66f}, {0.66f, -0.38f}, {-0.30f, 0.58f}}, true);
    line(0.16f, -0.44f, 0.44f, -0.16f);
    break;
  case Icon::Shot:
    // Visor: cuatro esquinas y el marco del objetivo.
    poly({{-0.85f, -0.30f}, {-0.85f, -0.70f}, {-0.45f, -0.70f}}, false);
    poly({{0.45f, -0.70f}, {0.85f, -0.70f}, {0.85f, -0.30f}}, false);
    poly({{0.85f, 0.30f}, {0.85f, 0.70f}, {0.45f, 0.70f}}, false);
    poly({{-0.45f, 0.70f}, {-0.85f, 0.70f}, {-0.85f, 0.30f}}, false);
    dl->AddRect(P(-0.25f, -0.25f), P(0.25f, 0.25f), color, 0, 0, t);
    break;
  case Icon::Marker:
    line(-0.48f, -0.82f, -0.48f, 0.86f);
    poly({{-0.48f, -0.76f}, {0.62f, -0.48f}, {-0.48f, -0.18f}}, false);
    break;
  case Icon::Search:
    dl->AddCircle(P(-0.15f, -0.15f), r * 0.52f, color, 20, t);
    line(0.23f, 0.23f, 0.78f, 0.78f);
    break;
  case Icon::Volume:
  case Icon::Muted:
    poly({{-0.80f, -0.26f}, {-0.42f, -0.26f}, {0.02f, -0.66f}, {0.02f, 0.66f}, {-0.42f, 0.26f}, {-0.80f, 0.26f}},
         true);
    if (icon == Icon::Volume) {
      dl->PathArcTo(P(0.02f, 0.0f), r * 0.40f, -kPi * 0.28f, kPi * 0.28f, 8);
      dl->PathStroke(color, 0, t);
      dl->PathArcTo(P(0.02f, 0.0f), r * 0.72f, -kPi * 0.28f, kPi * 0.28f, 10);
      dl->PathStroke(color, 0, t);
    } else {
      line(0.36f, -0.30f, 0.86f, 0.30f);
      line(0.86f, -0.30f, 0.36f, 0.30f);
    }
    break;
  case Icon::External:
    poly({{-0.05f, -0.70f}, {-0.75f, -0.70f}, {-0.75f, 0.75f}, {0.70f, 0.75f}, {0.70f, 0.05f}}, false);
    line(-0.05f, 0.10f, 0.78f, -0.73f);
    poly({{0.28f, -0.78f}, {0.78f, -0.78f}, {0.78f, -0.28f}}, false);
    break;
  case Icon::Share:
    // Bandeja con una flecha hacia arriba: enviar (exportar para Discord).
    poly({{-0.80f, 0.15f}, {-0.80f, 0.78f}, {0.80f, 0.78f}, {0.80f, 0.15f}}, false);
    line(0.0f, 0.42f, 0.0f, -0.80f);
    poly({{-0.40f, -0.40f}, {0.0f, -0.80f}, {0.40f, -0.40f}}, false);
    break;
  case Icon::Sliders:
    // Tres deslizadores: el editor.
    for (int i = 0; i < 3; ++i) {
      const float y = -0.55f + 0.55f * float(i), knob = i == 0 ? 0.35f : i == 1 ? -0.35f : 0.10f;
      line(-0.82f, y, 0.82f, y);
      dl->AddRectFilled(P(knob - 0.15f, y - 0.17f), P(knob + 0.15f, y + 0.17f), color);
    }
    break;
  case Icon::Undo:
  case Icon::Redo: {
    // Flecha que vuelve: hacia la izquierda (deshacer) o hacia la derecha (rehacer).
    const float k = icon == Icon::Undo ? 1.0f : -1.0f;
    line(-0.66f * k, -0.30f, 0.18f * k, -0.30f);
    if (k > 0) dl->PathArcTo(P(0.18f, 0.12f), r * 0.42f, -kPi * 0.5f, kPi * 0.5f, 12);
    else dl->PathArcTo(P(-0.18f, 0.12f), r * 0.42f, -kPi * 0.5f, -kPi * 1.5f, 12);
    dl->PathStroke(color, 0, t);
    line(0.18f * k, 0.54f, -0.40f * k, 0.54f);
    poly({{-0.36f * k, -0.62f}, {-0.68f * k, -0.30f}, {-0.36f * k, 0.02f}}, false);
    break;
  }
  }
}

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

void ResetHoverTrail(unsigned long long openedAt) {
  // El reloj de ImGui vuelve a cero con cada contexto nuevo: un resto
  // antiguo parpadearía.
  HoverTrail& trail = Trail();
  if (trail.openedAt == openedAt) return;
  ImGuiContext* ctx = trail.context;
  trail = HoverTrail{};
  trail.context = ctx;
  trail.openedAt = openedAt;
}

void TrackHover(ImGuiID id, ImVec2 a, ImVec2 b, bool hovered) {
  HoverTrail& trail = Trail();
  if (!hovered) {
    if (trail.current == id) {
      trail.ghostA = trail.a;
      trail.ghostB = trail.b;
      trail.ghostSince = ImGui::GetTime();
      trail.current = 0;
    }
    return;
  }
  if (trail.current != id) {
    if (trail.current != 0) {
      trail.ghostA = trail.a;
      trail.ghostB = trail.b;
      trail.ghostSince = ImGui::GetTime();
    }
    trail.current = id;
  }
  trail.a = a;
  trail.b = b;
}

void DrawHoverTrail(ImDrawList* dl) {
  if (!ThemeMotion()) return;
  const HoverTrail& trail = Trail();
  float t = float(ImGui::GetTime() - trail.ghostSince) / 0.2f;
  if (t < 0 || t >= 1) return;
  dl->AddRectFilled(trail.ghostA, trail.ghostB, C(Colors().accent, 0.12f * (1.0f - t)));
}

void DrawTrace(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color) {
  if (!ThemeMotion()) return;
  const float period = 2.8f, len = Px(34);
  float phase = float(std::fmod(ImGui::GetTime(), double(period))) / period;
  float alpha = phase < 0.1f ? phase / 0.1f : phase > 0.9f ? (1.0f - phase) / 0.1f : 1.0f;
  float x = a.x + (b.x - a.x - len) * phase;
  dl->AddRectFilled(ImVec2(x, b.y - Px(2)), ImVec2(x + len, b.y), C(color, alpha));
}

float Breathe() {
  if (!ThemeMotion()) return 1.0f;
  return 0.35f + 0.65f * (0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.0f * kPi / 2.6f));
}

float OpenProgress(unsigned long long openedAt) {
  float t = float(GetTickCount64() - openedAt) / 120.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t);
}

// ----------------------------------------------------------------- widgets

void Gap(float px) { ImGui::Dummy(ImVec2(0, Px(px))); }

void Divider(float above, float below) {
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

void SectionLabel(const char* text, const char* link, bool* linkClicked) {
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
    Text(dl, ImGui::GetFont(), ls, ImVec2(p.x + x, p.y + (h - ts.y) * 0.5f), C(hov ? Colors().text : Colors().textMuted),
         link);
    if (hov) dl->AddRectFilled(ImVec2(p.x + x, p.y + h - 1.0f * s_), ImVec2(p.x + x + ts.x, p.y + h), C(Colors().text));
    if (linkClicked) *linkClicked = clicked;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  } else {
    ImGui::Dummy(ImVec2(0, h));
  }
}

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

bool ActionButton(const char* id, const char* title, const std::string& key, Tone tone, bool enabled, bool recordMark,
                  float width) {
  const Palette& p = Colors();
  const float h = Px(44);
  ImVec2 size(width > 0 ? width : ImGui::GetContentRegionAvail().x, h);
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
    fg = p.onDanger;
    keyBorder = Mix(p.onDanger, bg, 0.53f);
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

int Segmented(const char* id, const std::vector<std::pair<int, std::string>>& options, int current, float width) {
  const Palette& p = Colors();
  ImGui::PushID(id);
  const float h = Px(32);
  ImVec2 a = ImGui::GetCursorScreenPos();
  float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), C(p.surface));
  float segW = w / float(std::max<size_t>(options.size(), 1));
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

int ClipLengthPicker(const char* id, int current, double fitSeconds, float width) {
  const Palette& p = Colors();
  ImGui::PushID(id);
  constexpr int kCols = 4;
  const int count = int(std::size(kClipLengths));
  const int rows = (count + kCols - 1) / kCols;
  const float h = Px(28), line = Px(1);
  ImVec2 a = ImGui::GetCursorScreenPos();
  float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Celdas planas separadas por una línea de 1 px del color del fondo.
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h * float(rows)), C(p.surface));
  float cellW = w / float(kCols);
  int clicked = -1;
  for (int i = 0; i < count; ++i) {
    const int sec = kClipLengths[i];
    ImVec2 q(a.x + cellW * float(i % kCols), a.y + h * float(i / kCols));
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("len", ImVec2(cellW, h))) clicked = sec;
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    // Con este bitrate no cabe entera en la RAM del replay: se ve atenuada y
    // lo explica al pasar el ratón (se puede elegir igual).
    const bool tight = fitSeconds > 0 && double(sec) > fitSeconds + 0.5;
    if (hov) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (tight)
        ImGui::SetTooltip("Con esta calidad no cabe entera en la memoria del replay: en escenas de mucho "
                          "movimiento el clip saldría más corto. Caben unos %s.",
                          FormatClipLength(std::max(5, int(fitSeconds) / 5 * 5)).c_str());
    }
    bool sel = sec == current;
    ImVec2 b(q.x + cellW, q.y + h);
    if (sel) dl->AddRectFilled(q, b, C(p.accent));
    else if (hov) dl->AddRectFilled(q, b, C(p.surfaceHover));
    if (i % kCols != 0) dl->AddRectFilled(q, ImVec2(q.x + line, b.y), C(p.bg));
    if (i >= kCols) dl->AddRectFilled(q, ImVec2(b.x, q.y + line), C(p.bg));
    float ss = Px(kSmall);
    ImFont* font = sel ? bold_ : ImGui::GetFont();
    std::string label = FormatClipLength(sec);
    ImVec2 ts = TextSize(font, ss, label.c_str());
    ImU32 fg = sel ? p.onAccent : tight ? p.textFaint : hov ? p.text : p.textMuted;
    Text(dl, font, ss, ImVec2(q.x + (cellW - ts.x) * 0.5f, q.y + (h - ts.y) * 0.5f), C(fg), label.c_str());
  }
  ImGui::SetCursorScreenPos(a);
  ImGui::Dummy(ImVec2(w, h * float(rows)));
  ImGui::PopID();
  return clicked;
}

bool IconButton(const char* id, Icon icon, float size, const char* tooltip, bool active) {
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
  DrawIcon(dl, icon, c, size * 0.25f, C(active ? p.accent : hov ? p.text : p.textMuted));
  return pressed;
}

bool GhostButton(const char* id, const char* label, float width, Tone tone, float height) {
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

bool SolidButton(const char* id, const char* label, float width, float height) {
  const Palette& p = Colors();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(width, Px(height)));
  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(held ? p.accentActive : hov ? p.accentHover : p.accent));
  float fs = Px(kSmall);
  ImVec2 ts = TextSize(bold_, fs, label);
  Text(dl, bold_, fs, ImVec2(a.x + (b.x - a.x - ts.x) * 0.5f, a.y + (b.y - a.y - ts.y) * 0.5f), C(p.onAccent), label);
  return pressed;
}

void Bar(ImVec2 size, float fraction, ImU32 color) {
  ImVec2 q = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(q, q + size, C(Colors().surface));
  float f = std::clamp(fraction, 0.0f, 1.0f);
  if (f > 0) dl->AddRectFilled(q, ImVec2(q.x + size.x * f, q.y + size.y), C(color));
  ImGui::Dummy(size);
}

// --------------------------------------------------------- formularios

float ControlWidth() { return std::min(ImGui::GetContentRegionAvail().x, Px(kControlWidth)); }

void FieldLabel(const char* label, const char* help) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", help);
  ImGui::SameLine(Px(kLabelColumn));
  ImGui::SetNextItemWidth(ControlWidth());
}

namespace {
void WrappedText(ImU32 color, const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, V(color));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, Px(560)));
  ImGui::TextWrapped("%s", text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}
} // namespace

void Help(const char* text) { WrappedText(Colors().textFaint, text); }
void Warning(const char* text) { WrappedText(Colors().warning, text); }
void ErrorText(const char* text) { WrappedText(Colors().danger, text); }

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

bool SwitchField(const char* label, bool* value, const char* help) {
  ImGui::PushID(label);
  float rowH = ImGui::GetFrameHeight();
  ImVec2 q = ImGui::GetCursorScreenPos();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", help);
  ImGui::SameLine(Px(kLabelColumn));
  bool pressed = ImGui::InvisibleButton("switch", ImVec2(Px(32), rowH));
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (pressed) *value = !*value;
  ImVec2 b = ImGui::GetItemRectMax();
  // Alfa 1: C() ya aplica style.Alpha, así dentro de BeginDisabled no se atenúa dos veces.
  DrawSwitch(ImGui::GetWindowDrawList(), ImVec2(b.x, (q.y + b.y) * 0.5f), *value, ImGui::GetItemID() ^ 0x5157u, 1.0f);
  ImGui::PopID();
  return pressed;
}

void ValueRow(const char* label, const std::string& value) {
  ImGui::PushStyleColor(ImGuiCol_Text, V(Colors().textMuted));
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::SameLine(Px(kLabelColumn));
  ImGui::TextUnformatted(value.c_str());
}

bool NavItem(int index, const char* name, bool selected, float height) {
  const Palette& p = Colors();
  ImGui::PushID(index);
  const float h = Px(height);
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

void NavLabel(const char* text) {
  Gap(12);
  ImVec2 q = ImGui::GetCursorScreenPos();
  float size = Px(kLabel);
  TrackedText(ImGui::GetWindowDrawList(), bold_, size, ImVec2(q.x + Px(16), q.y), C(Colors().textFaint), text,
              Px(1.5f));
  ImGui::Dummy(ImVec2(0, size + Px(6)));
}

} // namespace spectre::ui
