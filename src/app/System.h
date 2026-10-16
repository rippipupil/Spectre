#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spectre {

// "Alt+Shift+F10" -> modificadores de RegisterHotKey + tecla virtual.
bool ParseHotkey(const std::string& text, UINT* modifiers, UINT* vk);
std::string NormalizeHotkey(const std::string& text);

// App a pantalla completa en primer plano (juego, vídeo...).
struct ForegroundApp {
  HWND window = nullptr;
  DWORD processId = 0;
  std::string exeName; // sin extensión, p. ej. "VALORANT-Win64-Shipping"
  std::string title;
};
std::optional<ForegroundApp> DetectFullscreenApp();
std::optional<ForegroundApp> ForegroundWindowInfo();

bool SetAutostart(bool enabled);
bool AutostartEnabled();

struct ProcessUsage {
  double cpuPercent = 0;
  uint64_t privateBytes = 0;
  uint64_t workingSet = 0;
};
// Llamar periódicamente; la CPU se calcula respecto a la llamada anterior.
ProcessUsage SampleProcessUsage();

struct ClipFile {
  std::string path;
  std::string name;
  uint64_t size = 0;
  std::string when;
};
std::vector<ClipFile> ListClips(const std::string& folder, size_t max);

void OpenPath(const std::string& path);
void ShowInFolder(const std::string& path);

} // namespace spectre
