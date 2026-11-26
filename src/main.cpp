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

#include <filesystem>
#include <string>

namespace {

// Lo que el instalador apartó como .old porque un Spectre anterior aún lo
// tenía abierto (installer/spectre.iss, MoveAsideInUseFiles). Si ese proceso
// sigue vivo no se puede borrar: se reintenta en el próximo arranque. Solo en
// la copia instalada: la carpeta de la portable la elige el usuario.
void RemoveMovedAsideFiles() {
  std::error_code ec;
  const auto dir = spectre::ExecutablePath().parent_path();
  if (!std::filesystem::exists(dir / L"unins000.exe", ec)) return;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::wstring name = it->path().filename().wstring();
    auto endsWith = [&name](const wchar_t* suffix) {
      const size_t n = wcslen(suffix);
      return name.size() > n && _wcsicmp(name.c_str() + name.size() - n, suffix) == 0;
    };
    if (!endsWith(L".exe.old") && !endsWith(L".dll.old")) continue;
    std::error_code removeError;
    if (std::filesystem::remove(it->path(), removeError))
      spectre::Log("Borrado %s, que apartó una actualización", spectre::Narrow(name).c_str());
  }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
  using namespace spectre;
  InstallCrashHandler();

  std::wstring commandLine = cmdLine ? cmdLine : L"";
  bool tray = commandLine.find(L"--tray") != std::wstring::npos;          // inicio con Windows
  bool updated = commandLine.find(L"--updated") != std::wstring::npos;    // lo pasa el instalador tras actualizar
  // El instalador no pudo actualizar y vuelve a abrir la versión que había.
  bool updateFailed = commandLine.find(L"--update-failed") != std::wstring::npos;

  // Una sola instancia: si ya hay una, se le pide que traiga su ventana al
  // frente (abrir Spectre otra vez desde el menú Inicio o el exe).
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SpectreSingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (tray) return 0;
    // Recién abierta (doble clic dos veces seguidas), su ventana oculta puede
    // no existir todavía (se crea nada más arrancar). O la otra ya no tiene
    // ventana y está terminando de cerrarse (tras actualizar, o si el driver
    // de la cámara la retiene): en cuanto suelte el mutex, esta pasa a ser la
    // instancia. El Spectre que lanza una actualización destruye su ventana
    // antes: una que aparezca tras el instalador es otro Spectre abierto.
    HWND other = nullptr;
    bool owned = false;
    for (int i = 0; i < 100; ++i) {
      if ((other = FindWindowW(L"SpectreMain", nullptr)) != nullptr) break;
      DWORD wait = WaitForSingleObject(mutex, 100);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        owned = true;
        break;
      }
    }
    if (other) {
      // Le cedemos el primer plano: si no, Windows no deja que su ventana se ponga delante.
      DWORD pid = 0;
      GetWindowThreadProcessId(other, &pid);
      AllowSetForegroundWindow(pid);
      PostMessageW(other, WM_SPECTRE_SHOW_APP, 0, 0);
      return 0;
    }
    // 10 s sin ventana: lo que queda es un proceso que no termina de salir
    // (ya no graba, ni tiene atajos ni bandeja). Spectre se abre igualmente.
    if (!owned) Log("Otro Spectre sigue sin terminar de cerrarse: se abre igualmente");
  }
  RemoveMovedAsideFiles();

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

  Log("Spectre %s iniciado%s", SPECTRE_VERSION,
      tray ? " (con Windows)" : updated ? " (tras actualizar)" : updateFailed ? " (la actualización no se instaló)" : "");

  int code;
  {
    App app;
    try {
      code = app.Run(instance, tray, updated, updateFailed);
    } catch (const std::exception& e) {
      Log("CRASH: excepción C++ en el hilo principal: %s", e.what());
      throw; // std::terminate -> volcado
    }
  }
  Log("Spectre cerrado (código %d)", code);

  // Los hilos de las cámaras ya paradas usan Media Foundation hasta que el
  // driver las suelta.
  if (mf && !Camera::WaitForThreads(2000)) {
    // Uno sigue dentro del driver. MFShutdown o la descarga de las DLL al
    // salir pueden quedarse esperándolo, y el proceso seguiría vivo sin
    // ventana: con el exe y las DLL en uso (una actualización no podría
    // reemplazarlas) y el mutex tomado (Spectre no se volvería a abrir).
    // Todo está guardado y cerrado: se termina ya.
    Log("Spectre termina sin esperar a la cámara");
    TerminateProcess(GetCurrentProcess(), UINT(code));
  }
  if (mf) MFShutdown();
  winrt::uninit_apartment();
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  return code;
}
