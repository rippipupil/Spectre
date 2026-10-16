#pragma once

#include "app/Config.h"
#include "app/System.h"
#include "core/Pipeline.h"
#include "ui/Menu.h"
#include "ui/OverlayWindow.h"

#include <shellapi.h>

#include <optional>
#include <string>

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
  void ToggleMenu();
  void OpenMenu();
  void CloseMenu();
  void ApplyConfig(const Config& next);
  void Quit();

  // Estado
  bool WantPipeline() const;
  void UpdatePipeline(bool forceRestart = false);
  bool StartPipeline();
  PipelineSettings BuildSettings() const;
  std::string OutputName(const char* prefix) const;
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
  HWND previousForeground_ = nullptr;
  std::string toastText_;
  bool toastError_ = false;
  ULONGLONG toastUntil_ = 0;
  ULONGLONG lastStats_ = 0;
};

} // namespace spectre
