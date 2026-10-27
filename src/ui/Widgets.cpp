#include "ui/Widgets.h"

#include "app/Config.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
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
bool frameRequested_ = false; // algo se anima: hace falta otro frame

float Clamp01(float t) { return std::clamp(t, 0.0f, 1.0f); }

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
  frameRequested_ = false;
  BeginThemeFrame(config);
  // Las letras de la ventana son de otro diseño: que las cambie antes del
  // siguiente frame (este se dibuja con las que hay).
  const Style& st = CurrentStyle();
  if (sizes_ && (sizes_->body != st.body || sizes_->display != st.display)) RequestFonts(st.body, st.display);
}

float EaseOut(float t) {
  t = 1.0f - Clamp01(t);
  return 1.0f - t * t * t;
}

float BackOut(float t) {
  t = Clamp01(t) - 1.0f;
  const float k = 1.9f;
  return 1.0f + t * t * ((k + 1.0f) * t + k);
}

void RequestFrame() { frameRequested_ = true; }
bool FrameRequested() { return frameRequested_; }

float Px(float logical) { return logical * s_; }
ImFont* Bold() { return bold_; }
ImFont* Big() { return big_; }
ImFont* Display(float size) { return Sized(big_, size); }
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
  case Icon::Check:
    poly({{-0.66f, 0.02f}, {-0.20f, 0.48f}, {0.70f, -0.50f}}, false);
    break;
  case Icon::Alert:
    // Triángulo de aviso con la exclamación.
    poly({{0.0f, -0.82f}, {0.88f, 0.72f}, {-0.88f, 0.72f}}, true);
    line(0.0f, -0.28f, 0.0f, 0.22f);
    dl->AddRectFilled(P(-0.08f, 0.38f), P(0.08f, 0.54f), color);
    break;
  case Icon::Download:
    line(0.0f, -0.82f, 0.0f, 0.36f);
    poly({{-0.42f, -0.06f}, {0.0f, 0.36f}, {0.42f, -0.06f}}, false);
    line(-0.80f, 0.80f, 0.80f, 0.80f);
    break;
  case Icon::Upload:
    line(0.0f, 0.40f, 0.0f, -0.80f);
    poly({{-0.42f, -0.38f}, {0.0f, -0.80f}, {0.42f, -0.38f}}, false);
    line(-0.80f, 0.80f, 0.80f, 0.80f);
    break;
  case Icon::Info:
    dl->AddRect(P(-0.80f, -0.80f), P(0.80f, 0.80f), color, 0, 0, t);
    line(0.0f, -0.12f, 0.0f, 0.50f);
    dl->AddRectFilled(P(-0.09f, -0.48f), P(0.09f, -0.30f), color);
    break;
  case Icon::Gamepad:
    // Mando visto de frente: cuerpo, cruceta y dos botones.
    poly({{-0.90f, -0.40f}, {0.90f, -0.40f}, {0.90f, 0.50f}, {0.42f, 0.50f}, {0.24f, 0.22f}, {-0.24f, 0.22f},
          {-0.42f, 0.50f}, {-0.90f, 0.50f}},
         true);
    line(-0.62f, 0.02f, -0.26f, 0.02f);
    line(-0.44f, -0.16f, -0.44f, 0.20f);
    dl->AddRectFilled(P(0.30f, -0.14f), P(0.44f, 0.00f), color);
    dl->AddRectFilled(P(0.54f, 0.04f), P(0.68f, 0.18f), color);
    break;
  case Icon::Record:
    DrawMark(dl, c, r * 0.55f, color, true);
    break;
  case Icon::Home:
    poly({{-0.78f, -0.02f}, {0.0f, -0.78f}, {0.78f, -0.02f}}, false);
    poly({{-0.56f, -0.22f}, {-0.56f, 0.78f}, {0.56f, 0.78f}, {0.56f, -0.22f}}, false);
    dl->AddRect(P(-0.16f, 0.26f), P(0.16f, 0.78f), color, 0, 0, t);
    break;
  case Icon::Grid:
    // Cuatro miniaturas: la biblioteca.
    for (int i = 0; i < 4; ++i) {
      const float x = i % 2 ? 0.12f : -0.80f, y = i / 2 ? 0.12f : -0.80f;
      dl->AddRect(P(x, y), P(x + 0.68f, y + 0.68f), color, 0, 0, t);
    }
    break;
  case Icon::Gear: {
    // Rueda dentada de ocho dientes cuadrados.
    ImVec2 pts[32];
    for (int i = 0; i < 32; ++i) {
      const float a = float(i) / 32.0f * kPi * 2.0f - kPi / 16.0f;
      const float rr = (i / 2) % 2 == 0 ? 0.86f : 0.62f;
      pts[i] = ImVec2(c.x + std::cos(a) * rr * r, c.y + std::sin(a) * rr * r);
    }
    dl->AddPolyline(pts, 32, color, ImDrawFlags_Closed, t);
    dl->AddRect(P(-0.22f, -0.22f), P(0.22f, 0.22f), color, 0, 0, t);
    break;
  }
  }
}

void DrawSlash(ImDrawList* dl, ImVec2 c, float r, ImU32 color, ImU32 gap) {
  const float t = std::max(1.2f, r * 0.17f);
  ImVec2 a(c.x - r * 0.85f, c.y - r * 0.85f), b(c.x + r * 0.85f, c.y + r * 0.85f);
  // Un trazo del color del fondo debajo separa la raya del icono.
  dl->AddLine(a, b, gap, t * 3.0f);
  dl->AddLine(a, b, color, t);
}

void DrawCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float progress) {
  const float t = std::max(1.5f, r * 0.20f);
  const ImVec2 p0(c.x - r * 0.66f, c.y + r * 0.02f), p1(c.x - r * 0.20f, c.y + r * 0.48f),
      p2(c.x + r * 0.70f, c.y - r * 0.50f);
  // El trazo corto ocupa un tercio del recorrido; el largo, el resto.
  progress = Clamp01(progress);
  if (progress <= 0) return;
  const float split = 0.34f;
  float k = std::min(progress / split, 1.0f);
  dl->AddLine(p0, p0 + (p1 - p0) * k, color, t);
  if (progress > split) {
    k = (progress - split) / (1.0f - split);
    dl->AddLine(p1, p1 + (p2 - p1) * k, color, t);
  }
}

void DrawMark(ImDrawList* dl, ImVec2 c, float half, ImU32 color, bool filled) {
  const float t = 1.0f * s_;
  switch (CurrentStyle().mark) {
  case MarkShape::Diamond: {
    const float d = half * 1.3f;
    const ImVec2 pts[] = {ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)};
    if (filled) dl->AddConvexPolyFilled(pts, 4, color);
    else dl->AddPolyline(pts, 4, color, ImDrawFlags_Closed, t);
    return;
  }
  case MarkShape::Bar: {
    // Barra vertical estrecha: un indicador de nivel.
    ImVec2 a(c.x - half * 0.55f, c.y - half * 1.4f), b(c.x + half * 0.55f, c.y + half * 1.4f);
    if (filled) dl->AddRectFilled(a, b, color);
    else dl->AddRect(a, b, color, 0, 0, t);
    return;
  }
  case MarkShape::Block: {
    // Cursor de consola.
    ImVec2 a(c.x - half * 0.75f, c.y - half * 1.3f), b(c.x + half * 0.75f, c.y + half * 1.3f);
    if (filled) dl->AddRectFilled(a, b, color);
    else dl->AddRect(a, b, color, 0, 0, t);
    return;
  }
  case MarkShape::Square:
    break;
  }
  ImVec2 a(c.x - half, c.y - half), b(c.x + half, c.y + half);
  if (filled) dl->AddRectFilled(a, b, color);
  else dl->AddRect(a, b, color, 0, 0, t);
}

// ------------------------------------------------------- formas del diseño

float LineW() { return CurrentStyle().line * s_; }
float ActionHeight() { return std::round(Px(44) * CurrentStyle().density); }
float RowHeight(float logical) { return std::round(Px(logical) * CurrentStyle().density); }

namespace {

// Contorno con las esquinas cortadas del diseño (o el rectángulo). Hasta 8 puntos.
int ShapePoints(ImVec2 a, ImVec2 b, ImVec2* out) {
  const Style& st = CurrentStyle();
  const float k = st.button == ButtonShape::Chamfer
                      ? std::min(Px(st.cut), std::min(b.x - a.x, b.y - a.y) * 0.38f)
                      : 0.0f;
  const int c = k > 0 ? st.cutCorners : 0;
  int n = 0;
  if (c & 1) {
    out[n++] = ImVec2(a.x, a.y + k);
    out[n++] = ImVec2(a.x + k, a.y);
  } else {
    out[n++] = a;
  }
  if (c & 2) {
    out[n++] = ImVec2(b.x - k, a.y);
    out[n++] = ImVec2(b.x, a.y + k);
  } else {
    out[n++] = ImVec2(b.x, a.y);
  }
  if (c & 4) {
    out[n++] = ImVec2(b.x, b.y - k);
    out[n++] = ImVec2(b.x - k, b.y);
  } else {
    out[n++] = b;
  }
  if (c & 8) {
    out[n++] = ImVec2(a.x + k, b.y);
    out[n++] = ImVec2(a.x, b.y - k);
  } else {
    out[n++] = ImVec2(a.x, b.y);
  }
  return n;
}

bool Cut() { return CurrentStyle().button == ButtonShape::Chamfer && CurrentStyle().cutCorners != 0; }

} // namespace

void ShapeFill(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color) {
  if (!Cut()) {
    dl->AddRectFilled(a, b, color);
    return;
  }
  ImVec2 pts[8];
  int n = ShapePoints(a, b, pts);
  dl->AddConvexPolyFilled(pts, n, color);
}

void ShapeStroke(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness) {
  if (thickness <= 0) thickness = LineW();
  if (!Cut()) {
    dl->AddRect(a, b, color, 0, 0, thickness);
    return;
  }
  // Por dentro medio trazo: el borde cae en píxeles enteros, como AddRect.
  const float h = thickness * 0.5f;
  ImVec2 pts[8];
  int n = ShapePoints(a + ImVec2(h, h), b - ImVec2(h, h), pts);
  dl->AddPolyline(pts, n, color, ImDrawFlags_Closed, thickness);
}

void DrawBrackets(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float arm, float thickness) {
  if (thickness <= 0) thickness = LineW();
  arm = std::min(arm, std::min(b.x - a.x, b.y - a.y) * 0.5f);
  const float t = thickness;
  // Rectángulos llenos: los trazos quedan nítidos en píxeles enteros.
  dl->AddRectFilled(a, ImVec2(a.x + arm, a.y + t), color);
  dl->AddRectFilled(a, ImVec2(a.x + t, a.y + arm), color);
  dl->AddRectFilled(ImVec2(b.x - arm, a.y), ImVec2(b.x, a.y + t), color);
  dl->AddRectFilled(ImVec2(b.x - t, a.y), ImVec2(b.x, a.y + arm), color);
  dl->AddRectFilled(ImVec2(b.x - arm, b.y - t), b, color);
  dl->AddRectFilled(ImVec2(b.x - t, b.y - arm), b, color);
  dl->AddRectFilled(ImVec2(a.x, b.y - t), ImVec2(a.x + arm, b.y), color);
  dl->AddRectFilled(ImVec2(a.x, b.y - arm), ImVec2(a.x + t, b.y), color);
}

void CardFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 edge) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  float top = a.y, bottom = b.y;
  switch (st.button) {
  case ButtonShape::Outline:
    dl->AddRectFilled(a, b, C(p.bg));
    dl->AddRect(a, b, C(st.hover == HoverEffect::Invert ? p.text : p.lineStrong), 0, 0, LineW());
    break;
  case ButtonShape::Bracket:
    // Opaca: el fondo ilustrado del tema no se ve a través.
    dl->AddRectFilled(a, b, C(p.bg));
    dl->AddRectFilled(a, b, C(p.surface, 0.55f));
    DrawBrackets(dl, a, b, C(p.lineStrong), Px(12));
    break;
  case ButtonShape::Chamfer: {
    ShapeFill(dl, a, b, C(p.surface));
    const float k = std::min(Px(st.cut), std::min(b.x - a.x, b.y - a.y) * 0.38f);
    if (st.cutCorners & 1) top += k;
    if (st.cutCorners & 8) bottom -= k;
    break;
  }
  case ButtonShape::Double:
    dl->AddRectFilled(a, b, C(p.bg));
    DoubleStroke(dl, a, b, C(p.lineStrong), C(edge, 0.35f));
    break;
  default:
    dl->AddRectFilled(a, b, C(p.surface));
    break;
  }
  dl->AddRectFilled(ImVec2(a.x, top), ImVec2(a.x + Px(3), bottom), C(edge));
}

