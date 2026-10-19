#pragma once

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
};

struct Theme {
  const char* id;    // valor guardado en config.json
  const char* name;  // nombre que se muestra en Ajustes
  Palette colors;
  // Tema Psíquico: marca de estado en rombo y animaciones fantasmales
  // (rastro al abrir, estela en el botón principal, marca que respira).
  bool psychic;
};

// Todos los temas, en el orden del selector de Ajustes.
const std::vector<const Theme*>& Themes();
// Tema por identificador; si no existe, el primero (Negro).
const Theme& FindTheme(const std::string& id);
bool IsKnownTheme(const std::string& id);

// Tema y animaciones activos en el frame que se está dibujando. Se fijan al
// empezar a dibujar cada ventana a partir de la configuración que se le pasa,
// así Ajustes muestra el tema elegido antes de pulsar Aplicar.
void BeginThemeFrame(const Config& config);
const Theme& CurrentTheme();
const Palette& Colors();
bool ThemeMotion(); // animaciones del tema activas (solo las tiene Psíquico)

// Geometría del estilo de ImGui (sin esquinas redondeadas, bordes de 1 px,
// tamaños escalados al DPI). Se aplica una vez al crear el contexto.
void ApplyBaseStyle(float scale);
// Colores de ImGui para los controles nativos (desplegables, deslizadores,
// campos de texto). Se aplica en cada frame porque dependen del tema.
void ApplyThemeColors(ImGuiStyle& style);

} // namespace spectre
