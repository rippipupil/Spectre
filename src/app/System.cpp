#include "app/System.h"

#include "common/Util.h"

#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <sstream>

namespace spectre {

namespace {

std::string Upper(std::string s) {
  for (auto& c : s) c = char(toupper((unsigned char)c));
  return s;
}

std::string Trim(std::string s) {
  while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
  size_t i = 0;
  while (i < s.size() && isspace((unsigned char)s[i])) ++i;
  return s.substr(i);
}

struct KeyName {
  const char* name;    // en mayúsculas, para comparar
  const char* display; // como se muestra
  UINT vk;
};
const KeyName kKeys[] = {
    {"SPACE", "Space", VK_SPACE},          {"ESPACIO", "Space", VK_SPACE},
    {"TAB", "Tab", VK_TAB},                {"INSERT", "Insert", VK_INSERT},
    {"DELETE", "Delete", VK_DELETE},       {"HOME", "Home", VK_HOME},
    {"END", "End", VK_END},                {"PAGEUP", "PageUp", VK_PRIOR},
    {"PAGEDOWN", "PageDown", VK_NEXT},     {"PAUSE", "Pause", VK_PAUSE},
    {"SCROLLLOCK", "ScrollLock", VK_SCROLL}, {"PRINTSCREEN", "PrintScreen", VK_SNAPSHOT},
    {"IMPR", "PrintScreen", VK_SNAPSHOT},  {"UP", "Up", VK_UP},
    {"DOWN", "Down", VK_DOWN},             {"LEFT", "Left", VK_LEFT},
    {"RIGHT", "Right", VK_RIGHT},          {"`", "`", VK_OEM_3},
    {"-", "-", VK_OEM_MINUS},              {"=", "=", VK_OEM_PLUS},
    {",", ",", VK_OEM_COMMA},              {".", ".", VK_OEM_PERIOD},
};

} // namespace

bool ParseHotkey(const std::string& text, UINT* modifiers, UINT* vk) {
  UINT mods = 0;
  UINT key = 0;
  std::stringstream ss(text);
  std::string part;
  while (std::getline(ss, part, '+')) {
    std::string p = Upper(Trim(part));
    if (p.empty()) return false;
    if (p == "ALT") mods |= MOD_ALT;
    else if (p == "CTRL" || p == "CONTROL") mods |= MOD_CONTROL;
    else if (p == "SHIFT" || p == "MAYUS") mods |= MOD_SHIFT;
    else if (p == "WIN") mods |= MOD_WIN;
    else {
      if (key) return false; // dos teclas normales
      if (p.size() == 1 && (isalnum((unsigned char)p[0]))) key = UINT(p[0]);
      else if (p[0] == 'F' && p.size() <= 3 && isdigit((unsigned char)p[1])) {
        int n = atoi(p.c_str() + 1);
        if (n < 1 || n > 24) return false;
        key = UINT(VK_F1 + n - 1);
      } else if (p.rfind("NUM", 0) == 0 && p.size() == 4 && isdigit((unsigned char)p[3])) {
        key = UINT(VK_NUMPAD0 + (p[3] - '0'));
      } else {
        for (const auto& k : kKeys)
          if (p == k.name) key = k.vk;
        if (!key) return false;
      }
    }
  }
  if (!key) return false;
  // Sin Ctrl, Alt ni Win el atajo se "come" la tecla en todo Windows (no se
  // podría escribir esa letra). Solo se permiten solas F13-F24, Pausa y Bloq Despl.
  bool standalone = (key >= VK_F13 && key <= VK_F24) || key == VK_PAUSE || key == VK_SCROLL;
  if (!(mods & (MOD_ALT | MOD_CONTROL | MOD_WIN)) && !standalone) return false;
  *modifiers = mods | MOD_NOREPEAT;
  *vk = key;
  return true;
}

std::string NormalizeHotkey(const std::string& text) {
  UINT mods = 0, vk = 0;
  if (!ParseHotkey(text, &mods, &vk)) return text;
  std::string out;
  if (mods & MOD_CONTROL) out += "Ctrl+";
  if (mods & MOD_ALT) out += "Alt+";
  if (mods & MOD_SHIFT) out += "Shift+";
  if (mods & MOD_WIN) out += "Win+";
  if (vk >= VK_F1 && vk <= VK_F24) out += "F" + std::to_string(vk - VK_F1 + 1);
  else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) out += "Num" + std::to_string(vk - VK_NUMPAD0);
  else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) out += char(vk);
  else {
    for (const auto& k : kKeys)
      if (k.vk == vk) {
        out += k.display;
        break;
      }
  }
  return out;
}

