#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

namespace spectre::ui {

// Fuentes de una ventana de ImGui (panel, aviso o ventana de la app),
// rasterizadas con FreeType a cada tamaño de texto de la interfaz y a la
// escala de su monitor. Un texto dibujado a otro tamaño que el de su fuente se
// estira en la GPU y se ve borroso o pixelado: por eso hay una fuente por
// tamaño en vez de una sola escalada. Todas juntas ocupan una textura de 1-2 MB.
struct FontSet {
  static constexpr int kCount = 6;
  float scale = 1.0f;
  ImFont* regular[kCount] = {};
  ImFont* bold[kCount] = {};

  ImFont* Main() const; // texto normal (15 px): la fuente por defecto de ImGui
  ImFont* Bold() const; // negrita de 15 px
  ImFont* Big() const;  // títulos de página (22 px, negrita)
  // La fuente de la misma familia que `font` rasterizada al tamaño más
  // cercano a `size` (píxeles físicos). Una fuente que no es de aquí se
  // devuelve tal cual.
  ImFont* ForSize(ImFont* font, float size) const;
};

// Vacía el atlas del contexto de ImGui actual, añade Segoe UI y Segoe UI Bold
// a todos los tamaños y lo construye. Sin Segoe UI usa la fuente de ImGui.
// allSizes=false carga solo los dos tamaños que usan los avisos (15 px el
// título y 13 px el detalle): el atlas se construye unas tres veces más rápido.
void LoadFonts(float scale, FontSet* out, bool allSizes = true);
// Cuando el backend ya subió el atlas a la GPU (tras el primer frame): libera
// la copia en RAM. Fuera de un frame. false si aún no estaba subido.
bool ReleaseFontMemory();

} // namespace spectre::ui