void DoubleStroke(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 inner, ImU32 outer) {
  const float t = LineW(), o = std::round(Px(3));
  dl->AddRect(a, b, inner, 0, 0, t);
  dl->AddRect(a - ImVec2(o, o), b + ImVec2(o, o), outer, 0, 0, t);
}

void TileFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, float hover) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  switch (st.card) {
  case CardLook::Hud: {
    ShapeFill(dl, a, b, C(p.surface));
    DrawBrackets(dl, a, b, C(Mix(p.lineStrong, p.accent, hover)), Px(8));
    break;
  }
  case CardLook::Ascii: {
    // Bordes de guiones y una cruz en cada esquina, como "+----+".
    dl->AddRectFilled(a, b, C(p.bg));
    const ImU32 col = C(Mix(p.lineStrong, p.accent, hover));
    const float t = std::max(1.0f, std::round(Px(1)));
    for (float x = a.x + Px(8); x < b.x - Px(8); x += Px(7)) {
      dl->AddRectFilled(ImVec2(x, a.y), ImVec2(std::min(x + Px(4), b.x - Px(8)), a.y + t), col);
      dl->AddRectFilled(ImVec2(x, b.y - t), ImVec2(std::min(x + Px(4), b.x - Px(8)), b.y), col);
    }
    for (float y = a.y + Px(8); y < b.y - Px(8); y += Px(7)) {
      dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + t, std::min(y + Px(4), b.y - Px(8))), col);
      dl->AddRectFilled(ImVec2(b.x - t, y), ImVec2(b.x, std::min(y + Px(4), b.y - Px(8))), col);
    }
    const float r = Px(3);
    for (int i = 0; i < 4; ++i) {
      const ImVec2 c(i & 1 ? b.x - Px(2) : a.x + Px(2), i & 2 ? b.y - Px(2) : a.y + Px(2));
      dl->AddRectFilled(ImVec2(c.x - r, c.y - t * 0.5f), ImVec2(c.x + r, c.y + t * 0.5f), C(p.accent));
      dl->AddRectFilled(ImVec2(c.x - t * 0.5f, c.y - r), ImVec2(c.x + t * 0.5f, c.y + r), C(p.accent));
    }
    break;
  }
  case CardLook::Framed:
    ShapeFill(dl, a, b, C(p.bg));
    ShapeStroke(dl, a, b, C(Mix(p.lineStrong, p.accent, hover)));
    break;
  case CardLook::Neon:
    dl->AddRectFilled(a, b, C(p.bg));
    DoubleStroke(dl, a, b, C(Mix(p.lineStrong, p.accent, hover)), C(p.accent, 0.18f + 0.4f * hover));
    break;
  case CardLook::Polaroid:
    dl->AddRectFilled(a, b, C(p.surface));
    dl->AddRect(a, b, C(p.line), 0, 0, LineW());
    break;
  case CardLook::Heavy:
    dl->AddRectFilled(a, b, C(p.bg));
    dl->AddRect(a, b, C(p.text), 0, 0, LineW());
    break;
  case CardLook::Tile:
    dl->AddRectFilled(a, b, C(Mix(p.surface, p.surfaceHover, hover)));
    break;
  case CardLook::Plain:
    dl->AddRectFilled(a, b, C(p.surface));
    if (hover > 0) dl->AddRect(a, b, C(p.lineStrong, hover), 0, 0, LineW());
    break;
  }
}

// ------------------------------------------------------- movimiento

Motion Animate(bool enabled) {
  const ImGuiID id = ImGui::GetItemID();
  ImGuiStorage* st = ImGui::GetStateStorage();
  const float now = float(ImGui::GetTime());
  // GetFloatRef añade la clave si no está y eso puede mover todo el almacén:
  // con un control nuevo, las referencias tomadas antes apuntarían a memoria
  // liberada y escribir en ellas corrompía el montón. Primero se crean las
  // seis claves; después ninguna búsqueda inserta y las referencias valen.
  if (st->GetInt(id ^ 0x4D41u, -1) == -1) { // wasHeld: 0 o 1 una vez creada
    st->SetFloat(id ^ 0x4D48u, 0.0f);
    st->SetFloat(id ^ 0x4D50u, -100.0f);
    st->SetFloat(id ^ 0x4D52u, -100.0f);
    st->SetFloat(id ^ 0x4D58u, 0.0f);
    st->SetFloat(id ^ 0x4D59u, 0.0f);
    st->SetInt(id ^ 0x4D41u, 0);
  }
  float& hover = *st->GetFloatRef(id ^ 0x4D48u, 0.0f);
  float& pressedAt = *st->GetFloatRef(id ^ 0x4D50u, -100.0f);
  float& releasedAt = *st->GetFloatRef(id ^ 0x4D52u, -100.0f);
  float& atX = *st->GetFloatRef(id ^ 0x4D58u, 0.0f);
  float& atY = *st->GetFloatRef(id ^ 0x4D59u, 0.0f);
  int& wasHeld = *st->GetIntRef(id ^ 0x4D41u, 0);
  const bool hovered = enabled && ImGui::IsItemHovered();
  const bool held = enabled && ImGui::IsItemActive();
  if (enabled && ImGui::IsItemActivated()) {
    pressedAt = now;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    // Con el teclado (Intro) no hay ratón: desde el centro.
    if (!ImGui::IsMousePosValid(&mouse) || !ImGui::IsItemHovered()) mouse = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    atX = mouse.x;
    atY = mouse.y;
  }
  if (wasHeld && !held) releasedAt = now;
  wasHeld = held ? 1 : 0;
  // El reloj de ImGui empieza de cero con cada contexto: un valor del futuro
  // es de una apertura anterior.
  if (pressedAt > now) pressedAt = -100.0f;
  if (releasedAt > now) releasedAt = -100.0f;

  const float target = hovered || held ? 1.0f : 0.0f;
  if (!ThemeMotion()) {
    hover = target;
  } else if (hover != target) {
    hover += (target - hover) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
    if (std::fabs(hover - target) < 0.02f) hover = target;
    else RequestFrame();
  }
  Motion m;
  m.hover = hover;
  m.held = held;
  m.pressAge = now - pressedAt;
  m.releaseAge = now - releasedAt;
  m.at = ImVec2(atX, atY);
  if (ThemeMotion() && (m.pressAge < 0.5f || m.releaseAge < 0.4f)) RequestFrame();
  return m;
}

namespace {

// Desplazamiento del contenido al pulsar: se hunde (Sink) o tiembla (Glitch).
ImVec2 PressShift(const Motion& m) {
  const Style& st = CurrentStyle();
  if (st.press == PressEffect::Sink && m.held) return ImVec2(0, std::round(Px(1)));
  if (st.press == PressEffect::Glitch && ThemeMotion() && m.pressAge < 0.16f) {
    static const float kJitter[] = {3.0f, -2.0f, 2.0f, -1.0f, 0.0f};
    return ImVec2(std::round(Px(kJitter[std::min(int(m.pressAge / 0.035f), 4)])), 0);
  }
  return ImVec2(0, 0);
}

} // namespace

ButtonPaint PaintButton(ImDrawList* dl, ImVec2 a, ImVec2 b, Role role, const Motion& m, bool enabled) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const float h = enabled ? m.hover : 0.0f;
  const bool held = enabled && m.held;
  const bool solid = role == Role::Primary || role == Role::Danger;
  // Suave: encoge mientras se pulsa y vuelve con un pequeño rebote.
  if (enabled && st.press == PressEffect::Squash && ThemeMotion()) {
    float k = 1.0f;
    if (held) k = 0.965f;
    else if (m.releaseAge < 0.32f) k = 0.965f + 0.035f * BackOut(m.releaseAge / 0.32f);
    const ImVec2 c = (a + b) * 0.5f;
    a = c + (a - c) * k;
    b = c + (b - c) * k;
  }

  ButtonPaint out{a, b, p.text, p.textMuted, p.lineStrong, 0.0f};
  if (solid) {
    const bool danger = role == Role::Danger;
    const ImU32 base = danger ? p.danger : p.accent, over = danger ? p.dangerHover : p.accentHover,
                down = danger ? p.dangerActive : p.accentActive;
    out.fg = out.muted = danger ? p.onDanger : p.onAccent;
    out.keyBorder = Mix(out.fg, base, danger ? 0.53f : 0.45f);
    if (!enabled) {
      out.fg = out.muted = p.textFaint;
      out.keyBorder = p.line;
    }
    const ImU32 fill = !enabled ? p.surface : held ? down : Mix(base, over, h);
    if (st.button == ButtonShape::Bracket) {
      // Terminal: el bloque lleno con las esquinas recortadas en L del color del fondo.
      dl->AddRectFilled(a, b, C(fill));
      DrawBrackets(dl, a + ImVec2(Px(3), Px(3)), b - ImVec2(Px(3), Px(3)), C(p.bg, 0.55f), Px(6 + 4 * h));
    } else if (st.button == ButtonShape::Double) {
      // Neón: el tubo de fuera se enciende con el ratón.
      dl->AddRectFilled(a, b, C(fill));
      const float o = std::round(Px(3));
      dl->AddRect(a - ImVec2(o, o), b + ImVec2(o, o), C(enabled ? base : p.line, 0.30f + 0.55f * h), 0, 0, LineW());
    } else if (st.button == ButtonShape::Rail) {
      dl->AddRectFilled(a, b, C(fill));
      // El raíl, más oscuro, crece con el ratón.
      const float rail = Px(3) + Px(3) * h;
      dl->AddRectFilled(a, ImVec2(a.x + rail, b.y), C(enabled ? down : p.line));
      out.inset = Px(4);
    } else {
      ShapeFill(dl, a, b, C(fill));
      if (st.hover == HoverEffect::Invert && h > 0 && enabled)
        ShapeStroke(dl, a, b, C(p.text, h), LineW());
    }
    out.a = a + PressShift(m);
    out.b = b + PressShift(m);
    return out;
  }

  // Secundarios: Normal (superficie), Ghost (borde) y Quiet (nada hasta pasar el ratón).
  const bool ghost = role == Role::Ghost || role == Role::GhostDanger;
  if (role == Role::GhostDanger) out.fg = p.danger;
  if (role == Role::Normal) out.keyBorder = p.lineStrong;
  if (!enabled) {
    out.fg = out.muted = p.textFaint;
    out.keyBorder = p.line;
  }
  const ImU32 accentish = role == Role::GhostDanger ? p.danger : p.accent;
  switch (st.button) {
  case ButtonShape::Flat:
  case ButtonShape::Chamfer: {
    if (role == Role::Normal) {
      ShapeFill(dl, a, b, C(held ? p.surfaceActive : Mix(p.surface, p.surfaceHover, h)));
    } else {
      if (held) ShapeFill(dl, a, b, C(p.surfaceActive));
      else if (h > 0) ShapeFill(dl, a, b, C(p.surface, h));
    }
    if (ghost) {
      ImU32 border = Mix(p.line, role == Role::GhostDanger ? p.danger : p.lineStrong, h);
      ShapeStroke(dl, a, b, C(border));
    }
    // Táctico: el borde se enciende con el acento.
    if (st.hover == HoverEffect::Border && h > 0 && enabled) ShapeStroke(dl, a, b, C(accentish, h));
    break;
  }
  case ButtonShape::Outline: {
    if (st.hover == HoverEffect::Invert) {
      // Alto contraste: con el ratón encima, colores invertidos.
      const float k = held ? 1.0f : h;
      if (k > 0) dl->AddRectFilled(a, b, C(role == Role::GhostDanger ? p.danger : p.text, k));
      if (role != Role::Quiet || k > 0)
        dl->AddRect(a, b, C(role == Role::GhostDanger ? p.danger : p.text), 0, 0, LineW());
      if (enabled && k > 0.5f) out.fg = out.muted = out.keyBorder = p.bg;
      break;
    }
    if (held) dl->AddRectFilled(a, b, C(p.surfaceActive));
    else if (h > 0) dl->AddRectFilled(a, b, C(p.surface, h));
    if (role != Role::Quiet) {
      ImU32 rest = role == Role::Normal ? p.lineStrong : p.line;
      ImU32 border = st.hover == HoverEffect::Border ? Mix(rest, accentish, h) : Mix(rest, p.lineStrong, h);
      dl->AddRect(a, b, C(border), 0, 0, LineW());
    } else if (h > 0 && st.hover == HoverEffect::Border) {
      dl->AddRect(a, b, C(p.lineStrong, h), 0, 0, LineW());
    }
    if (st.hover == HoverEffect::Underline && h > 0 && enabled)
      dl->AddRectFilled(ImVec2(a.x, b.y - Px(2)), ImVec2(a.x + (b.x - a.x) * EaseOut(h), b.y), C(accentish));
    break;
  }
  case ButtonShape::Bracket: {
    if (held) dl->AddRectFilled(a, b, C(p.surfaceActive));
    else if (role == Role::Normal || h > 0) dl->AddRectFilled(a, b, C(p.surface, role == Role::Normal ? 0.35f + 0.65f * h : h));
    if (role != Role::Quiet || h > 0) {
      ImU32 col = role == Role::GhostDanger ? p.danger : Mix(p.lineStrong, p.text, h);
      DrawBrackets(dl, a, b, C(col, role == Role::Quiet ? h : 1.0f), Px(7) + Px(5) * h);
    }
    break;
  }
  case ButtonShape::Double: {
    if (role == Role::Normal) dl->AddRectFilled(a, b, C(held ? p.surfaceActive : Mix(p.surface, p.surfaceHover, h)));
    else if (held) dl->AddRectFilled(a, b, C(p.surfaceActive));
    else if (h > 0) dl->AddRectFilled(a, b, C(p.surface, 0.6f * h));
    if (role != Role::Quiet || h > 0) {
      const ImU32 inner = Mix(role == Role::Normal ? p.lineStrong : p.line, accentish, h);
      dl->AddRect(a, b, C(inner, role == Role::Quiet ? h : 1.0f), 0, 0, LineW());
    }
    if (h > 0 && enabled && role != Role::Quiet) {
      const float o = std::round(Px(3));
      dl->AddRect(a - ImVec2(o, o), b + ImVec2(o, o), C(accentish, 0.35f * h), 0, 0, LineW());
    }
    break;
  }
  case ButtonShape::Rail: {
    if (role == Role::Normal) dl->AddRectFilled(a, b, C(held ? p.surfaceActive : Mix(p.surface, p.surfaceHover, h)));
    else if (held) dl->AddRectFilled(a, b, C(p.surfaceActive));
    else if (ghost) dl->AddRectFilled(a, b, C(p.surface, 0.5f + 0.5f * h));
    else if (h > 0) dl->AddRectFilled(a, b, C(p.surface, h));
    // Raíl del acento a la izquierda: crece desde el centro con el ratón.
    const float frac = role == Role::Normal ? 0.34f + 0.66f * h : h;
    if (frac > 0 && enabled) {
      const float mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.5f * frac;
      dl->AddRectFilled(ImVec2(a.x, mid - half), ImVec2(a.x + Px(3), mid + half), C(accentish));
    }
    if (role != Role::Quiet) out.inset = Px(4);
    break;
  }
  }
  out.a = a + PressShift(m);
  out.b = b + PressShift(m);
  return out;
}

