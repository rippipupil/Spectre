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
  float panelWidth = 0, panelHeight = 0; // tamaño real del panel en el último frame
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

void DrawPanel(OverlayWindow& window, MenuModel& model, const MenuActions& actions);
// El aviso recibe la configuración activa para dibujarse con su tema.
void DrawToast(OverlayWindow& window, const Config& config, const std::string& text, bool error, float alpha);

} // namespace spectre
