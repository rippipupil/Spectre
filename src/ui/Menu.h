#pragma once

#include "app/Config.h"
#include "app/Library.h"
#include "app/System.h"
#include "core/Pipeline.h"

#include <functional>
#include <string>
#include <vector>

namespace spectre {

class OverlayWindow;
class PanelMedia;

// Datos del panel rápido (Alt+Z): una mini app sobre el juego para guardar
// clips, grabar, hacer capturas, ver y compartir lo último que se guardó y
// tocar los ajustes de la partida. El resto de ajustes y la biblioteca están
// en la ventana de Spectre.
struct MenuModel {
  Config config; // copia del config activo
  PipelineStats stats;
  bool replayActive = false;
  bool gameDetected = false;
  std::string gameName;
  std::vector<MediaItem> clips; // recientes (vídeos)
  std::string outputDir;
  // «Para Discord» en marcha (App lo pone en cada frame): el vídeo y el %; -1 = ninguna.
  std::string exportPath;
  int exportPercent = -1;

  // Clip abierto en el panel (clic en un reciente): se ve y se comparte sin
  // salir del juego. Es una copia: la lista de recientes puede cambiar debajo.
  bool viewing = false;
  MediaItem view;
  bool scrubbing = false;        // arrastrando la barra de la vista previa
  bool scrubWasPlaying = false;

  // Estado de la interfaz
  unsigned long long openedAt = 0; // GetTickCount64 al abrir (animación de entrada)
  bool confirmSource = false;      // se pulsó Fuente: el aviso espera Activar o Cancelar
  float panelWidth = 0, panelHeight = 0; // tamaño real del panel en el último frame
  float panelTop = 0;              // borde superior del panel dentro de la ventana en el último frame

  // Arrastre: DrawPanel lo empieza (clic en la cabecera o en un hueco del
  // panel) y lo termina al soltar; mientras tanto App mueve la ventana con el
  // cursor y actualiza la posición de config.
  bool dragging = false;
  float dragFromX = 0, dragFromY = 0; // dónde se pulsó, en coordenadas de la ventana
};

// Cómo va el enlace de la nube de un clip (lo da MenuActions::linkState).
struct PanelLinkState {
  bool configured = false; // hay nube en Ajustes; si no, el botón lo explica
  bool busy = false;       // en la cola o subiendo
  bool queued = false;     // en la cola, todavía sin subir
  bool copies = false;     // al acabar se copia el enlace (ya se pidió compartir)
  float progress = 0;      // 0..1 mientras sube
  bool ready = false;      // ya subido: el botón copia el enlace al momento
  std::string error;       // la última subida falló
};

// Todas las acciones se ejecutan después del frame de ImGui (App las aplaza):
// pueden cerrar o redimensionar la ventana sin romper el frame en curso.
struct MenuActions {
  std::function<void()> saveClip;
  std::function<void()> toggleRecording;
  std::function<void()> screenshot;
  std::function<void()> toggleReplay;
  std::function<void()> toggleMic;
  std::function<void()> toggleCamera;
  std::function<void(int)> setClipSeconds;
  std::function<void(const std::string&)> setQuality;
  std::function<void()> openApp;     // la ventana de Spectre (cierra el panel)
  std::function<void()> openLibrary; // la ventana de Spectre en la biblioteca
  std::function<void(const std::string&)> openClip; // la ventana de Spectre con ese clip en el visor
  std::function<void()> close;
  std::function<void(const std::string&)> openPath;
  std::function<void(const std::string&)> showInFolder;
  // Compartir sin salir del juego (no cierran el panel).
  std::function<void(const std::string&)> copyFile;         // al portapapeles, para Ctrl+V en Discord
  std::function<void(const std::string&)> exportForDiscord; // copia de menos de 20 MB, copiada al acabar
  std::function<void()> cancelExport;
  std::function<void(const std::string&, bool)> setFavorite;
  // Enlace de la nube (app/Cloud): sube el clip si hace falta y copia el
  // enlace. Sin poner, el panel no enseña el botón.
  std::function<void(const std::string&)> shareLink;
  std::function<PanelLinkState(const std::string&)> linkState;
};

// Ancho del panel rápido en píxeles lógicos (se multiplica por la escala DPI).
constexpr float kPanelWidth = 320.0f;
// Separación del panel compacto respecto a los bordes del monitor (lógicos).
constexpr float kPanelMargin = 18.0f;
// Lateral: todo el alto del monitor, como el overlay de NVIDIA. Compacto (el
// de antes): franja estrecha con el panel del alto de su contenido.
inline bool IsLateralPanel(const Config& config) { return config.panelLayout != "compacto"; }

// media: miniaturas y vista previa de los clips (las crea en la GPU del panel).
void DrawPanel(OverlayWindow& window, MenuModel& model, const MenuActions& actions, PanelMedia& media);

} // namespace spectre