void PaintPress(ImDrawList* dl, ImVec2 a, ImVec2 b, Role role, const Motion& m) {
  if (!ThemeMotion()) return;
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const bool solid = role == Role::Primary || role == Role::Danger;
  const ImU32 tint = role == Role::Danger || role == Role::GhostDanger ? p.danger : p.accent;
  switch (st.press) {
  case PressEffect::Echo: {
    // Dos contornos fantasma salen del botón, el segundo un poco después.
    for (int i = 0; i < 2; ++i) {
      float t = (m.pressAge - 0.07f * float(i)) / 0.34f;
      if (t < 0 || t >= 1) continue;
      float grow = Px(7) * EaseOut(t);
      ShapeStroke(dl, a - ImVec2(grow, grow), b + ImVec2(grow, grow), C(tint, 0.6f * (1.0f - t)), Px(1));
    }
    break;
  }
  case PressEffect::Glitch: {
    if (m.pressAge >= 0.16f) break;
    // Dos franjas del acento y del rojo desplazadas a lados opuestos.
    const int step = std::min(int(m.pressAge / 0.035f), 4);
    const float hgt = b.y - a.y, off = Px(float(4 - step));
    const float y0 = a.y + hgt * (0.18f + 0.11f * float(step % 3)), y1 = y0 + std::max(Px(2), hgt * 0.16f);
    const float y2 = a.y + hgt * (0.62f - 0.07f * float(step % 2)), y3 = y2 + std::max(Px(2), hgt * 0.12f);
    dl->PushClipRect(a - ImVec2(Px(6), 0), b + ImVec2(Px(6), 0), true);
    dl->AddRectFilled(ImVec2(a.x + off, y0), ImVec2(b.x + off, y1), C(solid ? p.text : p.accent, 0.45f));
    dl->AddRectFilled(ImVec2(a.x - off, y2), ImVec2(b.x - off, y3), C(p.danger, 0.40f));
    dl->PopClipRect();
    break;
  }
  case PressEffect::Scan: {
    float t = m.pressAge / 0.24f;
    if (t >= 1) break;
    // Una línea clara barre el botón y deja un rastro que se apaga.
    float y = a.y + (b.y - a.y) * EaseOut(t);
    ImU32 bright = solid ? p.onAccent : p.text;
    dl->AddRectFilled(a, ImVec2(b.x, y), C(tint, 0.14f * (1.0f - t)));
    dl->AddRectFilled(ImVec2(a.x, y - Px(1)), ImVec2(b.x, y + Px(1)), C(bright, 0.7f * (1.0f - t)));
    break;
  }
  case PressEffect::Ripple: {
    float t = m.pressAge / 0.40f;
    if (t >= 1) break;
    // Un cuadrado crece desde donde se pulsó, recortado al botón.
    ImVec2 c(std::clamp(m.at.x, a.x, b.x), std::clamp(m.at.y, a.y, b.y));
    float reach = std::max({c.x - a.x, b.x - c.x, c.y - a.y, b.y - c.y});
    float half = reach * EaseOut(t);
    dl->PushClipRect(a, b, true);
    dl->AddRectFilled(c - ImVec2(half, half), c + ImVec2(half, half), C(solid ? p.onAccent : tint, 0.22f * (1.0f - t)));
    dl->PopClipRect();
    break;
  }
  case PressEffect::Flash: {
    float t = m.pressAge / 0.26f;
    if (t >= 1) break;
    ShapeFill(dl, a, b, C(solid ? p.onAccent : tint, 0.30f * (1.0f - t)));
    break;
  }
  case PressEffect::Sink:
  case PressEffect::Squash:
    break; // se ven en PaintButton
  }
}

void HoverOutline(ImDrawList* dl, ImVec2 a, ImVec2 b, float h) {
  if (h <= 0) return;
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  switch (st.button) {
  case ButtonShape::Bracket:
    DrawBrackets(dl, a, b, C(p.text, h), Px(10) + Px(6) * h);
    break;
  case ButtonShape::Rail: {
    const float mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.5f * h;
    dl->AddRectFilled(ImVec2(a.x, mid - half), ImVec2(a.x + Px(3), mid + half), C(p.accent));
    break;
  }
  case ButtonShape::Chamfer:
    ShapeStroke(dl, a, b, C(st.hover == HoverEffect::Border ? p.accent : p.lineStrong, h));
    break;
  case ButtonShape::Outline:
    if (st.hover == HoverEffect::Invert) dl->AddRect(a, b, C(p.text, h), 0, 0, LineW());
    else if (st.hover == HoverEffect::Underline)
      dl->AddRectFilled(ImVec2(a.x, b.y - Px(2)), ImVec2(a.x + (b.x - a.x) * EaseOut(h), b.y), C(p.accent));
    else dl->AddRect(a, b, C(Mix(p.lineStrong, p.accent, h), h), 0, 0, LineW());
    break;
  case ButtonShape::Double:
    DoubleStroke(dl, a, b, C(Mix(p.lineStrong, p.accent, h), h), C(p.accent, 0.35f * h));
    break;
  case ButtonShape::Flat:
    dl->AddRect(a, b, C(p.lineStrong, h), 0, 0, LineW());
    break;
  }
}

// ------------------------------------------------------- rótulos

std::string Upper(const std::string& text) {
  std::string out = text;
  for (size_t i = 0; i < out.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(out[i]);
    if (c >= 'a' && c <= 'z') {
      out[i] = char(c - 32);
    } else if (c == 0xC3 && i + 1 < out.size()) {
      // á é í ó ú ñ ü...: U+00E0-U+00FE (salvo ÷) -> U+00C0-U+00DE
      unsigned char d = static_cast<unsigned char>(out[i + 1]);
      if (d >= 0xA0 && d <= 0xBE && d != 0xB7) out[i + 1] = char(d - 0x20);
      ++i;
    }
  }
  return out;
}

std::string Lowercase(const std::string& text) {
  std::string out = text;
  for (size_t i = 0; i < out.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(out[i]);
    if (c >= 'A' && c <= 'Z') {
      out[i] = char(c + 32);
    } else if (c == 0xC3 && i + 1 < out.size()) {
      unsigned char d = static_cast<unsigned char>(out[i + 1]);
      if (d >= 0x80 && d <= 0x9E && d != 0x97) out[i + 1] = char(d + 0x20);
      ++i;
    }
  }
  return out;
}

std::string SentenceCase(const std::string& upper) {
  std::string out = upper;
  bool first = true;
  for (size_t i = 0; i < out.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(out[i]);
    if (c >= 'A' && c <= 'Z') {
      if (!first) out[i] = char(c + 32);
      first = false;
    } else if (c >= 'a' && c <= 'z') {
      first = false;
    } else if (c == 0xC3 && i + 1 < out.size()) {
      unsigned char d = static_cast<unsigned char>(out[i + 1]);
      if (!first && d >= 0x80 && d <= 0x9E && d != 0x97) out[i + 1] = char(d + 0x20);
      first = false;
      ++i;
    }
  }
  return out;
}

namespace {

const char* LabelPrefix(LabelStyle style) {
  return style == LabelStyle::Slashes ? "// " : style == LabelStyle::Prompt ? "> " : nullptr;
}

float LabelTracking(LabelStyle style) {
  return style == LabelStyle::Tracked ? Px(1.5f) : style == LabelStyle::Slashes ? Px(1.0f) : 0.0f;
}

// Sentence se lee mejor un punto más grande: en minúsculas el ojo es menor.
float LabelSize(LabelStyle style, float size) { return style == LabelStyle::Sentence ? size + Px(1) : size; }

} // namespace

float LabelWidth(const char* upper, float size) {
  const LabelStyle style = CurrentStyle().label;
  const float fs = LabelSize(style, size);
  float w = 0;
  if (const char* prefix = LabelPrefix(style)) w += TextSize(bold_, fs, prefix).x;
  if (style == LabelStyle::Sentence) return w + TextSize(bold_, fs, SentenceCase(upper).c_str()).x;
  return w + TrackedWidth(bold_, fs, upper, LabelTracking(style));
}

float DrawLabel(ImDrawList* dl, ImVec2 pos, const char* upper, float size, ImU32 color) {
  const Palette& p = Colors();
  const LabelStyle style = CurrentStyle().label;
  const float fs = LabelSize(style, size);
  // Mismo centro vertical con cualquier tamaño.
  pos.y -= (fs - size) * 0.5f;
  float x = pos.x;
  if (const char* prefix = LabelPrefix(style)) {
    Text(dl, bold_, fs, ImVec2(x, pos.y), C(p.accent), prefix);
    x += TextSize(bold_, fs, prefix).x;
  }
  if (style == LabelStyle::Sentence) {
    std::string text = SentenceCase(upper);
    Text(dl, bold_, fs, ImVec2(x, pos.y), color, text.c_str());
    return x + TextSize(bold_, fs, text.c_str()).x - pos.x;
  }
  const float tracking = LabelTracking(style);
  TrackedText(dl, bold_, fs, ImVec2(x, pos.y), color, upper, tracking);
  return x + TrackedWidth(bold_, fs, upper, tracking) - pos.x;
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
  if (AmbientMotion() != Ambient::Ghost) return;
  const HoverTrail& trail = Trail();
  float t = float(ImGui::GetTime() - trail.ghostSince) / 0.2f;
  if (t < 0 || t >= 1) return;
  RequestFrame();
  ShapeFill(dl, trail.ghostA, trail.ghostB, C(Colors().accent, 0.12f * (1.0f - t)));
}

void DrawTrace(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color) {
  if (AmbientMotion() != Ambient::Ghost) return;
  const float period = 2.8f, len = Px(34);
  float phase = float(std::fmod(ImGui::GetTime(), double(period))) / period;
  float alpha = phase < 0.1f ? phase / 0.1f : phase > 0.9f ? (1.0f - phase) / 0.1f : 1.0f;
  float x = a.x + (b.x - a.x - len) * phase;
  dl->AddRectFilled(ImVec2(x, b.y - Px(2)), ImVec2(x + len, b.y), C(color, alpha));
}

