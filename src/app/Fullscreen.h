#pragma once

#include <windows.h>

#include <string>

namespace spectre {

// Cómo ocupa la pantalla una ventana (la del juego): sirve para colocar el
// panel en su monitor, con su DPI, y para saber si hay que mantenerlo por
// encima. Solo consulta ventanas: no abre el proceso del juego ni inyecta nada.
struct FullscreenInfo {
  HWND window = nullptr;      // ventana consultada (nula si no vale: oculta, minimizada o de Spectre)
  HMONITOR monitor = nullptr; // monitor que ocupa; sin ventana, el del cursor
  RECT bounds{};              // rcMonitor de ese monitor, en píxeles físicos
  UINT dpi = 96;              // DPI efectivo del monitor
  bool fullscreen = false;    // cubre el monitor entero: sin bordes, exclusiva o con optimizaciones
  bool topmost = false;       // WS_EX_TOPMOST (GLFW, Unity...): al activarse se pondría delante del panel
  bool exclusive = false;     // el shell dice que hay un Direct3D exclusivo (QUNS_RUNNING_D3D_FULL_SCREEN)
  bool modeChanged = false;   // la resolución actual no es la del escritorio (pantalla completa clásica)
  std::string Describe() const; // una línea para el log
};

// Nunca falla: si la ventana no vale (oculta, minimizada, de Spectre o del
// shell: escritorio, barra de tareas), devuelve fullscreen=false con el monitor
// del cursor. diagnostics=false deja exclusive y modeChanged sin rellenar:
// consultan al shell y al driver. exclusive decide si el panel coge el foco
// (App::ShouldActivateMenu); modeChanged solo sirve para el log.
FullscreenInfo QueryFullscreen(HWND window, bool diagnostics = true);

} // namespace spectre
