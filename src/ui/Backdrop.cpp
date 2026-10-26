#include "ui/Backdrop.h"

#include "ui/Widgets.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace spectre::ui {

namespace {

// Números pseudoaleatorios fijos: el dibujo es el mismo en cada frame y al
// cambiar el tamaño de la ventana no baila (todo cuelga de su celda).
uint32_t Hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
uint32_t Hash(int a, int b, uint32_t salt = 0) { return Hash(uint32_t(a) * 73856093U ^ uint32_t(b) * 19349663U ^ salt * 83492791U); }
float Rand(uint32_t h) { return float(h & 0xFFFFFF) / float(0x1000000); }
float Rand(int a, int b, uint32_t salt = 0) { return Rand(Hash(a, b, salt)); }

// Lo que necesita cada dibujo.
struct Scene {
  ImDrawList* dl;
  ImVec2 a, b;
  float s;   // píxeles por píxel lógico
  double t;  // segundos (0 = quieto)
  const Palette& p;
  bool light;
  float k;   // intensidad
  float W() const { return b.x - a.x; }
  float H() const { return b.y - a.y; }
  float Px(float v) const { return v * s; }
  // Color del dibujo: el acento o el texto con muy poca opacidad. En los
  // temas claros un poco más (las líneas finas oscuras se pierden menos).
  ImU32 Accent(float alpha) const { return C(p.accent, std::min(1.0f, alpha * k * (light ? 1.25f : 1.0f))); }
  ImU32 Ink(float alpha) const { return C(p.text, std::min(1.0f, alpha * k * (light ? 1.4f : 1.0f))); }
  ImU32 Danger(float alpha) const { return C(p.danger, std::min(1.0f, alpha * k)); }
  float Time() const { return float(std::fmod(t, 3600.0)); }
};

float Wrap(float v, float m) {
  v = std::fmod(v, m);
  return v < 0 ? v + m : v;
}

void Diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled, float thick) {
  const ImVec2 pts[] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
  if (filled) dl->AddConvexPolyFilled(pts, 4, col);
  else dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, thick);
}

// Disco a franjas horizontales (el sol de Synthwave y de Atardecer): solo la
// parte de encima de `cutY`. En la mitad de abajo los huecos crecen.
void StripedSun(const Scene& sc, ImVec2 c, float r, float cutY, ImU32 col) {
  const float band = sc.Px(8);
  for (float y = c.y - r; y < std::min(c.y + r, cutY); y += band) {
    const float u = (y - (c.y - r)) / (2.0f * r); // 0 arriba, 1 abajo
    const float gap = u < 0.45f ? 0.0f : band * std::min(0.85f, (u - 0.45f) * 1.6f + 0.15f);
    const float y1 = std::min(y + band - gap, cutY);
    if (y1 <= y) continue;
    // Cada franja es un trapecio con el ancho del círculo arriba y abajo: el
    // borde sale redondo, sin escalones.
    const auto half = [&](float yy) { return std::sqrt(std::max(0.0f, r * r - (yy - c.y) * (yy - c.y))); };
    const float h0 = half(y), h1 = half(y1);
    if (std::max(h0, h1) < 1) continue;
    const ImVec2 q[] = {ImVec2(c.x - h0, y), ImVec2(c.x + h0, y), ImVec2(c.x + h1, y1), ImVec2(c.x - h1, y1)};
    sc.dl->AddConvexPolyFilled(q, 4, col);
  }
}

// ------------------------------------------------------------------ dibujos

void Viewfinder(const Scene& sc) {
  // Visor de cámara: esquinas, cruz central, tercios y reglas de medida.
  const float in = sc.Px(22), arm = sc.Px(46), t = std::max(1.0f, sc.Px(1.5f));
  const ImU32 corner = sc.Ink(0.20f), faint = sc.Ink(0.06f), tick = sc.Ink(0.11f);
  const ImVec2 a = sc.a + ImVec2(in, in), b = sc.b - ImVec2(in, in);
  if (b.x - a.x < arm * 3 || b.y - a.y < arm * 3) return;
  DrawBrackets(sc.dl, a, b, corner, arm, t);
  // Tercios, punteados.
  for (int i = 1; i <= 2; ++i) {
    const float x = a.x + (b.x - a.x) * float(i) / 3.0f, y = a.y + (b.y - a.y) * float(i) / 3.0f;
    for (float yy = a.y + arm; yy < b.y - arm; yy += sc.Px(10))
      sc.dl->AddRectFilled(ImVec2(x, yy), ImVec2(x + 1, yy + sc.Px(4)), faint);
    for (float xx = a.x + arm; xx < b.x - arm; xx += sc.Px(10))
      sc.dl->AddRectFilled(ImVec2(xx, y), ImVec2(xx + sc.Px(4), y + 1), faint);
  }
  // Cruz del centro.
  const ImVec2 c = (a + b) * 0.5f;
  const float cr = sc.Px(12);
  sc.dl->AddRectFilled(ImVec2(c.x - cr, c.y), ImVec2(c.x + cr, c.y + t), corner);
  sc.dl->AddRectFilled(ImVec2(c.x, c.y - cr), ImVec2(c.x + t, c.y + cr), corner);
  // Reglas: una marca cada 8 px lógicos y una larga cada 40, arriba y a la izquierda.
  int n = 0;
  for (float x = a.x + arm + sc.Px(16); x < b.x - arm - sc.Px(16); x += sc.Px(8), ++n)
    sc.dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x + 1, a.y + sc.Px(n % 5 == 0 ? 9.0f : 4.0f)), tick);
  n = 0;
  for (float y = a.y + arm + sc.Px(16); y < b.y - arm - sc.Px(16); y += sc.Px(8), ++n)
    sc.dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + sc.Px(n % 5 == 0 ? 9.0f : 4.0f), y + 1), tick);
  // Marca de grabación arriba a la derecha, donde no llegan los títulos ni
  // los botones. En el panel (estrecho) no: caería junto a cerrar.
  if (sc.W() < sc.Px(600)) return;
  const float m = sc.Px(5);
  const ImVec2 rec(b.x - sc.Px(22), a.y + sc.Px(22));
  sc.dl->AddRectFilled(rec - ImVec2(m, m), rec + ImVec2(m, m), sc.Danger(0.35f));
}