float Breathe() {
  if (AmbientMotion() != Ambient::Ghost) return 1.0f;
  return 0.35f + 0.65f * (0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 2.0f * kPi / 2.6f));
}

float Blink() {
  if (AmbientMotion() != Ambient::Cursor) return 1.0f;
  // Con el reloj del sistema: la ventana de la app solo se redibuja en cada
  // medio segundo exacto (AppUi::Draw).
  return GetTickCount64() % 1000 < 500 ? 1.0f : 0.0f;
}

float OpenProgress(unsigned long long openedAt) {
  float t = float(GetTickCount64() - openedAt) / 120.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t);
}

Entrance EnterMotion(unsigned long long since, bool fromRight) {
  Entrance e;
  if (!ThemeMotion()) return e;
  const float t = float(GetTickCount64() - since) / 1000.0f;
  const float dur = 0.26f;
  if (t >= dur || t < 0) return e;
  e.done = false;
  RequestFrame();
  const float u = t / dur;
  switch (CurrentStyle().toast) {
  case ToastLook::Bar:
    // Plano: entra deslizándose desde su lado.
    e.offset.x = (1.0f - EaseOut(u)) * Px(18) * (fromRight ? 1.0f : -1.0f);
    e.alpha = std::min(1.0f, u * 1.6f);
    break;
  case ToastLook::Ghost:
    e.alpha = EaseOut(u);
    break;
  case ToastLook::Header: {
    // Táctico: unos tirones laterales que se calman.
    static const float kJitter[] = {7.0f, -5.0f, 4.0f, -3.0f, 2.0f, -1.0f, 0.0f, 0.0f};
    e.offset.x = std::round(Px(kJitter[std::min(int(t / 0.033f), 7)]));
    e.alpha = std::min(1.0f, u * 3.0f);
    break;
  }
  case ToastLook::Terminal:
    // Terminal: una línea barre de arriba abajo y descubre el contenido.
    e.reveal = EaseOut(u);
    break;
  case ToastLook::TopLine:
    e.offset.y = -(1.0f - EaseOut(u)) * Px(10);
    e.alpha = EaseOut(u);
    break;
  case ToastLook::Card:
    // Suave: sube y se pasa un poco.
    e.offset.y = (1.0f - BackOut(u)) * Px(14);
    e.alpha = std::min(1.0f, u * 2.0f);
    break;
  case ToastLook::Ink:
    e.alpha = EaseOut(u);
    break;
  case ToastLook::Bold:
    break;
  }
  return e;
}

// ----------------------------------------------------------------- widgets

void Gap(float px) { ImGui::Dummy(ImVec2(0, Px(px))); }

void Divider(float above, float below) {
  Gap(above);
  const Palette& p = Colors();
  ImVec2 q = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  const float t = std::round(1.0f * s_);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Siempre el mismo alto, se dibuje lo que se dibuje: el panel lateral
  // calcula dónde va el pie con él.
  switch (CurrentStyle().divider) {
  case DividerStyle::Line:
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + std::max(t, LineW() * 0.5f)), C(p.line));
    break;
  case DividerStyle::Ticked:
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + t), C(p.line));
    dl->AddRectFilled(ImVec2(q.x, q.y - t), ImVec2(q.x + Px(14), q.y + t * 2), C(p.accent));
    break;
  case DividerStyle::Dashed:
    for (float x = 0; x < w; x += Px(8)) dl->AddRectFilled(ImVec2(q.x + x, q.y), ImVec2(q.x + std::min(x + Px(4), w), q.y + t), C(p.lineStrong));
    break;
  case DividerStyle::Space:
    break;
  }
  ImGui::Dummy(ImVec2(0, t));
  Gap(below);
}

void Wordmark(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 color) {
  const Palette& p = Colors();
  const float fs = Px(kSmall);
  switch (CurrentStyle().label) {
  case LabelStyle::Slashes:
    // Táctico: un bloque del acento delante.
    dl->AddRectFilled(ImVec2(pos.x, pos.y), ImVec2(pos.x + Px(4), pos.y + fs), C(p.accent));
    TrackedText(dl, bold_, fs, ImVec2(pos.x + Px(10), pos.y), color, text, Px(3.2f));
    break;
  case LabelStyle::Prompt: {
    // Terminal: "> spectre_" con el cursor parpadeando.
    std::string lower = SentenceCase(text);
    for (auto& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    Text(dl, bold_, fs, pos, C(p.accent), ">");
    float x = pos.x + TextSize(bold_, fs, "> ").x;
    Text(dl, bold_, fs, ImVec2(x, pos.y), color, lower.c_str());
    x += TextSize(bold_, fs, lower.c_str()).x + Px(2);
    dl->AddRectFilled(ImVec2(x, pos.y + Px(1)), ImVec2(x + Px(7), pos.y + fs), C(p.accent, Blink()));
    break;
  }
  case LabelStyle::Sentence: {
    std::string name = SentenceCase(text);
    Text(dl, bold_, fs + Px(2), ImVec2(pos.x, pos.y - Px(1)), color, name.c_str());
    break;
  }
  case LabelStyle::Plain:
    Text(dl, bold_, fs, pos, color, text);
    break;
  case LabelStyle::Tracked:
    TrackedText(dl, bold_, fs, pos, color, text, Px(3.2f));
    break;
  }
}

void SectionLabel(const char* text, const char* link, bool* linkClicked) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float size = Px(kLabel);
  float h = Px(kHint) + Px(4);
  DrawLabel(dl, ImVec2(p.x, p.y + (h - size) * 0.5f), text, size, C(Colors().textMuted));
  if (link) {
    float ls = Px(kHint);
    ImVec2 ts = TextSize(ImGui::GetFont(), ls, link);
    float x = ImGui::GetContentRegionAvail().x - ts.x;
    ImGui::SetCursorScreenPos(ImVec2(p.x + x, p.y));
    bool clicked = ImGui::InvisibleButton(link, ImVec2(ts.x, h));
    Motion m = Animate();
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    Text(dl, ImGui::GetFont(), ls, ImVec2(p.x + x, p.y + (h - ts.y) * 0.5f),
         C(Mix(Colors().textMuted, Colors().text, m.hover)), link);
    // El subrayado crece desde la izquierda.
    if (m.hover > 0)
      dl->AddRectFilled(ImVec2(p.x + x, p.y + h - 1.0f * s_), ImVec2(p.x + x + ts.x * m.hover, p.y + h),
                        C(Colors().text));
    if (linkClicked) *linkClicked = clicked;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  } else {
    ImGui::Dummy(ImVec2(0, h));
  }
}

void DrawKeycap(ImDrawList* dl, ImVec2 rightMid, const std::string& key, ImU32 border, ImU32 fg, float alpha) {
  if (key.empty()) return;
  float size = Px(kHint);
  ImVec2 pad(Px(7), Px(3));
  switch (CurrentStyle().keycap) {
  case KeycapStyle::Brackets: {
    std::string text = "[" + Upper(key) + "]";
    ImVec2 ts = TextSize(ImGui::GetFont(), size, text.c_str());
    Text(dl, ImGui::GetFont(), size, ImVec2(rightMid.x - ts.x, rightMid.y - ts.y * 0.5f), C(fg, alpha), text.c_str());
    return;
  }
  case KeycapStyle::Plain: {
    ImVec2 ts = TextSize(ImGui::GetFont(), size, key.c_str());
    Text(dl, ImGui::GetFont(), size, ImVec2(rightMid.x - ts.x, rightMid.y - ts.y * 0.5f), C(fg, alpha), key.c_str());
    return;
  }
  case KeycapStyle::Box:
    break;
  }
  ImVec2 ts = TextSize(ImGui::GetFont(), size, key.c_str());
  ImVec2 b(rightMid.x, rightMid.y + ts.y * 0.5f + pad.y);
  ImVec2 a(b.x - ts.x - pad.x * 2, rightMid.y - ts.y * 0.5f - pad.y);
  dl->AddRect(a, b, C(border, alpha), 0, 0, LineW());
  Text(dl, ImGui::GetFont(), size, a + pad, C(fg, alpha), key.c_str());
}

float KeycapWidth(const std::string& key) {
  if (key.empty()) return 0;
  switch (CurrentStyle().keycap) {
  case KeycapStyle::Brackets: return TextSize(ImGui::GetFont(), Px(kHint), ("[" + Upper(key) + "]").c_str()).x;
  case KeycapStyle::Plain: return TextSize(ImGui::GetFont(), Px(kHint), key.c_str()).x;
  case KeycapStyle::Box: break;
  }
  return TextSize(ImGui::GetFont(), Px(kHint), key.c_str()).x + Px(14);
}

bool ActionButton(const char* id, const char* title, const std::string& key, Tone tone, bool enabled, bool recordMark,
                  float width) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const float h = ActionHeight();
  ImVec2 size(width > 0 ? width : ImGui::GetContentRegionAvail().x, h);
  ImGui::BeginDisabled(!enabled);
  bool pressed = ImGui::InvisibleButton(id, size);
  ImGui::EndDisabled();
  Motion m = Animate(enabled);
  if (enabled && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  const ImVec2 a0 = ImGui::GetItemRectMin(), b0 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  const Role role = tone == Tone::Primary ? Role::Primary : tone == Tone::Danger ? Role::Danger : Role::Normal;
  ButtonPaint bp = PaintButton(dl, a0, b0, role, m, enabled);
  const ImVec2 a = bp.a, b = bp.b;
  if (tone == Tone::Primary && enabled) DrawTrace(dl, a, b, p.text);

  float mid = (a.y + b.y) * 0.5f;
  float x = a.x + Px(14) + bp.inset;
  if (recordMark) {
    DrawMark(dl, ImVec2(x + Px(4), mid), Px(4), C(enabled ? p.danger : p.textFaint), true);
    x += Px(18);
  }
  float fs = Px(kBody);
  float textW = b.x - x - KeycapWidth(key) - Px(20);
  if (st.upperButtons) {
    // Táctico: MAYÚSCULAS espaciadas, un punto más pequeñas.
    const float us = Px(kSmall), tracking = Px(1.2f);
    std::string up = Upper(title);
    if (TrackedWidth(bold_, us, up.c_str(), tracking) > textW)
      up = Ellipsize(bold_, us, up, textW - tracking * float(up.size() / 2));
    TrackedText(dl, bold_, us, ImVec2(x, mid - us * 0.5f), C(bp.fg), up.c_str(), tracking);
  } else {
    std::string t = Ellipsize(bold_, fs, title, textW);
    Text(dl, bold_, fs, ImVec2(x, mid - fs * 0.5f), C(bp.fg), t.c_str());
  }
  ImU32 keyFg = tone == Tone::Normal ? (enabled ? bp.muted : p.textFaint) : bp.fg;
  DrawKeycap(dl, ImVec2(b.x - Px(12), mid), key, bp.keyBorder, keyFg, 1.0f);
  if (enabled) PaintPress(dl, a0, b0, role, m);
  return pressed && enabled;
}

void DrawSwitch(ImDrawList* dl, ImVec2 rightMid, bool on, ImGuiID animId, float alpha) {
  const Palette& p = Colors();
  float& t = *ImGui::GetStateStorage()->GetFloatRef(animId, on ? 1.0f : 0.0f);
  float target = on ? 1.0f : 0.0f;
  if (!ThemeMotion()) {
    t = target;
  } else if (t != target) {
    t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
    if (std::fabs(t - target) < 0.01f) t = target;
    else RequestFrame();
  }
  switch (CurrentStyle().toggle) {
  case SwitchShape::Box: {
    // Casilla cuadrada: el relleno crece desde el centro.
    const float side = Px(16), inner = Px(8) * t;
    ImVec2 a(rightMid.x - side, rightMid.y - side * 0.5f), b(rightMid.x, rightMid.y + side * 0.5f);
    dl->AddRect(a, b, C(Mix(p.lineStrong, p.accent, t), alpha), 0, 0, LineW());
    if (inner > Px(1)) {
      ImVec2 c = (a + b) * 0.5f;
      dl->AddRectFilled(c - ImVec2(inner * 0.5f, inner * 0.5f), c + ImVec2(inner * 0.5f, inner * 0.5f),
                        C(p.accent, alpha));
    }
    return;
  }
  case SwitchShape::Line: {
    // Línea fina con un cuadrado que la recorre.
    const float w = Px(30), knob = Px(10);
    ImVec2 a(rightMid.x - w, rightMid.y - Px(1)), b(rightMid.x, rightMid.y + Px(1));
    dl->AddRectFilled(a, b, C(Mix(p.lineStrong, p.accent, t), alpha));
    float knobX = a.x + (w - knob) * t;
    ImVec2 ka(knobX, rightMid.y - knob * 0.5f), kb(knobX + knob, rightMid.y + knob * 0.5f);
    dl->AddRectFilled(ka, kb, C(Mix(p.bg, p.accent, t), alpha));
    dl->AddRect(ka, kb, C(Mix(p.textMuted, p.accent, t), alpha), 0, 0, LineW());
    return;
  }
  case SwitchShape::Track:
    break;
  }
  const float w = Px(32), h = Px(16), knob = Px(10), pad = Px(3);
  ImVec2 a(rightMid.x - w, rightMid.y - h * 0.5f), b(rightMid.x, rightMid.y + h * 0.5f);
  dl->AddRectFilled(a, b, C(Mix(p.surfaceActive, p.accent, t), alpha));
  float knobX = a.x + pad + (w - knob - pad * 2) * t;
  dl->AddRectFilled(ImVec2(knobX, a.y + pad), ImVec2(knobX + knob, a.y + pad + knob),
                    C(Mix(p.textMuted, p.onAccent, t), alpha));
}

