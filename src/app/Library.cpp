#include "app/Library.h"

#include "common/Util.h"
#include "core/Mp4Writer.h"

#include <nlohmann/json.hpp>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <utility>

namespace spectre {

using Microsoft::WRL::ComPtr;

namespace {

constexpr const char* kMonths[] = {"ene", "feb", "mar", "abr", "may", "jun",
                                   "jul", "ago", "sep", "oct", "nov", "dic"};

std::string LowerPath(const std::string& path) {
  std::wstring w = Widen(path);
  if (!w.empty()) CharLowerBuffW(w.data(), DWORD(w.size()));
  return Narrow(w);
}

std::wstring LowerExt(const std::wstring& name) {
  size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring::npos) return {};
  std::wstring ext = name.substr(dot);
  CharLowerBuffW(ext.data(), DWORD(ext.size()));
  return ext;
}

// 0 = no es de la biblioteca, 1 = vídeo, 2 = imagen
int ClassifyExtension(const std::wstring& name) {
  std::wstring ext = LowerExt(name);
  // .m4a: «Solo audio» del editor; .gif: GIF del editor (se ve como una imagen).
  if (ext == L".mp4" || ext == L".mkv" || ext == L".mov" || ext == L".m4a") return 1;
  if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif") return 2;
  return 0;
}

bool IsTimestampAt(const std::string& s, size_t p) {
  // 2026-10-16_05-30-12
  static const char shape[] = "dddd-dd-dd_dd-dd-dd";
  if (p + 19 > s.size()) return false;
  for (size_t i = 0; i < 19; ++i) {
    char c = s[p + i];
    if (shape[i] == 'd' ? !isdigit((unsigned char)c) : c != shape[i]) return false;
  }
  return true;
}

void FillDates(MediaItem& item) {
  FILETIME ft, local;
  ft.dwLowDateTime = DWORD(uint64_t(item.modified) & 0xFFFFFFFFu);
  ft.dwHighDateTime = DWORD(uint64_t(item.modified) >> 32);
  SYSTEMTIME st;
  if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return;
  char buf[64];
  snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
  item.when = buf;
  snprintf(buf, sizeof(buf), "%d %s %d, %02d:%02d", st.wDay, kMonths[std::clamp<int>(st.wMonth, 1, 12) - 1], st.wYear,
           st.wHour, st.wMinute);
  item.date = buf;
}

std::filesystem::path LibraryPath() { return AppDataDir() / L"library.json"; }

bool DigitsAt(const std::string& s, size_t p, size_t n) {
  if (p + n > s.size()) return false;
  for (size_t i = 0; i < n; ++i)
    if (!isdigit((unsigned char)s[p + i])) return false;
  return true;
}

// Fecha de ShadowPlay que acaba en `end`: "2026.10.16" (o "10.16.2026" en
// las versiones antiguas de GeForce Experience). Devuelve dónde empieza.
size_t ShadowPlayDateBefore(const std::string& s, size_t end) {
  if (end < 10) return std::string::npos;
  size_t p = end - 10;
  if (DigitsAt(s, p, 4) && s[p + 4] == '.' && DigitsAt(s, p + 5, 2) && s[p + 7] == '.' && DigitsAt(s, p + 8, 2))
    return p;
  if (DigitsAt(s, p, 2) && s[p + 2] == '.' && DigitsAt(s, p + 3, 2) && s[p + 5] == '.' && DigitsAt(s, p + 6, 4))
    return p;
  return std::string::npos;
}

// Hora de ShadowPlay desde p hasta el final: "05.30.12" con centésimas o sin ellas.
bool ShadowPlayTimeAt(const std::string& s, size_t p) {
  if (!DigitsAt(s, p, 2) || p + 8 > s.size() || s[p + 2] != '.' || !DigitsAt(s, p + 3, 2) || s[p + 5] != '.' ||
      !DigitsAt(s, p + 6, 2))
    return false;
  p += 8;
  if (p == s.size()) return true;
  return s[p] == '.' && p + 1 < s.size() && s.size() - p - 1 <= 3 && DigitsAt(s, p + 1, s.size() - p - 1);
}

bool EndsWithNoCase(const std::string& s, const char* suffix) {
  size_t n = strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suffix[i])) return false;
  return true;
}