void Psychic(const Scene& sc) {
  // Ondas de energía que salen de la esquina de abajo a la derecha y rombos sueltos.
  const ImVec2 o(sc.b.x + sc.Px(40), sc.b.y + sc.Px(40));
  const float step = sc.Px(64), reach = std::hypot(sc.W(), sc.H()) + sc.Px(80);
  const float shift = Wrap(sc.Time() * sc.Px(14), step);
  int i = 0;
  for (float r = sc.Px(70) + shift; r < reach; r += step, ++i) {
    const float fade = std::max(0.0f, 1.0f - (r - sc.Px(70)) / (reach * 0.85f));
    sc.dl->PathArcTo(o, r, kPi, kPi * 1.5f, 48);
    sc.dl->PathStroke(i % 3 == 0 ? sc.Accent(0.20f * fade) : sc.Ink(0.07f * fade), 0, std::max(1.0f, sc.Px(1)));
  }
  const float cell = sc.Px(86);
  const int cols = int(sc.W() / cell) + 1, rows = int(sc.H() / cell) + 1;
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < cols; ++x) {
      const uint32_t h = Hash(x, y, 7);
      if (Rand(h) > 0.32f) continue;
      ImVec2 c(sc.a.x + (float(x) + Rand(h >> 3)) * cell, sc.a.y + (float(y) + Rand(h >> 7)) * cell);
      const float r = sc.Px(3.0f + 6.0f * Rand(h >> 11));
      const bool fill = (h >> 20) % 4 == 0;
      Diamond(sc.dl, c, r, fill ? sc.Accent(0.30f) : sc.Accent(0.16f), fill, std::max(1.0f, sc.Px(1)));
    }
}

void Stars(const Scene& sc, bool constellation) {
  const float cell = sc.Px(constellation ? 70.0f : 54.0f);
  const int cols = int(sc.W() / cell) + 1, rows = int(sc.H() / cell) + 1;
  const float tm = sc.Time();
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < cols; ++x) {
      const uint32_t h = Hash(x, y, 11);
      if (Rand(h) > 0.75f) continue;
      ImVec2 c(sc.a.x + (float(x) + Rand(h >> 2)) * cell, sc.a.y + (float(y) + Rand(h >> 9)) * cell);
      float tw = 0.65f + 0.35f * std::sin(tm * (0.8f + 1.7f * Rand(h >> 5)) + Rand(h >> 13) * 6.28f);
      const float size = Rand(h >> 17) < 0.85f ? std::max(1.0f, sc.Px(1.0f)) : std::max(2.0f, sc.Px(2.0f));
      sc.dl->AddRectFilled(c, c + ImVec2(size, size), sc.Ink((0.18f + 0.30f * Rand(h >> 21)) * tw));
      if ((h >> 26) % 17 == 0) {
        // Destello en cruz.
        const float r = sc.Px(5.0f + 4.0f * Rand(h >> 3)) * tw;
        const ImVec2 m = c + ImVec2(size * 0.5f, size * 0.5f);
        sc.dl->AddLine(ImVec2(m.x - r, m.y), ImVec2(m.x + r, m.y), sc.Accent(0.35f * tw));
        sc.dl->AddLine(ImVec2(m.x, m.y - r), ImVec2(m.x, m.y + r), sc.Accent(0.35f * tw));
      }
    }
  if (!constellation) return;
  // Constelaciones: cadenas de cuatro a seis estrellas unidas por líneas.
  const float zone = sc.Px(260);
  const int zc = int(sc.W() / zone) + 1, zr = int(sc.H() / zone) + 1;
  for (int zy = 0; zy < zr; ++zy)
    for (int zx = 0; zx < zc; ++zx) {
      const uint32_t h = Hash(zx, zy, 23);
      if (Rand(h) > 0.55f) continue;
      ImVec2 p(sc.a.x + (float(zx) + 0.2f + 0.3f * Rand(h >> 3)) * zone, sc.a.y + (float(zy) + 0.2f + 0.3f * Rand(h >> 8)) * zone);
      const int n = 4 + int((h >> 13) % 3);
      for (int i = 0; i < n; ++i) {
        const uint32_t hi = Hash(int(h & 0xFFFF), i, 29);
        ImVec2 q = p + ImVec2(sc.Px(20.0f + 50.0f * Rand(hi)), sc.Px(-35.0f + 70.0f * Rand(hi >> 9)));
        sc.dl->AddLine(p, q, sc.Accent(0.18f), std::max(1.0f, sc.Px(1)));
        Diamond(sc.dl, p, sc.Px(2.5f), sc.Accent(0.45f), true, 1);
        p = q;
      }
      Diamond(sc.dl, p, sc.Px(2.5f), sc.Accent(0.45f), true, 1);
    }
}

void Circuit(const Scene& sc) {
  // Pistas de circuito: horizontal, un tramo a 45 grados y otra vez horizontal,
  // con una almohadilla al final.
  const float cell = sc.Px(56), th = std::max(1.0f, sc.Px(1.2f));
  const int cols = int(sc.W() / cell) + 2, rows = int(sc.H() / cell) + 1;
  for (int y = 0; y < rows; ++y)
    for (int x = -1; x < cols; ++x) {
      const uint32_t h = Hash(x, y, 31);
      if (Rand(h) > 0.42f) continue;
      const bool accent = (h >> 22) % 5 == 0;
      const ImU32 col = accent ? sc.Accent(0.26f) : sc.Ink(0.08f);
      ImVec2 p(sc.a.x + float(x) * cell + cell * 0.5f, sc.a.y + float(y) * cell + cell * 0.5f);
      const float run1 = cell * (0.4f + 0.8f * Rand(h >> 3)), diag = cell * 0.5f * (((h >> 9) & 1) ? 1.0f : -1.0f);
      const float run2 = cell * (0.3f + 0.9f * Rand(h >> 12));
      ImVec2 p1 = p + ImVec2(run1, 0), p2 = p1 + ImVec2(std::fabs(diag), diag), p3 = p2 + ImVec2(run2, 0);
      const ImVec2 pts[] = {p, p1, p2, p3};
      sc.dl->AddPolyline(pts, 4, col, 0, th);
      const float pad = sc.Px(3);
      sc.dl->AddRect(p - ImVec2(pad, pad), p + ImVec2(pad, pad), col, 0, 0, th);
      if ((h >> 17) & 1) sc.dl->AddRectFilled(p3 - ImVec2(pad, pad), p3 + ImVec2(pad, pad), col);
      else sc.dl->AddCircle(p3, pad, col, 8, th);
    }
}

void Hex(const Scene& sc) {
  // Panal que se deshace hacia la izquierda: más celdas cuanto más a la derecha.
  const float r = sc.Px(24), w = r * std::sqrt(3.0f), vh = r * 1.5f;
  const int cols = int(sc.W() / w) + 2, rows = int(sc.H() / vh) + 2;
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < cols; ++x) {
      const float cx = sc.a.x + float(x) * w + (y % 2 ? w * 0.5f : 0.0f), cy = sc.a.y + float(y) * vh;
      const float bias = (cx - sc.a.x) / std::max(1.0f, sc.W());
      const uint32_t h = Hash(x, y, 37);
      if (Rand(h) > bias * bias * 0.95f) continue;
      ImVec2 pts[6];
      for (int i = 0; i < 6; ++i) {
        const float ang = kPi / 6.0f + float(i) * kPi / 3.0f;
        pts[i] = ImVec2(cx + std::cos(ang) * r * 0.92f, cy + std::sin(ang) * r * 0.92f);
      }
      const bool lit = (h >> 20) % 7 == 0;
      if (lit) sc.dl->AddConvexPolyFilled(pts, 6, sc.Accent(0.08f));
      sc.dl->AddPolyline(pts, 6, lit ? sc.Accent(0.30f) : sc.Ink(0.08f), ImDrawFlags_Closed, std::max(1.0f, sc.Px(1)));
    }
}