static std::string ExeNameOf(DWORD pid) {
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return {};
  wchar_t buf[MAX_PATH];
  DWORD n = MAX_PATH;
  std::string name;
  if (QueryFullProcessImageNameW(proc, 0, buf, &n)) name = Narrow(std::filesystem::path(buf).stem().wstring());
  CloseHandle(proc);
  return name;
}

std::optional<ForegroundApp> ForegroundWindowInfo() {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd) return std::nullopt;
  ForegroundApp app;
  app.window = hwnd;
  GetWindowThreadProcessId(hwnd, &app.processId);
  app.exeName = ExeNameOf(app.processId);
  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, 256);
  app.title = Narrow(title);
  return app;
}

std::optional<ForegroundApp> DetectFullscreenApp() {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return std::nullopt;
  wchar_t cls[128] = {};
  GetClassNameW(hwnd, cls, 128);
  for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                               L"Windows.UI.Core.CoreWindow", L"SpectreOverlay"})
    if (wcscmp(cls, shell) == 0) return std::nullopt;

  RECT wr;
  if (!GetWindowRect(hwnd, &wr)) return std::nullopt;
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return std::nullopt;
  const RECT& m = mi.rcMonitor;
  if (wr.left > m.left || wr.top > m.top || wr.right < m.right || wr.bottom < m.bottom) return std::nullopt;

  auto app = ForegroundWindowInfo();
  if (!app || app->processId == GetCurrentProcessId()) return std::nullopt;
  std::string exe = app->exeName;
  for (auto& c : exe) c = char(tolower((unsigned char)c));
  if (exe == "explorer" || exe == "applicationframehost" || exe == "searchhost" || exe == "startmenuexperiencehost")
    return std::nullopt;
  return app;
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool SetAutostart(bool enabled) {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
  LONG r;
  if (enabled) {
    std::wstring cmd = L"\"" + ExecutablePath().wstring() + L"\" --tray";
    r = RegSetValueExW(key, L"Spectre", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       DWORD((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    r = RegDeleteValueW(key, L"Spectre");
    if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return r == ERROR_SUCCESS;
}

bool AutostartEnabled() {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
  bool exists = RegQueryValueExW(key, L"Spectre", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
  RegCloseKey(key);
  return exists;
}

ProcessUsage SampleProcessUsage() {
  static uint64_t lastProc = 0, lastWall = 0;
  ProcessUsage u;
  FILETIME create, exit, kernel, user;
  GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
  auto toU64 = [](FILETIME ft) { return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; };
  uint64_t proc = toU64(kernel) + toU64(user);
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  uint64_t wall = toU64(now);
  if (lastWall && wall > lastWall) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    u.cpuPercent = 100.0 * double(proc - lastProc) / double(wall - lastWall) / double(si.dwNumberOfProcessors);
  }
  lastProc = proc;
  lastWall = wall;
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    u.privateBytes = pmc.PrivateUsage;
    u.workingSet = pmc.WorkingSetSize;
  }
  return u;
}

std::vector<ClipFile> ListClips(const std::string& folder, size_t max) {
  std::vector<std::pair<std::filesystem::file_time_type, ClipFile>> found;
  std::error_code ec;
  // increment(ec) en vez del ++ del range-for, que lanza si la carpeta
  // desaparece a mitad (unidad de red o USB) y tumbaría la app.
  std::filesystem::directory_iterator it(Widen(folder), ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    const auto& entry = *it;
    std::error_code fec; // errores de un archivo concreto: no cortan el listado
    if (!entry.is_regular_file(fec)) continue;
    auto ext = entry.path().extension().wstring();
    for (auto& c : ext) c = wchar_t(towlower(c));
    if (ext != L".mp4") continue;
    ClipFile clip;
    clip.path = Narrow(entry.path().wstring());
    clip.name = Narrow(entry.path().filename().wstring());
    clip.size = entry.file_size(fec);
    found.emplace_back(entry.last_write_time(fec), std::move(clip));
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<ClipFile> out;
  for (auto& [time, clip] : found) {
    if (out.size() >= max) break;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(Widen(clip.path).c_str(), GetFileExInfoStandard, &data)) {
      FILETIME local;
      SYSTEMTIME st;
      FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
      FileTimeToSystemTime(&local, &st);
      char buf[32];
      snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
      clip.when = buf;
    }
    out.push_back(std::move(clip));
  }
  return out;
}

void OpenPath(const std::string& path) {
  ShellExecuteW(nullptr, L"open", Widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowInFolder(const std::string& path) {
  std::wstring args = L"/select,\"" + Widen(path) + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace spectre
