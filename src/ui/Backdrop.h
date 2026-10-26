#pragma once

// Fondo ilustrado de cada tema: un dibujo de líneas y puntos finos (estrellas,
// lluvia de código, una ciudad, olas, un visor de cámara...) muy tenue sobre
// el color de fondo, detrás de todo. Plano: sin degradados, sombras ni
// brillos. Son unos cientos de primitivas de ImGui; los que se mueven solo
// piden frames con la ventana delante (ui/Theme.h BackdropMoves).

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

#include "ui/Theme.h"

namespace spectre::ui {

// El fondo del tema actual en ese rectángulo. strength 0..1: 1 en las páginas
// de la app, algo menos en el panel y en los avisos (encima del juego).
void DrawBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b, float strength = 1.0f);
// El de cualquier tema a cualquier escala y en cualquier momento (las
// miniaturas del selector de temas). time en segundos; 0 = quieto.
void DrawMotif(ImDrawList* dl, Motif motif, const Palette& palette, bool light, ImVec2 a, ImVec2 b, float scale,
               double time, float strength);

} // namespace spectre::ui
