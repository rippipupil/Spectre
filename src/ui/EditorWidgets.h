#pragma once

// Piezas de la columna de herramientas de los editores (vídeo y fotos), para
// que los dos se vean y se usen igual. Están en EditorUi.cpp.

#include <imgui.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace spectre::editui {

bool TypingText();
std::string NameOf(const std::string& path);
// Título de una herramienta. Con `on`, un interruptor a la derecha y toda la
// fila lo cambia; bold=false para las opciones dentro de una herramienta.
bool SwitchRow(const char* label, bool* on, bool bold = false);
bool ToolHeader(const char* title, bool* on = nullptr);
// Texto pequeño encima de un control.
void Label(const char* text);
void Note(const std::string& text);
// Dos botones del mismo ancho en una fila; devuelve 1, 2 o 0.
int TwoButtons(const char* id1, const char* label1, const char* id2, const char* label2);
// Rejilla de opciones planas. Devuelve la pulsada o -1.
int ChoiceGrid(const char* id, const std::vector<std::pair<int, std::string>>& options, int current, int cols);
// Deslizador de -100 a 100 a todo el ancho; clic derecho lo deja en 0.
bool CenteredSlider(const char* id, int* value);
// Volumen en % (0 a 200) a todo el ancho; clic derecho lo deja en 100 %.
bool PercentSlider(const char* id, float* value);
// 0xRRGGBB de un color de ImGui (ABGR).
uint32_t Rgb(ImU32 c);

} // namespace spectre::editui