void Dots(const Scene& sc) {
  const float step = sc.Px(22), size = std::max(1.0f, sc.Px(1.5f));
  int y = 0;
  for (float py = sc.a.y + step * 0.5f; py < sc.b.y; py += step, ++y) {
    int x = 0;
    for (float px = sc.a.x + step * 0.5f; px < sc.b.x; px += step, ++x) {
      const bool accent = Hash(x, y, 41) % 23 == 0;
      sc.dl->AddRectFilled(ImVec2(px, py), ImVec2(px + size, py + size), accent ? sc.Accent(0.55f) : sc.Ink(0.12f));
    }
  }
}

void Hatch(const Scene& sc) {
  // Rayado en diagonal en dos esquinas opuestas, como cristal esmerilado.
  const float zone = std::min(sc.W(), sc.H()) * 0.55f, step = sc.Px(9);
  const ImU32 col = sc.Ink(0.07f);
  sc.dl->PushClipRect(sc.a, sc.b, true);
  for (float d = 0; d < zone; d += step) {
    sc.dl->AddLine(ImVec2(sc.b.x - zone + d, sc.a.y), ImVec2(sc.b.x, sc.a.y + zone - d), col);
    sc.dl->AddLine(ImVec2(sc.a.x, sc.b.y - zone + d), ImVec2(sc.a.x + zone - d, sc.b.y), col);
  }
  sc.dl->PopClipRect();
  // Un borde del acento que corta la esquina.
  sc.dl->AddLine(ImVec2(sc.b.x - zone, sc.a.y), ImVec2(sc.b.x, sc.a.y + zone), sc.Accent(0.22f), std::max(1.0f, sc.Px(1)));
  sc.dl->AddLine(ImVec2(sc.a.x, sc.b.y - zone), ImVec2(sc.a.x + zone, sc.b.y), sc.Accent(0.22f), std::max(1.0f, sc.Px(1)));
}

void Orbits(const Scene& sc) {
  const ImVec2 c(sc.b.x - sc.W() * 0.18f, sc.a.y + sc.H() * 0.30f);
  const float th = std::max(1.0f, sc.Px(1)), tm = sc.Time();
  sc.dl->AddCircle(c, sc.Px(26), sc.Accent(0.30f), 40, th);
  sc.dl->AddCircleFilled(c, sc.Px(8), sc.Accent(0.20f), 20);
  const float tilt = -0.32f;
  for (int i = 0; i < 6; ++i) {
    const float rx = sc.Px(90.0f + 95.0f * float(i)), ry = rx * 0.36f;
    sc.dl->AddEllipse(c, ImVec2(rx, ry), i % 2 ? sc.Ink(0.07f) : sc.Accent(0.14f), tilt, 72, th);
    // El planeta de esa órbita: cuanto más lejos, más despacio.
    const float ang = tm * 0.35f / (1.0f + float(i) * 0.6f) + float(i) * 1.9f;
    const ImVec2 local(std::cos(ang) * rx, std::sin(ang) * ry);
    const ImVec2 pos(c.x + local.x * std::cos(tilt) - local.y * std::sin(tilt), c.y + local.x * std::sin(tilt) + local.y * std::cos(tilt));
    const float pr = sc.Px(3.0f + float((i * 5) % 4));
    sc.dl->AddCircleFilled(pos, pr, sc.Accent(0.45f), 12);
  }
  Stars(sc, false);
}

void Horizon(const Scene& sc) {
  // Synthwave: sol a franjas, horizonte y rejilla en perspectiva que avanza.
  const float hy = sc.a.y + sc.H() * 0.62f;
  const ImVec2 sun((sc.a.x + sc.b.x) * 0.5f + sc.W() * 0.18f, hy - sc.Px(8));
  StripedSun(sc, sun, std::min(sc.Px(110), sc.H() * 0.3f), hy, sc.Accent(0.12f));
  const float th = std::max(1.0f, sc.Px(1));
  sc.dl->AddRectFilled(ImVec2(sc.a.x, hy), ImVec2(sc.b.x, hy + th), sc.Accent(0.20f));
  const float depth = sc.b.y - hy;
  if (depth < sc.Px(20)) return;
  // Horizontales: más juntas cerca del horizonte; avanzan hacia abajo.
  const float phase = Wrap(sc.Time() * 0.35f, 1.0f);
  for (int i = 0; i < 14; ++i) {
    const float u = (float(i) + phase) / 14.0f;
    const float y = hy + depth * u * u;
    sc.dl->AddRectFilled(ImVec2(sc.a.x, y), ImVec2(sc.b.x, y + th), sc.Accent(0.10f + 0.10f * u));
  }
  // Verticales hacia el punto de fuga.
  const ImVec2 vp(sun.x, hy);
  for (int i = -14; i <= 14; ++i) {
    const float x = vp.x + float(i) * sc.Px(70);
    sc.dl->AddLine(ImVec2(vp.x + float(i) * sc.Px(6), hy), ImVec2(x + (x - vp.x) * 1.5f, sc.b.y + sc.Px(4)), sc.Accent(0.11f), th);
  }
}

void Hazard(const Scene& sc) {
  // Bandas de peligro abajo, marcas de HUD y un código de barras.
  const float band = sc.Px(7), stripe = sc.Px(10);
  const float y0 = sc.b.y - band;
  sc.dl->PushClipRect(ImVec2(sc.a.x, y0), ImVec2(sc.b.x, y0 + band), true);
  for (float x = sc.a.x - band; x < sc.b.x + band; x += stripe * 2) {
    const ImVec2 q[] = {ImVec2(x, y0 + band), ImVec2(x + band, y0), ImVec2(x + band + stripe, y0), ImVec2(x + stripe, y0 + band)};
    sc.dl->AddConvexPolyFilled(q, 4, sc.Accent(0.18f));
  }
  sc.dl->PopClipRect();
  // Código de barras arriba a la derecha, donde los títulos no llegan.
  float x = sc.b.x - sc.Px(150);
  for (int i = 0; i < 30; ++i) {
    const uint32_t h = Hash(i, 3, 43);
    const float w = sc.Px(1.0f + float(h % 3));
    sc.dl->AddRectFilled(ImVec2(x, sc.a.y + sc.Px(14)), ImVec2(x + w, sc.a.y + sc.Px(30)), sc.Ink(0.09f));
    x += w + sc.Px(1.0f + float((h >> 4) % 3));
  }
  // Marcas en L de un visor táctico.
  const float arm = sc.Px(18), in = sc.Px(6), th = std::max(1.0f, sc.Px(2));
  DrawBrackets(sc.dl, sc.a + ImVec2(in, in), ImVec2(sc.b.x - in, y0 - sc.Px(6)), sc.Accent(0.22f), arm, th);
  // Ticks numerados a la izquierda.
  for (int i = 0; i < 12; ++i) {
    const float y = sc.a.y + sc.Px(80) + float(i) * sc.Px(26);
    if (y > y0 - sc.Px(60)) break;
    sc.dl->AddRectFilled(ImVec2(sc.a.x + in, y), ImVec2(sc.a.x + in + sc.Px(i % 4 == 0 ? 14.0f : 6.0f), y + th * 0.5f), sc.Ink(0.14f));
  }
}

