#pragma once

// Controles comunes del panel (Alt+Z), de la ventana de la app y de los avisos.
// Reglas del rediseño (investigacion/diseno-interfaz.md): sin esquinas
// redondeadas, sin degradados ni sombras, superficies planas con líneas de
// 1 px, un solo acento por tema y el rojo únicamente para grabar y errores.
// Los colores vienen de ui/Theme.h y pasan por ImGui::GetColorU32 para
// respetar la transparencia de las animaciones de entrada.

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

#include <string>
#include <utility>
#include <vector>

namespace spectre {

struct Config;

namespace ui {

struct FontSet;

// Tamaños de texto en píxeles lógicos (se multiplican por la escala DPI).
constexpr float kBody = 15.0f;  // texto del panel y de los formularios
constexpr float kSmall = 13.0f; // filas de clips, estado, keycaps
constexpr float kHint = 12.0f;  // ayudas y enlaces
constexpr float kLabel = 11.0f; // títulos de sección en mayúsculas
constexpr float kTitle = 22.0f; // título de página

constexpr float kPi = 3.14159265358979f;

// Escala y fuentes de la ventana que se está dibujando. Se fijan al empezar
// cada frame con la configuración que se le pasa (también el tema: así
// Ajustes muestra el tema elegido antes de pulsar Aplicar).
struct Fonts {
  float scale = 1.0f;
  ImFont* bold = nullptr;
  ImFont* big = nullptr;
  // Las mismas familias rasterizadas a cada tamaño de texto (ui/Fonts.h):
  // Text, TextSize, Ellipsize y TrackedText dibujan con la del tamaño pedido.
  const FontSet* sizes = nullptr;
};
void BeginFrame(const Fonts& fonts, const Config& config);
float Px(float logical);
ImFont* Bold();
ImFont* Big();
// La fuente de la familia de `font` hecha para `size` píxeles. Para dibujar
// texto directamente con dl->AddText a un tamaño que no es el de la fuente:
// con la fuente escalada el texto sale borroso.
ImFont* Sized(ImFont* font, float size);

ImU32 C(ImU32 color, float alpha = 1.0f);
ImVec4 V(ImU32 color);
ImU32 Mix(ImU32 a, ImU32 b, float t);

ImVec2 TextSize(ImFont* font, float size, const char* text);
void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text);
// Texto con espaciado entre letras (marca y títulos de sección), respetando UTF-8.
float TrackedWidth(ImFont* font, float size, const char* text, float tracking);
void TrackedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text,
                 float tracking);
// Recorta el texto con "..." para que quepa en maxWidth (respeta UTF-8).
std::string Ellipsize(ImFont* font, float size, const std::string& text, float maxWidth);

std::string FormatDuration(double seconds); // 00:42, 1:02:03
std::string FormatClipLength(int seconds);  // 30 s, 2 min

// Iconos de trazo fino y constante, sin fondos ni círculos detrás.
enum class Icon {
  Replay, Mic, Camera, Close, Back, Play, Pause, Folder, Trash, Star, StarFilled, Scissors, Copy, Pencil,
  Shot, Marker, Search, Volume, Muted, Next, Prev, External, Share
};
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float radius, ImU32 color);
// Marca de estado: cuadrado en Negro, rombo en Psíquico. Nunca un círculo de
// color: siempre va con el texto que explica el estado.
void DrawMark(ImDrawList* dl, ImVec2 center, float half, ImU32 color, bool filled);

// Animaciones del tema Psíquico (unas decenas de vértices; nada si están
// apagadas o el tema es Negro).
// Sombra psíquica: al cambiar la fila bajo el ratón, la anterior se desvanece
// en 200 ms. Una por contexto de ImGui; se reinicia con cada apertura.
void ResetHoverTrail(unsigned long long openedAt);
void TrackHover(ImGuiID id, ImVec2 a, ImVec2 b, bool hovered);
void DrawHoverTrail(ImDrawList* dl);
// Estela: un segmento de 2 px recorre el borde inferior del rectángulo cada 2,8 s.
void DrawTrace(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color);
// Respiración de la marca de estado (2,6 s); 1 sin animaciones.
float Breathe();
// Animación de entrada: 0 -> 1 en ~120 ms, sin rebote.
float OpenProgress(unsigned long long openedAt);

void Gap(float px);
void Divider(float above = 10, float below = 10);
void Wordmark(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 color);
// Título de sección en mayúsculas espaciadas; opcionalmente un enlace a la
// derecha (p. ej. "Ver todos").
void SectionLabel(const char* text, const char* link = nullptr, bool* linkClicked = nullptr);
// Tecla dibujada como texto con borde de 1 px, alineada a la derecha de rightMid.
void DrawKeycap(ImDrawList* dl, ImVec2 rightMid, const std::string& key, ImU32 border, ImU32 fg, float alpha);
float KeycapWidth(const std::string& key);

enum class Tone { Normal, Primary, Danger };

// Botón grande: título, atajo y, si se pide, la marca roja de grabar a la izquierda.
bool ActionButton(const char* id, const char* title, const std::string& key, Tone tone, bool enabled,
                  bool recordMark = false, float width = 0);
// Interruptor rectangular dibujado con el borde derecho en rightMid.
void DrawSwitch(ImDrawList* dl, ImVec2 rightMid, bool on, ImGuiID animId, float alpha);
// Fila con icono, texto y un interruptor; toda la fila es clicable.
bool ToggleRow(const char* id, Icon icon, const char* label, bool on);
// Control segmentado. Devuelve el valor pulsado o -1.
int Segmented(const char* id, const std::vector<std::pair<int, std::string>>& options, int current,
              float width = 0);
bool IconButton(const char* id, Icon icon, float size, const char* tooltip, bool active = false);
// Botón con borde de 1 px y fondo transparente (acciones secundarias).
bool GhostButton(const char* id, const char* label, float width, Tone tone = Tone::Normal, float height = 32);
// Botón relleno con el acento (la acción principal de un formulario).
bool SolidButton(const char* id, const char* label, float width, float height = 32);
// Barra fina de progreso o de uso (0..1).
void Bar(ImVec2 size, float fraction, ImU32 color);

// --------------------------------------------------------- formularios
constexpr float kLabelColumn = 230.0f;
constexpr float kControlWidth = 340.0f;

float ControlWidth();
// Fila de formulario: etiqueta (y ayuda) a la izquierda, control a la derecha.
void FieldLabel(const char* label, const char* help = nullptr);
void Help(const char* text);
void Warning(const char* text);
void ErrorText(const char* text);
// Título de sección: mayúsculas espaciadas y una línea hasta el borde.
void Heading(const char* text);
// Interruptor de ajustes: etiqueta a la izquierda, switch en la columna de controles.
bool SwitchField(const char* label, bool* value, const char* help = nullptr);
// Valor de solo lectura en la columna de controles.
void ValueRow(const char* label, const std::string& value);

template <typename T>
bool ChoiceCombo(const char* id, T& value, const std::vector<std::pair<T, const char*>>& options) {
  const char* current = options.empty() ? "" : options.front().second;
  for (const auto& [v, name] : options)
    if (v == value) current = name;
  bool changed = false;
  if (ImGui::BeginCombo(id, current)) {
    for (const auto& [v, name] : options) {
      if (ImGui::Selectable(name, v == value)) {
        value = v;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Entrada de la columna de navegación (Ajustes de la app).
bool NavItem(int index, const char* name, bool selected, float height = 34);
// Rótulo de grupo en la columna de navegación.
void NavLabel(const char* text);

} // namespace ui
} // namespace spectre
