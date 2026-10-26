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
constexpr float kHeadline = 28.0f; // títulos grandes de algunos diseños
constexpr float kHero = 36.0f;     // los más grandes (Plano, Psíquico, Suave, Papel)

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
// La letra de los títulos del diseño (Style::display) al tamaño más cercano.
ImFont* Display(float size);
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
  Shot, Marker, Search, Volume, Muted, Next, Prev, External, Share, Sliders, Undo, Redo,
  Check, Alert, Download, Info, Gamepad, Record, Home, Grid, Gear
};
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float radius, ImU32 color);
// Una raya en diagonal encima de un icono: apagado (micrófono, cámara, replay).
void DrawSlash(ImDrawList* dl, ImVec2 center, float radius, ImU32 color, ImU32 gap);
// Marca de estado con la forma del diseño (cuadrado, rombo, barra o cursor).
// Nunca un círculo de color: siempre va con el texto que explica el estado.
void DrawMark(ImDrawList* dl, ImVec2 center, float half, ImU32 color, bool filled);
// Visto que se dibuja solo: progress 0..1 recorre el trazo.
void DrawCheck(ImDrawList* dl, ImVec2 center, float radius, ImU32 color, float progress = 1.0f);

// ------------------------------------------------- diseño y movimiento
// Cada tema trae un diseño (ui/Theme.h Style): forma de los botones y lo que
// hacen al pasar el ratón y al pulsarlos. Los efectos son unas decenas de
// vértices y solo se dibujan mientras duran.

// Estado animado del último control (justo después de su InvisibleButton):
// ratón encima suavizado, pulsado y cuánto hace de la última pulsación. Se
// guarda en la ventana de ImGui por id de control.
struct Motion {
  float hover = 0;          // 0..1, sube y baja en ~100 ms
  bool held = false;
  float pressAge = 1e9f;    // segundos desde que se pulsó
  float releaseAge = 1e9f;  // segundos desde que se soltó
  ImVec2 at;                // dónde se pulsó (pantalla)
};
Motion Animate(bool enabled = true);
// Curvas de las animaciones (t de 0 a 1): salida rápida y llegada suave, y
// llegada con un pequeño rebote (Suave).
float EaseOut(float t);
float BackOut(float t);
// Algo se está animando: la ventana de la app (que dibuja bajo demanda) pide
// otro frame. BeginFrame lo pone a cero.
void RequestFrame();
bool FrameRequested();

// Papel de un botón: decide sus colores con cualquier diseño.
enum class Role {
  Primary,     // relleno del acento (la acción principal)
  Danger,      // relleno rojo (detener la grabación)
  Normal,      // superficie (acciones grandes secundarias)
  Ghost,       // secundario con borde
  GhostDanger, // secundario con borde y texto rojo
  Quiet,       // sin nada hasta pasar el ratón (iconos, filas)
};
// Fondo del botón según el diseño. Devuelve dónde va el contenido (se hunde,
// tiembla o encoge al pulsar) y sus colores.
struct ButtonPaint {
  ImVec2 a, b;
  ImU32 fg;        // texto e icono
  ImU32 muted;     // texto secundario (tecla)
  ImU32 keyBorder; // borde de la tecla
  float inset;     // margen extra a la izquierda (raíl)
};
ButtonPaint PaintButton(ImDrawList* dl, ImVec2 a, ImVec2 b, Role role, const Motion& m, bool enabled = true);
// Efecto de pulsación por encima del contenido (eco, glitch, barrido, onda...).
void PaintPress(ImDrawList* dl, ImVec2 a, ImVec2 b, Role role, const Motion& m);
// Resalte de una tarjeta o fila con el ratón encima (hover 0..1).
void HoverOutline(ImDrawList* dl, ImVec2 a, ImVec2 b, float hover);

// Formas del diseño: rectángulo, esquinas cortadas o corchetes.
float LineW(); // grosor de bordes y separadores en píxeles
void ShapeFill(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color);
void ShapeStroke(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness = 0);
void DrawBrackets(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float arm, float thickness = 0);
// Caja plana (tarjeta de estado, aviso de Fuente) con una barra de color a la izquierda.
void CardFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 edge);
// Doble contorno de Neón: el de dentro entero y otro 3 px por fuera, tenue.
void DoubleStroke(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 inner, ImU32 outer);
// Fondo de una casilla (números de Inicio) con la forma de tarjeta del diseño.
void TileFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, float hover = 0.0f);

// Tarjeta de un archivo de la biblioteca con la forma del diseño (Style::card):
// dónde van la miniatura y el texto, y el marco por debajo y por encima.
struct MediaCard {
  ImVec2 a, b;           // toda la tarjeta
  ImVec2 thumbA, thumbB; // miniatura
  ImVec2 text;           // primera línea del pie
  float textWidth = 0;
};
float MediaCardHeight(float width);
MediaCard MediaCardRects(ImVec2 a, float width);
// Fondo y marco antes de dibujar la miniatura.
void MediaCardBack(ImDrawList* dl, const MediaCard& card, float hover, bool selected);
// Lo que va encima de la miniatura: esquinas cortadas, corchetes, bordes.
void MediaCardFront(ImDrawList* dl, const MediaCard& card, float hover, bool selected);
// Pie: título, datos y, en Táctico, el número de la tarjeta.
void MediaCardCaption(ImDrawList* dl, const MediaCard& card, const std::string& title, const std::string& detail,
                      float hover, int number);
