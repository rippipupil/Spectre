#pragma once

// Los operadores de ImVec2 (a + b, a * k) solo existen si se piden antes de la
// primera inclusión de imgui.h.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

#include <string>
#include <vector>

namespace spectre {

struct Config;

// Paleta de un tema. Colores en formato IM_COL32, listos para ImDrawList y
// para ImGui::GetColorU32 (que aplica la transparencia de la animación).
struct Palette {
  ImU32 bg;            // fondo del panel, de Ajustes y de los avisos
  ImU32 bgAlt;         // columna lateral de Ajustes
  ImU32 surface;       // botones secundarios, campos, segmentos
  ImU32 surfaceHover;  // superficie con el ratón encima
  ImU32 surfaceActive; // superficie pulsada o seleccionada
  ImU32 line;          // separadores y bordes de 1 px
  ImU32 lineStrong;    // bordes de keycaps y botones con borde
  ImU32 text;
  ImU32 textMuted;
  ImU32 textFaint;
  ImU32 accent;        // acción principal y estado activo: el único color con trabajo
  ImU32 accentHover;
  ImU32 accentActive;
  ImU32 onAccent;      // texto sobre el acento
  ImU32 danger;        // grabando y errores
  ImU32 dangerHover;
  ImU32 dangerActive;
  ImU32 warning;       // nivel alto en los medidores de audio
  ImU32 onDanger;      // texto sobre el rojo (botón de detener la grabación)
};

// Diseño de un tema: todo lo que no es color. Forma de los botones, cómo
// reaccionan al ratón y al pulsarlos, rótulos, separadores, interruptores y
// avisos. Siempre dentro de las reglas de la interfaz: plana, sin degradados,
// sombras ni esquinas redondeadas; lo que cambia son cortes, contornos,
// corchetes, raíles y el movimiento.
enum class ButtonShape {
  Flat,    // bloque relleno
  Outline, // contorno fino; se rellena al pasar el ratón
  Chamfer, // esquinas cortadas en diagonal
  Bracket, // corchetes en las cuatro esquinas, sin relleno
  Rail,    // bloque con un raíl del acento a la izquierda que crece
};
enum class HoverEffect { Fill, Border, Underline, Invert };
enum class PressEffect {
  Sink,   // el contenido se hunde 1 px mientras se pulsa
  Echo,   // un contorno fantasma sale del botón y se desvanece
  Glitch, // franjas desplazadas durante un instante
  Scan,   // una línea barre el botón de arriba abajo
  Ripple, // un cuadrado crece desde donde se pulsó
  Squash, // el botón encoge y vuelve con un pequeño rebote
  Flash,  // destello del acento que se apaga
};
enum class LabelStyle {
  Tracked,  // MAYÚSCULAS ESPACIADAS
  Slashes,  // // MAYÚSCULAS
  Prompt,   // > MAYÚSCULAS
  Sentence, // Solo la primera en mayúscula
  Plain,    // MAYÚSCULAS sin espaciar
};
enum class DividerStyle { Line, Ticked, Dashed, Space };
enum class MarkShape { Square, Diamond, Bar, Block };
enum class SwitchShape { Track, Box, Line };
enum class KeycapStyle { Box, Brackets, Plain };
// Movimiento continuo: pide frames aunque nadie toque nada.
enum class Ambient {
  None,
  Ghost,  // Psíquico: estela en el botón principal, marca que respira, sombra de fila
  Cursor, // Terminal: el cursor parpadea
};
// Forma de los avisos (y de la animación de guardar un clip).
enum class ToastLook {
  Bar,      // barra del acento a la izquierda; entra deslizándose
  Ghost,    // esquinas cortadas; dos contornos fantasma convergen
  Header,   // franja del acento con el título en mayúsculas; entra con glitch
  Terminal, // "> " y el texto escribiéndose con cursor
  TopLine,  // contorno con una línea arriba; cae desde el borde
  Card,     // tarjeta con el icono en un cuadro; sube con rebote
  Ink,      // borde fino y una línea gruesa abajo; aparece fundiéndose
  Bold,     // bordes de 2 px y sin movimiento
};

struct Style {
  const char* id;      // valor guardado en config.json (themeStyle)
  const char* name;    // nombre en Ajustes
  const char* summary; // una línea que lo describe
  ButtonShape button;
  HoverEffect hover;
  PressEffect press;
  LabelStyle label;
  DividerStyle divider;
  MarkShape mark;
  SwitchShape toggle;
  KeycapStyle keycap;
  ToastLook toast;
  float cut = 0;        // tamaño del corte de las esquinas (px lógicos)
  int cutCorners = 0;   // esquinas cortadas: 1 arriba izq., 2 arriba der., 4 abajo der., 8 abajo izq.
  float line = 1;       // grosor de bordes y separadores (px lógicos)
  float density = 1;    // alto de botones grandes, filas y segmentos
  bool upperButtons = false; // títulos de los botones grandes en mayúsculas
  Ambient ambient = Ambient::None;
  bool motion = true;   // tiene animaciones (alto contraste no)
};

struct Theme {
  const char* id;    // valor guardado en config.json
  const char* name;  // nombre que se muestra en Ajustes
  Palette colors;
  bool light = false;                // fondo claro (barra de título clara en Windows 10)
  const char* group = "Spectre";     // grupo del selector: los de Spectre o los de RoCer0
  const char* style = "plano";       // diseño que trae el tema (Style::id)
};

// Todos los temas, en el orden del selector de Ajustes: Negro y Psíquico, y
// después los de RoCer0.
const std::vector<const Theme*>& Themes();
// Tema por identificador; si no existe, el primero (Negro).
const Theme& FindTheme(const std::string& id);
bool IsKnownTheme(const std::string& id);

// Los ocho diseños, en el orden del selector.
const std::vector<const Style*>& Styles();
// Diseño por identificador; si no existe, Plano.
const Style& FindStyle(const std::string& id);
bool IsKnownStyle(const std::string& id);
// El diseño que se usa con un tema: el elegido en Ajustes o, si se dejó en
// "el del tema" (override vacío), el que trae el tema.
const Style& StyleFor(const Theme& theme, const std::string& override);

// Tema y animaciones activos en el frame que se está dibujando. Se fijan al
// empezar a dibujar cada ventana a partir de la configuración que se le pasa,
// así Ajustes muestra el tema elegido antes de pulsar Aplicar.
void BeginThemeFrame(const Config& config);
const Theme& CurrentTheme();
const Style& CurrentStyle();
const Palette& Colors();
// Transiciones al pasar el ratón, efectos al pulsar y entrada de los avisos.
// Las apaga el interruptor "Animaciones" de Ajustes (y Alto contraste).
bool ThemeMotion();
// Movimiento continuo del diseño (estela, respiración, cursor que parpadea):
// pide frames aunque nadie toque nada, así que solo lo tienen algunos.
// Ambient::None si no hay o las animaciones están apagadas.
Ambient AmbientMotion();

// Geometría del estilo de ImGui (sin esquinas redondeadas, bordes de 1 px,
// tamaños escalados al DPI). Se aplica una vez al crear el contexto.
void ApplyBaseStyle(float scale);
// Colores de ImGui para los controles nativos (desplegables, deslizadores,
// campos de texto). Se aplica en cada frame porque dependen del tema.
void ApplyThemeColors(ImGuiStyle& style);

} // namespace spectre