void Rain(const Scene& sc) {
  // Columnas de "letras" (rejillas de 3x5 puntos) que caen; la cabeza va en el acento.
  const float col = sc.Px(20), cellH = sc.Px(16), dot = std::max(1.0f, sc.Px(2));
  const int cols = int(sc.W() / col) + 1;
  const int rowsOnScreen = int(sc.H() / cellH) + 1;
  const float tm = sc.Time();
  for (int c = 0; c < cols; ++c) {
    const uint32_t h = Hash(c, 0, 47);
    if (Rand(h) > 0.50f) continue;
    const float speed = 3.0f + 6.0f * Rand(h >> 4); // celdas por segundo
    const int len = 6 + int((h >> 9) % 12);
    const int cycle = rowsOnScreen + len + int((h >> 14) % 20);
    const int head = int(Wrap(tm * speed + Rand(h >> 18) * float(cycle), float(cycle)));
    const float x = sc.a.x + float(c) * col + sc.Px(4);
    for (int i = 0; i < len; ++i) {
      const int row = head - i;
      if (row < 0 || row >= rowsOnScreen) continue;
      const float y = sc.a.y + float(row) * cellH;
      // La letra cambia de vez en cuando, como en la película.
      const uint32_t g = Hash(c, row, 53 + uint32_t(tm * 2.0f) * (i == 0 ? 1u : 0u));
      const float fade = 1.0f - float(i) / float(len);
      const ImU32 color = i == 0 ? sc.Accent(0.45f) : sc.Accent(0.05f + 0.15f * fade);
      for (int gy = 0; gy < 5; ++gy)
        for (int gx = 0; gx < 3; ++gx)
          if ((g >> (gy * 3 + gx)) & 1)
            sc.dl->AddRectFilled(ImVec2(x + float(gx) * dot * 1.5f, y + float(gy) * dot * 1.5f),
                                 ImVec2(x + float(gx) * dot * 1.5f + dot, y + float(gy) * dot * 1.5f + dot), color);
    }
  }
}

void Confetti(const Scene& sc) {
  const float cell = sc.Px(64), th = std::max(1.0f, sc.Px(1.5f));
  const int cols = int(sc.W() / cell) + 1, rows = int(sc.H() / cell) + 1;
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < cols; ++x) {
      const uint32_t h = Hash(x, y, 59);
      if (Rand(h) > 0.55f) continue;
      const ImVec2 c(sc.a.x + (float(x) + Rand(h >> 3)) * cell, sc.a.y + (float(y) + Rand(h >> 8)) * cell);
      const float ang = Rand(h >> 12) * kPi, r = sc.Px(4.0f + 3.0f * Rand(h >> 16));
      const ImU32 color = (h >> 20) % 3 == 0 ? sc.Accent(0.35f) : (h >> 20) % 3 == 1 ? sc.Ink(0.14f) : sc.Danger(0.22f);
      switch ((h >> 24) % 3) {
      case 0: sc.dl->AddLine(c - ImVec2(std::cos(ang), std::sin(ang)) * r, c + ImVec2(std::cos(ang), std::sin(ang)) * r, color, th); break;
      case 1: sc.dl->AddRect(c - ImVec2(r, r) * 0.6f, c + ImVec2(r, r) * 0.6f, color, 0, 0, th); break;
      default:
        sc.dl->AddTriangle(c + ImVec2(0, -r), c + ImVec2(r * 0.87f, r * 0.5f), c + ImVec2(-r * 0.87f, r * 0.5f), color, th);
        break;
      }
    }
}

void Diamonds(const Scene& sc) {
  const float step = sc.Px(40), r = sc.Px(5);
  int y = 0;
  for (float py = sc.a.y + step * 0.5f; py < sc.b.y + step; py += step * 0.5f, ++y) {
    int x = 0;
    for (float px = sc.a.x + (y % 2 ? step * 0.5f : 0.0f); px < sc.b.x + step; px += step, ++x) {
      const uint32_t h = Hash(x, y, 61);
      const bool lit = h % 11 == 0;
      Diamond(sc.dl, ImVec2(px, py), lit ? r * 1.3f : r, lit ? sc.Accent(0.40f) : sc.Ink(0.07f), lit, std::max(1.0f, sc.Px(1)));
    }
  }
}

// Una línea quebrada del suelo hacia arriba (montañas, pinos, edificios) se
// rellena con columnas convexas: ImGui solo rellena polígonos convexos.
void FillDown(const Scene& sc, const ImVec2* pts, int n, ImU32 fill) {
  for (int i = 0; i + 1 < n; ++i) {
    const ImVec2 q[] = {pts[i], pts[i + 1], ImVec2(pts[i + 1].x, sc.b.y), ImVec2(pts[i].x, sc.b.y)};
    sc.dl->AddConvexPolyFilled(q, 4, fill);
  }
}

void Mountains(const Scene& sc) {
  const float th = std::max(1.0f, sc.Px(1));
  for (int layer = 0; layer < 3; ++layer) {
    const float base = sc.b.y - sc.H() * (0.16f + 0.10f * float(2 - layer));
    const float peakH = sc.Px(90.0f + 50.0f * float(2 - layer));
    std::vector<ImVec2> pts;
    float x = sc.a.x - sc.Px(40);
    int i = 0;
    while (x < sc.b.x + sc.Px(80)) {
      const uint32_t h = Hash(i, layer, 67);
      const bool peak = i % 2 == 0;
      const float y = peak ? base - peakH * (0.45f + 0.55f * Rand(h)) : base - peakH * 0.15f * Rand(h >> 5);
      pts.emplace_back(x, y);
      x += sc.Px(70.0f + 80.0f * Rand(h >> 9));
      ++i;
    }
    FillDown(sc, pts.data(), int(pts.size()), C(Mix(sc.p.bg, sc.p.text, (0.02f + 0.015f * float(layer)) * sc.k)));
    sc.dl->AddPolyline(pts.data(), int(pts.size()), layer == 2 ? sc.Accent(0.22f) : sc.Ink(0.10f), 0, th);
    // Nieve en las cumbres de la capa de delante.
    if (layer == 2)
      for (size_t j = 1; j + 1 < pts.size(); ++j)
        if (pts[j].y < pts[j - 1].y && pts[j].y < pts[j + 1].y) {
          const ImVec2 l = pts[j] + (pts[j - 1] - pts[j]) * 0.22f, r = pts[j] + (pts[j + 1] - pts[j]) * 0.22f;
          const ImVec2 cap[] = {pts[j], r, ImVec2((l.x + r.x) * 0.5f, std::max(l.y, r.y) - sc.Px(2)), l};
          sc.dl->AddConvexPolyFilled(cap, 4, sc.Ink(0.20f));
        }
  }
}

