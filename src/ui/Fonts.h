#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

#include "ui/Theme.h"

namespace spectre::ui {

// Fuentes de una ventana de ImGui (panel, aviso o ventana de la app),
// rasterizadas con FreeType a cada tamaño de texto de la interfaz y a la
// escala de su monitor. Un texto dibujado a otro tamaño que el de su fuente se
// estira en la GPU y se ve borroso o pixelado: por eso hay una fuente por
// tamaño en vez de una sola escalada. Todas juntas ocupan una textura de 2-3 MB.
//
// Cada diseño (ui/Theme.h Style) trae sus letras: una para el texto (body,
// con su negrita) y otra para los títulos y los números grandes (display).
struct FontSet {
  static constexpr int kCount = 6;        // tamaños del texto
  static constexpr int kDisplayCount = 4; // tamaños de los títulos: kBody + 2, kTitle, kHeadline, kHero
  float scale = 1.0f;
  Face body = Face::Segoe;    // letras con las que se hizo
  Face display = Face::Segoe;
  ImFont* regular[kCount] = {};
  ImFont* bold[kCount] = {};
  ImFont* heading[kDisplayCount] = {};

  ImFont* Main() const; // texto normal (15 px): la fuente por defecto de ImGui
  ImFont* Bold() const; // negrita de 15 px
  ImFont* Big() const;  // títulos (22 px, la letra de los títulos del diseño)
  // La fuente de la misma familia que `font` rasterizada al tamaño más
  // cercano a `size` (píxeles físicos). Una fuente que no es de aquí se
  // devuelve tal cual.
  ImFont* ForSize(ImFont* font, float size) const;
};

// Qué tamaños hacen falta en cada ventana:
// - App: el texto (regular y negrita) a todos los tamaños y los cuatro títulos.
// - Panel: el texto a todos los tamaños y solo los títulos que dibuja (17 y
//   22 px: la marca y la "S"); el atlas ocupa la mitad que con los cuatro.
// - Toast: los dos tamaños de los avisos (15 px el título y 13 px el detalle)
//   y ningún título: el atlas se construye unas tres veces más rápido.
enum class FontUse { App, Panel, Toast };

// Vacía el atlas del contexto de ImGui actual, añade las letras que pide `use`
// y lo construye. Sin Segoe UI usa la fuente de ImGui.
void LoadFonts(float scale, FontSet* out, FontUse use, Face body = Face::Segoe, Face display = Face::Segoe);
// Cuando el backend ya subió el atlas a la GPU (tras el primer frame): libera
// la copia en RAM. Fuera de un frame. false si aún no estaba subido.
bool ReleaseFontMemory();
// El frame que se acaba de dibujar es de un diseño con otras letras que las
// cargadas en su ventana (BeginFrame lo apunta). La ventana lo mira después
// de cada frame y, si hace falta, reconstruye el atlas antes del siguiente.
bool TakeFontRequest(Face* body, Face* display);
// Lo apunta BeginFrame (Widgets.cpp).
void RequestFonts(Face body, Face display);

} // namespace spectre::ui