// "<juego> 2026.10.16 - 05.30.12.02[.DVR]" (vídeo; .DVR = Instant Replay) o
// "<juego> Screenshot 2026.10.16 - 05.30.12.02" (captura).
bool ParseShadowPlayName(std::string stem, bool image, MediaItem* item) {
  const bool replay = !image && EndsWithNoCase(stem, ".DVR");
  if (replay) stem.resize(stem.size() - 4);
  size_t dash = stem.rfind(" - ");
  if (dash == std::string::npos || !ShadowPlayTimeAt(stem, dash + 3)) return false;
  size_t date = ShadowPlayDateBefore(stem, dash);
  if (date == std::string::npos || date == 0 || stem[date - 1] != ' ') return false;
  std::string game = stem.substr(0, date - 1);
  if (image && EndsWithNoCase(game, " Screenshot")) game.resize(game.size() - 11);
  while (!game.empty() && game.back() == ' ') game.pop_back();
  // Lo del escritorio: "Desktop" en inglés (y "Escritorio" en otros idiomas).
  if (game == "Desktop" || game == "Escritorio") game.clear();
  item->game = game;
  item->kind = image ? MediaKind::Screenshot : replay ? MediaKind::Clip : MediaKind::Recording;
  item->nvidia = true;
  return true;
}

// "..._discord" o "..._discord_2" (ver ExportPathFor).
bool IsExportStem(const std::string& stem) {
  size_t at = stem.rfind("_discord");
  if (at == std::string::npos) return false;
  size_t end = at + 8;
  if (end == stem.size()) return true;
  if (stem[end] != '_' || end + 1 == stem.size()) return false;
  for (size_t i = end + 1; i < stem.size(); ++i)
    if (!isdigit((unsigned char)stem[i])) return false;
  return true;
}

} // namespace

void ParseMediaName(const std::string& fileName, MediaItem* item) {
  std::string stem = fileName;
  size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos) stem.erase(dot);
  item->stem = stem;
  item->game.clear();
  item->spectre = false;
  item->nvidia = false;
  const std::wstring ext = LowerExt(Widen(fileName));
  bool image = ClassifyExtension(Widen(fileName)) == 2;
  item->kind = image ? MediaKind::Screenshot : MediaKind::Clip;
  item->audioOnly = ext == L".m4a";
  // GIF hecho en el editor a partir de un clip: es de Spectre aunque sea una imagen.
  const bool editedGif = ext == L".gif" && stem.find("_editado_") != std::string::npos;

  struct Prefix {
    const char* text;
    MediaKind kind;
  };
  const Prefix prefixes[] = {{"Clip_", MediaKind::Clip},
                             {"Grabacion_", MediaKind::Recording},
                             {"Captura_", MediaKind::Screenshot}};
  for (const auto& p : prefixes) {
    size_t n = strlen(p.text);
    if (stem.compare(0, n, p.text) != 0) continue;
    // <prefijo><juego>_<fecha>[_n][_recorte_<fecha>[_n]]
    for (size_t i = n; i + 19 <= stem.size(); ++i) {
      if (stem[i - 1] != '_' || !IsTimestampAt(stem, i)) continue;
      if (i - 1 > n) item->game = stem.substr(n, i - 1 - n);
      item->kind = p.kind;
      item->spectre = (p.kind == MediaKind::Screenshot) == image || editedGif;
      break;
    }
    break;
  }
  if (item->spectre && item->kind != MediaKind::Screenshot && stem.find("_recorte_") != std::string::npos)
    item->kind = MediaKind::Trim;
  // Unidos, en cámara lenta, divididos o del editor (app/Edit.h, app/VideoEdit.h).
  if (item->spectre && item->kind != MediaKind::Screenshot &&
      (stem.find("_union_") != std::string::npos || stem.find("_lenta") != std::string::npos ||
       stem.find("_editado_") != std::string::npos || stem.find("_parte") != std::string::npos))
    item->kind = MediaKind::Edit;
  if (item->spectre && item->kind != MediaKind::Screenshot && IsExportStem(stem)) item->kind = MediaKind::Export;
  // Las imágenes se abren siempre como imagen, aunque el nombre empiece por Clip_.
  if (image) item->kind = MediaKind::Screenshot;
  if (item->game == "Escritorio") item->game.clear();
  // Un .m4a o un .gif no salen de ShadowPlay.
  if (!item->spectre && !item->audioOnly && ext != L".gif") ParseShadowPlayName(stem, image, item);
}

