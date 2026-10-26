#include "ui/Theme.h"

#include "app/Config.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace spectre {

namespace {

// Tema 1: negro puro, superficies gris oscuro y detalles en blanco. El blanco
// es el acento (botón principal blanco con texto negro); el rojo solo aparece
// grabando o en un error.
constexpr Theme kNegro{"negro",
                       "Negro",
                       {IM_COL32(0, 0, 0, 255),       // bg
                        IM_COL32(13, 13, 13, 255),    // bgAlt
                        IM_COL32(26, 26, 26, 255),    // surface
                        IM_COL32(36, 36, 36, 255),    // surfaceHover
                        IM_COL32(46, 46, 46, 255),    // surfaceActive
                        IM_COL32(42, 42, 42, 255),    // line
                        IM_COL32(58, 58, 58, 255),    // lineStrong
                        IM_COL32(255, 255, 255, 255), // text
                        IM_COL32(154, 154, 154, 255), // textMuted
                        IM_COL32(124, 124, 124, 255), // textFaint (5,0:1 sobre bg, 4,7:1 sobre bgAlt)
                        IM_COL32(255, 255, 255, 255), // accent
                        IM_COL32(229, 229, 229, 255), // accentHover
                        IM_COL32(204, 204, 204, 255), // accentActive
                        IM_COL32(0, 0, 0, 255),       // onAccent
                        IM_COL32(229, 72, 77, 255),   // danger
                        IM_COL32(239, 92, 96, 255),   // dangerHover
                        IM_COL32(204, 58, 63, 255),   // dangerActive
                        IM_COL32(217, 163, 58, 255),  // warning
                        IM_COL32(255, 255, 255, 255)}, // onDanger
                       false,
                       "Spectre",
                       "plano",
                       Motif::Viewfinder};

// Tema 2: paleta del personaje de Valorant que eligió Pablo. Negro violáceo
// de la ropa, superficies con la luz azul-violeta, texto blanco lavanda del
// pelo y el magenta de la energía psíquica como único acento.
constexpr Theme kPsiquico{"psiquico",
                          "Psíquico",
                          {IM_COL32(11, 10, 20, 255),    // bg
                           IM_COL32(16, 13, 30, 255),    // bgAlt
                           IM_COL32(26, 21, 48, 255),    // surface
                           IM_COL32(35, 28, 64, 255),    // surfaceHover
                           IM_COL32(44, 36, 82, 255),    // surfaceActive
                           IM_COL32(42, 35, 69, 255),    // line
                           IM_COL32(58, 49, 96, 255),    // lineStrong
                           IM_COL32(242, 236, 255, 255), // text
                           IM_COL32(167, 155, 203, 255), // textMuted
                           IM_COL32(132, 122, 172, 255), // textFaint (5,0:1 sobre bg, 4,9:1 sobre bgAlt)
                           IM_COL32(255, 63, 209, 255),  // accent
                           IM_COL32(255, 102, 220, 255), // accentHover
                           IM_COL32(229, 43, 184, 255),  // accentActive
                           IM_COL32(18, 10, 26, 255),    // onAccent
                           IM_COL32(240, 64, 90, 255),   // danger
                           IM_COL32(247, 90, 112, 255),  // dangerHover
                           IM_COL32(214, 48, 74, 255),   // dangerActive
                           IM_COL32(232, 170, 60, 255),  // warning
                           IM_COL32(255, 255, 255, 255)}, // onDanger
                          false,
                          "Spectre",
                          "psiquico",
                          Motif::Psychic};

// Temas de RoCer0, la extensión de Roblox de Pablo: sus colores tal cual
// (extension/common/themes.js, versión 1.2.0) en este orden: fondo, tarjeta,
// tarjeta elevada, borde, texto, texto secundario, acento y rojo. accent2 y
// success no se usan (aquí hay un solo acento). Sus colores y uno de los
// diseños de Spectre (kStyles) que le pega: el cristal, los brillos, los
// redondeos y los fondos animados de RoCer0 no van con las reglas de esta
// interfaz (plana, sin degradados ni esquinas redondeadas).
// Un tema nuevo de RoCer0 = otra línea aquí.
struct RoCer0Colors {
  uint32_t bg, surface, surface2, border, text, muted, accent, danger;
};
struct RoCer0Theme {
  const char* id;
  const char* name;
  bool light;
  const char* style; // diseño que trae (Style::id)
  Motif motif;       // fondo ilustrado
  RoCer0Colors c;
};
constexpr RoCer0Theme kRoCer0[] = {
    {"medianoche", "Medianoche", false, "contorno", Motif::Stars, {0x0a0d16, 0x10141f, 0x181e2d, 0x222a3c, 0xeaf0fa, 0x8c95a8, 0x3b82f6, 0xef4444}},
    {"daniel", "Daniel", false, "tactico", Motif::Circuit, {0x0f0f0f, 0x0f0f0f, 0x1c1c1c, 0x7a7a7a, 0xfdfdfd, 0xc8c8c8, 0xe5191c, 0xff3d3d}},
    {"pablo", "Pablo", false, "psiquico", Motif::Hex, {0x130f2e, 0x1d1846, 0x272060, 0x4a4390, 0xf1f0ff, 0xb4aee0, 0x2be4ff, 0xff4d6a}},
    {"amoled", "AMOLED", false, "plano", Motif::Dots, {0x000000, 0x0a0a0a, 0x141414, 0x232323, 0xf5f5f5, 0x9a9a9a, 0x00a2ff, 0xff4d4d}},
    {"vidrio", "Vidrio esmerilado", false, "contorno", Motif::Hatch, {0x0b1020, 0x1a2238, 0x24304d, 0x33406a, 0xf1f5ff, 0xa5b1cf, 0x38bdf8, 0xfb7185}},
    {"nebulosa", "Nebulosa", false, "psiquico", Motif::Constellation, {0x0c0716, 0x150e24, 0x1f1533, 0x2c2045, 0xf1ebff, 0xa497c4, 0xa855f7, 0xf43f5e}},
    {"galaxia", "Galaxia", false, "neon", Motif::Orbits, {0x05060f, 0x0d1022, 0x151a33, 0x222947, 0xe8ecff, 0x8f97c0, 0x60a5fa, 0xf87171}},
    {"synthwave", "Synthwave '84", false, "neon", Motif::Horizon, {0x1b1029, 0x241734, 0x2e1d44, 0x43296a, 0xfdf0ff, 0xb79ad6, 0xff7edb, 0xfe4450}},
    {"cyberpunk", "Cyberpunk", false, "tactico", Motif::Hazard, {0x0a0a0c, 0x121216, 0x1b1b21, 0x2d2d36, 0xf2f2f2, 0x9a9aa5, 0xfcee0a, 0xff003c}},
    {"matrix", "Matrix", false, "terminal", Motif::Rain, {0x000500, 0x031003, 0x062006, 0x0d3d0d, 0xc8ffc8, 0x5fbf5f, 0x00ff41, 0xff3b3b}},
    {"catppuccin", "Catppuccin Mocha", false, "suave", Motif::Confetti, {0x11111b, 0x1e1e2e, 0x313244, 0x45475a, 0xcdd6f4, 0xa6adc8, 0xcba6f7, 0xf38ba8}},
    {"dracula", "Drácula", false, "suave", Motif::Diamonds, {0x1e1f29, 0x282a36, 0x343746, 0x44475a, 0xf8f8f2, 0x8f9bc9, 0xbd93f9, 0xff5555}},
    {"nord", "Nord", false, "contorno", Motif::Mountains, {0x2a303c, 0x2e3440, 0x3b4252, 0x4c566a, 0xeceff4, 0xa9b4c7, 0x88c0d0, 0xbf616a}},
    {"tokyo", "Tokyo Night", false, "neon", Motif::Skyline, {0x16161e, 0x1a1b26, 0x24283b, 0x3b4261, 0xc0caf5, 0x7a83ad, 0x7aa2f7, 0xf7768e}},
    {"rosepine", "Rosé Pine", false, "suave", Motif::Topo, {0x191724, 0x1f1d2e, 0x26233a, 0x403d52, 0xe0def4, 0x908caa, 0xebbcba, 0xeb6f92}},
    {"gruvbox", "Gruvbox", false, "terminal", Motif::Scanlines, {0x1d2021, 0x282828, 0x32302f, 0x504945, 0xebdbb2, 0xa89984, 0xfe8019, 0xfb4934}},
    {"oceano", "Océano", false, "contorno", Motif::Waves, {0x04141f, 0x082232, 0x0c2e43, 0x12405c, 0xe0f7ff, 0x7fb2c8, 0x22d3ee, 0xf87171}},
    {"bosque", "Bosque", false, "terminal", Motif::Pines, {0x0c140f, 0x121d16, 0x1a281f, 0x25392c, 0xe6f4ea, 0x8fb39b, 0x4ade80, 0xf87171}},
    {"atardecer", "Atardecer", false, "suave", Motif::Sunset, {0x170d14, 0x21121c, 0x2c1825, 0x432438, 0xffeef2, 0xc79aa9, 0xfb923c, 0xf43f5e}},
    {"lunasangre", "Luna de sangre", false, "tactico", Motif::Moon, {0x0f0507, 0x1a090c, 0x250d12, 0x3d141c, 0xffe9ea, 0xc58b91, 0xef4444, 0xff6b6b}},
    {"oro", "Oro negro", false, "plano", Motif::Deco, {0x0b0a07, 0x15130d, 0x1f1c13, 0x3a3320, 0xf7f1de, 0xb3a77f, 0xf5c542, 0xff6b5b}},
    {"menta", "Menta", false, "contorno", Motif::Bubbles, {0x0d1514, 0x13201e, 0x1b2c29, 0x294440, 0xe3fbf6, 0x86b5ab, 0x2dd4bf, 0xfb7185}},
    {"claro", "Clásico claro", true, "papel", Motif::Grid, {0xf2f4f7, 0xffffff, 0xeef1f5, 0xdde2ea, 0x1d2433, 0x5f6b7d, 0x0b6cff, 0xdc2626}},
    {"latte", "Catppuccin Latte", true, "papel", Motif::Plaid, {0xe6e9ef, 0xeff1f5, 0xdce0e8, 0xccd0da, 0x4c4f69, 0x6c6f85, 0x8839ef, 0xd20f39}},
    {"sakura", "Sakura", true, "papel", Motif::Petals, {0xfff4f7, 0xffffff, 0xffe8ef, 0xf5cbd8, 0x4a2b36, 0x9a6b7b, 0xe75480, 0xd64545}},
    {"papel", "Papel", true, "papel", Motif::Ruled, {0xf6efdc, 0xfdf6e3, 0xeee8d5, 0xdfd6bd, 0x36454a, 0x6f7f84, 0x268bd2, 0xdc322f}},
    {"contraste", "Alto contraste", false, "contraste", Motif::None, {0x000000, 0x000000, 0x101010, 0xffffff, 0xffffff, 0xe6e6e6, 0xffff00, 0xff4040}},
    {"halloween", "Halloween", false, "tactico", Motif::Webs, {0x0f0a14, 0x1a1222, 0x241a30, 0x3a2a4a, 0xfff3e6, 0xbfa3c9, 0xff7a1a, 0xff4d6d}},
    {"navidad", "Navidad", false, "suave", Motif::Snow, {0x0b1410, 0x12201a, 0x1a2c23, 0x2c4436, 0xf4fff8, 0x9cc3ad, 0xe63946, 0xff6b6b}},
};

// Diseños. Cada tema trae uno (Theme::style) y en Ajustes se puede elegir
// otro. Cambian la forma de todo y la estructura de la app: navegación,
// letras, títulos, tarjetas y márgenes. Los efectos son primitivas de ImGui
// (unas decenas de vértices) y solo se dibujan mientras duran: nada cuando no
// se toca la interfaz, salvo los que tienen movimiento continuo (ambient),
// que la ventana de la app solo pide teniendo el foco.
constexpr Style kStyles[] = {
    {.id = "plano", .name = "Plano",
     .summary = "Bloques sólidos, títulos en negrita pesada y la barra lateral de siempre. Al pulsar, el botón se hunde.",
     .title = TitleLook::Block, .display = Face::SegoeBlack, .titleSize = 36},
    {.id = "psiquico", .name = "Psíquico",
     .summary = "Raíl de iconos, esquinas cortadas, títulos finos con eco y estela en el botón principal.",
     .button = ButtonShape::Chamfer, .press = PressEffect::Echo, .mark = MarkShape::Diamond, .toast = ToastLook::Ghost,
     .cut = 6, .cutCorners = 1 | 4, .ambient = Ambient::Ghost, .nav = NavLayout::Rail, .title = TitleLook::Echo,
     .card = CardLook::Framed, .display = Face::SegoeLight, .titleSize = 36, .pad = 36, .navWidth = 88},
    {.id = "neon", .name = "Neón",
     .summary = "Pestañas arriba, dobles contornos como tubos de neón y títulos con aberración de color.",
     .button = ButtonShape::Double, .hover = HoverEffect::Border, .press = PressEffect::Echo, .mark = MarkShape::Diamond,
     .toggle = SwitchShape::Line, .toast = ToastLook::TopLine, .upperButtons = true, .nav = NavLayout::Topbar,
     .title = TitleLook::Chroma, .card = CardLook::Neon, .display = Face::SegoeSemibold, .titleSize = 28, .gap = 16},
    {.id = "tactico", .name = "Táctico",
     .summary = "Pestañas de HUD, letra condensada, todo en mayúsculas numerado, cortes duros y glitch al pulsar.",
     .button = ButtonShape::Chamfer, .hover = HoverEffect::Border, .press = PressEffect::Glitch,
     .label = LabelStyle::Slashes, .divider = DividerStyle::Ticked, .mark = MarkShape::Bar, .toggle = SwitchShape::Box,
     .keycap = KeycapStyle::Brackets, .toast = ToastLook::Header, .cut = 11, .cutCorners = 2, .density = 0.94f,
     .upperButtons = true, .nav = NavLayout::Topbar, .title = TitleLook::Index, .card = CardLook::Hud,
     .body = Face::Bahnschrift, .display = Face::BahnschriftCondensed, .titleSize = 28, .pad = 26, .gap = 10},
    {.id = "terminal", .name = "Terminal",
     .summary = "Letra de consola, menú en árbol, marcos de texto, cursor que parpadea y barrido al pulsar.",
     .button = ButtonShape::Bracket, .press = PressEffect::Scan, .label = LabelStyle::Prompt,
     .divider = DividerStyle::Dashed, .mark = MarkShape::Block, .toggle = SwitchShape::Box,
     .keycap = KeycapStyle::Brackets, .toast = ToastLook::Terminal, .ambient = Ambient::Cursor,
     .title = TitleLook::Prompt, .card = CardLook::Ascii, .body = Face::Consolas, .display = Face::Consolas,
     .titleSize = 22, .pad = 28, .gap = 12, .navWidth = 244},
    {.id = "contorno", .name = "Contorno",
     .summary = "Raíl de iconos, contornos finos que se iluminan, títulos subrayados y una onda al pulsar.",
     .button = ButtonShape::Outline, .hover = HoverEffect::Border, .press = PressEffect::Ripple,
     .label = LabelStyle::Sentence, .toggle = SwitchShape::Line, .toast = ToastLook::TopLine, .nav = NavLayout::Rail,
     .title = TitleLook::Rule, .card = CardLook::Framed, .display = Face::SegoeSemibold, .titleSize = 28,
     .navWidth = 84},
    {.id = "suave", .name = "Suave",
     .summary = "Mucho aire, menú flotante, títulos grandes, un raíl de color que crece y rebote al pulsar.",
     .button = ButtonShape::Rail, .press = PressEffect::Squash, .label = LabelStyle::Sentence,
     .divider = DividerStyle::Space, .keycap = KeycapStyle::Plain, .toast = ToastLook::Card, .density = 1.08f,
     .title = TitleLook::Soft, .card = CardLook::Tile, .titleSize = 36, .pad = 40, .gap = 18, .navWidth = 232,
     .floatingNav = true},
    {.id = "papel", .name = "Papel",
     .summary = "Como una revista: cabecera con serifa, fotos con marco, subrayados que crecen y destello al pulsar.",
     .button = ButtonShape::Outline, .hover = HoverEffect::Underline, .press = PressEffect::Flash,
     .label = LabelStyle::Sentence, .divider = DividerStyle::Dashed, .toggle = SwitchShape::Line,
     .keycap = KeycapStyle::Plain, .toast = ToastLook::Ink, .nav = NavLayout::Topbar, .title = TitleLook::Masthead,
     .card = CardLook::Polaroid, .display = Face::Georgia, .titleSize = 36, .pad = 40, .gap = 18},
    {.id = "contraste", .name = "Alto contraste",
     .summary = "Bordes de 2 px, títulos en negro sobre blanco, colores invertidos al pasar el ratón y nada de movimiento.",
     .button = ButtonShape::Outline, .hover = HoverEffect::Invert, .label = LabelStyle::Plain,
     .toggle = SwitchShape::Box, .toast = ToastLook::Bold, .line = 2, .motion = false, .title = TitleLook::Heavy,
     .card = CardLook::Heavy, .display = Face::SegoeBlack, .titleSize = 28, .pad = 28},
};

ImU32 Rgb(uint32_t hex) { return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, 255); }

