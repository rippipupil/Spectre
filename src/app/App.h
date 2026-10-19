#pragma once

#include "app/Config.h"
#include "app/System.h"
#include "app/Updater.h"
#include "core/Pipeline.h"
#include "ui/KeyboardHook.h"
#include "ui/Menu.h"
#include "ui/OverlayWindow.h"

#include <shellapi.h>

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace spectre {

class App {
public:
  int Run(HINSTANCE instance, bool startInTray, bool updated);

private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  // Acciones
  void SaveClip();
  void ToggleRecording();
  void ToggleReplay();
  void ToggleMicrophone();
  void ToggleCamera();
  void SetClipSeconds(int seconds);
  void ToggleMenu();
  void OpenMenu();
  void OpenSettings();
  void BackToPanel();
  // restoreFocus=false: el programa que se acaba de abrir desde el panel
  // (reproductor, Explorador) se queda con el foco en vez del juego.
  void CloseMenu(bool restoreFocus = true);
  void ApplyConfig(const Config& next, bool quiet = false);
  void Quit();
  // Hay una actualización descargada esperando a que acabe la grabación.
  void InstallPendingUpdate();

  // Estado
  bool WantPipeline() const;
  void UpdatePipeline(bool forceRestart = false);
  bool StartPipeline();
  PipelineSettings BuildSettings() const;
  std::string OutputName(const char* prefix);
  bool RegisterHotkeys();
  void UnregisterHotkeys();
  void CheckGame();

  // UI
  void Toast(const std::string& text, bool error = false);
  void RefreshMenuModel(bool devices);
  void AddTrayIcon();
  void RemoveTrayIcon();
  void ShowTrayMenu();
  void RenderOverlays();
  RECT MenuRect() const;
  void PlaceMenu(bool activate);
  // Si el panel coge el foco al abrirse (escritorio, pantalla completa
  // exclusiva, juego que encierra el cursor en parte del monitor) o no (juego
  // a pantalla completa con el cursor libre, aunque esté escondido: el teclado
  // llega entonces por KeyboardHook).
  bool ShouldActivateMenu(HWND foreground);
  void SyncKeyboardHook();
  std::vector<KeyboardHook::Hotkey> PassThroughHotkeys() const;
  void WatchGameWhileOpen();
  void PollClickOutside();
  // Las acciones del menú se ejecutan después del frame de ImGui: cerrar o
  // redimensionar la ventana en mitad del frame tumbaba la app.
  void Defer(std::function<void()> action) { deferred_.push_back(std::move(action)); }
  void RunDeferred();

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  UINT taskbarCreated_ = 0;
  NOTIFYICONDATAW tray_{};
  bool quitting_ = false;

  Config config_;
  Pipeline pipeline_;
  std::optional<ForegroundApp> game_;
  ULONGLONG lastGameSeen_ = 0;

  enum class MenuMode { Panel, Settings };

  OverlayWindow menu_;
  OverlayWindow toast_;
  MenuMode menuMode_ = MenuMode::Panel;
  MenuModel menuModel_;
  MenuActions menuActions_;
  std::vector<std::function<void()>> deferred_;
  HWND previousForeground_ = nullptr;
  HMONITOR menuMonitor_ = nullptr;
  UINT menuGeneration_ = 0;     // descarta avisos de cierre de un menú anterior
  ULONGLONG menuClosedAt_ = 0;
  ULONGLONG startFailedAt_ = 0; // la captura falló al arrancar: no reintentar en bucle
  DWORD pipelinePid_ = 0;       // proceso cuyo audio se captura (0 = todo el sistema)
  HWND pipelineWindow_ = nullptr; // ventana pedida al arrancar (nula = monitor)
  std::deque<std::string> recentNames_; // nombres ya usados aunque el archivo aún no exista
  bool restartPending_ = false; // cambios de captura a aplicar al terminar la grabación
  std::string toastText_;
  bool toastError_ = false;
  ULONGLONG toastUntil_ = 0;
  ULONGLONG lastStats_ = 0;
  Updater updater_;
  std::wstring installerPath_; // actualización descargada: se lanza al terminar de cerrar
  bool relaunchAfterUpdate_ = false; // la salida la pidió la actualización, no el usuario

  // Panel sobre el juego. Abierto sin foco (juego a pantalla completa), el
  // teclado llega por el gancho; con foco, el gancho no se instala.
  KeyboardHook keyboard_;
  bool menuActivated_ = false;    // el panel cogió el foco al abrirse
  ULONGLONG menuOpenedAt_ = 0;
  bool menuFocusChecked_ = false; // ya se comprobó que el foco llegó al panel tras abrirlo
  bool gameRestored_ = false;     // el juego minimizado al perder el foco ya se restauró
  bool mouseWasDown_ = false;     // sondeo del clic fuera del panel o de Ajustes
  RECT menuHitScreen_{};          // zona del panel (o la ventana de Ajustes) que recibe clics, en pantalla
  bool menuHitValid_ = false;
};

} // namespace spectre
