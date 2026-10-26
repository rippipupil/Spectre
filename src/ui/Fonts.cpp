#include "ui/Fonts.h"

#include "ui/Widgets.h"

#include <windows.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <misc/freetype/imgui_freetype.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace spectre::ui {

namespace {

// Tamaños de texto de la interfaz en píxeles lógicos (Widgets.h). kBody + 2
// son los títulos de las tarjetas de estado y de la biblioteca.
constexpr float kSizes[FontSet::kCount] = {kLabel, kHint, kSmall, kBody, kBody + 2, kTitle};
constexpr float kDisplaySizes[FontSet::kDisplayCount] = {kBody + 2, kTitle, kHeadline, kHero};
constexpr int kSmallIndex = 2; // kSmall
constexpr int kMainIndex = 3;  // kBody

// Lo que pidió el último frame dibujado (BeginFrame) y aún no ha mirado su ventana.
bool requested_ = false;
Face wantBody_ = Face::Segoe, wantDisplay_ = Face::Segoe;

std::vector<unsigned char> ReadFont(const wchar_t* file) {
  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  std::ifstream in(std::filesystem::path(windir) / L"Fonts" / file, std::ios::binary);
  if (!in) return {};
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Una fuente variable (Bahnschrift) trae sus pesos y anchos como instancias
// con nombre: FreeType abre la número i con el índice (i << 16). Devuelve ese
// índice para la instancia `style` o -1 si no la tiene.
long NamedInstance(const std::vector<unsigned char>& data, const char* style) {
  if (data.empty()) return -1;
  FT_Library lib = nullptr;
  if (FT_Init_FreeType(&lib) != 0) return -1;
  long found = -1;
  FT_Face face = nullptr;
  if (FT_New_Memory_Face(lib, data.data(), FT_Long(data.size()), 0, &face) == 0) {
    const long count = long(face->style_flags >> 16);
    FT_Done_Face(face);
    for (long i = 1; i <= count && found < 0; ++i) {
      FT_Face named = nullptr;
      if (FT_New_Memory_Face(lib, data.data(), FT_Long(data.size()), FT_Long(i << 16), &named) != 0) continue;
      if (named->style_name && std::strcmp(named->style_name, style) == 0) found = i << 16;
      FT_Done_Face(named);
    }
  }
  FT_Done_FreeType(lib);
  return found;
}

// Un archivo de letra y cómo cargarlo: instancia de la fuente variable o
// negrita sintética si no la tiene.
struct FaceData {
  std::vector<unsigned char> data;
  long index = 0;     // ImFontConfig::FontNo
  bool embolden = false;
};

FaceData Load(const wchar_t* file, const char* instance = nullptr) {
  FaceData f;
  f.data = ReadFont(file);
  if (instance && !f.data.empty()) {
    f.index = NamedInstance(f.data, instance);
    // Sin esa instancia, la de por defecto engordada por FreeType.
    if (f.index < 0) {
      f.index = 0;
      f.embolden = true;
    }
  }
  return f;
}

// Letras del texto de un diseño: regular y negrita.
void BodyFaces(Face face, FaceData* regular, FaceData* bold) {
  switch (face) {
  case Face::Bahnschrift:
  case Face::BahnschriftCondensed:
    *regular = Load(L"bahnschrift.ttf");
    *bold = Load(L"bahnschrift.ttf", "Bold");
    break;
  case Face::Consolas:
    *regular = Load(L"consola.ttf");
    *bold = Load(L"consolab.ttf");
    break;
  default:
    break;
  }
  // Cualquier otra, o sin el archivo (Windows recortado): Segoe UI.
  if (regular->data.empty() || bold->data.empty()) {
    *regular = Load(L"segoeui.ttf");
    *bold = Load(L"segoeuib.ttf");
  }
}

// Letra de los títulos y los números grandes de un diseño.
FaceData DisplayFace(Face face) {
  FaceData f;
  switch (face) {
  case Face::SegoeLight: f = Load(L"segoeuil.ttf"); break;
  case Face::SegoeSemibold: f = Load(L"seguisb.ttf"); break;
  case Face::SegoeBlack: f = Load(L"seguibl.ttf"); break;
  case Face::Bahnschrift: f = Load(L"bahnschrift.ttf", "Bold"); break;
  case Face::BahnschriftCondensed: f = Load(L"bahnschrift.ttf", "SemiBold SemiCondensed"); break;
  case Face::Consolas: f = Load(L"consolab.ttf"); break;
  case Face::Georgia: f = Load(L"georgiab.ttf"); break;
  case Face::Segoe: break;
  }
  if (f.data.empty()) f = Load(L"segoeuib.ttf");
  return f;
}

ImFont* Add(FaceData& face, float pixels) {
  if (face.data.empty()) return nullptr;
  ImFontConfig cfg;
  // Un solo archivo en memoria para todos los tamaños; se libera al construir.
  cfg.FontDataOwnedByAtlas = false;
  // Cada letra empieza en un píxel entero: sin esto los trazos verticales caen
  // entre dos píxeles y se ven grises.
  cfg.PixelSnapH = true;
  cfg.FontNo = int(face.index);
  if (face.embolden) cfg.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_Bold;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(face.data.data(), int(face.data.size()), pixels, &cfg);
}

} // namespace

ImFont* FontSet::Main() const { return regular[kMainIndex]; }
ImFont* FontSet::Bold() const { return bold[kMainIndex]; }
ImFont* FontSet::Big() const { return heading[1]; }

ImFont* FontSet::ForSize(ImFont* font, float size) const {
  if (!font) return font;
  ImFont* const* family = nullptr;
  int count = kCount;
  for (ImFont* const* f : {regular, bold})
    for (int i = 0; i < kCount; ++i)
      if (f[i] == font) family = f;
  if (!family) {
    for (int i = 0; i < kDisplayCount; ++i)
      if (heading[i] == font) family = heading;
    count = kDisplayCount;
  }
  if (!family) return font;
  ImFont* best = font;
  float bestDiff = std::fabs(font->FontSize - size);
  for (int i = 0; i < count; ++i) {
    float diff = std::fabs(family[i]->FontSize - size);
    if (diff < bestDiff) {
      best = family[i];
      bestDiff = diff;
    }
  }
  return best;
}

void LoadFonts(float scale, FontSet* out, FontUse use, Face body, Face display) {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  atlas->Clear();
  *out = FontSet{};
  out->scale = scale;
  out->body = body;
  out->display = display;
  // Hinting ligero: ajusta las letras a la rejilla de píxeles solo en vertical,
  // como ClearType. Nítido y con el mismo ancho que el diseño de la fuente (el
  // hinting completo ensancha el texto un 7 % y descuadraría los paneles).
  atlas->FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
  const bool allSizes = use != FontUse::Toast;
  FaceData regular, bold;
  BodyFaces(body, &regular, &bold);
  for (int i = 0; i < FontSet::kCount; ++i) {
    if (!allSizes && i != kMainIndex && i != kSmallIndex) continue;
    out->regular[i] = Add(regular, kSizes[i] * scale);
    out->bold[i] = Add(bold, kSizes[i] * scale);
  }
  FaceData heading;
  if (allSizes) heading = DisplayFace(display);
  for (int i = 0; i < FontSet::kDisplayCount && allSizes; ++i) {
    if (use == FontUse::Panel && i > 1) continue; // el panel no dibuja 28 ni 36 px
    out->heading[i] = Add(heading, kDisplaySizes[i] * scale);
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
  // Sin letra de títulos (los avisos no la cargan): la negrita más grande. Los
  // tamaños que el panel no carga usan su título de 22 px, de la misma familia.
  for (int i = 0; i < FontSet::kDisplayCount; ++i)
    if (!out->heading[i]) out->heading[i] = out->heading[1] ? out->heading[1] : out->bold[FontSet::kCount - 1];
  ImGui::GetIO().FontDefault = out->Main();
  atlas->Build();
  // Ya rasterizadas: los TTF (unos 2-4 MB) no hacen falta.
  atlas->ClearInputData();
}

bool ReleaseFontMemory() {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  // Sin textura (el backend no pudo crearla) la copia hace falta para reintentarlo.
  if (!atlas->TexID) return false;
  atlas->ClearTexData();
  return true;
}

void RequestFonts(Face body, Face display) {
  requested_ = true;
  wantBody_ = body;
  wantDisplay_ = display;
}

bool TakeFontRequest(Face* body, Face* display) {
  if (!requested_) return false;
  requested_ = false;
  *body = wantBody_;
  *display = wantDisplay_;
  return true;
}

} // namespace spectre::ui