bool ToggleRow(const char* id, Icon icon, const char* label, bool on) {
  const Palette& p = Colors();
  const float h = RowHeight(36);
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  bool pressed = ImGui::InvisibleButton(id, size);
  Motion m = Animate();
  bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 rowA(a.x - Px(8), a.y), rowB(b.x + Px(8), b.y);
  TrackHover(ImGui::GetItemID(), rowA, rowB, hov);
  PaintButton(dl, rowA, rowB, Role::Quiet, m);
  float mid = (a.y + b.y) * 0.5f;
  DrawIcon(dl, icon, ImVec2(a.x + Px(8), mid), Px(8), C(on ? p.text : p.textMuted));
  float fs = Px(kBody);
  Text(dl, ImGui::GetFont(), fs, ImVec2(a.x + Px(26), mid - fs * 0.5f), C(p.text), label);
  DrawSwitch(dl, ImVec2(b.x, mid), on, ImGui::GetItemID() ^ 0x5157u, 1.0f);
  PaintPress(dl, rowA, rowB, Role::Quiet, m);
  return pressed;
}

namespace {

// Resalte del elemento elegido de un control segmentado o de la rejilla de
// duraciones, con la forma del diseño.
void DrawSelection(ImDrawList* dl, ImVec2 a, ImVec2 b) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  switch (st.button) {
  case ButtonShape::Outline:
    if (st.hover == HoverEffect::Invert) {
      dl->AddRectFilled(a, b, C(p.text));
      return;
    }
    dl->AddRectFilled(a, b, C(p.accent, 0.16f));
    dl->AddRect(a, b, C(p.accent), 0, 0, LineW());
    return;
  case ButtonShape::Bracket:
    dl->AddRectFilled(a, b, C(p.accent, 0.16f));
    DrawBrackets(dl, a, b, C(p.accent), Px(6));
    return;
  case ButtonShape::Rail:
    dl->AddRectFilled(a, b, C(p.surfaceActive));
    dl->AddRectFilled(ImVec2(a.x, b.y - Px(3)), b, C(p.accent));
    return;
  case ButtonShape::Double: {
    // Dos contornos hacia dentro: el resalte no pisa las celdas vecinas.
    const float o = std::round(Px(3));
    dl->AddRectFilled(a, b, C(p.accent, 0.12f));
    dl->AddRect(a, b, C(p.accent), 0, 0, LineW());
    dl->AddRect(a + ImVec2(o, o), b - ImVec2(o, o), C(p.accent, 0.4f), 0, 0, LineW());
    return;
  }
  case ButtonShape::Flat:
  case ButtonShape::Chamfer:
    break;
  }
  ShapeFill(dl, a, b, C(p.accent));
}

// Controles con borde en vez de relleno (segmentados, rejilla de duraciones).
bool Outlined() { return CurrentStyle().button == ButtonShape::Outline || CurrentStyle().button == ButtonShape::Double; }

// Color del texto encima del elemento elegido.
ImU32 SelectedText() {
  const Style& st = CurrentStyle();
  const Palette& p = Colors();
  if (st.button == ButtonShape::Outline) return st.hover == HoverEffect::Invert ? p.bg : p.text;
  if (st.button == ButtonShape::Bracket || st.button == ButtonShape::Rail || st.button == ButtonShape::Double) return p.text;
  return p.onAccent;
}

// Posición animada del resalte (en celdas): se desliza hasta la elegida.
float SlideTo(ImGuiID id, float target) {
  float& v = *ImGui::GetStateStorage()->GetFloatRef(id, -1.0f);
  if (v < 0 || target < 0 || !ThemeMotion()) {
    v = target;
    return v;
  }
  if (v != target) {
    v += (target - v) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
    if (std::fabs(v - target) < 0.01f) v = target;
    else RequestFrame();
  }
  return v;
}

} // namespace

int Segmented(const char* id, const std::vector<std::pair<int, std::string>>& options, int current, float width) {
  const Palette& p = Colors();
  ImGui::PushID(id);
  const float h = RowHeight(32);
  ImVec2 a = ImGui::GetCursorScreenPos();
  float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (Outlined())
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), C(CurrentStyle().hover == HoverEffect::Invert ? p.text : p.line), 0, 0,
                LineW());
  else dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), C(p.surface));
  float segW = w / float(std::max<size_t>(options.size(), 1));
  int selected = -1;
  for (size_t i = 0; i < options.size(); ++i)
    if (options[i].first == current) selected = int(i);
  // El resalte se desliza de la opción anterior a la nueva.
  const float at = SlideTo(ImGui::GetID("sel"), float(selected));
  if (at >= 0) DrawSelection(dl, ImVec2(a.x + segW * at, a.y), ImVec2(a.x + segW * (at + 1), a.y + h));
  int clicked = -1;
  for (size_t i = 0; i < options.size(); ++i) {
    ImVec2 q(a.x + segW * float(i), a.y);
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(int(i));
    if (ImGui::InvisibleButton("seg", ImVec2(segW, h))) clicked = options[i].first;
    Motion m = Animate();
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    bool sel = int(i) == selected;
    ImVec2 qb(q.x + segW, q.y + h);
    if (!sel && m.hover > 0) dl->AddRectFilled(q, qb, C(p.surfaceHover, m.hover));
    float ss = Px(kSmall);
    ImFont* font = sel ? bold_ : ImGui::GetFont();
    ImVec2 ts = TextSize(font, ss, options[i].second.c_str());
    Text(dl, font, ss, ImVec2(q.x + (segW - ts.x) * 0.5f, q.y + (h - ts.y) * 0.5f),
         C(sel ? SelectedText() : Mix(p.textMuted, p.text, m.hover)), options[i].second.c_str());
    PaintPress(dl, q, qb, Role::Quiet, m);
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
  const float h = RowHeight(28), line = Px(1);
  ImVec2 a = ImGui::GetCursorScreenPos();
  float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Celdas planas separadas por una línea de 1 px del color del fondo.
  const bool outline = Outlined();
  if (outline) dl->AddRect(a, ImVec2(a.x + w, a.y + h * float(rows)), C(p.line), 0, 0, LineW());
  else dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h * float(rows)), C(p.surface));
  float cellW = w / float(kCols);
  int selected = -1;
  for (int i = 0; i < count; ++i)
    if (kClipLengths[i] == current) selected = i;
  // El resalte se desliza por la rejilla hasta la duración elegida.
  const float col = SlideTo(ImGui::GetID("selx"), selected >= 0 ? float(selected % kCols) : -1.0f);
  const float row = SlideTo(ImGui::GetID("sely"), selected >= 0 ? float(selected / kCols) : -1.0f);
  if (col >= 0 && row >= 0) {
    ImVec2 q(a.x + cellW * col, a.y + h * row);
    DrawSelection(dl, q, ImVec2(q.x + cellW, q.y + h));
  }
  int clicked = -1;
  for (int i = 0; i < count; ++i) {
    const int sec = kClipLengths[i];
    ImVec2 q(a.x + cellW * float(i % kCols), a.y + h * float(i / kCols));
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("len", ImVec2(cellW, h))) clicked = sec;
    Motion m = Animate();
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
    bool sel = i == selected;
    ImVec2 b(q.x + cellW, q.y + h);
    if (!sel && m.hover > 0) dl->AddRectFilled(q, b, C(p.surfaceHover, m.hover));
    if (!outline) {
      if (i % kCols != 0) dl->AddRectFilled(q, ImVec2(q.x + line, b.y), C(p.bg));
      if (i >= kCols) dl->AddRectFilled(q, ImVec2(b.x, q.y + line), C(p.bg));
    }
    float ss = Px(kSmall);
    ImFont* font = sel ? bold_ : ImGui::GetFont();
    std::string label = FormatClipLength(sec);
    ImVec2 ts = TextSize(font, ss, label.c_str());
    ImU32 fg = sel ? SelectedText() : tight ? p.textFaint : Mix(p.textMuted, p.text, m.hover);
    Text(dl, font, ss, ImVec2(q.x + (cellW - ts.x) * 0.5f, q.y + (h - ts.y) * 0.5f), C(fg), label.c_str());
    PaintPress(dl, q, b, Role::Quiet, m);
  }
  ImGui::SetCursorScreenPos(a);
  ImGui::Dummy(ImVec2(w, h * float(rows)));
  ImGui::PopID();
  return clicked;
}

bool IconButton(const char* id, Icon icon, float size, const char* tooltip, bool active) {
  const Palette& p = Colors();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  Motion m = Animate();
  bool hov = ImGui::IsItemHovered();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (tooltip) ImGui::SetTooltip("%s", tooltip);
  }
  ButtonPaint bp = PaintButton(dl, a, b, Role::Quiet, m);
  ImU32 fg = active ? p.accent : Mix(p.textMuted, bp.fg, m.hover);
  DrawIcon(dl, icon, (bp.a + bp.b) * 0.5f, (bp.b.x - bp.a.x) * 0.25f, C(fg));
  PaintPress(dl, a, b, Role::Quiet, m);
  return pressed;
}

bool GhostButton(const char* id, const char* label, float width, Tone tone, float height) {
  bool pressed = ImGui::InvisibleButton(id, ImVec2(width, Px(height)));
  Motion m = Animate();
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  const ImVec2 a0 = ImGui::GetItemRectMin(), b0 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Role role = tone == Tone::Danger ? Role::GhostDanger : Role::Ghost;
  ButtonPaint bp = PaintButton(dl, a0, b0, role, m);
  float fs = Px(kSmall);
  ImVec2 ts = TextSize(ImGui::GetFont(), fs, label);
  Text(dl, ImGui::GetFont(), fs,
       ImVec2(bp.a.x + (bp.b.x - bp.a.x - ts.x) * 0.5f, bp.a.y + (bp.b.y - bp.a.y - ts.y) * 0.5f), C(bp.fg), label);
  PaintPress(dl, a0, b0, role, m);
  return pressed;
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

bool SolidButton(const char* id, const char* label, float width, float height) {
  bool pressed = ImGui::InvisibleButton(id, ImVec2(width, Px(height)));
  // Dentro de BeginDisabled ImGui no lo deja pulsar y lo atenúa (style.Alpha).
  Motion m = Animate();
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  const ImVec2 a0 = ImGui::GetItemRectMin(), b0 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ButtonPaint bp = PaintButton(dl, a0, b0, Role::Primary, m);
  float fs = Px(kSmall);
  ImVec2 ts = TextSize(bold_, fs, label);
  Text(dl, bold_, fs, ImVec2(bp.a.x + (bp.b.x - bp.a.x - ts.x) * 0.5f, bp.a.y + (bp.b.y - bp.a.y - ts.y) * 0.5f),
       C(bp.fg), label);
  PaintPress(dl, a0, b0, Role::Primary, m);
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
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q = ImGui::GetCursorScreenPos();
  float size = Px(kLabel), h = Px(kHint) + Px(2);
  float w = DrawLabel(dl, ImVec2(q.x, q.y + (h - size) * 0.5f), text, size, C(p.textMuted));
  // Una línea hasta el borde, con el estilo de los separadores del diseño.
  float avail = ImGui::GetContentRegionAvail().x;
  const float x0 = q.x + w + Px(12), x1 = q.x + avail, y = q.y + h * 0.5f, t = std::round(1.0f * s_);
  if (avail - w > Px(24)) {
    switch (CurrentStyle().divider) {
    case DividerStyle::Line:
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + std::max(t, LineW() * 0.5f)), C(p.line));
      break;
    case DividerStyle::Ticked:
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + t), C(p.line));
      dl->AddRectFilled(ImVec2(x1 - Px(14), y - t), ImVec2(x1, y + t * 2), C(p.accent));
      break;
    case DividerStyle::Dashed:
      for (float x = x0; x < x1; x += Px(8)) dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + Px(4), x1), y + t), C(p.lineStrong));
      break;
    case DividerStyle::Space:
      break;
    }
  }
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
  // El interruptor del diseño, pegado a la izquierda de la columna de controles.
  const float switchW = CurrentStyle().toggle == SwitchShape::Box ? Px(16) : CurrentStyle().toggle == SwitchShape::Line ? Px(30) : Px(32);
  bool pressed = ImGui::InvisibleButton("switch", ImVec2(switchW, rowH));
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

