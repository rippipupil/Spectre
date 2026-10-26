#include "app/PhotoEdit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spectre {

namespace {

int Rotation(const PhotoSettings& s) { return ((s.rotate % 360) + 360) % 360; }

uint32_t Load(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

void Store(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

uint8_t Byte(float v) { return uint8_t(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// Pinta color (0xRRGGBB) encima con esa cobertura (0..1).
void Blend(uint8_t* p, uint32_t color, float a) {
  if (a <= 0) return;
  a = std::min(a, 1.0f);
  const float r = float((color >> 16) & 0xFF), g = float((color >> 8) & 0xFF), b = float(color & 0xFF);
  p[0] = Byte(p[0] + (b - p[0]) * a);
  p[1] = Byte(p[1] + (g - p[1]) * a);
  p[2] = Byte(p[2] + (r - p[2]) * a);
  p[3] = 255;
}

// Girar y voltear. El píxel (x, y) de la imagen girada sale del original
// igual que en la vista previa del editor de vídeo (primero el volteo, sobre
// la imagen girada).
PhotoImage Orient(const PhotoImage& in, const PhotoSettings& s) {
  const int rot = Rotation(s);
  if (rot == 0 && !s.flipH && !s.flipV) return in;
  const int W = in.width, H = in.height;
  const bool swap = rot == 90 || rot == 270;
  PhotoImage out;
  out.width = swap ? H : W;
  out.height = swap ? W : H;
  out.bgra.resize(size_t(out.width) * size_t(out.height) * 4);
  for (int y = 0; y < out.height; ++y) {
    const int yy = s.flipV ? out.height - 1 - y : y;
    uint8_t* row = out.bgra.data() + size_t(y) * size_t(out.width) * 4;
    for (int x = 0; x < out.width; ++x) {
      const int xx = s.flipH ? out.width - 1 - x : x;
      int sx = xx, sy = yy;
      switch (rot) {
      case 90: sx = yy; sy = H - 1 - xx; break;
      case 180: sx = W - 1 - xx; sy = H - 1 - yy; break;
      case 270: sx = W - 1 - yy; sy = xx; break;
      default: break;
      }
      Store(row + size_t(x) * 4, Load(in.bgra.data() + (size_t(sy) * size_t(W) + size_t(sx)) * 4));
    }
  }
  return out;
}

PhotoImage Cut(const PhotoImage& in, const PhotoRect& r) {
  if (r.x == 0 && r.y == 0 && r.w == in.width && r.h == in.height) return in;
  PhotoImage out;
  out.width = r.w;
  out.height = r.h;
  out.bgra.resize(size_t(r.w) * size_t(r.h) * 4);
  for (int y = 0; y < r.h; ++y)
    memcpy(out.bgra.data() + size_t(y) * size_t(r.w) * 4,
           in.bgra.data() + (size_t(r.y + y) * size_t(in.width) + size_t(r.x)) * 4, size_t(r.w) * 4);
  return out;
}

// ------------------------------------------------------------ color

// Los mismos números que el editor de vídeo (ColorOf en VideoEdit.cpp): brillo
// y contraste en la luma, saturación y tinte en las dos diferencias de color.
struct Tone {
  float contrast = 1, bright = 0, sat = 1, du = 0, dv = 0;
  bool Any() const {
    return std::fabs(contrast - 1) > 1e-3f || std::fabs(bright) > 1e-3f || std::fabs(sat - 1) > 1e-3f ||
           std::fabs(du) > 1e-3f || std::fabs(dv) > 1e-3f;
  }
};

Tone ToneOf(const PhotoSettings& s) {
  Tone t;
  t.contrast = 1.0f + float(std::clamp(s.contrast, -100, 100)) / 200.0f;
  t.bright = float(std::clamp(s.brightness, -100, 100)) * 0.5f;
  t.sat = 1.0f + float(std::clamp(s.saturation, -100, 100)) / 100.0f;
  switch (s.look) {
  case EditLook::Vivid:
    t.sat *= 1.35f;
    t.contrast *= 1.08f;
    break;
  case EditLook::Mono: t.sat = 0; break;
  case EditLook::Sepia:
    t.sat = 0;
    t.du = -15;
    t.dv = 13;
    break;
  case EditLook::Warm:
    t.sat *= 1.05f;
    t.du = -7;
    t.dv = 9;
    break;
  case EditLook::Cold:
    t.du = 9;
    t.dv = -6;
    break;
  case EditLook::Faded:
    t.contrast *= 0.82f;
    t.bright += 12;
    t.sat *= 0.7f;
    break;
  case EditLook::None: break;
  }
  return t;
}

void ApplyTone(PhotoImage& img, const Tone& t) {
  const size_t n = size_t(img.width) * size_t(img.height);
  uint8_t* p = img.bgra.data();
  for (size_t i = 0; i < n; ++i, p += 4) {
    const float b = p[0], g = p[1], r = p[2];
    float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float cb = (b - y) / 1.8556f, cr = (r - y) / 1.5748f;
    y = (y - 128.0f) * t.contrast + 128.0f + t.bright;
    cb = cb * t.sat + t.du;
    cr = cr * t.sat + t.dv;
    const float r2 = y + 1.5748f * cr, b2 = y + 1.8556f * cb;
    const float g2 = (y - 0.2126f * r2 - 0.0722f * b2) / 0.7152f;
    p[0] = Byte(b2);
    p[1] = Byte(g2);
    p[2] = Byte(r2);
  }
}

// Media de una caja de 2·radius+1 en cada eje, n pasadas (casi gaussiano),
// solo dentro de r: los bordes no se mezclan con lo de fuera.
void BoxBlur(PhotoImage& img, const PhotoRect& r, int radius, int passes) {
  if (r.w <= 1 || r.h <= 1 || radius <= 0) return;
  const size_t w = size_t(r.w), h = size_t(r.h);
  std::vector<float> c(w * h * 3);
  for (size_t y = 0; y < h; ++y) {
    const uint8_t* row = img.bgra.data() + ((size_t(r.y) + y) * size_t(img.width) + size_t(r.x)) * 4;
    for (size_t x = 0; x < w; ++x)
      for (int k = 0; k < 3; ++k) c[(y * w + x) * 3 + size_t(k)] = row[x * 4 + size_t(k)];
  }
  std::vector<float> sum(std::max(w, h) + 1);
  auto pass = [&](size_t count, size_t length, size_t lineStep, size_t itemStep) {
    for (size_t line = 0; line < count; ++line)
      for (int k = 0; k < 3; ++k) {
        float* base = c.data() + line * lineStep + size_t(k);
        sum[0] = 0;
        for (size_t i = 0; i < length; ++i) sum[i + 1] = sum[i] + base[i * itemStep];
        for (size_t i = 0; i < length; ++i) {
          const size_t lo = i > size_t(radius) ? i - size_t(radius) : 0;
          const size_t hi = std::min(length - 1, i + size_t(radius));
          base[i * itemStep] = (sum[hi + 1] - sum[lo]) / float(hi - lo + 1);
        }
      }
  };
  for (int i = 0; i < passes; ++i) {
    pass(h, w, w * 3, 3); // filas
    pass(w, h, 3, w * 3); // columnas
  }
  for (size_t y = 0; y < h; ++y) {
    uint8_t* row = img.bgra.data() + ((size_t(r.y) + y) * size_t(img.width) + size_t(r.x)) * 4;
    for (size_t x = 0; x < w; ++x)
      for (int k = 0; k < 3; ++k) row[x * 4 + size_t(k)] = Byte(c[(y * w + x) * 3 + size_t(k)]);
  }
}

void Pixelate(PhotoImage& img, const PhotoRect& r, int block) {
  block = std::max(2, block);
  for (int by = r.y; by < r.y + r.h; by += block)
    for (int bx = r.x; bx < r.x + r.w; bx += block) {
      const int ex = std::min(r.x + r.w, bx + block), ey = std::min(r.y + r.h, by + block);
      unsigned sum[3] = {0, 0, 0};
      for (int y = by; y < ey; ++y)
        for (int x = bx; x < ex; ++x) {
          const uint8_t* p = img.bgra.data() + (size_t(y) * size_t(img.width) + size_t(x)) * 4;
          for (int k = 0; k < 3; ++k) sum[k] += p[k];
        }
      const unsigned n = unsigned((ex - bx) * (ey - by));
      for (int y = by; y < ey; ++y)
        for (int x = bx; x < ex; ++x) {
          uint8_t* p = img.bgra.data() + (size_t(y) * size_t(img.width) + size_t(x)) * 4;
          for (int k = 0; k < 3; ++k) p[k] = uint8_t(sum[k] / n);
        }
    }
}

// Enfoque: la imagen más su diferencia con una versión un poco difuminada.
void Sharpen(PhotoImage& img) {
  PhotoImage soft = img;
  BoxBlur(soft, PhotoRect{0, 0, img.width, img.height}, 1, 1);
  const size_t n = size_t(img.width) * size_t(img.height) * 4;
  for (size_t i = 0; i < n; ++i) {
    if ((i & 3) == 3) continue;
    img.bgra[i] = Byte(float(img.bgra[i]) + 0.8f * (float(img.bgra[i]) - float(soft.bgra[i])));
  }
}

// Esquinas más oscuras, suave desde un tercio del centro.
void Vignette(PhotoImage& img) {
  const float cx = img.width * 0.5f, cy = img.height * 0.5f;
  const float inv = 1.0f / std::sqrt(cx * cx + cy * cy);
  for (int y = 0; y < img.height; ++y) {
    uint8_t* row = img.bgra.data() + size_t(y) * size_t(img.width) * 4;
    const float dy = (float(y) + 0.5f - cy) * inv;
    for (int x = 0; x < img.width; ++x) {
      const float dx = (float(x) + 0.5f - cx) * inv;
      const float d = std::sqrt(dx * dx + dy * dy);
      const float t = std::clamp((d - 0.35f) / 0.65f, 0.0f, 1.0f);
      const float k = 1.0f - 0.55f * t * t * (3 - 2 * t);
      for (int c = 0; c < 3; ++c) row[x * 4 + c] = Byte(row[x * 4 + c] * k);
    }
  }
}

// ------------------------------------------------------------ marcas

PhotoRect MarkRect(const PhotoMark& m, int w, int h) {
  const float ax = std::clamp(std::min(m.x0, m.x1), 0.0f, 1.0f), bx = std::clamp(std::max(m.x0, m.x1), 0.0f, 1.0f);
  const float ay = std::clamp(std::min(m.y0, m.y1), 0.0f, 1.0f), by = std::clamp(std::max(m.y0, m.y1), 0.0f, 1.0f);
  PhotoRect r;
  r.x = std::clamp(int(std::lround(ax * float(w))), 0, w);
  r.y = std::clamp(int(std::lround(ay * float(h))), 0, h);
  r.w = std::clamp(int(std::lround(bx * float(w))), 0, w) - r.x;
  r.h = std::clamp(int(std::lround(by * float(h))), 0, h) - r.y;
  return r;
}

float Stroke(int size, int shortSide) {
  static const float kStroke[] = {0.004f, 0.007f, 0.012f};
  return std::max(2.0f, float(shortSide) * kStroke[std::clamp(size, 0, 2)]);
}

// Recuadro hueco: el grosor va hacia dentro.
void DrawBox(PhotoImage& img, float ax, float ay, float bx, float by, float thick, uint32_t color) {
  if (ax > bx) std::swap(ax, bx);
  if (ay > by) std::swap(ay, by);
  const int x0 = std::max(0, int(std::floor(ax))), x1 = std::min(img.width, int(std::ceil(bx)));
  const int y0 = std::max(0, int(std::floor(ay))), y1 = std::min(img.height, int(std::ceil(by)));
  for (int y = y0; y < y1; ++y) {
    uint8_t* row = img.bgra.data() + size_t(y) * size_t(img.width) * 4;
    const float py = float(y) + 0.5f;
    for (int x = x0; x < x1; ++x) {
      const float px = float(x) + 0.5f;
      const float d = std::min(std::min(px - ax, bx - px), std::min(py - ay, by - py));
      const float cov = std::clamp(d + 0.5f, 0.0f, 1.0f) - std::clamp(d - thick + 0.5f, 0.0f, 1.0f);
      Blend(row + size_t(x) * 4, color, cov);
    }
  }
}

// Flecha de (x0, y0) a la punta (x1, y1): un trazo con el extremo redondeado
// y una punta triangular.
void DrawArrow(PhotoImage& img, float x0, float y0, float x1, float y1, float thick, uint32_t color) {
  const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
  if (len < 1.0f) return;
  const float ux = dx / len, uy = dy / len;
  const float head = std::min(len * 0.6f, std::max(thick * 4.5f, 10.0f)), half = head * 0.55f;
  const float bx = x1 - ux * head, by = y1 - uy * head;
  const float segLen = std::max(0.0f, len - head * 0.7f);
  const float tx[3] = {x1, bx - uy * half, bx + uy * half};
  const float ty[3] = {y1, by + ux * half, by - ux * half};
  // Distancia con signo a cada lado del triángulo (dentro, positiva).
  float ex[3], ey[3], en[3];
  const float orient = (tx[1] - tx[0]) * (ty[2] - ty[0]) - (ty[1] - ty[0]) * (tx[2] - tx[0]) > 0 ? 1.0f : -1.0f;
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    ex[i] = tx[j] - tx[i];
    ey[i] = ty[j] - ty[i];
    en[i] = orient / std::max(1e-3f, std::sqrt(ex[i] * ex[i] + ey[i] * ey[i]));
  }
  const float pad = thick + 2;
  const int xa = std::max(0, int(std::floor(std::min({x0, tx[1], tx[2], x1}) - pad)));
  const int xb = std::min(img.width, int(std::ceil(std::max({x0, tx[1], tx[2], x1}) + pad)));
  const int ya = std::max(0, int(std::floor(std::min({y0, ty[1], ty[2], y1}) - pad)));
  const int yb = std::min(img.height, int(std::ceil(std::max({y0, ty[1], ty[2], y1}) + pad)));
  for (int y = ya; y < yb; ++y) {
    uint8_t* row = img.bgra.data() + size_t(y) * size_t(img.width) * 4;
    const float py = float(y) + 0.5f;
    for (int x = xa; x < xb; ++x) {
      const float px = float(x) + 0.5f;
      const float t = std::clamp((px - x0) * ux + (py - y0) * uy, 0.0f, segLen);
      const float qx = px - (x0 + ux * t), qy = py - (y0 + uy * t);
      float cov = std::clamp(thick * 0.5f - std::sqrt(qx * qx + qy * qy) + 0.5f, 0.0f, 1.0f);
      float inside = 1e9f;
      for (int i = 0; i < 3; ++i)
        inside = std::min(inside, (ex[i] * (py - ty[i]) - ey[i] * (px - tx[i])) * en[i]);
      cov = std::max(cov, std::clamp(inside + 0.5f, 0.0f, 1.0f));
      Blend(row + size_t(x) * 4, color, cov);
    }
  }
}

// El texto blanco del rasterizador se tiñe del color elegido; el borde negro
// y la caja oscura se quedan como están.
bool DrawText(PhotoImage& img, const PhotoMark& m, const TextRasterizer& raster, int shortSide,
              std::array<float, 4>* box) {
  if (!raster || m.text.empty()) return false;
  static const float kSizes[] = {0.04f, 0.06f, 0.085f};
  const int px = std::max(12, int(float(shortSide) * kSizes[std::clamp(m.size, 0, 2)]));
  EditOverlay o;
  if (!raster(m.text, px, std::max(32, int(img.width * 0.9f)), m.box, &o) || o.width <= 0 || o.height <= 0 ||
      o.rgba.size() < size_t(o.width) * size_t(o.height) * 4)
    return false;
  const int w = std::min(o.width, img.width), h = std::min(o.height, img.height);
  const int x = std::clamp(int(std::lround(m.x0 * float(img.width) - float(w) * 0.5f)), 0, img.width - w);
  const int y = std::clamp(int(std::lround(m.y0 * float(img.height) - float(h) * 0.5f)), 0, img.height - h);
  const int x0 = (o.width - w) / 2;
  const float cr = float((m.color >> 16) & 0xFF) / 255.0f, cg = float((m.color >> 8) & 0xFF) / 255.0f,
              cb = float(m.color & 0xFF) / 255.0f;
  for (int j = 0; j < h; ++j) {
    const uint8_t* s = o.rgba.data() + (size_t(j) * size_t(o.width) + size_t(x0)) * 4;
    uint8_t* d = img.bgra.data() + (size_t(y + j) * size_t(img.width) + size_t(x)) * 4;
    for (int i = 0; i < w; ++i, s += 4, d += 4) {
      const float a = float(s[3]) / 255.0f;
      if (a <= 0) continue;
      const float r = s[0] * cr, g = s[1] * cg, b = s[2] * cb;
      d[0] = Byte(d[0] + (b - d[0]) * a);
      d[1] = Byte(d[1] + (g - d[1]) * a);
      d[2] = Byte(d[2] + (r - d[2]) * a);
    }
  }
  if (box)
    *box = {float(x) / float(img.width), float(y) / float(img.height), float(x + w) / float(img.width),
            float(y + h) / float(img.height)};
  return true;
}

void DrawMarks(PhotoImage& img, const PhotoSettings& s, const TextRasterizer& raster, PhotoMarkBoxes* boxes) {
  const int shortSide = std::min(img.width, img.height);
  if (boxes) boxes->assign(s.marks.size(), {0, 0, 0, 0});
  // Primero lo que tapa: una flecha o un texto encima no se difumina.
  for (size_t i = 0; i < s.marks.size(); ++i) {
    const PhotoMark& m = s.marks[i];
    if (m.kind != PhotoMarkKind::Blur && m.kind != PhotoMarkKind::Pixelate) continue;
    const PhotoRect r = MarkRect(m, img.width, img.height);
    if (r.w < 2 || r.h < 2) continue;
    if (m.kind == PhotoMarkKind::Blur) BoxBlur(img, r, std::max(3, int(shortSide * 0.02f)), 3);
    else Pixelate(img, r, std::max(4, int(shortSide * 0.03f)));
  }
  for (size_t i = 0; i < s.marks.size(); ++i) {
    const PhotoMark& m = s.marks[i];
    const float W = float(img.width), H = float(img.height);
    std::array<float, 4> box = {std::min(m.x0, m.x1), std::min(m.y0, m.y1), std::max(m.x0, m.x1),
                                std::max(m.y0, m.y1)};
    switch (m.kind) {
    case PhotoMarkKind::Box:
      DrawBox(img, m.x0 * W, m.y0 * H, m.x1 * W, m.y1 * H, Stroke(m.size, shortSide), m.color);
      break;
    case PhotoMarkKind::Arrow:
      DrawArrow(img, m.x0 * W, m.y0 * H, m.x1 * W, m.y1 * H, Stroke(m.size, shortSide) * 1.4f, m.color);
      break;
    case PhotoMarkKind::Text:
      box = {m.x0, m.y0, m.x0, m.y0};
      DrawText(img, m, raster, shortSide, &box);
      break;
    default: break;
    }
    if (boxes) (*boxes)[i] = box;
  }
}

PhotoImage Frame(const PhotoImage& in, int border, uint32_t color) {
  const int t = std::max(2, int(std::lround(float(std::min(in.width, in.height)) * (border >= 2 ? 0.05f : 0.02f))));
  PhotoImage out;
  out.width = in.width + 2 * t;
  out.height = in.height + 2 * t;
  out.bgra.resize(size_t(out.width) * size_t(out.height) * 4);
  const uint8_t px[4] = {uint8_t(color & 0xFF), uint8_t((color >> 8) & 0xFF), uint8_t((color >> 16) & 0xFF), 255};
  for (size_t i = 0; i < out.bgra.size(); i += 4) memcpy(out.bgra.data() + i, px, 4);
  for (int y = 0; y < in.height; ++y)
    memcpy(out.bgra.data() + (size_t(y + t) * size_t(out.width) + size_t(t)) * 4,
           in.bgra.data() + size_t(y) * size_t(in.width) * 4, size_t(in.width) * 4);
  return out;
}

void SizeStep(const PhotoSettings& s, int w, int h, int* outW, int* outH) {
  double k = 1.0;
  const int longSide = std::max(w, h);
  switch (s.size) {
  case PhotoSize::Large: k = std::min(1.0, 1920.0 / std::max(1, longSide)); break;
  case PhotoSize::Medium: k = std::min(1.0, 1280.0 / std::max(1, longSide)); break;
  case PhotoSize::Half: k = 0.5; break;
  case PhotoSize::Same: break;
  }
  *outW = std::max(1, int(std::lround(w * k)));
  *outH = std::max(1, int(std::lround(h * k)));
}

} // namespace

void PhotoRotatedSize(const PhotoSettings& s, int width, int height, int* rw, int* rh) {
  const int rot = Rotation(s);
  const bool swap = rot == 90 || rot == 270;
  *rw = swap ? height : width;
  *rh = swap ? width : height;
}

PhotoRect PhotoCrop(const PhotoSettings& s, int width, int height) {
  int rw = 0, rh = 0;
  PhotoRotatedSize(s, width, height, &rw, &rh);
  PhotoRect r;
  if (rw <= 0 || rh <= 0) return r;
  r.x = std::clamp(int(std::lround(double(s.cropX) * rw)), 0, rw - 1);
  r.y = std::clamp(int(std::lround(double(s.cropY) * rh)), 0, rh - 1);
  r.w = std::clamp(int(std::lround(double(s.cropW) * rw)), 1, rw - r.x);
  r.h = std::clamp(int(std::lround(double(s.cropH) * rh)), 1, rh - r.y);
  return r;
}

double PhotoAspectRatio(PhotoAspect aspect, int rotatedW, int rotatedH) {
  switch (aspect) {
  case PhotoAspect::Original: return rotatedH > 0 ? double(rotatedW) / rotatedH : 0.0;
  case PhotoAspect::Wide: return 16.0 / 9.0;
  case PhotoAspect::Vertical: return 9.0 / 16.0;
  case PhotoAspect::Square: return 1.0;
  case PhotoAspect::Classic: return 4.0 / 3.0;
  case PhotoAspect::Portrait: return 4.0 / 5.0;
  case PhotoAspect::Free: break;
  }
  return 0.0;
}

void ApplyPhotoAspect(PhotoSettings* s, int width, int height) {
  int rw = 0, rh = 0;
  PhotoRotatedSize(*s, width, height, &rw, &rh);
  const double ratio = PhotoAspectRatio(s->aspect, rw, rh);
  if (ratio <= 0 || rw <= 0 || rh <= 0) return;
  // En píxeles: lo más grande que quepa en la imagen, centrado donde estaba.
  double w = rw, h = w / ratio;
  if (h > rh) {
    h = rh;
    w = h * ratio;
  }
  const double cx = (s->cropX + s->cropW * 0.5) * rw, cy = (s->cropY + s->cropH * 0.5) * rh;
  const double x = std::clamp(cx - w * 0.5, 0.0, rw - w), y = std::clamp(cy - h * 0.5, 0.0, rh - h);
  s->cropX = float(x / rw);
  s->cropY = float(y / rh);
  s->cropW = float(w / rw);
  s->cropH = float(h / rh);
}

void PhotoOutputSize(const PhotoSettings& s, int width, int height, int* outW, int* outH) {
  const PhotoRect r = PhotoCrop(s, width, height);
  int w = 0, h = 0;
  SizeStep(s, r.w, r.h, &w, &h);
  if (s.border > 0) {
    const int t = std::max(2, int(std::lround(float(std::min(w, h)) * (s.border >= 2 ? 0.05f : 0.02f))));
    w += 2 * t;
    h += 2 * t;
  }
  *outW = w;
  *outH = h;
}

bool PhotoTouches(const PhotoSettings& s) {
  PhotoSettings plain = s;
  plain.aspect = PhotoAspect::Free; // elegir la proporción ya cambia el recorte
  plain.borderColor = PhotoSettings{}.borderColor;
  return !(plain == PhotoSettings{});
}

bool RenderPhoto(const PhotoSettings& s, const PhotoImage& source, const TextRasterizer& raster,
                 const PhotoRender& options, PhotoImage* out, PhotoMarkBoxes* boxes) {
  if (source.Empty() || !out) return false;
  PhotoImage img = Orient(source, s);
  if (!options.cropView) img = Cut(img, PhotoCrop(s, source.width, source.height));
  const Tone tone = ToneOf(s);
  if (tone.Any()) ApplyTone(img, tone);
  if (options.cropView) {
    *out = std::move(img);
    return true;
  }
  if (s.sharpen) Sharpen(img);
  if (s.vignette) Vignette(img);
  DrawMarks(img, s, raster, boxes);
  if (options.finalSize) {
    int w = 0, h = 0;
    SizeStep(s, img.width, img.height, &w, &h);
    if (w != img.width || h != img.height) img = ScalePhoto(img, w, h);
  }
  if (s.border > 0) img = Frame(img, s.border, s.borderColor);
  *out = std::move(img);
  return true;
}

PhotoImage ScalePhoto(const PhotoImage& in, int w, int h) {
  PhotoImage out;
  if (in.Empty() || w <= 0 || h <= 0) return out;
  out.width = w;
  out.height = h;
  out.bgra.resize(size_t(w) * size_t(h) * 4);
  if (w > in.width || h > in.height) {
    // Más grande: bilineal.
    for (int y = 0; y < h; ++y) {
      const float sy = std::clamp((float(y) + 0.5f) * float(in.height) / float(h) - 0.5f, 0.0f, float(in.height - 1));
      const int y0 = int(sy), y1 = std::min(in.height - 1, y0 + 1);
      const float fy = sy - float(y0);
      for (int x = 0; x < w; ++x) {
        const float sx = std::clamp((float(x) + 0.5f) * float(in.width) / float(w) - 0.5f, 0.0f, float(in.width - 1));
        const int x0 = int(sx), x1 = std::min(in.width - 1, x0 + 1);
        const float fx = sx - float(x0);
        const uint8_t* a = in.bgra.data() + (size_t(y0) * size_t(in.width) + size_t(x0)) * 4;
        const uint8_t* b = in.bgra.data() + (size_t(y0) * size_t(in.width) + size_t(x1)) * 4;
        const uint8_t* c = in.bgra.data() + (size_t(y1) * size_t(in.width) + size_t(x0)) * 4;
        const uint8_t* d = in.bgra.data() + (size_t(y1) * size_t(in.width) + size_t(x1)) * 4;
        uint8_t* o = out.bgra.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
        for (int k = 0; k < 4; ++k)
          o[k] = Byte((a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy);
      }
    }
    return out;
  }
  // Más pequeña: cada píxel es la media de lo que cubre (por filas y luego por columnas).
  struct Tap {
    int from = 0;
    std::vector<float> weights;
  };
  auto taps = [](int src, int dst) {
    std::vector<Tap> t(static_cast<size_t>(dst));
    const double step = double(src) / dst;
    for (int i = 0; i < dst; ++i) {
      const double a = i * step, b = (i + 1) * step;
      Tap& tap = t[size_t(i)];
      tap.from = int(a);
      for (int j = tap.from; j < src && j < b; ++j) {
        const double lo = std::max(a, double(j)), hi = std::min(b, double(j + 1));
        if (hi > lo) tap.weights.push_back(float((hi - lo) / step));
      }
    }
    return t;
  };
  const auto tx = taps(in.width, w), ty = taps(in.height, h);
  std::vector<float> mid(size_t(w) * size_t(in.height) * 4);
  for (int y = 0; y < in.height; ++y) {
    const uint8_t* row = in.bgra.data() + size_t(y) * size_t(in.width) * 4;
    float* m = mid.data() + size_t(y) * size_t(w) * 4;
    for (int x = 0; x < w; ++x) {
      const Tap& t = tx[size_t(x)];
      float acc[4] = {0, 0, 0, 0};
      for (size_t k = 0; k < t.weights.size(); ++k)
        for (int c = 0; c < 4; ++c) acc[c] += row[(size_t(t.from) + k) * 4 + size_t(c)] * t.weights[k];
      for (int c = 0; c < 4; ++c) m[size_t(x) * 4 + size_t(c)] = acc[c];
    }
  }
  for (int y = 0; y < h; ++y) {
    const Tap& t = ty[size_t(y)];
    uint8_t* o = out.bgra.data() + size_t(y) * size_t(w) * 4;
    for (int x = 0; x < w; ++x) {
      float acc[4] = {0, 0, 0, 0};
      for (size_t k = 0; k < t.weights.size(); ++k) {
        const float* m = mid.data() + ((size_t(t.from) + k) * size_t(w) + size_t(x)) * 4;
        for (int c = 0; c < 4; ++c) acc[c] += m[c] * t.weights[k];
      }
      for (int c = 0; c < 4; ++c) o[size_t(x) * 4 + size_t(c)] = Byte(acc[c]);
    }
  }
  return out;
}

std::vector<uint8_t> PhotoToBgr(const PhotoImage& image) {
  std::vector<uint8_t> bgr(size_t(image.width) * size_t(image.height) * 3);
  const size_t n = size_t(image.width) * size_t(image.height);
  for (size_t i = 0; i < n; ++i) memcpy(bgr.data() + i * 3, image.bgra.data() + i * 4, 3);
  return bgr;
}

} // namespace spectre