void Skyline(const Scene& sc) {
  const float th = std::max(1.0f, sc.Px(1));
  for (int layer = 0; layer < 2; ++layer) {
    float x = sc.a.x - sc.Px(10) + float(layer) * sc.Px(23);
    int i = 0;
    const ImU32 fill = C(Mix(sc.p.bg, sc.p.text, (0.025f + 0.02f * float(layer)) * sc.k));
    while (x < sc.b.x) {
      const uint32_t h = Hash(i, layer, 71);
      const float w = sc.Px(34.0f + 50.0f * Rand(h));
      const float hgt = sc.Px((layer == 0 ? 140.0f : 70.0f) + (layer == 0 ? 170.0f : 120.0f) * Rand(h >> 6));
      const ImVec2 ta(x, sc.b.y - hgt), tb(x + w, sc.b.y);
      sc.dl->AddRectFilled(ta, tb, fill);
      sc.dl->AddLine(ta, ImVec2(tb.x, ta.y), layer == 1 ? sc.Ink(0.14f) : sc.Ink(0.07f), th);
      if ((h >> 12) % 4 == 0) // antena
        sc.dl->AddLine(ImVec2(x + w * 0.5f, ta.y), ImVec2(x + w * 0.5f, ta.y - sc.Px(18)), sc.Ink(0.12f), th);
      if (layer == 1) {
        // Ventanas encendidas.
        const float wx = sc.Px(4), wy = sc.Px(6), gx = sc.Px(9), gy = sc.Px(12);
        int r = 0;
        for (float y = ta.y + sc.Px(10); y < sc.b.y - sc.Px(14); y += gy, ++r) {
          int c = 0;
          for (float xx = x + sc.Px(7); xx < tb.x - sc.Px(7); xx += gx, ++c)
            if (Hash(i * 64 + c, r, 73) % 5 == 0) sc.dl->AddRectFilled(ImVec2(xx, y), ImVec2(xx + wx, y + wy), sc.Accent(0.35f));
        }
      }
      x += w + sc.Px(2.0f + 10.0f * Rand(h >> 15));
      ++i;
    }
  }
}

void Topo(const Scene& sc) {
  // Curvas de nivel alrededor de tres cimas.
  const float th = std::max(1.0f, sc.Px(1));
  const ImVec2 centers[] = {ImVec2(sc.a.x + sc.W() * 0.82f, sc.a.y + sc.H() * 0.22f),
                            ImVec2(sc.a.x + sc.W() * 0.18f, sc.a.y + sc.H() * 0.86f),
                            ImVec2(sc.a.x + sc.W() * 0.62f, sc.a.y + sc.H() * 1.05f)};
  ImVec2 pts[72];
  for (int c = 0; c < 3; ++c) {
    const uint32_t h = Hash(c, 0, 79);
    for (int ring = 0; ring < 9; ++ring) {
      const float r0 = sc.Px(30.0f + 34.0f * float(ring)) * (c == 2 ? 1.4f : 1.0f);
      for (int i = 0; i < 72; ++i) {
        const float ang = float(i) / 72.0f * kPi * 2.0f;
        const float wob = std::sin(ang * 3.0f + Rand(h) * 6.0f + float(ring) * 0.3f) * 0.12f +
                          std::sin(ang * 5.0f + Rand(h >> 7) * 6.0f) * 0.06f;
        pts[i] = centers[c] + ImVec2(std::cos(ang), std::sin(ang) * 0.8f) * (r0 * (1.0f + wob));
      }
      sc.dl->AddPolyline(pts, 72, ring % 4 == 3 ? sc.Accent(0.22f) : sc.Ink(0.07f), ImDrawFlags_Closed, th);
    }
  }
}

void Scanlines(const Scene& sc) {
  const float step = std::max(3.0f, sc.Px(3));
  const ImU32 col = sc.light ? sc.Ink(0.05f) : C(IM_COL32(0, 0, 0, 255), std::min(1.0f, 0.22f * sc.k));
  for (float y = sc.a.y; y < sc.b.y; y += step) sc.dl->AddRectFilled(ImVec2(sc.a.x, y), ImVec2(sc.b.x, y + 1), col);
  // Marco del tubo y una línea de refresco quieta.
  const float in = sc.Px(14);
  sc.dl->AddRect(sc.a + ImVec2(in, in), sc.b - ImVec2(in, in), sc.Ink(0.07f), 0, 0, std::max(1.0f, sc.Px(1)));
  const float ry = sc.a.y + sc.H() * 0.34f;
  sc.dl->AddRectFilled(ImVec2(sc.a.x, ry), ImVec2(sc.b.x, ry + sc.Px(2)), sc.Accent(0.08f));
}

void Waves(const Scene& sc) {
  const float th = std::max(1.0f, sc.Px(1.2f)), tm = sc.Time();
  const float top = sc.a.y + sc.H() * 0.55f;
  ImVec2 pts[160];
  for (int i = 0; i < 9; ++i) {
    const float y = top + (sc.b.y - top) * float(i) / 9.0f;
    const float amp = sc.Px(5.0f + 1.4f * float(i)), len = sc.Px(220.0f + 30.0f * float(i));
    const float ph = tm * (0.5f + 0.12f * float(i)) * (i % 2 ? 1.0f : -1.0f) + float(i);
    int n = 0;
    for (float x = sc.a.x; n < 160; x += std::max(sc.Px(8), sc.W() / 150.0f)) {
      pts[n++] = ImVec2(x, y + std::sin((x - sc.a.x) / len * kPi * 2.0f + ph) * amp);
      if (x > sc.b.x) break;
    }
    sc.dl->AddPolyline(pts, n, i % 3 == 0 ? sc.Accent(0.24f) : sc.Ink(0.08f), 0, th);
  }
}