bool NavItem(int index, const char* name, bool selected, float height, int depth, bool last) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  ImGui::PushID(index);
  const float h = Px(height);
  // Terminal: menú en árbol, en minúsculas y con "> " delante del elegido.
  const bool tree = st.title == TitleLook::Prompt;
  bool pressed = ImGui::InvisibleButton("nav", ImVec2(ImGui::GetContentRegionAvail().x, h));
  Motion m = Animate();
  bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  TrackHover(ImGui::GetItemID(), a, b, hov);
  ImU32 fg = selected ? p.text : Mix(p.textMuted, p.text, m.hover);
  const float sel = selected ? 1.0f : 0.0f;
  if (tree) {
    float mid = (a.y + b.y) * 0.5f, fs = Px(kBody), x = a.x + Px(16);
    const float t = std::max(1.0f, std::round(Px(1)));
    if (depth > 0) {
      // Rama: una línea que baja y otra hasta el nombre ("├─" o "└─").
      const float bx = a.x + Px(22);
      dl->AddRectFilled(ImVec2(bx, a.y), ImVec2(bx + t, last ? mid : b.y), C(p.lineStrong));
      dl->AddRectFilled(ImVec2(bx, mid), ImVec2(bx + Px(10), mid + t), C(p.lineStrong));
      x = bx + Px(16);
    }
    if (selected) dl->AddRectFilled(ImVec2(x - Px(6), a.y + Px(3)), ImVec2(b.x - Px(8), b.y - Px(3)), C(p.accent, 0.14f));
    else if (m.hover > 0) dl->AddRectFilled(ImVec2(x - Px(6), a.y + Px(3)), ImVec2(b.x - Px(8), b.y - Px(3)), C(p.surface, m.hover));
    std::string label = Lowercase(name);
    if (selected) {
      Text(dl, bold_, fs, ImVec2(x, mid - fs * 0.5f), C(p.accent), ">");
      x += TextSize(bold_, fs, "> ").x;
    }
    Text(dl, selected ? bold_ : ImGui::GetFont(), fs, ImVec2(x, mid - fs * 0.5f), C(selected ? p.text : fg), label.c_str());
    PaintPress(dl, a, b, Role::Quiet, m);
    return pressed;
  }
  // La entrada elegida con la forma del diseño; las demás, un resalte suave.
  switch (st.button) {
  case ButtonShape::Bracket:
    if (selected) dl->AddRectFilled(a, b, C(p.surface, 0.6f));
    else if (m.hover > 0) dl->AddRectFilled(a, b, C(p.surface, 0.4f * m.hover));
    if (selected || m.hover > 0)
      DrawBrackets(dl, a + ImVec2(Px(6), Px(2)), b - ImVec2(Px(6), Px(2)), C(selected ? p.accent : p.lineStrong,
                   selected ? 1.0f : m.hover), Px(6));
    break;
  case ButtonShape::Rail: {
    if (selected || m.hover > 0) dl->AddRectFilled(a, b, C(p.surface, selected ? 1.0f : 0.6f * m.hover));
    const float frac = std::max(sel, 0.5f * m.hover), mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.5f * frac;
    if (frac > 0) dl->AddRectFilled(ImVec2(a.x, mid - half), ImVec2(a.x + Px(4), mid + half), C(p.accent));
    break;
  }
  case ButtonShape::Outline:
    if (st.hover == HoverEffect::Invert) {
      if (selected) {
        dl->AddRectFilled(a, b, C(p.text));
        fg = p.bg;
      } else if (m.hover > 0) {
        dl->AddRect(a + ImVec2(Px(4), 0), b - ImVec2(Px(4), 0), C(p.text, m.hover), 0, 0, LineW());
      }
      break;
    }
    if (selected) dl->AddRect(a + ImVec2(Px(4), Px(1)), b - ImVec2(Px(4), Px(1)), C(p.lineStrong), 0, 0, LineW());
    else if (m.hover > 0) dl->AddRectFilled(a, b, C(p.surface, 0.6f * m.hover));
    if (selected) dl->AddRectFilled(ImVec2(a.x + Px(4), a.y + Px(1)), ImVec2(a.x + Px(6), b.y - Px(1)), C(p.accent));
    if (st.hover == HoverEffect::Underline && !selected && m.hover > 0) {
      float tw = TextSize(ImGui::GetFont(), Px(kBody), name).x;
      float y = (a.y + b.y) * 0.5f + Px(kBody) * 0.5f + Px(2);
      dl->AddRectFilled(ImVec2(a.x + Px(16), y), ImVec2(a.x + Px(16) + tw * EaseOut(m.hover), y + Px(1)), C(p.accent));
    }
    break;
  case ButtonShape::Double: {
    const ImVec2 ia = a + ImVec2(Px(6), Px(2)), ib = b - ImVec2(Px(6), Px(2));
    if (selected) {
      dl->AddRect(ia, ib, C(p.accent), 0, 0, LineW());
      dl->AddRect(ia + ImVec2(Px(3), Px(3)), ib - ImVec2(Px(3), Px(3)), C(p.accent, 0.35f), 0, 0, LineW());
    } else if (m.hover > 0) {
      dl->AddRect(ia, ib, C(p.lineStrong, m.hover), 0, 0, LineW());
    }
    break;
  }
  case ButtonShape::Chamfer:
    if (selected) ShapeFill(dl, a, b, C(p.surface));
    else if (m.hover > 0) ShapeFill(dl, a, b, C(p.surface, 0.6f * m.hover));
    if (selected) dl->AddRectFilled(a, ImVec2(a.x + Px(2), b.y), C(p.accent));
    if (st.hover == HoverEffect::Border && m.hover > 0) ShapeStroke(dl, a, b, C(p.accent, m.hover));
    break;
  case ButtonShape::Flat:
    if (selected) {
      dl->AddRectFilled(a, b, C(p.surface));
      dl->AddRectFilled(a, ImVec2(a.x + Px(2), b.y), C(p.accent));
    } else if (m.hover > 0) {
      dl->AddRectFilled(a, b, C(p.surface, 0.6f * m.hover));
    }
    break;
  }
  float mid = (a.y + b.y) * 0.5f;
  float fs = Px(kBody);
  Text(dl, selected ? bold_ : ImGui::GetFont(), fs, ImVec2(a.x + Px(16), mid - fs * 0.5f), C(fg), name);
  PaintPress(dl, a, b, Role::Quiet, m);
  return pressed;
}

void NavLabel(const char* text) {
  Gap(12);
  ImVec2 q = ImGui::GetCursorScreenPos();
  float size = Px(kLabel);
  DrawLabel(ImGui::GetWindowDrawList(), ImVec2(q.x + Px(16), q.y), text, size, C(Colors().textFaint));
  ImGui::Dummy(ImVec2(0, size + Px(6)));
}

// ------------------------------------------------------- estructura del diseño

namespace {

// Margen de la tarjeta alrededor de la miniatura y alto del pie (px lógicos).
float CardPad() {
  switch (CurrentStyle().card) {
  case CardLook::Framed:
  case CardLook::Ascii: return 8;
  case CardLook::Neon: return 6;
  case CardLook::Tile:
  case CardLook::Polaroid: return 10;
  default: return 0;
  }
}
float CaptionHeight() { return CurrentStyle().card == CardLook::Polaroid ? 56.0f : 50.0f; }

// Tapa una esquina de la miniatura con el color de debajo: la imagen queda con
// la esquina cortada del diseño.
void CutCorners(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 under) {
  const Style& st = CurrentStyle();
  if (st.button != ButtonShape::Chamfer || !st.cutCorners) return;
  const float k = std::min(Px(st.cut) * 1.4f, std::min(b.x - a.x, b.y - a.y) * 0.3f);
  if (st.cutCorners & 1) dl->AddTriangleFilled(a, ImVec2(a.x + k, a.y), ImVec2(a.x, a.y + k), under);
  if (st.cutCorners & 2) dl->AddTriangleFilled(ImVec2(b.x, a.y), ImVec2(b.x, a.y + k), ImVec2(b.x - k, a.y), under);
  if (st.cutCorners & 4) dl->AddTriangleFilled(b, ImVec2(b.x - k, b.y), ImVec2(b.x, b.y - k), under);
  if (st.cutCorners & 8) dl->AddTriangleFilled(ImVec2(a.x, b.y), ImVec2(a.x, b.y - k), ImVec2(a.x + k, b.y), under);
}

// Primera letra de un texto UTF-8 (la que Suave pinta del acento).
const char* AfterFirst(const char* text) {
  if (!*text) return text;
  const char* next = text + 1;
  while (*next && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
  return next;
}

// Texto de una pestaña según el diseño.
std::string TabText(const char* name) {
  switch (CurrentStyle().title) {
  case TitleLook::Index:
  case TitleLook::Chroma: return Upper(name);
  case TitleLook::Prompt: return Lowercase(name);
  default: return name;
  }
}
bool TabUpper() { return CurrentStyle().title == TitleLook::Index || CurrentStyle().title == TitleLook::Chroma; }
float TabFontSize() { return TabUpper() ? Px(kSmall) : Px(kBody); }

} // namespace

float MediaCardHeight(float width) {
  const float pad = Px(CardPad());
  const float thumbH = std::floor((width - pad * 2) * 9.0f / 16.0f);
  return pad + thumbH + Px(CaptionHeight()) + (pad > 0 ? Px(4) : 0.0f);
}

MediaCard MediaCardRects(ImVec2 a, float width) {
  MediaCard c;
  const float pad = Px(CardPad());
  const float thumbW = width - pad * 2, thumbH = std::floor(thumbW * 9.0f / 16.0f);
  c.a = a;
  c.b = ImVec2(a.x + width, a.y + MediaCardHeight(width));
  c.thumbA = ImVec2(a.x + pad, a.y + pad);
  c.thumbB = ImVec2(c.thumbA.x + thumbW, c.thumbA.y + thumbH);
  const float inset = CurrentStyle().card == CardLook::Heavy ? Px(10) : 0.0f;
  c.text = ImVec2(c.thumbA.x + inset, c.thumbB.y + Px(10));
  c.textWidth = thumbW - inset * 2 - Px(4);
  return c;
}

void MediaCardBack(ImDrawList* dl, const MediaCard& c, float hover, bool selected) {
  const Palette& p = Colors();
  (void)selected;
  switch (CurrentStyle().card) {
  case CardLook::Ascii:
  case CardLook::Framed:
  case CardLook::Neon:
  case CardLook::Tile:
  case CardLook::Polaroid:
    TileFrame(dl, c.a, c.b, hover);
    break;
  case CardLook::Heavy:
    // El pie es una franja invertida debajo de la miniatura.
    dl->AddRectFilled(ImVec2(c.a.x, c.thumbB.y), c.b, C(Mix(p.text, p.accent, hover)));
    break;
  case CardLook::Plain:
  case CardLook::Hud:
    break;
  }
  ShapeFill(dl, c.thumbA, c.thumbB, C(CurrentStyle().card == CardLook::Tile ? p.bgAlt : p.surface));
}

void MediaCardFront(ImDrawList* dl, const MediaCard& c, float hover, bool selected) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  (void)selected;
  switch (st.card) {
  case CardLook::Plain:
    HoverOutline(dl, c.a, c.b, hover);
    break;
  case CardLook::Hud: {
    CutCorners(dl, c.thumbA, c.thumbB, C(p.bg));
    // Corchetes de visor alrededor de la miniatura; se abren con el ratón.
    const float o = Px(3) + Px(2) * hover;
    DrawBrackets(dl, c.thumbA - ImVec2(o, o), c.thumbB + ImVec2(o, o), C(Mix(p.lineStrong, p.accent, hover)),
                 Px(10) + Px(8) * hover, std::max(1.0f, std::round(Px(2))));
    break;
  }
  case CardLook::Framed:
    CutCorners(dl, c.thumbA, c.thumbB, C(p.bg));
    break;
  case CardLook::Tile: {
    // Raíl del acento que crece desde el centro con el ratón.
    if (hover > 0) {
      const float mid = (c.a.y + c.b.y) * 0.5f, half = (c.b.y - c.a.y) * 0.5f * EaseOut(hover);
      dl->AddRectFilled(ImVec2(c.a.x, mid - half), ImVec2(c.a.x + Px(3), mid + half), C(p.accent));
    }
    break;
  }
  case CardLook::Heavy:
    dl->AddRect(c.thumbA, ImVec2(c.thumbB.x, c.b.y), C(p.text), 0, 0, LineW());
    break;
  case CardLook::Polaroid:
    dl->AddRect(c.thumbA, c.thumbB, C(p.line), 0, 0, 1.0f);
    break;
  case CardLook::Ascii:
  case CardLook::Neon:
    break;
  }
}

