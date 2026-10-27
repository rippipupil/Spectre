#include "app/ShadowPlay.h"

#include "common/Util.h"

#include <shlobj.h>

#include <algorithm>
#include <filesystem>

namespace spectre {

namespace {

// La carpeta de ShadowPlay en su configuración: DefaultPathW es binario con
// el texto en UTF-16 (o una cadena en versiones nuevas), con su cero al final.
std::wstring ConfiguredFolder() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\NVIDIA Corporation\\Global\\ShadowPlay\\NVSPCAPS", 0, KEY_READ,
                    &key) != ERROR_SUCCESS)
    return {};
  std::wstring out;
  DWORD type = 0, size = 0;
  if (RegQueryValueExW(key, L"DefaultPathW", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size >= 4 &&
      size < 64 * 1024 && (type == REG_BINARY || type == REG_SZ || type == REG_EXPAND_SZ)) {
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, L"DefaultPathW", nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) ==
        ERROR_SUCCESS)
      out.assign(buf.data());
  }
  RegCloseKey(key);
  if (out.find(L'%') != std::wstring::npos) {
    wchar_t expanded[MAX_PATH * 2]{};
    if (ExpandEnvironmentStringsW(out.c_str(), expanded, DWORD(std::size(expanded)))) out = expanded;
  }
  return out;
}

std::wstring VideosFolder() {
  PWSTR raw = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &raw)) && raw) out = raw;
  CoTaskMemFree(raw);
  return out;
}

bool IsFolder(const std::wstring& path) {
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Lower(std::wstring s) {
  if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size()));
  return s;
}

DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                            LPVOID data) {
  return static_cast<const std::atomic<bool>*>(data)->load() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

// La fecha de modificación del original: la biblioteca ordena por ella.
void KeepDate(const std::filesystem::path& file, int64_t modified) {
  HANDLE h = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  FILETIME ft;
  ft.dwLowDateTime = DWORD(uint64_t(modified) & 0xFFFFFFFFu);
  ft.dwHighDateTime = DWORD(uint64_t(modified) >> 32);
  SetFileTime(h, nullptr, nullptr, &ft);
  CloseHandle(h);
}

} // namespace

std::vector<std::string> ShadowPlayFolders(const std::string& custom) {
  std::vector<std::wstring> candidates;
  if (!custom.empty()) {
    candidates.push_back(Widen(custom));
  } else {
    candidates.push_back(ConfiguredFolder());
    std::wstring videos = VideosFolder();
    if (!videos.empty()) {
      candidates.push_back(videos + L"\\NVIDIA");
      candidates.push_back(videos);
    }
  }
  std::vector<std::string> out;
  std::vector<std::wstring> seen;
  for (auto& dir : candidates) {
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty() || !IsFolder(dir)) continue;
    std::wstring key = Lower(dir);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
    seen.push_back(key);
    out.push_back(Narrow(dir));
  }
  return out;
}

ShadowPlayImport ImportShadowPlay(Library& library, const std::vector<MediaItem>& items, const std::string& outputDir,
                                  bool perGame, bool move, const std::atomic<bool>& cancel,
                                  const std::function<void(int, int)>& progress) {
  ShadowPlayImport r;
  const std::filesystem::path root(Widen(outputDir));
  const int total = int(items.size());
  int done = 0;
  for (const auto& item : items) {
    if (cancel) break;
    if (progress) progress(done, total);
    ++done;
    std::filesystem::path dir = root;
    if (perGame && !item.game.empty()) dir /= Widen(SanitizeFileName(item.game));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path from(Widen(item.path));
    const std::filesystem::path to = dir / from.filename();
    if (std::filesystem::equivalent(from, to, ec)) {
      ++r.skipped; // ya está en la carpeta de Spectre
      continue;
    }
    // Ya importado (mismo nombre y tamaño). Uno más pequeño es una copia que
    // se cortó a medias: se sustituye.
    bool replace = false;
    if (std::filesystem::exists(to, ec)) {
      uint64_t size = std::filesystem::file_size(to, ec);
      if (!ec && size == item.size) {
        ++r.skipped;
        continue;
      }
      if (ec || size > item.size) {
        ++r.failed;
        if (r.error.empty()) r.error = "Ya hay otro archivo llamado " + item.name + " en la carpeta de Spectre.";
        continue;
      }
      replace = true;
    }
    BOOL ok;
    void* data = const_cast<std::atomic<bool>*>(&cancel);
    if (move)
      ok = MoveFileWithProgressW(from.c_str(), to.c_str(), CopyProgress, data,
                                 MOVEFILE_COPY_ALLOWED | (replace ? MOVEFILE_REPLACE_EXISTING : 0));
    else
      ok = CopyFileExW(from.c_str(), to.c_str(), CopyProgress, data, nullptr, 0);
    if (!ok) {
      DWORD e = GetLastError();
      if (e == ERROR_REQUEST_ABORTED || cancel) break;
      ++r.failed;
      Log("ShadowPlay: no se pudo %s %s (%lu)", move ? "mover" : "copiar", item.name.c_str(), e);
      if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) {
        r.error = "No queda espacio en el disco de la carpeta de Spectre.";
        break;
      }
      if (r.error.empty())
        r.error = e == ERROR_SHARING_VIOLATION ? item.name + " está abierto en otro programa."
                                               : "No se pudo " + std::string(move ? "mover " : "copiar ") + item.name +
                                                     " (error " + std::to_string(e) + ").";
      continue;
    }
    KeepDate(to, item.modified);
    library.CarryMeta(item.path, Narrow(to.wstring()), !move);
    ++r.copied;
    r.bytes += item.size;
  }
  if (progress) progress(done, total);
  Log("ShadowPlay: %s %d (%s), %d ya estaban, %d fallaron", move ? "movidos" : "copiados", r.copied,
      FormatBytes(r.bytes).c_str(), r.skipped, r.failed);
  return r;
}

} // namespace spectre
