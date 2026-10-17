#include "common/Util.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>

namespace spectre {

std::wstring Widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
  return out;
}

std::string Narrow(std::wstring_view wide) {
  if (wide.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

int64_t QpcNow() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

int64_t QpcFrequency() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  return freq;
}

static std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) result = raw;
  CoTaskMemFree(raw);
  return result;
}

std::filesystem::path AppDataDir() {
  auto dir = KnownFolder(FOLDERID_RoamingAppData) / L"Spectre";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::filesystem::path DefaultOutputDir() {
  auto videos = KnownFolder(FOLDERID_Videos);
  if (videos.empty()) videos = KnownFolder(FOLDERID_Profile) / L"Videos";
  return videos / L"Spectre";
}

std::filesystem::path ExecutablePath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return std::filesystem::path(std::wstring(buf, n));
}

std::string TimestampForFile() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
           st.wMinute, st.wSecond);
  return buf;
}

std::string SanitizeFileName(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
        c == '*' || (unsigned char)c < 32)
      out += '_';
    else
      out += c;
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  return out.empty() ? "Spectre" : out;
}

std::string FormatBytes(uint64_t bytes) {
  char buf[32];
  if (bytes >= (1ull << 30))
    snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / double(1ull << 30));
  else if (bytes >= (1ull << 20))
    snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / double(1ull << 20));
  else
    snprintf(buf, sizeof(buf), "%.0f KB", double(bytes) / 1024.0);
  return buf;
}

void Log(const char* fmt, ...) {
  static std::mutex mutex;
  static std::ofstream file;
  char msg[2048];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[2200];
  snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond,
           st.wMilliseconds, msg);

  std::lock_guard lock(mutex);
  if (!file.is_open()) {
    auto path = AppDataDir() / L"spectre.log";
    // El log se rota simplemente al pasar de 2 MB para no crecer sin límite.
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > 2 * 1024 * 1024) std::filesystem::remove(path, ec);
    file.open(path, std::ios::app);
  }
  file << line;
  file.flush();
  OutputDebugStringA(line);
}

std::recursive_mutex& GpuMutex() {
  static std::recursive_mutex m;
  return m;
}

bool MediaFoundationAvailable() {
  static const bool available = [] {
    for (const wchar_t* dll : {L"mfplat.dll", L"mfreadwrite.dll", L"mf.dll"})
      if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return false;
    return true;
  }();
  return available;
}

std::atomic<int>& PendingWrites() {
  static std::atomic<int> count{0};
  return count;
}

void WaitForPendingWrites(DWORD timeoutMs) {
  ULONGLONG until = GetTickCount64() + timeoutMs;
  while (PendingWrites() > 0 && GetTickCount64() < until) Sleep(50);
}

} // namespace spectre
