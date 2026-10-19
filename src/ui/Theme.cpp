#include "ui/Theme.h"

#include "app/Config.h"

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
                        IM_COL32(106, 106, 106, 255), // textFaint
                        IM_COL32(255, 255, 255, 255), // accent
                        IM_COL32(229, 229, 229, 255), // accentHover
                        IM_COL32(204, 204, 204, 255), // accentActive
                        IM_COL32(0, 0, 0, 255),       // onAccent
                        IM_COL32(229, 72, 77, 255),   // danger
                        IM_COL32(239, 92, 96, 255),   // dangerHover
                        IM_COL32(204, 58, 63, 255),   // dangerActive
                        IM_COL32(217, 163, 58, 255)}, // warning
                       false};

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
                           IM_COL32(110, 100, 144, 255), // textFaint
                           IM_COL32(255, 63, 209, 255),  // accent
                           IM_COL32(255, 102, 220, 255), // accentHover
                           IM_COL32(229, 43, 184, 255),  // accentActive
                           IM_COL32(18, 10, 26, 255),    // onAccent
                           IM_COL32(240, 64, 90, 255),   // danger
                           IM_COL32(247, 90, 112, 255),  // dangerHover
                           IM_COL32(214, 48, 74, 255),   // dangerActive
                           IM_COL32(232, 170, 60, 255)}, // warning
                          true};

const Theme* current_ = &kNegro;
bool motion_ = true;

ImVec4 V(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }
ImVec4 V(ImU32 color, float alpha) {
  ImVec4 v = V(color);
  v.w = alpha;
  return v;
}

} // namespace

const std::vector<const Theme*>& Themes() {
  static const std::vector<const Theme*> themes = {&kNegro, &kPsiquico};
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

void BeginThemeFrame(const Config& config) {
  current_ = &FindTheme(config.theme);
  motion_ = config.themeMotion && current_->psychic;
  ApplyThemeColors(ImGui::GetStyle());
}

const Theme& CurrentTheme() { return *current_; }
const Palette& Colors() { return current_->colors; }
bool ThemeMotion() { return motion_; }

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
