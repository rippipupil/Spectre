#include "ui/Fonts.h"

#include "ui/Widgets.h"

#include <windows.h>

#include <misc/freetype/imgui_freetype.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace spectre::ui {

namespace {

// Tamaños de texto de la interfaz en píxeles lógicos (Widgets.h). kBody + 2
// son los títulos de las tarjetas de estado y de la biblioteca.
constexpr float kSizes[FontSet::kCount] = {kLabel, kHint, kSmall, kBody, kBody + 2, kTitle};
constexpr int kSmallIndex = 2; // kSmall
constexpr int kMainIndex = 3;  // kBody
constexpr int kBigIndex = 5;   // kTitle

std::vector<unsigned char> ReadFont(const wchar_t* file) {
  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  std::ifstream in(std::filesystem::path(windir) / L"Fonts" / file, std::ios::binary);
  if (!in) return {};
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ImFont* Add(std::vector<unsigned char>& data, float pixels) {
  if (data.empty()) return nullptr;
  ImFontConfig cfg;
  // Un solo archivo en memoria para todos los tamaños; se libera al construir.
  cfg.FontDataOwnedByAtlas = false;
  // Cada letra empieza en un píxel entero: sin esto los trazos verticales caen
  // entre dos píxeles y se ven grises.
  cfg.PixelSnapH = true;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data.data(), int(data.size()), pixels, &cfg);
}

} // namespace

ImFont* FontSet::Main() const { return regular[kMainIndex]; }
ImFont* FontSet::Bold() const { return bold[kMainIndex]; }
ImFont* FontSet::Big() const { return bold[kBigIndex]; }

ImFont* FontSet::ForSize(ImFont* font, float size) const {
  ImFont* const* family = nullptr;
  for (ImFont* const* f : {regular, bold})
    for (int i = 0; i < kCount; ++i)
      if (f[i] == font) family = f;
  if (!family || !font) return font;
  ImFont* best = font;
  float bestDiff = std::fabs(font->FontSize - size);
  for (int i = 0; i < kCount; ++i) {
    float diff = std::fabs(family[i]->FontSize - size);
    if (diff < bestDiff) {
      best = family[i];
      bestDiff = diff;
    }
  }
  return best;
}

void LoadFonts(float scale, FontSet* out, bool allSizes) {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  atlas->Clear();
  *out = FontSet{};
  out->scale = scale;
  // Hinting ligero: ajusta las letras a la rejilla de píxeles solo en vertical,
  // como ClearType. Nítido y con el mismo ancho que el diseño de la fuente (el
  // hinting completo ensancha el texto un 7 % y descuadraría los paneles).
  atlas->FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
  std::vector<unsigned char> regular = ReadFont(L"segoeui.ttf");
  std::vector<unsigned char> bold = ReadFont(L"segoeuib.ttf");
  for (int i = 0; i < FontSet::kCount; ++i) {
    if (!allSizes && i != kMainIndex && i != kSmallIndex) continue;
    out->regular[i] = Add(regular, kSizes[i] * scale);
    out->bold[i] = Add(bold, kSizes[i] * scale);
  }
  if (!out->regular[kMainIndex]) {
    // Sin Segoe UI (Windows recortado): la fuente de ImGui a todos los tamaños.
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * scale;
    ImFont* fallback = atlas->AddFontDefault(&cfg);
    for (int i = 0; i < FontSet::kCount; ++i) out->regular[i] = fallback;
  }
  // Los tamaños que no se cargaron usan el del texto normal (escalado).
  for (int i = 0; i < FontSet::kCount; ++i) {
    if (!out->regular[i]) out->regular[i] = out->regular[kMainIndex];
    if (!out->bold[i]) out->bold[i] = out->bold[kMainIndex] ? out->bold[kMainIndex] : out->regular[i];
  }
  ImGui::GetIO().FontDefault = out->Main();
  atlas->Build();
  // Ya rasterizadas: los dos TTF (unos 2 MB) no hacen falta.
  atlas->ClearInputData();
}

bool ReleaseFontMemory() {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  // Sin textura (el backend no pudo crearla) la copia hace falta para reintentarlo.
  if (!atlas->TexID) return false;
  atlas->ClearTexData();
  return true;
}

} // namespace spectre::ui