void Pines(const Scene& sc) {
  for (int layer = 0; layer < 2; ++layer) {
    float x = sc.a.x - sc.Px(20) + float(layer) * sc.Px(17);
    int i = 0;
    const ImU32 fill = layer == 0 ? C(Mix(sc.p.bg, sc.p.accent, 0.05f * sc.k)) : C(Mix(sc.p.bg, sc.p.accent, 0.10f * sc.k));
    while (x < sc.b.x + sc.Px(30)) {
      const uint32_t h = Hash(i, layer, 83);
      const float hgt = sc.Px((layer == 0 ? 150.0f : 80.0f) + 110.0f * Rand(h));
      const float w = hgt * 0.42f;
      const float base = sc.b.y - sc.Px(layer == 0 ? 26.0f : 0.0f);
      // Tres triángulos apilados y el tronco.
      for (int t = 0; t < 3; ++t) {
        const float tt = float(t) / 3.0f;
        const float top = base - hgt + hgt * tt * 0.55f, bot = base - hgt * 0.18f - hgt * (1.0f - tt) * 0.25f;
        const float half = w * (0.5f + 0.25f * tt);
        sc.dl->AddTriangleFilled(ImVec2(x, top), ImVec2(x + half, bot), ImVec2(x - half, bot), fill);
      }
      sc.dl->AddRectFilled(ImVec2(x - sc.Px(2), base - hgt * 0.2f), ImVec2(x + sc.Px(2), base), fill);
      x += w * (0.55f + 0.5f * Rand(h >> 7));
      ++i;
    }
  }
  // Luciérnagas.
  for (int i = 0; i < 26; ++i) {
    const uint32_t h = Hash(i, 0, 89);
    const ImVec2 c(sc.a.x + Rand(h) * sc.W(), sc.a.y + sc.H() * (0.35f + 0.5f * Rand(h >> 8)));
    sc.dl->AddRectFilled(c, c + ImVec2(std::max(1.0f, sc.Px(2)), std::max(1.0f, sc.Px(2))), sc.Accent(0.45f));
  }
}

void Sunset(const Scene& sc) {
  const float hy = sc.a.y + sc.H() * 0.72f;
  const ImVec2 sun(sc.a.x + sc.W() * 0.72f, hy + sc.Px(20));
  const float r = std::min(sc.Px(140), sc.H() * 0.32f);
  StripedSun(sc, sun, r, hy, sc.Accent(0.17f));
  const float th = std::max(1.0f, sc.Px(1));
  sc.dl->AddRectFilled(ImVec2(sc.a.x, hy), ImVec2(sc.b.x, hy + th), sc.Accent(0.28f));
  // Reflejo en el agua: rayas cortas cada vez más cortas.
  for (int i = 1; i < 9; ++i) {
    const float y = hy + sc.Px(9) * float(i);
    if (y > sc.b.y) break;
    const float half = r * (1.0f - float(i) * 0.09f) * (0.6f + 0.4f * Rand(i, 0, 97));
    sc.dl->AddRectFilled(ImVec2(sun.x - half, y), ImVec2(sun.x + half, y + th), sc.Accent(0.20f - 0.018f * float(i)));
  }
  // Pájaros: dos trazos.
  for (int i = 0; i < 5; ++i) {
    const uint32_t h = Hash(i, 1, 101);
    const ImVec2 c(sun.x - r * 1.6f + Rand(h) * r * 1.4f, hy - r * (1.2f + 0.6f * Rand(h >> 8)));
    const float w = sc.Px(5.0f + 3.0f * Rand(h >> 16));
    sc.dl->AddLine(c - ImVec2(w, w * 0.4f), c, sc.Ink(0.20f), th);
    sc.dl->AddLine(c, c + ImVec2(w, -w * 0.4f), sc.Ink(0.20f), th);
  }
}

void Moon(const Scene& sc) {
  const float r = std::min(sc.H() * 0.30f, sc.Px(170));
  const ImVec2 c(sc.b.x - r * 0.75f, sc.a.y + r * 0.95f);
  const float th = std::max(1.0f, sc.Px(1));
  sc.dl->AddCircleFilled(c, r, sc.Accent(0.06f), 96);
  sc.dl->AddCircle(c, r, sc.Accent(0.30f), 96, th);
  sc.dl->AddCircle(c, r * 1.18f, sc.Accent(0.10f), 96, th);
  sc.dl->AddCircle(c, r * 1.42f, sc.Ink(0.05f), 96, th);
  for (int i = 0; i < 9; ++i) {
    const uint32_t h = Hash(i, 0, 103);
    const float ang = Rand(h) * kPi * 2.0f, d = r * 0.75f * std::sqrt(Rand(h >> 8));
    const float cr = r * (0.05f + 0.10f * Rand(h >> 16));
    sc.dl->AddCircle(c + ImVec2(std::cos(ang), std::sin(ang)) * d, cr, sc.Accent(0.20f), 24, th);
  }
}

void Deco(const Scene& sc) {
  // Abanicos art déco desde las dos esquinas de abajo y esquinas escalonadas arriba.
  const float th = std::max(1.0f, sc.Px(1));
  const float R = std::min(sc.W(), sc.H()) * 0.42f;
  for (int side = 0; side < 2; ++side) {
    const ImVec2 o(side == 0 ? sc.a.x : sc.b.x, sc.b.y);
    const float a0 = side == 0 ? -kPi * 0.5f : kPi, a1 = side == 0 ? 0.0f : kPi * 1.5f;
    for (int ring = 1; ring <= 4; ++ring) {
      sc.dl->PathArcTo(o, R * float(ring) / 4.0f, a0, a1, 32);
      sc.dl->PathStroke(ring == 4 ? sc.Accent(0.26f) : sc.Accent(0.12f), 0, th);
    }
    for (int i = 1; i < 12; ++i) {
      const float ang = a0 + (a1 - a0) * float(i) / 12.0f;
      sc.dl->AddLine(o + ImVec2(std::cos(ang), std::sin(ang)) * (R * 0.25f), o + ImVec2(std::cos(ang), std::sin(ang)) * R,
                     sc.Accent(0.10f), th);
    }
  }
  const float in = sc.Px(16), st = sc.Px(10);
  for (int i = 0; i < 3; ++i) {
    const float d = in + st * float(i);
    // Esquinas de arriba: escalones.
    sc.dl->AddLine(ImVec2(sc.a.x + d, sc.a.y + in + st * 3), ImVec2(sc.a.x + d, sc.a.y + d), sc.Accent(0.20f), th);
    sc.dl->AddLine(ImVec2(sc.a.x + d, sc.a.y + d), ImVec2(sc.a.x + in + st * 3, sc.a.y + d), sc.Accent(0.20f), th);
    sc.dl->AddLine(ImVec2(sc.b.x - d, sc.a.y + in + st * 3), ImVec2(sc.b.x - d, sc.a.y + d), sc.Accent(0.20f), th);
    sc.dl->AddLine(ImVec2(sc.b.x - d, sc.a.y + d), ImVec2(sc.b.x - in - st * 3, sc.a.y + d), sc.Accent(0.20f), th);
  }
}