void MediaCardCaption(ImDrawList* dl, const MediaCard& c, const std::string& title, const std::string& detail, float hover,
                      int number) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  ImFont* font = ImGui::GetFont();
  const float fs = Px(kBody), ss = Px(kSmall);
  float w = c.textWidth;
  switch (st.card) {
  case CardLook::Hud: {
    char num[16];
    snprintf(num, sizeof(num), "#%02d", number);
    const float nw = number > 0 ? TextSize(bold_, ss, num).x + Px(8) : 0.0f;
    if (number > 0) Text(dl, bold_, ss, ImVec2(c.thumbB.x - nw + Px(8), c.text.y + Px(1)), C(p.accent), num);
    std::string up = Upper(title);
    const float tracking = Px(1.0f);
    up = Ellipsize(bold_, ss, up, w - nw - tracking * 12);
    TrackedText(dl, bold_, ss, c.text + ImVec2(0, Px(1)), C(Mix(p.text, p.accent, hover)), up.c_str(), tracking);
    Text(dl, font, ss, ImVec2(c.text.x, c.text.y + Px(20)), C(p.textFaint), Ellipsize(font, ss, detail, w).c_str());
    return;
  }
  case CardLook::Ascii: {
    Text(dl, bold_, fs, c.text, C(p.accent), ">");
    const float x = c.text.x + TextSize(bold_, fs, "> ").x;
    Text(dl, bold_, fs, ImVec2(x, c.text.y), C(p.text), Ellipsize(bold_, fs, title, w - (x - c.text.x)).c_str());
    Text(dl, font, ss, ImVec2(c.text.x, c.text.y + Px(20)), C(p.textFaint), Ellipsize(font, ss, detail, w).c_str());
    return;
  }
  case CardLook::Polaroid: {
    // Pie centrado con la letra de títulos (con serifa en Papel).
    const float ds = Px(kBody) + Px(2);
    ImFont* df = Display(ds);
    std::string t = Ellipsize(df, ds, title, w);
    const float tw = TextSize(df, ds, t.c_str()).x;
    Text(dl, df, ds, ImVec2(c.text.x + (w - tw) * 0.5f, c.text.y + Px(2)), C(p.text), t.c_str());
    std::string d = Ellipsize(font, ss, detail, w);
    const float dw = TextSize(font, ss, d.c_str()).x;
    Text(dl, font, ss, ImVec2(c.text.x + (w - dw) * 0.5f, c.text.y + Px(26)), C(p.textMuted), d.c_str());
    return;
  }
  case CardLook::Heavy:
    Text(dl, bold_, fs, c.text, C(p.bg), Ellipsize(bold_, fs, title, w).c_str());
    Text(dl, font, ss, ImVec2(c.text.x, c.text.y + Px(20)), C(p.bg), Ellipsize(font, ss, detail, w).c_str());
    return;
  case CardLook::Neon: {
    std::string up = Upper(title);
    const float tracking = Px(1.2f);
    up = Ellipsize(bold_, ss, up, w - tracking * 12);
    TrackedText(dl, bold_, ss, c.text + ImVec2(0, Px(1)), C(Mix(p.text, p.accent, hover)), up.c_str(), tracking);
    Text(dl, font, ss, ImVec2(c.text.x, c.text.y + Px(20)), C(p.textFaint), Ellipsize(font, ss, detail, w).c_str());
    return;
  }
  case CardLook::Plain:
  case CardLook::Framed:
  case CardLook::Tile:
    break;
  }
  Text(dl, bold_, fs, c.text, C(p.text), Ellipsize(bold_, fs, title, w).c_str());
  Text(dl, font, ss, ImVec2(c.text.x, c.text.y + Px(20)), C(p.textFaint), Ellipsize(font, ss, detail, w).c_str());
}

void Tag(ImDrawList* dl, ImVec2 pos, const std::string& text, bool alignRight) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  float fs = Px(kLabel);
  std::string shown = st.card == CardLook::Ascii ? "[" + text + "]" : text;
  ImVec2 ts = TextSize(bold_, fs, shown.c_str());
  ImVec2 pad(Px(6), Px(3));
  ImVec2 a = alignRight ? ImVec2(pos.x - ts.x - pad.x * 2, pos.y) : pos;
  ImVec2 b(a.x + ts.x + pad.x * 2, a.y + ts.y + pad.y * 2);
  ImU32 fill = C(p.bg, 0.86f), fg = C(p.text);
  switch (st.card) {
  case CardLook::Hud:
    // Táctico: el tipo en el acento.
    if (!alignRight) {
      fill = C(p.accent);
      fg = C(p.onAccent);
    }
    break;
  case CardLook::Heavy:
    fill = C(p.text);
    fg = C(p.bg);
    break;
  case CardLook::Ascii:
    fg = C(p.accent);
    break;
  default:
    break;
  }
  dl->AddRectFilled(a, b, fill);
  if (st.card == CardLook::Neon) dl->AddRect(a, b, C(p.accent, 0.7f), 0, 0, 1.0f);
  Text(dl, bold_, fs, a + pad, fg, shown.c_str());
}

float HeadlineHeight() {
  const TitleLook t = CurrentStyle().title;
  return t == TitleLook::Prompt ? Px(kBody + 2) : t == TitleLook::Index || t == TitleLook::Chroma ? Px(kBody + 2) : Px(kTitle);
}

float Headline(ImDrawList* dl, ImVec2 pos, const std::string& text, ImU32 color, float maxWidth) {
  const Palette& p = Colors();
  const TitleLook look = CurrentStyle().title;
  const float size = HeadlineHeight();
  switch (look) {
  case TitleLook::Prompt: {
    Text(dl, bold_, size, pos, C(p.accent), ">");
    const float x = pos.x + TextSize(bold_, size, "> ").x;
    Text(dl, bold_, size, ImVec2(x, pos.y), color, Ellipsize(bold_, size, text, maxWidth - (x - pos.x)).c_str());
    return size;
  }
  case TitleLook::Index:
  case TitleLook::Chroma: {
    ImFont* df = Display(size);
    const float tracking = Px(1.2f);
    std::string up = Upper(text);
    if (TrackedWidth(df, size, up.c_str(), tracking) > maxWidth) up = Ellipsize(df, size, up, maxWidth - tracking * 30);
    if (look == TitleLook::Chroma) {
      TrackedText(dl, df, size, pos + ImVec2(-Px(1.5f), 0), C(p.danger, 0.5f), up.c_str(), tracking);
      TrackedText(dl, df, size, pos + ImVec2(Px(1.5f), 0), C(p.accent, 0.7f), up.c_str(), tracking);
    }
    TrackedText(dl, df, size, pos, color, up.c_str(), tracking);
    return size;
  }
  default: {
    ImFont* df = Display(size);
    Text(dl, df, size, pos, color, Ellipsize(df, size, text, maxWidth).c_str());
    return size;
  }
  }
}

void PageTitle(const char* title, const std::string& subtitle, int index, const char* section) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float ts = Px(st.titleSize);
  ImFont* df = Display(ts);
  const float line = std::max(1.0f, std::round(Px(1)));
  float h = ts;
  float subX = q.x;
  bool centered = false;
  switch (st.title) {
  case TitleLook::Block: {
    // Un bloque del acento a la altura de las mayúsculas.
    const float bar = Px(7);
    dl->AddRectFilled(ImVec2(q.x, q.y + ts * 0.22f), ImVec2(q.x + bar, q.y + ts * 0.90f), C(p.accent));
    Text(dl, df, ts, ImVec2(q.x + bar + Px(14), q.y), C(p.text), title);
    subX = q.x + bar + Px(14);
    break;
  }
  case TitleLook::Echo:
    // El eco del acento, desplazado, detrás del título.
    Text(dl, df, ts, q + ImVec2(Px(4), Px(3)), C(p.accent, 0.30f), title);
    Text(dl, df, ts, q, C(p.text), title);
    break;
  case TitleLook::Chroma: {
    std::string up = Upper(title);
    const float tr = Px(4);
    TrackedText(dl, df, ts, q + ImVec2(-Px(2.5f), 0), C(p.danger, 0.55f), up.c_str(), tr);
    TrackedText(dl, df, ts, q + ImVec2(Px(2.5f), Px(1)), C(p.accent, 0.80f), up.c_str(), tr);
    TrackedText(dl, df, ts, q, C(p.text), up.c_str(), tr);
    break;
  }
  case TitleLook::Index: {
    float x = q.x;
    if (index > 0) {
      char num[8];
      snprintf(num, sizeof(num), "%02d", index);
      Text(dl, df, ts, ImVec2(x, q.y), C(p.accent), num);
      x += TextSize(df, ts, num).x + Px(12);
    }
    Text(dl, df, ts, ImVec2(x, q.y), C(p.textFaint), "//");
    x += TextSize(df, ts, "//").x + Px(12);
    std::string up = section ? Upper(section) + "  /  " + Upper(title) : Upper(title);
    TrackedText(dl, df, ts, ImVec2(x, q.y), C(p.text), up.c_str(), Px(1.5f));
    // Una regla con marcas debajo, como en un HUD.
    const float y = q.y + ts + Px(8);
    dl->AddRectFilled(ImVec2(q.x, y), ImVec2(q.x + avail, y + line), C(p.line));
    dl->AddRectFilled(ImVec2(q.x, y - line), ImVec2(q.x + Px(40), y + line * 2), C(p.accent));
    for (float tx = q.x + Px(60); tx < q.x + avail; tx += Px(24))
      dl->AddRectFilled(ImVec2(tx, y - Px(3)), ImVec2(tx + line, y), C(p.lineStrong));
    h = ts + Px(9);
    break;
  }
  case TitleLook::Prompt: {
    std::string path = "~/" + (section ? Lowercase(section) + "/" : std::string()) + Lowercase(title);
    float x = q.x;
    Text(dl, df, ts, ImVec2(x, q.y), C(p.accent), path.c_str());
    x += TextSize(df, ts, path.c_str()).x + Px(10);
    Text(dl, df, ts, ImVec2(x, q.y), C(p.textMuted), "$");
    x += TextSize(df, ts, "$").x + Px(8);
    dl->AddRectFilled(ImVec2(x, q.y + Px(3)), ImVec2(x + ts * 0.5f, q.y + ts), C(p.accent, Blink()));
    break;
  }
  case TitleLook::Rule: {
    Text(dl, df, ts, q, C(p.text), title);
    const float tw = TextSize(df, ts, title).x, y = q.y + ts + Px(8);
    dl->AddRectFilled(ImVec2(q.x, y), ImVec2(q.x + tw, y + Px(2)), C(p.accent));
    dl->AddRectFilled(ImVec2(q.x + tw, y + Px(1)), ImVec2(q.x + avail, y + Px(1) + line), C(p.line));
    h = ts + Px(10);
    break;
  }
  case TitleLook::Soft: {
    const char* rest = AfterFirst(title);
    std::string first(title, rest);
    Text(dl, df, ts, q, C(p.accent), first.c_str());
    Text(dl, df, ts, ImVec2(q.x + TextSize(df, ts, first.c_str()).x, q.y), C(p.text), rest);
    break;
  }
  case TitleLook::Masthead: {
    // Cabecera de revista: filete doble arriba, el título centrado y un filete debajo.
    dl->AddRectFilled(q, ImVec2(q.x + avail, q.y + line * 2), C(p.text));
    dl->AddRectFilled(ImVec2(q.x, q.y + line * 2 + Px(3)), ImVec2(q.x + avail, q.y + line * 3 + Px(3)), C(p.text));
    const float top = q.y + Px(14);
    const float tw = TextSize(df, ts, title).x;
    Text(dl, df, ts, ImVec2(q.x + (avail - tw) * 0.5f, top), C(p.text), title);
    const float y = top + ts + Px(10);
    dl->AddRectFilled(ImVec2(q.x, y), ImVec2(q.x + avail, y + line), C(p.lineStrong));
    h = y + line - q.y;
    centered = true;
    break;
  }
  case TitleLook::Heavy: {
    const float tw = TextSize(df, ts, title).x;
    dl->AddRectFilled(q, ImVec2(q.x + tw + Px(24), q.y + ts + Px(12)), C(p.text));
    Text(dl, df, ts, ImVec2(q.x + Px(12), q.y + Px(6)), C(p.bg), title);
    h = ts + Px(12);
    break;
  }
  }
  if (!subtitle.empty()) {
    ImFont* font = ImGui::GetFont();
    float ss = st.title == TitleLook::Soft ? Px(kBody) : Px(kSmall);
    std::string text = subtitle;
    ImU32 color = C(p.textMuted);
    if (st.title == TitleLook::Prompt) {
      text = "# " + subtitle;
      color = C(p.textFaint);
    }
    const float y = q.y + h + Px(st.title == TitleLook::Masthead ? 10.0f : 8.0f);
    if (st.title == TitleLook::Index || st.title == TitleLook::Chroma) {
      std::string up = Upper(text);
      ss = Px(kLabel);
      const float tracking = Px(1.4f);
      if (TrackedWidth(bold_, ss, up.c_str(), tracking) > avail - (subX - q.x))
        up = Ellipsize(bold_, ss, up, avail - (subX - q.x) - tracking * 40);
      TrackedText(dl, bold_, ss, ImVec2(subX, y), color, up.c_str(), tracking);
    } else {
      text = Ellipsize(font, ss, text, avail - (subX - q.x));
      const float x = centered ? q.x + (avail - TextSize(font, ss, text.c_str()).x) * 0.5f : subX;
      Text(dl, font, ss, ImVec2(x, y), color, text.c_str());
    }
    h = y + ss - q.y;
  }
  ImGui::Dummy(ImVec2(0, h + Px(4)));
  Gap(st.pad * 0.5f);
}