ImU32 MixRgb(ImU32 a, ImU32 b, float t) {
  auto ch = [&](int shift) {
    float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
    return ImU32(std::lround(x + (y - x) * t)) << shift;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (255u << IM_COL32_A_SHIFT);
}

// Luminancia relativa y contraste de WCAG 2.
double Luminance(ImU32 c) {
  auto lin = [&](int shift) {
    double v = double((c >> shift) & 0xFF) / 255.0;
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * lin(IM_COL32_R_SHIFT) + 0.7152 * lin(IM_COL32_G_SHIFT) + 0.0722 * lin(IM_COL32_B_SHIFT);
}
double Contrast(ImU32 a, ImU32 b) {
  double la = Luminance(a), lb = Luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
// El texto más legible sobre un color: casi negro o blanco.
ImU32 TextOn(ImU32 c) {
  const ImU32 dark = IM_COL32(17, 17, 17, 255), white = IM_COL32(255, 255, 255, 255);
  return Contrast(dark, c) > Contrast(white, c) ? dark : white;
}

// La paleta completa a partir de los colores de RoCer0. La tarjeta de RoCer0
// es la columna lateral de Ajustes y la tarjeta elevada, la de los botones y
// campos; el ratón encima y pulsado acercan la superficie al color del texto.
// Los textos secundarios se aclaran (u oscurecen) hasta tener un contraste de
// 4,5:1 sobre el fondo y la columna.
Theme FromRoCer0(const RoCer0Theme& t) {
  const ImU32 bg = Rgb(t.c.bg), text = Rgb(t.c.text), border = Rgb(t.c.border);
  const ImU32 black = IM_COL32(0, 0, 0, 255), white = IM_COL32(255, 255, 255, 255);
  Palette p{};
  p.bg = bg;
  if (t.light) {
    // En los claros la tarjeta es blanca (más clara que el fondo): se usan
    // tonos entre el fondo y el borde para que los botones se distingan.
    p.bgAlt = MixRgb(bg, border, 0.25f);
    p.surface = MixRgb(bg, border, 0.5f);
    p.surfaceHover = MixRgb(p.surface, text, 0.05f);
    p.surfaceActive = MixRgb(p.surface, text, 0.10f);
  } else {
    p.bgAlt = t.c.surface != t.c.bg ? Rgb(t.c.surface) : MixRgb(bg, Rgb(t.c.surface2), 0.5f);
    p.surface = Rgb(t.c.surface2);
    p.surfaceHover = MixRgb(p.surface, text, 0.06f);
    p.surfaceActive = MixRgb(p.surface, text, 0.12f);
  }
  p.line = border;
  p.lineStrong = MixRgb(border, Rgb(t.c.muted), 0.3f);
  p.text = text;
  auto readable = [&](ImU32 c) { return std::min(Contrast(c, bg), Contrast(c, p.bgAlt)) >= 4.5; };
  p.textMuted = Rgb(t.c.muted);
  for (int i = 1; i <= 20 && !readable(p.textMuted); ++i) p.textMuted = MixRgb(Rgb(t.c.muted), text, 0.05f * float(i));
  p.textFaint = p.textMuted;
  for (int i = 1; i <= 11; ++i) {
    ImU32 c = MixRgb(p.textMuted, bg, 0.05f * float(i));
    if (!readable(c)) break;
    p.textFaint = c;
  }
  const ImU32 toward = t.light ? black : white;
  p.accent = Rgb(t.c.accent);
  p.accentHover = MixRgb(p.accent, toward, 0.15f);
  p.accentActive = MixRgb(p.accent, black, t.light ? 0.2f : 0.15f);
  p.onAccent = TextOn(p.accent);
  p.danger = Rgb(t.c.danger);
  p.dangerHover = MixRgb(p.danger, toward, 0.12f);
  p.dangerActive = MixRgb(p.danger, black, 0.15f);
  p.warning = t.light ? IM_COL32(183, 121, 31, 255) : IM_COL32(232, 170, 60, 255);
  p.onDanger = TextOn(p.danger);
  return Theme{t.id, t.name, p, t.light, "RoCer0", t.style, t.motif};
}

const Theme* current_ = &kNegro;
const Style* style_ = &kStyles[0];
bool motion_ = true;
int backdrop_ = 2; // 0 sin fondo, 1 quieto, 2 animado (Config::themeBackdrop)

ImVec4 V(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }
ImVec4 V(ImU32 color, float alpha) {
  ImVec4 v = V(color);
  v.w = alpha;
  return v;
}

} // namespace

const std::vector<const Theme*>& Themes() {
  // Se calculan una vez; los punteros no cambian en toda la ejecución.
  static const std::vector<Theme> rocer0 = [] {
    std::vector<Theme> v;
    v.reserve(std::size(kRoCer0));
    for (const RoCer0Theme& t : kRoCer0) v.push_back(FromRoCer0(t));
    return v;
  }();
  static const std::vector<const Theme*> themes = [] {
    std::vector<const Theme*> v = {&kNegro, &kPsiquico};
    for (const Theme& t : rocer0) v.push_back(&t);
    return v;
  }();
  return themes;
}

const Theme& FindTheme(const std::string& id) {
  for (const Theme* t : Themes())
    if (id == t->id) return *t;
  return kNegro;
}

bool IsKnownTheme(const std::string& id) {
  for (const Theme* t : Themes())
    if (id == t->id) return true;
  return false;
}

const std::vector<const Style*>& Styles() {
  static const std::vector<const Style*> styles = [] {
    std::vector<const Style*> v;
    for (const Style& s : kStyles) v.push_back(&s);
    return v;
  }();
  return styles;
}

const Style& FindStyle(const std::string& id) {
  for (const Style& s : kStyles)
    if (id == s.id) return s;
  return kStyles[0];
}

bool IsKnownStyle(const std::string& id) {
  for (const Style& s : kStyles)
    if (id == s.id) return true;
  return false;
}

const Style& StyleFor(const Theme& theme, const std::string& override) {
  return FindStyle(override.empty() ? std::string(theme.style) : override);
}

void BeginThemeFrame(const Config& config) {
  current_ = &FindTheme(config.theme);
  style_ = &StyleFor(*current_, config.themeStyle);
  motion_ = config.themeMotion && style_->motion;
  backdrop_ = config.themeBackdrop == "no" ? 0 : config.themeBackdrop == "quieto" ? 1 : 2;
  ImGuiStyle& s = ImGui::GetStyle();
  // Los bordes no se escalan con el DPI (ScaleAllSizes no los toca): 1 px, o
  // 2 en Alto contraste, en los controles nativos (desplegables, campos).
  s.FrameBorderSize = style_->line;
  s.PopupBorderSize = style_->line;
  ApplyThemeColors(s);
}

const Theme& CurrentTheme() { return *current_; }
const Style& CurrentStyle() { return *style_; }
const Palette& Colors() { return current_->colors; }
bool ThemeMotion() { return motion_; }
Ambient AmbientMotion() { return motion_ ? style_->ambient : Ambient::None; }

bool BackdropVisible() { return backdrop_ > 0 && current_->motif != Motif::None; }

bool BackdropMoves() {
  if (!motion_ || backdrop_ < 2) return false;
  switch (current_->motif) {
  case Motif::Psychic:
  case Motif::Stars:
  case Motif::Orbits:
  case Motif::Horizon:
  case Motif::Rain:
  case Motif::Waves:
  case Motif::Bubbles:
  case Motif::Petals:
  case Motif::Snow:
    return true;
  default:
    return false;
  }
}

void ApplyBaseStyle(float scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();
  // Sin esquinas redondeadas en ningún sitio: es lo que más distingue la
  // interfaz de una plantilla.
  s.WindowRounding = 0;
  s.ChildRounding = 0;
  s.FrameRounding = 0;
  s.PopupRounding = 0;
  s.GrabRounding = 0;
  s.TabRounding = 0;
  s.ScrollbarRounding = 0;
  s.WindowBorderSize = 1;
  s.ChildBorderSize = 1;
  s.PopupBorderSize = 1;
  s.FrameBorderSize = 1;
  s.WindowPadding = ImVec2(16, 16);
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(10, 8);
  s.ItemInnerSpacing = ImVec2(6, 4);
  s.ScrollbarSize = 8;
  s.GrabMinSize = 10;
  s.WindowMenuButtonPosition = ImGuiDir_None;
  s.ScaleAllSizes(scale);
  ApplyThemeColors(s);
}

void ApplyThemeColors(ImGuiStyle& s) {
  const Palette& p = current_->colors;
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = V(p.text);
  c[ImGuiCol_TextDisabled] = V(p.textMuted);
  c[ImGuiCol_WindowBg] = V(p.bg);
  c[ImGuiCol_ChildBg] = V(p.bg, 0.0f);
  c[ImGuiCol_PopupBg] = V(p.bg);
  c[ImGuiCol_Border] = V(p.line);
  c[ImGuiCol_BorderShadow] = V(p.bg, 0.0f);
  c[ImGuiCol_FrameBg] = V(p.surface);
  c[ImGuiCol_FrameBgHovered] = V(p.surfaceHover);
  c[ImGuiCol_FrameBgActive] = V(p.surfaceActive);
  c[ImGuiCol_TitleBg] = V(p.bg);
  c[ImGuiCol_TitleBgActive] = V(p.bg);
  c[ImGuiCol_TitleBgCollapsed] = V(p.bg);
  c[ImGuiCol_MenuBarBg] = V(p.bgAlt);
  c[ImGuiCol_ScrollbarBg] = V(p.bg, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = V(p.lineStrong);
  c[ImGuiCol_ScrollbarGrabHovered] = V(p.textFaint);
  c[ImGuiCol_ScrollbarGrabActive] = V(p.textMuted);
  c[ImGuiCol_CheckMark] = V(p.accent);
  c[ImGuiCol_SliderGrab] = V(p.accent);
  c[ImGuiCol_SliderGrabActive] = V(p.accentActive);
  c[ImGuiCol_Button] = V(p.surface);
  c[ImGuiCol_ButtonHovered] = V(p.surfaceHover);
  c[ImGuiCol_ButtonActive] = V(p.surfaceActive);
  c[ImGuiCol_Header] = V(p.surfaceActive);
  c[ImGuiCol_HeaderHovered] = V(p.surfaceHover);
  c[ImGuiCol_HeaderActive] = V(p.surfaceActive);
  c[ImGuiCol_Separator] = V(p.line);
  c[ImGuiCol_SeparatorHovered] = V(p.lineStrong);
  c[ImGuiCol_SeparatorActive] = V(p.textFaint);
  c[ImGuiCol_ResizeGrip] = V(p.bg, 0.0f);
  c[ImGuiCol_ResizeGripHovered] = V(p.bg, 0.0f);
  c[ImGuiCol_ResizeGripActive] = V(p.bg, 0.0f);
  c[ImGuiCol_Tab] = V(p.bg, 0.0f);
  c[ImGuiCol_TabHovered] = V(p.surfaceHover);
  c[ImGuiCol_TabSelected] = V(p.surfaceActive);
  c[ImGuiCol_TabSelectedOverline] = V(p.accent);
  c[ImGuiCol_TabDimmed] = V(p.bg, 0.0f);
  c[ImGuiCol_TabDimmedSelected] = V(p.surface);
  c[ImGuiCol_TabDimmedSelectedOverline] = V(p.lineStrong);
  c[ImGuiCol_PlotLines] = V(p.text);
  c[ImGuiCol_PlotLinesHovered] = V(p.accent);
  c[ImGuiCol_PlotHistogram] = V(p.text);
  c[ImGuiCol_PlotHistogramHovered] = V(p.accent);
  c[ImGuiCol_TableHeaderBg] = V(p.bgAlt);
  c[ImGuiCol_TableBorderStrong] = V(p.lineStrong);
  c[ImGuiCol_TableBorderLight] = V(p.line);
  c[ImGuiCol_TableRowBg] = V(p.bg, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = V(p.surface, 0.4f);
  c[ImGuiCol_TextLink] = V(p.textMuted);
  c[ImGuiCol_TextSelectedBg] = V(p.accent, 0.35f);
  c[ImGuiCol_DragDropTarget] = V(p.accent);
  c[ImGuiCol_NavCursor] = V(p.accent);
  c[ImGuiCol_NavWindowingHighlight] = V(p.accent);
  c[ImGuiCol_NavWindowingDimBg] = V(p.bg, 0.0f);
  c[ImGuiCol_ModalWindowDimBg] = V(p.bg, 0.0f);
}

} // namespace spectre