void Bubbles(const Scene& sc) {
  const float th = std::max(1.0f, sc.Px(1)), tm = sc.Time();
  const int count = std::max(8, int(sc.W() * sc.H() / (sc.Px(120) * sc.Px(120))));
  const float span = sc.H() + sc.Px(60);
  for (int i = 0; i < count; ++i) {
    const uint32_t h = Hash(i, 0, 107);
    const float r = sc.Px(3.0f + 14.0f * Rand(h) * Rand(h >> 5));
    const float speed = sc.Px(10.0f + 22.0f * Rand(h >> 10));
    const float y = sc.b.y + sc.Px(30) - Wrap(tm * speed + Rand(h >> 15) * span, span);
    const float x = sc.a.x + Rand(h >> 20) * sc.W() + std::sin(tm * 0.9f + float(i)) * sc.Px(5);
    sc.dl->AddCircle(ImVec2(x, y), r, (h >> 28) % 3 == 0 ? sc.Accent(0.35f) : sc.Accent(0.16f), 20, th);
    if (r > sc.Px(8)) sc.dl->AddLine(ImVec2(x - r * 0.45f, y - r * 0.25f), ImVec2(x - r * 0.25f, y - r * 0.5f), sc.Ink(0.18f), th);
  }
}

void Grid(const Scene& sc) {
  const float minor = sc.Px(16);
  int i = 0;
  for (float x = sc.a.x; x < sc.b.x; x += minor, ++i)
    sc.dl->AddRectFilled(ImVec2(x, sc.a.y), ImVec2(x + 1, sc.b.y), i % 5 == 0 ? sc.Accent(0.10f) : sc.Ink(0.035f));
  i = 0;
  for (float y = sc.a.y; y < sc.b.y; y += minor, ++i)
    sc.dl->AddRectFilled(ImVec2(sc.a.x, y), ImVec2(sc.b.x, y + 1), i % 5 == 0 ? sc.Accent(0.10f) : sc.Ink(0.035f));
  // Una cota de plano abajo a la derecha.
  const float th = std::max(1.0f, sc.Px(1));
  const ImVec2 c(sc.b.x - sc.Px(120), sc.b.y - sc.Px(60));
  sc.dl->AddRect(c, c + ImVec2(sc.Px(96), sc.Px(40)), sc.Accent(0.25f), 0, 0, th);
  sc.dl->AddLine(c + ImVec2(0, sc.Px(20)), c + ImVec2(sc.Px(96), sc.Px(20)), sc.Accent(0.25f), th);
  sc.dl->AddLine(c + ImVec2(sc.Px(48), 0), c + ImVec2(sc.Px(48), sc.Px(20)), sc.Accent(0.25f), th);
}

void Plaid(const Scene& sc) {
  const float group = sc.Px(72), gap = sc.Px(4);
  for (int pass = 0; pass < 2; ++pass) {
    int i = 0;
    const float from = pass == 0 ? sc.a.x : sc.a.y, to = pass == 0 ? sc.b.x : sc.b.y;
    for (float v = from + sc.Px(20); v < to; v += group, ++i) {
      for (int l = 0; l < 3; ++l) {
        const float w = l == 1 ? sc.Px(3) : 1.0f;
        const ImU32 col = l == 1 ? (i % 2 ? sc.Danger(0.08f) : sc.Accent(0.09f)) : sc.Ink(0.05f);
        const float q = v + float(l) * (gap + sc.Px(1));
        if (pass == 0) sc.dl->AddRectFilled(ImVec2(q, sc.a.y), ImVec2(q + w, sc.b.y), col);
        else sc.dl->AddRectFilled(ImVec2(sc.a.x, q), ImVec2(sc.b.x, q + w), col);
      }
    }
  }
}

void Petals(const Scene& sc) {
  const float tm = sc.Time();
  const int count = std::max(10, int(sc.W() * sc.H() / (sc.Px(110) * sc.Px(110))));
  const float span = sc.H() + sc.Px(40);
  for (int i = 0; i < count; ++i) {
    const uint32_t h = Hash(i, 0, 109);
    const float speed = sc.Px(14.0f + 18.0f * Rand(h >> 4));
    const float y = sc.a.y - sc.Px(20) + Wrap(tm * speed + Rand(h >> 9) * span, span);
    const float x = sc.a.x + Rand(h >> 14) * sc.W() + std::sin(tm * 0.7f + float(i) * 1.3f) * sc.Px(18);
    const float rot = tm * (0.6f + Rand(h >> 19)) + float(i);
    const float r = sc.Px(3.5f + 3.0f * Rand(h >> 23));
    sc.dl->AddEllipseFilled(ImVec2(x, y), ImVec2(r, r * 0.55f), (h >> 27) % 3 == 0 ? sc.Accent(0.45f) : sc.Accent(0.24f), rot, 12);
  }
  // Una rama en la esquina de arriba a la derecha.
  const float th = std::max(1.0f, sc.Px(2));
  ImVec2 p(sc.b.x + sc.Px(4), sc.a.y + sc.Px(30));
  for (int i = 0; i < 6; ++i) {
    const ImVec2 q = p + ImVec2(-sc.Px(40.0f + 10.0f * float(i % 2)), sc.Px(i % 2 ? 14.0f : -6.0f));
    sc.dl->AddLine(p, q, sc.Ink(0.12f), th);
    const uint32_t h = Hash(i, 2, 113);
    for (int f = 0; f < 3; ++f)
      sc.dl->AddCircleFilled(q + ImVec2(sc.Px(-6.0f + 12.0f * Rand(h >> (f * 5))), sc.Px(-6.0f + 12.0f * Rand(h >> (f * 5 + 2)))),
                             sc.Px(3.5f), sc.Accent(0.32f), 10);
    p = q;
  }
}

void Ruled(const Scene& sc) {
  const float step = sc.Px(28);
  for (float y = sc.a.y + sc.Px(64); y < sc.b.y; y += step)
    sc.dl->AddRectFilled(ImVec2(sc.a.x, y), ImVec2(sc.b.x, y + 1), sc.Accent(0.10f));
  const float mx = sc.a.x + sc.Px(18);
  sc.dl->AddRectFilled(ImVec2(mx, sc.a.y), ImVec2(mx + 1, sc.b.y), sc.Danger(0.30f));
  sc.dl->AddRectFilled(ImVec2(mx + sc.Px(3), sc.a.y), ImVec2(mx + sc.Px(3) + 1, sc.b.y), sc.Danger(0.18f));
}

