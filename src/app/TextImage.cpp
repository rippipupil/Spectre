#include "app/TextImage.h"

#include "common/Util.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace spectre {

namespace {

// Contexto de GDI con una DIB de 32 bits (de arriba abajo) y una fuente.
struct Canvas {
  HDC dc = nullptr;
  HFONT font = nullptr;
  HGDIOBJ oldFont = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ oldBitmap = nullptr;
  uint32_t* bits = nullptr;
  ~Canvas() {
    if (dc && oldBitmap) SelectObject(dc, oldBitmap);
    if (bitmap) DeleteObject(bitmap);
    if (dc && oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    if (dc) DeleteDC(dc);
  }
};

} // namespace

bool RasterizeText(const std::string& text, int pixelHeight, int maxWidth, bool box, EditOverlay* out) {
  if (text.empty() || pixelHeight < 4 || maxWidth < 16) return false;
  const std::wstring wide = Widen(text);
  Canvas c;
  c.dc = CreateCompatibleDC(nullptr);
  if (!c.dc) return false;
  // Gris suavizado (no ClearType): la cobertura de cada píxel sale igual en
  // los tres canales y sirve como transparencia.
  c.font = CreateFontW(-pixelHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  if (!c.font) return false;
  c.oldFont = SelectObject(c.dc, c.font);

  const UINT format = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL;
  RECT measure{0, 0, maxWidth, 0};
  DrawTextW(c.dc, wide.c_str(), int(wide.size()), &measure, format | DT_CALCRECT);
  const int textW = std::clamp(int(measure.right - measure.left), 1, maxWidth);
  const int textH = std::max(1, int(measure.bottom - measure.top));
  // Borde de ~1/14 de la letra; con caja, un margen alrededor del texto.
  const int outline = box ? 0 : std::max(2, int(std::lround(pixelHeight / 14.0)));
  const int pad = box ? std::max(4, int(std::lround(pixelHeight * 0.35))) : outline + 1;
  const int w = textW + pad * 2, h = textH + pad * 2;
  if (w > 8192 || h > 8192) return false;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; // de arriba abajo
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  c.bitmap = CreateDIBSection(c.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!c.bitmap || !bits) return false;
  c.bits = static_cast<uint32_t*>(bits);
  c.oldBitmap = SelectObject(c.dc, c.bitmap);
  std::fill(c.bits, c.bits + size_t(w) * h, 0u);
  SetBkMode(c.dc, TRANSPARENT);
  SetTextColor(c.dc, RGB(255, 255, 255));
  RECT target{pad, pad, pad + textW, pad + textH};
  DrawTextW(c.dc, wide.c_str(), int(wide.size()), &target, format);
  GdiFlush();

  // Cobertura del texto (0..1) a partir del verde de la DIB.
  std::vector<float> cover(size_t(w) * h);
  for (size_t i = 0; i < cover.size(); ++i) cover[i] = float((c.bits[i] >> 8) & 0xFF) / 255.0f;

  // Fondo: caja oscura semitransparente o el borde (la cobertura dilatada
  // con un disco del radio del borde).
  std::vector<float> under(cover.size(), 0.0f);
  if (box) {
    std::fill(under.begin(), under.end(), 0.6f);
  } else {
    std::vector<std::pair<int, int>> disk;
    for (int dy = -outline; dy <= outline; ++dy)
      for (int dx = -outline; dx <= outline; ++dx)
        if (dx * dx + dy * dy <= outline * outline + outline) disk.emplace_back(dx, dy);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        float m = 0;
        for (const auto& [dx, dy] : disk) {
          const int sx = x + dx, sy = y + dy;
          if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
          m = std::max(m, cover[size_t(sy) * w + sx]);
          if (m >= 1.0f) break;
        }
        under[size_t(y) * w + x] = m;
      }
  }

  // Blanco encima de negro, con transparencia sin premultiplicar.
  out->width = w;
  out->height = h;
  out->rgba.assign(size_t(w) * h * 4, 0);
  for (size_t i = 0; i < cover.size(); ++i) {
    const float t = cover[i], u = under[i];
    const float a = t + u * (1.0f - t);
    if (a <= 0.0f) continue;
    const uint8_t v = uint8_t(std::lround(std::clamp(t / a, 0.0f, 1.0f) * 255.0f));
    uint8_t* p = &out->rgba[i * 4];
    p[0] = p[1] = p[2] = v;
    p[3] = uint8_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
  }
  return true;
}

} // namespace spectre