float PageTitleActionY(float height) {
  const Style& st = CurrentStyle();
  const float ts = Px(st.titleSize);
  const float top = st.title == TitleLook::Masthead ? Px(14) : st.title == TitleLook::Heavy ? Px(6) : 0.0f;
  return std::max(0.0f, top + (ts - Px(height)) * 0.5f);
}

bool RailItem(int index, Icon icon, const char* name, bool selected, float height) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  ImGui::PushID(index);
  const float w = ImGui::GetContentRegionAvail().x;
  bool pressed = ImGui::InvisibleButton("rail", ImVec2(w, height));
  Motion m = Animate();
  bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 ca = a + ImVec2(Px(10), Px(3)), cb = b - ImVec2(Px(10), Px(3));
  TrackHover(ImGui::GetItemID(), ca, cb, hov);
  // La celda elegida con la forma del diseño y un raíl del acento en el borde.
  if (st.button == ButtonShape::Outline || st.button == ButtonShape::Double) {
    if (selected) dl->AddRect(ca, cb, C(p.accent), 0, 0, LineW());
    else if (m.hover > 0) dl->AddRect(ca, cb, C(p.lineStrong, m.hover), 0, 0, LineW());
  } else {
    if (selected) ShapeFill(dl, ca, cb, C(p.accent, 0.16f));
    else if (m.hover > 0) ShapeFill(dl, ca, cb, C(p.surface, m.hover));
    if (selected) ShapeStroke(dl, ca, cb, C(p.accent, 0.8f));
  }
  const float frac = std::max(selected ? 1.0f : 0.0f, 0.5f * m.hover), mid = (a.y + b.y) * 0.5f;
  if (frac > 0) {
    const float half = (b.y - a.y) * 0.32f * frac;
    dl->AddRectFilled(ImVec2(a.x, mid - half), ImVec2(a.x + Px(3), mid + half), C(p.accent));
  }
  const ImU32 fg = selected ? p.text : Mix(p.textMuted, p.text, m.hover);
  const ImVec2 ic((a.x + b.x) * 0.5f, a.y + height * 0.38f);
  DrawIcon(dl, icon, ic, Px(9), C(selected ? p.accent : fg));
  const float ls = Px(kLabel);
  std::string label = Ellipsize(ImGui::GetFont(), ls, name, cb.x - ca.x - Px(4));
  const float lw = TextSize(selected ? bold_ : ImGui::GetFont(), ls, label.c_str()).x;
  Text(dl, selected ? bold_ : ImGui::GetFont(), ls, ImVec2((a.x + b.x - lw) * 0.5f, a.y + height * 0.64f), C(fg),
       label.c_str());
  PaintPress(dl, ca, cb, Role::Quiet, m);
  return pressed;
}

float TabWidth(const char* name) {
  const std::string text = TabText(name);
  const float fs = TabFontSize();
  float w = TabUpper() ? TrackedWidth(bold_, fs, text.c_str(), Px(1.4f)) : TextSize(bold_, fs, text.c_str()).x;
  if (CurrentStyle().title == TitleLook::Prompt) w += TextSize(bold_, fs, "[]").x;
  return w + Px(30);
}

bool TabItem(int index, const char* name, bool selected, float height) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  ImGui::PushID(index);
  bool pressed = ImGui::InvisibleButton("tab", ImVec2(TabWidth(name), height));
  Motion m = Animate();
  bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const std::string text = TabText(name);
  const float fs = TabFontSize();
  ImFont* font = selected || TabUpper() ? bold_ : ImGui::GetFont();
  ImU32 fg = selected ? p.text : Mix(p.textMuted, p.text, m.hover);
  const float mid = (a.y + b.y) * 0.5f;
  const ImVec2 ca = a + ImVec2(0, Px(8)), cb = b - ImVec2(0, Px(8));
  switch (st.title) {
  case TitleLook::Index:
    // Táctico: la elegida es un bloque del acento con la esquina cortada.
    if (selected) {
      ShapeFill(dl, ca, cb, C(p.accent));
      fg = p.onAccent;
    } else if (m.hover > 0) {
      ShapeStroke(dl, ca, cb, C(p.accent, m.hover));
    }
    break;
  case TitleLook::Chroma:
    // Neón: doble raya debajo de la elegida.
    if (selected) {
      dl->AddRectFilled(ImVec2(a.x + Px(6), b.y - Px(3)), ImVec2(b.x - Px(6), b.y - Px(1)), C(p.accent));
      dl->AddRectFilled(ImVec2(a.x + Px(6), b.y - Px(7)), ImVec2(b.x - Px(6), b.y - Px(6)), C(p.accent, 0.4f));
    } else if (m.hover > 0) {
      dl->AddRectFilled(ImVec2(a.x + Px(6), b.y - Px(3)), ImVec2(b.x - Px(6), b.y - Px(2)), C(p.lineStrong, m.hover));
    }
    break;
  case TitleLook::Masthead: {
    // Papel: subrayado de tinta bajo el texto; con el ratón, del acento y creciendo.
    const float tw = TextSize(font, fs, text.c_str()).x, x0 = (a.x + b.x - tw) * 0.5f, y = mid + fs * 0.5f + Px(3);
    if (selected) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + tw, y + Px(2)), C(p.text));
    else if (m.hover > 0) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + tw * EaseOut(m.hover), y + Px(1)), C(p.accent));
    break;
  }
  case TitleLook::Prompt:
    if (selected) dl->AddRectFilled(ca, cb, C(p.accent, 0.14f));
    else if (m.hover > 0) dl->AddRectFilled(ca, cb, C(p.surface, m.hover));
    break;
  default:
    if (selected) dl->AddRectFilled(ImVec2(a.x + Px(8), b.y - Px(2)), ImVec2(b.x - Px(8), b.y), C(p.accent));
    else if (m.hover > 0) dl->AddRectFilled(ImVec2(a.x + Px(8), b.y - Px(2)), ImVec2(b.x - Px(8), b.y), C(p.lineStrong, m.hover));
    break;
  }
  if (st.title == TitleLook::Prompt) {
    // Terminal: [inicio] con los corchetes del acento en la elegida.
    const float lw = TextSize(font, fs, "[").x, tw = TextSize(font, fs, text.c_str()).x;
    float x = (a.x + b.x - tw - lw * 2) * 0.5f;
    Text(dl, font, fs, ImVec2(x, mid - fs * 0.5f), C(selected ? p.accent : p.textFaint), "[");
    Text(dl, font, fs, ImVec2(x + lw, mid - fs * 0.5f), C(fg), text.c_str());
    Text(dl, font, fs, ImVec2(x + lw + tw, mid - fs * 0.5f), C(selected ? p.accent : p.textFaint), "]");
  } else if (TabUpper()) {
    const float tr = Px(1.4f), tw = TrackedWidth(font, fs, text.c_str(), tr);
    TrackedText(dl, font, fs, ImVec2((a.x + b.x - tw) * 0.5f, mid - fs * 0.5f), C(fg), text.c_str(), tr);
  } else {
    const float tw = TextSize(font, fs, text.c_str()).x;
    Text(dl, font, fs, ImVec2((a.x + b.x - tw) * 0.5f, mid - fs * 0.5f), C(fg), text.c_str());
  }
  PaintPress(dl, ca, cb, Role::Quiet, m);
  return pressed;
}

void NavChrome(ImDrawList* dl, ImVec2 a, ImVec2 b, bool horizontal) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const float t = std::max(1.0f, std::round(LineW()));
  if (st.floatingNav) {
    // Suave: la navegación es una tarjeta, sin bordes.
    dl->AddRectFilled(a, b, C(p.bgAlt));
    return;
  }
  dl->AddRectFilled(a, b, C(p.bgAlt));
  // El borde que da a la página.
  const ImVec2 ea = horizontal ? ImVec2(a.x, b.y - t) : ImVec2(b.x - t, a.y), eb = b;
  switch (st.title) {
  case TitleLook::Index: {
    dl->AddRectFilled(ea, eb, C(p.line));
    // Marcas de HUD a lo largo del borde y un tramo del acento.
    if (horizontal) {
      dl->AddRectFilled(ImVec2(a.x, b.y - t * 2), ImVec2(a.x + Px(120), b.y), C(p.accent));
      for (float x = a.x + Px(140); x < b.x; x += Px(16))
        dl->AddRectFilled(ImVec2(x, b.y - Px(4)), ImVec2(x + t, b.y), C(p.lineStrong));
    } else {
      dl->AddRectFilled(ImVec2(b.x - t * 2, a.y), ImVec2(b.x, a.y + Px(120)), C(p.accent));
    }
    break;
  }
  case TitleLook::Chroma: {
    // Doble línea: la del borde y otra del acento 3 px más allá, tenue.
    dl->AddRectFilled(ea, eb, C(p.lineStrong));
    const float o = std::round(Px(3));
    if (horizontal) dl->AddRectFilled(ImVec2(a.x, b.y - t - o), ImVec2(b.x, b.y - o), C(p.accent, 0.45f));
    else dl->AddRectFilled(ImVec2(b.x - t - o, a.y), ImVec2(b.x - o, b.y), C(p.accent, 0.45f));
    break;
  }
  case TitleLook::Prompt:
    // Terminal: borde de guiones.
    if (horizontal)
      for (float x = a.x; x < b.x; x += Px(8)) dl->AddRectFilled(ImVec2(x, b.y - t), ImVec2(std::min(x + Px(4), b.x), b.y), C(p.lineStrong));
    else
      for (float y = a.y; y < b.y; y += Px(8)) dl->AddRectFilled(ImVec2(b.x - t, y), ImVec2(b.x, std::min(y + Px(4), b.y)), C(p.lineStrong));
    break;
  case TitleLook::Masthead:
    // Papel: doble filete.
    dl->AddRectFilled(ea, eb, C(p.text));
    if (horizontal) dl->AddRectFilled(ImVec2(a.x, b.y - t * 2 - Px(3)), ImVec2(b.x, b.y - t - Px(3)), C(p.text));
    break;
  case TitleLook::Heavy:
    dl->AddRectFilled(ea, eb, C(p.text));
    break;
  default:
    dl->AddRectFilled(ea, eb, C(p.line));
    break;
  }
}

void Monogram(ImDrawList* dl, ImVec2 c, float size) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const ImVec2 a = c - ImVec2(size * 0.5f, size * 0.5f), b = c + ImVec2(size * 0.5f, size * 0.5f);
  ImU32 fg = p.onAccent;
  switch (st.button) {
  case ButtonShape::Outline:
    dl->AddRect(a, b, C(p.accent), 0, 0, LineW());
    fg = p.accent;
    break;
  case ButtonShape::Double:
    DoubleStroke(dl, a, b, C(p.accent), C(p.accent, 0.4f));
    fg = p.accent;
    break;
  case ButtonShape::Bracket:
    DrawBrackets(dl, a, b, C(p.accent), size * 0.3f);
    fg = p.accent;
    break;
  default:
    ShapeFill(dl, a, b, C(p.accent));
    break;
  }
  ImFont* df = Display(size * 0.62f);
  const float fs = size * 0.62f;
  const ImVec2 ts = TextSize(df, fs, "S");
  Text(dl, df, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), C(fg), "S");
}

} // namespace spectre::ui