const char* MediaKindName(MediaKind kind) {
  switch (kind) {
  case MediaKind::Clip: return "Clip";
  case MediaKind::Recording: return "Grabación";
  case MediaKind::Trim: return "Recorte";
  case MediaKind::Export: return "Para Discord";
  case MediaKind::Edit: return "Editado";
  case MediaKind::Screenshot: return "Captura";
  }
  return "";
}

// ---------------------------------------------------------------- metadatos

std::string Library::Key(const std::string& path) { return LowerPath(path); }

void Library::Load() {
  if (loaded_) return;
  loaded_ = true;
  std::ifstream in(LibraryPath());
  if (!in) return;
  try {
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
    for (auto it = j["items"].begin(); it != j["items"].end(); ++it) {
      const auto& v = it.value();
      Meta m;
      m.favorite = v.value("fav", false);
      m.duration = v.value("dur", 0.0);
      m.size = v.value("size", uint64_t(0));
      m.modified = v.value("mtime", int64_t(0));
      if (v.contains("marks")) m.markers = v["marks"].get<std::vector<double>>();
      if (v.contains("tail")) m.tailMarkers = v["tail"].get<std::vector<double>>();
      meta_[it.key()] = std::move(m);
    }
  } catch (const std::exception& e) {
    Log("library.json inválido, se ignora: %s", e.what());
    meta_.clear();
  }
}

void Library::Save() {
  nlohmann::json items = nlohmann::json::object();
  for (const auto& [key, m] : meta_) {
    if (m.Empty()) continue;
    nlohmann::json v = nlohmann::json::object();
    if (m.favorite) v["fav"] = true;
    if (m.duration > 0) {
      v["dur"] = m.duration;
      v["size"] = m.size;
      v["mtime"] = m.modified;
    }
    if (!m.markers.empty()) v["marks"] = m.markers;
    if (!m.tailMarkers.empty()) v["tail"] = m.tailMarkers;
    items[key] = std::move(v);
  }
  nlohmann::json j = {{"version", 1}, {"items", std::move(items)}};
  dirty_ = false;
  auto path = LibraryPath();
  auto tmp = path;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return;
    out << j.dump();
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) Log("No se pudo guardar library.json: %s", ec.message().c_str());
}

void Library::Apply(MediaItem& item) {
  auto it = meta_.find(Key(item.path));
  if (it == meta_.end()) return;
  const Meta& m = it->second;
  item.favorite = m.favorite;
  // La duración vale mientras el archivo sea el mismo que se midió.
  if (m.duration > 0 && m.size == item.size && m.modified == item.modified) item.duration = m.duration;
  item.markers = m.markers;
  if (item.duration > 0)
    for (double t : m.tailMarkers) {
      double at = item.duration - t;
      if (at >= 0 && at <= item.duration) item.markers.push_back(at);
    }
  std::sort(item.markers.begin(), item.markers.end());
}

