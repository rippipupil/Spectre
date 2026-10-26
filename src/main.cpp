#include "app/App.h"
#include "app/CrashHandler.h"
#include "capture/Camera.h"
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

  std::wstring commandLine = cmdLine ? cmdLine : L"";
  bool tray = commandLine.find(L"--tray") != std::wstring::npos;          // inicio con Windows
  bool updated = commandLine.find(L"--updated") != std::wstring::npos;    // lo pasa el instalador tras actualizar

  // Una sola instancia: si ya hay una, se le pide que traiga su ventana al
  // frente (abrir Spectre otra vez desde el menú Inicio o el exe).
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SpectreSingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (tray) return 0;
    // Recién abierta (doble clic dos veces seguidas), su ventana oculta puede
    // no existir todavía: se espera un poco.
    HWND other = FindWindowW(L"SpectreMain", nullptr);
    for (int i = 0; !other && i < 40; ++i) {
      Sleep(50);
      other = FindWindowW(L"SpectreMain", nullptr);
    }
    if (other) {
      // Le cedemos el primer plano: si no, Windows no deja que su ventana se ponga delante.
      DWORD pid = 0;
      GetWindowThreadProcessId(other, &pid);
      AllowSetForegroundWindow(pid);
      PostMessageW(other, WM_SPECTRE_SHOW_APP, 0, 0);
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

  Log("Spectre %s iniciado%s", SPECTRE_VERSION, tray ? " (con Windows)" : updated ? " (tras actualizar)" : "");

  int code;
  {
    App app;
    try {
      code = app.Run(instance, tray, updated);
    } catch (const std::exception& e) {
      Log("CRASH: excepción C++ en el hilo principal: %s", e.what());
      throw; // std::terminate -> volcado
    }
  }
  Log("Spectre cerrado (código %d)", code);

  // Los hilos de las cámaras ya paradas usan Media Foundation hasta que el
  // driver las suelta.
  if (mf) Camera::WaitForThreads(2000);
  if (mf) MFShutdown();
  winrt::uninit_apartment();
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  return code;
}
