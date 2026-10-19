#include "app/Fullscreen.h"

#include "common/Util.h"

#include <shellapi.h>
#include <shellscalingapi.h>

#include <cstdio>

namespace spectre {

namespace {
bool OwnWindow(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return pid == GetCurrentProcessId();
}

// Un juego "a pantalla completa" clásico (OpenGL, D3D9) cambia la resolución
// del monitor; la de registro es la del escritorio.
bool ModeDiffersFromDesktop(const MONITORINFOEXW& mi) {
  DEVMODEW current{}, desktop{};
  current.dmSize = desktop.dmSize = sizeof(DEVMODEW);
  if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &current) ||
      !EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desktop))
    return false;
  return current.dmPelsWidth != desktop.dmPelsWidth || current.dmPelsHeight != desktop.dmPelsHeight;
}
} // namespace

FullscreenInfo QueryFullscreen(HWND window) {
  FullscreenInfo info;
  if (window && (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) || OwnWindow(window)))
    window = nullptr;
  HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
  if (!monitor) {
    POINT cursor{};
    GetCursorPos(&cursor);
    monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  }
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi)) {
    mi.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    mi.rcWork = mi.rcMonitor;
  }
  info.monitor = monitor;
  info.bounds = mi.rcMonitor;
  UINT dpiX = 96, dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = GetDpiForSystem();
  info.dpi = dpiX;
  if (!window) return info;

  info.window = window;
  RECT wr{};
  LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if (GetWindowRect(window, &wr)) {
    const RECT& m = mi.rcMonitor;
    // Cubre el monitor entero (los bordes pueden sobresalir). Es lo que hacen
    // la pantalla completa exclusiva, la "sin bordes" y la emulada por Windows.
    // Una ventana maximizada con barra de título es una app normal (con la
    // barra de tareas oculta también cubriría el monitor).
    bool covers = wr.left <= m.left && wr.top <= m.top && wr.right >= m.right && wr.bottom >= m.bottom;
    bool framed = IsZoomed(window) && (style & WS_CAPTION) == WS_CAPTION;
    info.fullscreen = covers && !framed;
  }
  info.topmost = (ex & WS_EX_TOPMOST) != 0;
  if (info.fullscreen) {
    // Solo informativo: con las optimizaciones de pantalla completa de Windows
    // el shell puede decir "exclusivo" aunque DWM siga componiendo encima.
    QUERY_USER_NOTIFICATION_STATE state{};
    info.exclusive = SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
    info.modeChanged = ModeDiffersFromDesktop(mi);
  }
  return info;
}

std::string FullscreenInfo::Describe() const {
  wchar_t cls[64] = {};
  if (window) GetClassNameW(window, cls, 64);
  char buf[320];
  snprintf(buf, sizeof(buf), "ventana %p (%s) %s%s%s%s, monitor %ldx%ld en (%ld,%ld), %u ppp", static_cast<void*>(window),
           Narrow(cls).c_str(), fullscreen ? "a pantalla completa" : "en ventana", topmost ? ", topmost" : "",
           exclusive ? ", D3D exclusivo según el shell" : "", modeChanged ? ", resolución cambiada" : "",
           bounds.right - bounds.left, bounds.bottom - bounds.top, bounds.left, bounds.top, dpi);
  return buf;
}

} // namespace spectre