std::vector<MediaItem> Library::Scan(const std::string& folder, size_t max, bool videosOnly) {
  std::vector<MediaItem> items;
  std::vector<std::wstring> dirs = {Widen(folder)};
  for (size_t d = 0; d < dirs.size(); ++d) {
    WIN32_FIND_DATAW fd;
    std::wstring pattern = dirs[d] + L"\\*";
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) continue;
    do {
      if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
        continue;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        // Una subcarpeta por juego (Ajustes > Clips); no se baja más.
        if (d == 0 && !(fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
          dirs.push_back(dirs[0] + L"\\" + fd.cFileName);
        continue;
      }
      int cls = ClassifyExtension(fd.cFileName);
      if (cls == 0 || (videosOnly && cls != 1)) continue;
      MediaItem item;
      item.path = Narrow(dirs[d] + L"\\" + fd.cFileName);
      item.name = Narrow(fd.cFileName);
      item.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
      item.modified = int64_t((uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime);
      ParseMediaName(item.name, &item);
      items.push_back(std::move(item));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
  }
  std::sort(items.begin(), items.end(), [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
  if (max > 0 && items.size() > max) items.resize(max);
  std::lock_guard lock(mutex_);
  Load();
  for (auto& item : items) {
    FillDates(item);
    Apply(item);
  }
  return items;
}

std::vector<MediaItem> Library::ScanShadowPlay(const std::vector<std::string>& folders, const std::string& skipFolder) {
  std::vector<MediaItem> items;
  // Vídeos\NVIDIA sale dos veces (sola y como subcarpeta de Vídeos): cada archivo, una.
  std::unordered_set<std::string> seen;
  const std::string skip = skipFolder.empty() ? std::string() : Key(skipFolder);
  // La carpeta de Spectre o una de dentro (Vídeos\Spectre cuando ShadowPlay usa Vídeos).
  auto skipped = [&](const std::wstring& dir) {
    if (skip.empty()) return false;
    std::string key = Key(Narrow(dir));
    return key == skip || (key.size() > skip.size() && key.compare(0, skip.size(), skip) == 0 &&
                           (key[skip.size()] == '\\' || key[skip.size()] == '/'));
  };
  for (const auto& folder : folders) {
    std::vector<std::wstring> dirs = {Widen(folder)};
    if (skipped(dirs[0])) continue;
    for (size_t d = 0; d < dirs.size(); ++d) {
      WIN32_FIND_DATAW fd;
      std::wstring pattern = dirs[d] + L"\\*";
      HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                     FIND_FIRST_EX_LARGE_FETCH);
      if (find == INVALID_HANDLE_VALUE) continue;
      do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
          continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          // ShadowPlay guarda una subcarpeta por juego; no se baja más.
          std::wstring sub = dirs[0] + L"\\" + fd.cFileName;
          if (d == 0 && !(fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && !skipped(sub))
            dirs.push_back(std::move(sub));
          continue;
        }
        if (ClassifyExtension(fd.cFileName) == 0) continue;
        MediaItem item;
        item.name = Narrow(fd.cFileName);
        ParseMediaName(item.name, &item);
        // Solo lo que lleva el nombre de ShadowPlay: Vídeos puede tener de todo.
        if (!item.nvidia) continue;
        item.path = Narrow(dirs[d] + L"\\" + fd.cFileName);
        if (!seen.insert(Key(item.path)).second) continue;
        item.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        item.modified = int64_t((uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime);
        items.push_back(std::move(item));
      } while (FindNextFileW(find, &fd));
      FindClose(find);
    }
  }
  std::sort(items.begin(), items.end(), [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
  std::lock_guard lock(mutex_);
  Load();
  for (auto& item : items) {
    FillDates(item);
    Apply(item);
  }
  return items;
}

void Library::SetFavorite(const std::string& path, bool favorite) {
  std::lock_guard lock(mutex_);
  Load();
  meta_[Key(path)].favorite = favorite;
  Save();
}

void Library::SetFavorite(const std::vector<std::string>& paths, bool favorite) {
  if (paths.empty()) return;
  std::lock_guard lock(mutex_);
  Load();
  for (const auto& path : paths) meta_[Key(path)].favorite = favorite;
  Save();
}

void Library::SetTailMarkers(const std::string& path, std::vector<double> secondsBeforeEnd) {
  if (secondsBeforeEnd.empty()) return;
  std::lock_guard lock(mutex_);
  Load();
  meta_[Key(path)].tailMarkers = std::move(secondsBeforeEnd);
  Save();
}

void Library::SetDuration(const std::string& path, uint64_t size, int64_t modified, double seconds) {
  if (seconds <= 0) return;
  std::lock_guard lock(mutex_);
  Load();
  Meta& m = meta_[Key(path)];
  if (m.duration == seconds && m.size == size && m.modified == modified) return;
  m.duration = seconds;
  m.size = size;
  m.modified = modified;
  dirty_ = true;
}

void Library::Flush() {
  std::lock_guard lock(mutex_);
  if (dirty_) Save();
}

void Library::Annotate(MediaItem& item) {
  std::lock_guard lock(mutex_);
  Load();
  Apply(item);
}

bool Library::Rename(const std::string& path, const std::string& newStem, std::string* newPath, std::string* error) {
  std::string stem = newStem;
  while (!stem.empty() && isspace((unsigned char)stem.back())) stem.pop_back();
  while (!stem.empty() && isspace((unsigned char)stem.front())) stem.erase(stem.begin());
  if (stem.empty()) {
    *error = "Escribe un nombre.";
    return false;
  }
  stem = SanitizeFileName(stem);
  std::filesystem::path from(Widen(path));
  std::filesystem::path to = from.parent_path() / (Widen(stem) + from.extension().wstring());
  if (to == from) {
    *newPath = path;
    return true;
  }
  std::error_code ec;
  // Mismo nombre con otras mayúsculas: Windows lo trata como el mismo archivo.
  bool caseOnly = LowerPath(Narrow(to.wstring())) == LowerPath(path);
  if (!caseOnly && std::filesystem::exists(to, ec)) {
    *error = "Ya hay un archivo con ese nombre.";
    return false;
  }
  if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
    DWORD e = GetLastError();
    *error = e == ERROR_SHARING_VIOLATION ? "El archivo está abierto en otro programa."
                                          : "No se pudo renombrar (error " + std::to_string(e) + ").";
    return false;
  }
  *newPath = Narrow(to.wstring());
  std::lock_guard lock(mutex_);
  Load();
  auto it = meta_.find(Key(path));
  if (it != meta_.end()) {
    Meta m = std::move(it->second);
    meta_.erase(it);
    meta_[Key(*newPath)] = std::move(m);
    Save();
  }
  return true;
}

void Library::CarryMeta(const std::string& from, const std::string& to, bool keepSource) {
  std::lock_guard lock(mutex_);
  Load();
  auto it = meta_.find(Key(from));
  if (it == meta_.end()) return;
  Meta m = it->second;
  if (!keepSource) meta_.erase(it);
  meta_[Key(to)] = std::move(m);
  Save();
}

void Library::Forget(const std::vector<std::string>& paths) {
  std::lock_guard lock(mutex_);
  Load();
  bool changed = false;
  for (const auto& path : paths) changed = meta_.erase(Key(path)) > 0 || changed;
  if (changed) Save();
}

bool Library::Recycle(HWND owner, const std::string& path, std::string* error, bool* cancelled) {
  return !Recycle(owner, std::vector<std::string>{path}, error, cancelled).empty();
}

std::vector<std::string> Library::Recycle(HWND owner, const std::vector<std::string>& paths, std::string* error,
                                          bool* cancelled) {
  *cancelled = false;
  error->clear();
  std::vector<std::string> gone;
  if (paths.empty()) return gone;
  std::wstring from;
  for (const auto& path : paths) {
    from += Widen(path);
    from.push_back(L'\0'); // cada ruta termina en \0 y c_str() pone el último
  }
  SHFILEOPSTRUCTW op{};
  op.hwnd = owner;
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  // Cuando el archivo no puede ir a la papelera, NOCONFIRMATION solo lo
  // borraría para siempre sin preguntar; WANTNUKEWARNING hace que Windows
  // pregunte antes. Sin NOERRORUI: si el archivo está abierto, Windows dice
  // en qué programa. Con muchos archivos puede tardar unos segundos: entonces
  // se ve la barra de progreso de Windows.
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING;
  if (paths.size() <= 10) op.fFlags |= FOF_SILENT;
  int r = SHFileOperationW(&op);
  for (const auto& path : paths) {
    std::wstring file = Widen(path);
    bool exists = GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES ||
                  (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND);
    if (!exists) gone.push_back(path);
  }
  if (gone.size() < paths.size()) {
    // Alguno sigue ahí: se dijo que no (aquí o en el aviso de archivo abierto) o falló.
    if (op.fAnyOperationsAborted || r == ERROR_CANCELLED || r == 0x75 /* DE_OPCANCELLED */) {
      *cancelled = true;
      *error = "No se ha borrado.";
    } else {
      *error = paths.size() == 1 ? "No se pudo mover a la papelera (¿está abierto en otro programa?)."
                                 : "Algunos no se pudieron mover a la papelera (¿están abiertos en otro programa?).";
    }
  }
  if (!gone.empty()) {
    std::lock_guard lock(mutex_);
    Load();
    bool changed = false;
    for (const auto& path : gone) changed = meta_.erase(Key(path)) > 0 || changed;
    if (changed) Save();
  }
  return gone;
}

LimitResult Library::EnforceLimit(const std::string& folder, uint64_t limitBytes,
                                  const std::vector<std::string>& keepPaths, int maxAgeDays) {
  LimitResult r;
  r.limitBytes = limitBytes;
  r.maxAgeDays = maxAgeDays;
  if (limitBytes == 0 && maxAgeDays <= 0) return r;
  auto items = Scan(folder);
  // Lo de los últimos 2 minutos puede estar escribiéndose todavía y no se toca.
  FILETIME nowFt;
  GetSystemTimeAsFileTime(&nowFt);
  int64_t now = int64_t((uint64_t(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime);
  const int64_t recent = int64_t(2) * 60 * 10000000;
  std::vector<std::string> keep;
  for (const auto& path : keepPaths)
    if (!path.empty()) keep.push_back(Key(path));
  auto kept = [&](const MediaItem& item) {
    return item.favorite || now - item.modified < recent ||
           (!keep.empty() && std::find(keep.begin(), keep.end(), Key(item.path)) != keep.end());
  };
  auto remove = [&](const MediaItem& item, const char* why) {
    if (!DeleteFileW(Widen(item.path).c_str())) return false; // abierto en otro programa
    r.freedBytes += item.size;
    if (r.deleted == 0) r.kind = item.kind;
    else if (item.kind != r.kind) r.mixed = true;
    ++r.deleted;
    Log("%s: borrado %s (%s)", why, item.name.c_str(), FormatBytes(item.size).c_str());
    std::lock_guard lock(mutex_);
    meta_.erase(Key(item.path));
    return true;
  };
  // Ajustes > Almacenamiento > Borrar lo antiguo: lo de Spectre con más días
  // que el tope, salvo favoritos y la grabación en curso.
  std::vector<bool> gone(items.size(), false);
  if (maxAgeDays > 0) {
    const int64_t maxAge = int64_t(maxAgeDays) * 24 * 3600 * 10000000;
    for (size_t i = 0; i < items.size(); ++i) {
      const MediaItem& item = items[i];
      if (!item.spectre || kept(item) || now - item.modified <= maxAge) continue;
      if (remove(item, "Borrado de lo antiguo")) {
        gone[i] = true;
        ++r.agedDeleted;
      }
    }
  }
  if (limitBytes > 0) {
    // Solo cuenta lo que lleva el nombre de Spectre: lo renombrado y lo de
    // otros programas no se puede borrar, y si contara, el tope no bajaría
    // nunca y se acabaría borrando todo lo demás.
    for (size_t i = 0; i < items.size(); ++i) {
      const MediaItem& item = items[i];
      if (!item.spectre || gone[i]) continue;
      r.usedBytes += item.size;
      if (!kept(item)) continue;
      r.keptBytes += item.size;
      if (item.favorite) r.favoriteBytes += item.size;
    }
    // Si lo que no se puede borrar ya pasa del tope, borrar el resto tampoco
    // bastaría: no se borra nada más y App avisa.
    if (r.usedBytes > limitBytes && r.keptBytes > limitBytes) {
      r.blocked = true;
    } else {
      // Del más antiguo al más reciente.
      for (size_t i = items.size(); i-- > 0 && r.usedBytes > limitBytes;) {
        const MediaItem& item = items[i];
        if (!item.spectre || gone[i] || kept(item)) continue;
        if (remove(item, "Límite de espacio")) r.usedBytes -= item.size;
      }
    }
  }
  if (r.deleted > 0) {
    std::lock_guard lock(mutex_);
    Save();
  }
  return r;
}

// ------------------------------------------------------------------ utilidades

bool CopyFileToClipboard(HWND owner, const std::string& path) {
  return CopyFilesToClipboard(owner, std::vector<std::string>{path});
}

bool CopyFilesToClipboard(HWND owner, const std::vector<std::string>& paths) {
  if (paths.empty()) return false;
  std::wstring list; // rutas separadas por \0; el cero final lo pone GMEM_ZEROINIT
  for (const auto& path : paths) {
    list += Widen(path);
    list.push_back(L'\0');
  }
  size_t bytes = sizeof(DROPFILES) + (list.size() + 1) * sizeof(wchar_t);
  HGLOBAL drop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!drop) return false;
  auto* df = static_cast<DROPFILES*>(GlobalLock(drop));
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
  GlobalUnlock(drop);
  // "Copiar" (no "cortar"): el Explorador lo pega como copia.
  HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  if (effect) {
    *static_cast<DWORD*>(GlobalLock(effect)) = DROPEFFECT_COPY;
    GlobalUnlock(effect);
  }
  if (!OpenClipboard(owner)) {
    GlobalFree(drop);
    if (effect) GlobalFree(effect);
    return false;
  }
  EmptyClipboard();
  bool ok = SetClipboardData(CF_HDROP, drop) != nullptr;
  if (!ok) GlobalFree(drop);
  if (effect && !SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect)) GlobalFree(effect);
  CloseClipboard();
  return ok;
}

bool CopyTextToClipboard(HWND owner, const std::string& text, bool privateText) {
  std::wstring w = Widen(text);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (!mem) return false;
  memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(mem);
  if (!OpenClipboard(owner)) {
    GlobalFree(mem);
    return false;
  }
  EmptyClipboard();
  bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
  if (!ok) GlobalFree(mem);
  if (ok && privateText) {
    // Formatos que el historial y la sincronización del portapapeles respetan.
    for (const wchar_t* format : {L"ExcludeClipboardContentFromMonitorProcessing", L"CanIncludeInClipboardHistory",
                                  L"CanUploadToCloudClipboard"}) {
      HGLOBAL zero = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
      if (zero && !SetClipboardData(RegisterClipboardFormatW(format), zero)) GlobalFree(zero);
    }
  }
  CloseClipboard();
  return ok;
}

std::string ClipboardText(HWND owner) {
  std::string out;
  if (!OpenClipboard(owner)) return out;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
      out = Narrow(text);
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return out;
}

StorageInfo SummarizeStorage(const std::vector<MediaItem>& items, const std::string& folder) {
  StorageInfo info;
  for (const auto& item : items) {
    if (!item.spectre) {
      info.otherBytes += item.size;
      ++info.others;
      continue;
    }
    info.usedBytes += item.size;
    if (item.favorite) info.favoriteBytes += item.size;
    else info.deletable.emplace_back(item.modified, item.size);
    if (item.Video()) ++info.videos;
    else ++info.screenshots;
  }
  ULARGE_INTEGER avail{}, total{}, totalFree{};
  if (GetDiskFreeSpaceExW(Widen(folder).c_str(), &avail, &total, &totalFree)) {
    info.freeBytes = avail.QuadPart;
    info.totalBytes = total.QuadPart;
    info.valid = true;
  }
  return info;
}

uint64_t FreeDiskBytes(const std::string& folder) {
  ULARGE_INTEGER avail{}, total{}, totalFree{};
  if (!GetDiskFreeSpaceExW(Widen(folder).c_str(), &avail, &total, &totalFree)) return 0;
  return avail.QuadPart;
}

bool HasRecycleBin(const std::string& path) {
  std::wstring w = Widen(path);
  if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') return false; // \\servidor\carpeta
  wchar_t root[MAX_PATH];
  if (!GetVolumePathNameW(w.c_str(), root, MAX_PATH)) return true; // no se sabe: Windows avisará si hace falta
  return GetDriveTypeW(root) == DRIVE_FIXED;
}

// ------------------------------------------------------------------ trabajador

void MediaWorker::Start(HWND notifyWindow, UINT notifyMsg) {
  if (thread_.joinable()) {
    {
      std::lock_guard lock(mutex_);
      if (!stop_) return; // ya está en marcha
    }
    thread_.join(); // el de la vez anterior, que estaba acabando su último trabajo
  }
  notify_ = notifyWindow;
  msg_ = notifyMsg;
  stop_ = false;
  results_.clear();
  thread_ = std::thread([this] { Loop(); });
}

void MediaWorker::Stop(bool wait) {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    queue_.clear();
    results_.clear(); // después de stop_ el hilo ya no añade nada
  }
  cv_.notify_all();
  if (wait) thread_.join();
}

void MediaWorker::Request(Job job, const std::string& path, int width, int height) {
  {
    std::lock_guard lock(mutex_);
    for (const auto& r : queue_)
      if (r.job == job && r.path == path) return;
    queue_.push_front({job, path, width, height});
  }
  cv_.notify_one();
}

void MediaWorker::CancelAll() {
  std::lock_guard lock(mutex_);
  queue_.clear();
}

std::vector<MediaWorker::Result> MediaWorker::Take() {
  std::lock_guard lock(mutex_);
  return std::exchange(results_, {});
}

void MediaWorker::Loop() {
  // MTA: las extensiones del shell que necesiten STA se alojan solas en otro hilo.
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    Request_ request;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) break;
      request = std::move(queue_.front());
      queue_.pop_front();
    }
    Result result = Run(request);
    bool wake = false;
    {
      std::lock_guard lock(mutex_);
      if (stop_) break;
      wake = results_.empty();
      results_.push_back(std::move(result));
    }
    if (wake && notify_) PostMessageW(notify_, msg_, 0, 0);
  }
  if (SUCCEEDED(co)) CoUninitialize();
}

