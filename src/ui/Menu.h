#pragma once

#include "app/Config.h"
#include "app/System.h"
#include "core/Pipeline.h"

#include <functional>
#include <string>
#include <vector>

namespace spectre {

class OverlayWindow;

// Datos del panel rápido (Alt+Z): una mini app sobre el juego para guardar
// clips, grabar, hacer capturas y tocar los ajustes de la partida. El resto
// de ajustes y la biblioteca están en la ventana de Spectre.
struct MenuModel {
  Config config; // copia del config activo
  PipelineStats stats;
  bool replayActive = false;
  bool gameDetected = false;
  std::string gameName;
  std::vector<ClipFile> clips; // recientes
  std::string outputDir;

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
};

// Ancho del panel rápido en píxeles lógicos (se multiplica por la escala DPI).
constexpr float kPanelWidth = 320.0f;
// Separación del panel compacto respecto a los bordes del monitor (lógicos).
constexpr float kPanelMargin = 18.0f;
// Lateral: todo el alto del monitor, como el overlay de NVIDIA. Compacto (el
// de antes): franja estrecha con el panel del alto de su contenido.
inline bool IsLateralPanel(const Config& config) { return config.panelLayout != "compacto"; }

void DrawPanel(OverlayWindow& window, MenuModel& model, const MenuActions& actions);

} // namespace spectre
