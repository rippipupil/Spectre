#pragma once

#include "app/Config.h"
#include "app/Fullscreen.h"
#include "app/Library.h"
#include "app/System.h"
#include "app/Updater.h"
#include "core/Pipeline.h"
#include "ui/AppUi.h"
#include "ui/AppWindow.h"
#include "ui/KeyboardHook.h"
#include "ui/Menu.h"
#include "ui/OverlayWindow.h"

#include <shellapi.h>

#include <deque>
#include <functional>
#include <memory>
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
  void SetQuality(const std::string& quality);
  void CaptureScreenshot();
  void AddMarker();
  void TrimClip(const std::string& path, double start, double end);
  void ToggleMenu();
  void OpenMenu();
  // restoreFocus=false: el programa que se acaba de abrir desde el panel
  // (reproductor, Explorador, la ventana de Spectre) se queda con el foco en
  // vez del juego.
  void CloseMenu(bool restoreFocus = true);
  void EndMenuSession(bool giveFocusBack); // la parte de CloseMenu que no depende de la ventana
  void ApplyConfig(const Config& next, bool quiet = false);
  void Quit();
  // Hay una actualización descargada esperando a que acabe la grabación.
  void InstallPendingUpdate();

  // Estado
  bool WantPipeline() const;
  void UpdatePipeline(bool forceRestart = false);
  bool StartPipeline();
  PipelineSettings BuildSettings() const;
  bool GameActive() const;
  // Ruta nueva en la carpeta de destino (o en la del juego): Clip_<juego>_<fecha>.mp4
  std::string OutputName(const char* prefix, const char* extension = ".mp4");
  bool RegisterHotkeys();
  void UnregisterHotkeys();
  // Mientras se graba un atajo en Ajustes, los globales no se quedan con la combinación.
  void SuspendHotkeys(bool suspend);
  void CheckGame();
  // Límite de espacio de Ajustes > Almacenamiento, en un hilo aparte.
  void EnforceStorageLimit();
  // " Queda poco espacio..." para añadir al aviso de un clip o una grabación (vacío si sobra).
  std::string LowDiskNote();
  // La grabación terminó (atajo, panel o captura perdida): marcas, biblioteca y límite de espacio.
  void OnRecordingStopped(const std::string& path);

  // UI
  void Toast(const std::string& text, bool error = false);
  void SyncModels(); // el config activo cambió: panel y ventana lo muestran
  void RefreshMenuModel();
  void RefreshPanelClips();
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
  bool ShouldActivateMenu(const FullscreenInfo& screen);
  void SyncKeyboardHook();
  std::vector<KeyboardHook::Hotkey> PassThroughHotkeys() const;
  void WatchGameWhileOpen();
  void PollClickOutside();
  // Las acciones del menú y de la ventana se ejecutan después del frame de
  // ImGui: cerrar o redimensionar una ventana (o crear la de un aviso) en
  // mitad del frame tumbaba la app.
  void Defer(std::function<void()> action) { deferred_.push_back(std::move(action)); }
  void RunDeferred();

  // Ventana de Spectre (la app). Solo existe mientras está abierta.
  void OpenApp(std::optional<AppPage> page = std::nullopt);
  void CloseApp();
  void AppCloseRequested(); // X o Alt+F4: a la bandeja o salir, según Ajustes > General
  void RenderApp();
  void RefreshAppModel();
  void RescanLibrary();
  void BeginSettingsEdit(); // copia de trabajo de Ajustes = config activo
  void ApplySettings();
  void LoadDevices();
  void ChooseOutputFolder();

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  UINT taskbarCreated_ = 0;
  NOTIFYICONDATAW tray_{};
  bool quitting_ = false;

  Config config_;
  Pipeline pipeline_;
  std::optional<ForegroundApp> game_;
  ULONGLONG lastGameSeen_ = 0;

  OverlayWindow menu_;
  OverlayWindow toast_;
  MenuModel menuModel_;
  MenuActions menuActions_;
  std::vector<std::function<void()>> deferred_;
  bool inFrame_ = false;        // dentro de un frame de ImGui (ver Toast)
  HWND previousForeground_ = nullptr;
  HMONITOR menuMonitor_ = nullptr;
  FullscreenInfo menuScreen_;   // lo que había debajo al abrir el panel (juego a pantalla completa, su monitor)
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
  std::string hotkeyError_;     // atajos que no se pudieron registrar
  unsigned hotkeyFailedMask_ = 0; // bit i: el atajo i de HotkeyConfig no se pudo registrar
  bool hotkeysSuspended_ = false;
  Updater updater_;
  std::wstring installerPath_; // actualización descargada: se lanza al terminar de cerrar
  bool relaunchAfterUpdate_ = false; // la salida la pidió la actualización, no el usuario

  // Marcas (atajo de marca): momentos de la partida en QPC. Al guardar un clip
  // entran las de sus últimos segundos; las de una grabación, desde que empezó.
  std::deque<int64_t> markers_;
  int64_t recordStartQpc_ = 0;
  std::vector<double> recordMarkers_;
  ULONGLONG lowDiskWarnedAt_ = 0;

  // Panel sobre el juego. Abierto sin foco (juego a pantalla completa), el
  // teclado llega por el gancho; con foco, el gancho no se instala.
  KeyboardHook keyboard_;
  bool menuActivated_ = false;    // el panel cogió el foco al abrirse; se repite al recrearlo
  ULONGLONG menuOpenedAt_ = 0;
  bool menuFocusChecked_ = false; // ya se comprobó que el foco llegó al panel tras abrirlo
  bool gameRestored_ = false;     // el juego minimizado al perder el foco ya se restauró
  bool mouseWasDown_ = false;     // sondeo del clic fuera del panel
  RECT menuHitScreen_{};          // zona del panel que recibe clics, en pantalla
  bool menuHitValid_ = false;

  // Ventana de Spectre
  std::shared_ptr<Library> library_ = std::make_shared<Library>(); // también la usa el hilo del límite de espacio
  AppWindow appWindow_;
  AppUi appUi_;
  AppModel appModel_;
  AppActions appActions_;
  ULONGLONG lastAppStats_ = 0;
  ULONGLONG lastAppUsage_ = 0;
  ULONGLONG lastAppFrame_ = 0;
};

} // namespace spectre
