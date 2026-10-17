#pragma once

#include "app/Config.h"
#include "app/System.h"
#include "core/Pipeline.h"
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
  int Run(HINSTANCE instance, bool startInTray);

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
  void CloseMenu();
  void ApplyConfig(const Config& next, bool quiet = false);
  void Quit();

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
};

} // namespace spectre