namespace {

bool ShellThumbnail(const std::wstring& path, int width, int height, MediaWorker::Result* out) {
  ComPtr<IShellItemImageFactory> factory;
  if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
  HBITMAP bitmap = nullptr;
  // THUMBNAILONLY: sin miniatura no se quiere el icono genérico del archivo.
  if (FAILED(factory->GetImage(SIZE{width, height}, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &bitmap)) || !bitmap)
    return false;
  BITMAP bm{};
  bool ok = false;
  if (GetObjectW(bitmap, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight != 0) {
    int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // de arriba abajo
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out->bgra.resize(size_t(w) * size_t(h) * 4);
    HDC dc = GetDC(nullptr);
    ok = GetDIBits(dc, bitmap, 0, UINT(h), out->bgra.data(), &bi, DIB_RGB_COLORS) == h;
    ReleaseDC(nullptr, dc);
    if (ok) {
      // Las miniaturas de vídeo vienen sin alfa (0): son opacas.
      for (size_t i = 3; i < out->bgra.size(); i += 4) out->bgra[i] = 255;
      out->width = w;
      out->height = h;
    }
  }
  DeleteObject(bitmap);
  return ok;
}

bool DecodeImage(const std::wstring& path, MediaWorker::Result* out) {
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
    return false;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)))
    return false;
  UINT w = 0, h = 0;
  frame->GetSize(&w, &h);
  if (w == 0 || h == 0) return false;
  ComPtr<IWICBitmapSource> source = frame;
  // Una textura de D3D11 nivel 10 admite hasta 8192 px por lado.
  const UINT maxSide = 8192;
  if (w > maxSide || h > maxSide) {
    float k = float(maxSide) / float(std::max(w, h));
    UINT sw = std::max(1u, UINT(float(w) * k)), sh = std::max(1u, UINT(float(h) * k));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(source.Get(), sw, sh, WICBitmapInterpolationModeFant)))
      return false;
    source = scaler;
    w = sw;
    h = sh;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(wic->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
    return false;
  out->bgra.resize(size_t(w) * size_t(h) * 4);
  if (FAILED(converter->CopyPixels(nullptr, w * 4, UINT(out->bgra.size()), out->bgra.data()))) return false;
  out->width = int(w);
  out->height = int(h);
  return true;
}

} // namespace

MediaWorker::Result MediaWorker::Run(const Request_& request) {
  Result r;
  r.job = request.job;
  r.path = request.path;
  try {
    switch (request.job) {
    case Job::Thumbnail:
      r.ok = ShellThumbnail(Widen(request.path), request.width, request.height, &r);
      break;
    case Job::Duration:
      r.ok = ProbeDuration(request.path, &r.duration) && r.duration > 0;
      break;
    case Job::Image:
      r.ok = DecodeImage(Widen(request.path), &r);
      break;
    }
  } catch (const std::exception& e) {
    Log("Biblioteca: error con %s: %s", request.path.c_str(), e.what());
    r.ok = false;
  }
  if (!r.ok) r.bgra.clear();
  return r;
}

} // namespace spectre