// Etiqueta pequeña sobre la miniatura (tipo de archivo, duración).
void Tag(ImDrawList* dl, ImVec2 pos, const std::string& text, bool alignRight);
// Alto de los botones grandes y de las filas según la densidad del diseño.
float ActionHeight();
float RowHeight(float logical);

// Rótulos de sección con el estilo del diseño (MAYÚSCULAS ESPACIADAS, // , >,
// solo la primera en mayúscula). El texto se escribe siempre en mayúsculas.
float LabelWidth(const char* upper, float size);
float DrawLabel(ImDrawList* dl, ImVec2 pos, const char* upper, float size, ImU32 color);
std::string Upper(const std::string& text);         // respeta las vocales con tilde y la ñ
std::string Lowercase(const std::string& text);     // "Biblioteca" -> "biblioteca" (también tildes y ñ)
std::string SentenceCase(const std::string& upper); // "DURACIÓN DEL CLIP" -> "Duración del clip"

// Animaciones de Psíquico y Terminal (solo con AmbientMotion()).
// Sombra psíquica: al cambiar la fila bajo el ratón, la anterior se desvanece
// en 200 ms. Una por contexto de ImGui; se reinicia con cada apertura.
void ResetHoverTrail(unsigned long long openedAt);
void TrackHover(ImGuiID id, ImVec2 a, ImVec2 b, bool hovered);
void DrawHoverTrail(ImDrawList* dl);
// Estela: un segmento de 2 px recorre el borde inferior del rectángulo cada 2,8 s.
void DrawTrace(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color);
// Respiración de la marca de estado (2,6 s); 1 sin animaciones.
float Breathe();
// Cursor de Terminal: 1 o 0 cada medio segundo; siempre 1 sin animaciones.
float Blink();
// Animación de entrada: 0 -> 1 en ~120 ms, sin rebote.
float OpenProgress(unsigned long long openedAt);
// Entrada de una página o panel según el diseño: desplazamiento y opacidad
// para los primeros ~250 ms desde `since` (GetTickCount64).
struct Entrance {
  ImVec2 offset;
  float alpha = 1;
  float reveal = 1; // Terminal: fracción de alto ya barrida (0..1)
  bool done = true;
};
Entrance EnterMotion(unsigned long long since, bool fromRight);

// Título de página con el estilo del diseño (Style::title). section: la
// página de la que cuelga (Ajustes), para "~/ajustes/replay" o "03 // AJUSTES".
// index: el número de la página en Táctico (0 = sin número).
void PageTitle(const char* title, const std::string& subtitle, int index = 0, const char* section = nullptr);
// Dónde poner un botón de `height` px a la derecha del título: distancia
// desde arriba del título, centrado en su línea (Papel lleva filetes encima).
float PageTitleActionY(float height);
// Una frase destacada (estado de Inicio) con la letra de títulos del diseño.
// Devuelve el alto.
float Headline(ImDrawList* dl, ImVec2 pos, const std::string& text, ImU32 color, float maxWidth);
float HeadlineHeight();

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
// Duración del clip: rejilla de 4 columnas con Config::kClipLengths. Las que
// pasan de fitSeconds (lo que cabe en la RAM del replay; 0 = no se sabe) se
// ven atenuadas. Devuelve la pulsada o -1.
int ClipLengthPicker(const char* id, int current, double fitSeconds, float width = 0);
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

// Entrada de la columna de navegación (Ajustes de la app). depth 1: un
// apartado dentro de otro (en Terminal se dibuja como una rama del árbol).
bool NavItem(int index, const char* name, bool selected, float height = 34, int depth = 0, bool last = false);
// Rótulo de grupo en la columna de navegación.
void NavLabel(const char* text);
// Raíl de iconos (NavLayout::Rail): icono con el nombre debajo.
bool RailItem(int index, Icon icon, const char* name, bool selected, float height);
// Pestaña de la barra de arriba (NavLayout::Topbar); el ancho sale del texto.
bool TabItem(int index, const char* name, bool selected, float height);
float TabWidth(const char* name);
// Fondo de la zona de navegación (columna, raíl o barra de arriba) con los
// detalles del diseño: borde, franjas de peligro, doble línea...
void NavChrome(ImDrawList* dl, ImVec2 a, ImVec2 b, bool horizontal);
// La marca del raíl: una "S" en un cuadro con la forma del diseño.
void Monogram(ImDrawList* dl, ImVec2 center, float size);

} // namespace ui
} // namespace spectre
