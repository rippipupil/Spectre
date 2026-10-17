#include "app/App.h"
#include "app/CrashHandler.h"
#include "common/Util.h"

#include <mfapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <winrt/base.h>

extern "C" {
#include <libavutil/log.h>
}

#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
  using namespace spectre;
  InstallCrashHandler();

  // Una sola instancia: si ya hay una, le pedimos que abra el menú.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SpectreSingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(L"SpectreMain", nullptr)) {
      // Le cedemos el primer plano: si no, Windows no deja que su panel coja el foco.
      DWORD pid = 0;
      GetWindowThreadProcessId(other, &pid);
      AllowSetForegroundWindow(pid);
      PostMessageW(other, WM_SPECTRE_SHOW_MENU, 0, 0);
    }
    return 0;
  }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  const bool mf = MediaFoundationAvailable();
  if (mf) MFStartup(MF_VERSION, MFSTARTUP_LITE);
  else Log("Media Foundation no está instalado (Windows N): la webcam no estará disponible");
  av_log_set_level(AV_LOG_WARNING);
  av_log_set_callback([](void*, int level, const char* fmt, va_list args) {
    if (level > av_log_get_level()) return;
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, args);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (!text.empty()) Log("ffmpeg: %s", text.c_str());
  });

  std::wstring args = cmdLine ? cmdLine : L"";
  bool tray = args.find(L"--tray") != std::wstring::npos;
  Log("Spectre %s iniciado", SPECTRE_VERSION);

  int code;
  {
    App app;
    try {
      code = app.Run(instance, tray);
    } catch (const std::exception& e) {
      Log("CRASH: excepción C++ en el hilo principal: %s", e.what());
      throw; // std::terminate -> volcado
    }
  }
  Log("Spectre cerrado (código %d)", code);

  if (mf) MFShutdown();
  winrt::uninit_apartment();
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  return code;
}