void Webs(const Scene& sc) {
  const float th = std::max(1.0f, sc.Px(1));
  for (int side = 0; side < 2; ++side) {
    const ImVec2 o(side == 0 ? sc.a.x : sc.b.x, sc.a.y);
    const float R = std::min(sc.Px(230), sc.H() * 0.5f);
    const int spokes = 7;
    ImVec2 dirs[spokes];
    for (int i = 0; i < spokes; ++i) {
      const float ang = kPi * 0.5f * float(i) / float(spokes - 1);
      dirs[i] = side == 0 ? ImVec2(std::cos(ang), std::sin(ang)) : ImVec2(-std::cos(ang), std::sin(ang));
      sc.dl->AddLine(o, o + dirs[i] * R, sc.Ink(0.12f), th);
    }
    for (int ring = 1; ring <= 6; ++ring) {
      const float rr = R * float(ring) / 6.5f;
      for (int i = 0; i + 1 < spokes; ++i) {
        const ImVec2 p0 = o + dirs[i] * rr, p1 = o + dirs[i + 1] * rr;
        // Cada hilo cuelga un poco hacia la esquina.
        const ImVec2 mid = (p0 + p1) * 0.5f, sag = (o - mid) * 0.12f;
        sc.dl->AddBezierQuadratic(p0, mid + sag, p1, ring == 4 ? sc.Accent(0.24f) : sc.Ink(0.10f), th, 8);
      }
    }
  }
  // Una araña colgada de su hilo.
  const float x = sc.a.x + sc.W() * 0.33f, y = sc.a.y + sc.H() * 0.26f;
  sc.dl->AddLine(ImVec2(x, sc.a.y), ImVec2(x, y), sc.Ink(0.14f), th);
  sc.dl->AddCircleFilled(ImVec2(x, y + sc.Px(5)), sc.Px(5), sc.Accent(0.40f), 12);
  for (int i = 0; i < 4; ++i) {
    const float dy = y + sc.Px(2.0f + 2.5f * float(i));
    sc.dl->AddLine(ImVec2(x - sc.Px(4), dy), ImVec2(x - sc.Px(11), dy + sc.Px(float(i) * 2.0f - 3.0f)), sc.Accent(0.40f), th);
    sc.dl->AddLine(ImVec2(x + sc.Px(4), dy), ImVec2(x + sc.Px(11), dy + sc.Px(float(i) * 2.0f - 3.0f)), sc.Accent(0.40f), th);
  }
}

void Snow(const Scene& sc) {
  const float tm = sc.Time(), th = std::max(1.0f, sc.Px(1));
  const int count = std::max(14, int(sc.W() * sc.H() / (sc.Px(70) * sc.Px(70))));
  const float span = sc.H() + sc.Px(20);
  for (int i = 0; i < count; ++i) {
    const uint32_t h = Hash(i, 0, 127);
    const float speed = sc.Px(8.0f + 22.0f * Rand(h >> 3));
    const float y = sc.a.y - sc.Px(10) + Wrap(tm * speed + Rand(h >> 8) * span, span);
    const float x = sc.a.x + Rand(h >> 13) * sc.W() + std::sin(tm * 0.8f + float(i)) * sc.Px(6);
    if ((h >> 26) % 9 == 0) {
      // Copo: tres trazos cruzados.
      const float r = sc.Px(4.0f + 2.0f * Rand(h >> 18));
      for (int k = 0; k < 3; ++k) {
        const float ang = float(k) * kPi / 3.0f + tm * 0.3f;
        const ImVec2 d(std::cos(ang) * r, std::sin(ang) * r);
        sc.dl->AddLine(ImVec2(x, y) - d, ImVec2(x, y) + d, sc.Ink(0.35f), th);
      }
    } else {
      const float r = std::max(1.0f, sc.Px(1.0f + 1.5f * Rand(h >> 18)));
      sc.dl->AddRectFilled(ImVec2(x - r * 0.5f, y - r * 0.5f), ImVec2(x + r * 0.5f, y + r * 0.5f), sc.Ink(0.25f + 0.2f * Rand(h >> 22)));
    }
  }
  // Nieve acumulada abajo: una loma suave hecha de rectángulos.
  for (float x = sc.a.x; x < sc.b.x; x += sc.Px(6)) {
    const float hgt = sc.Px(6) + std::sin((x - sc.a.x) / sc.Px(160)) * sc.Px(4) + sc.Px(3);
    sc.dl->AddRectFilled(ImVec2(x, sc.b.y - hgt), ImVec2(x + sc.Px(6), sc.b.y), sc.Ink(0.06f));
  }
}

} // namespace

void DrawMotif(ImDrawList* dl, Motif motif, const Palette& palette, bool light, ImVec2 a, ImVec2 b, float scale, double time,
               float strength) {
  if (motif == Motif::None || b.x - a.x < 8 || b.y - a.y < 8 || strength <= 0) return;
  Scene sc{dl, a, b, scale, time, palette, light, strength};
  dl->PushClipRect(a, b, true);
  switch (motif) {
  case Motif::None: break;
  case Motif::Viewfinder: Viewfinder(sc); break;
  case Motif::Psychic: Psychic(sc); break;
  case Motif::Stars: Stars(sc, false); break;
  case Motif::Circuit: Circuit(sc); break;
  case Motif::Hex: Hex(sc); break;
  case Motif::Dots: Dots(sc); break;
  case Motif::Hatch: Hatch(sc); break;
  case Motif::Constellation: Stars(sc, true); break;
  case Motif::Orbits: Orbits(sc); break;
  case Motif::Horizon: Horizon(sc); break;
  case Motif::Hazard: Hazard(sc); break;
  case Motif::Rain: Rain(sc); break;
  case Motif::Confetti: Confetti(sc); break;
  case Motif::Diamonds: Diamonds(sc); break;
  case Motif::Mountains: Mountains(sc); break;
  case Motif::Skyline: Skyline(sc); break;
  case Motif::Topo: Topo(sc); break;
  case Motif::Scanlines: Scanlines(sc); break;
  case Motif::Waves: Waves(sc); break;
  case Motif::Pines: Pines(sc); break;
  case Motif::Sunset: Sunset(sc); break;
  case Motif::Moon: Moon(sc); break;
  case Motif::Deco: Deco(sc); break;
  case Motif::Bubbles: Bubbles(sc); break;
  case Motif::Grid: Grid(sc); break;
  case Motif::Plaid: Plaid(sc); break;
  case Motif::Petals: Petals(sc); break;
  case Motif::Ruled: Ruled(sc); break;
  case Motif::Webs: Webs(sc); break;
  case Motif::Snow: Snow(sc); break;
  }
  dl->PopClipRect();
}

void DrawBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b, float strength) {
  if (!BackdropVisible()) return;
  const Theme& t = CurrentTheme();
  // Quieto: el dibujo de un instante fijo (la lluvia o la nieve, paradas).
  const double time = BackdropMoves() ? double(GetTickCount64() % 3600000ull) / 1000.0 : 12.5;
  DrawMotif(dl, t.motif, t.colors, t.light, a, b, Px(1.0f), time, strength);
}

} // namespace spectre::ui
