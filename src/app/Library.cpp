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
  if (ext == L".mp4" || ext == L".mkv" || ext == L".mov") return 1;
  if (ext == L".png" || ext == L".jpg" || ext == L".jpeg") return 2;
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

} // namespace

void ParseMediaName(const std::string& fileName, MediaItem* item) {
  std::string stem = fileName;
  size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos) stem.erase(dot);
  item->stem = stem;
  item->game.clear();
  item->spectre = false;
  bool image = ClassifyExtension(Widen(fileName)) == 2;
  item->kind = image ? MediaKind::Screenshot : MediaKind::Clip;

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
      item->spectre = (p.kind == MediaKind::Screenshot) == image;
      break;
    }
    break;
  }
  if (item->spectre && item->kind != MediaKind::Screenshot && stem.find("_recorte_") != std::string::npos)
    item->kind = MediaKind::Trim;
  if (item->game == "Escritorio") item->game.clear();
}

const char* MediaKindName(MediaKind kind) {
  switch (kind) {
  case MediaKind::Clip: return "Clip";
  case MediaKind::Recording: return "Grabación";
  case MediaKind::Trim: return "Recorte";
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

StorageInfo Library::Storage(const std::string& folder) {
  StorageInfo info;
  for (const auto& item : Scan(folder)) {
    info.usedBytes += item.size;
    if (item.Video()) ++info.videos;
    else ++info.screenshots;
    if (item.favorite) ++info.favorites;
  }
  ULARGE_INTEGER avail{}, total{}, totalFree{};
  if (GetDiskFreeSpaceExW(Widen(folder).c_str(), &avail, &total, &totalFree)) {
    info.freeBytes = avail.QuadPart;
    info.totalBytes = total.QuadPart;
    info.valid = true;
  }
  return info;
}

void Library::SetFavorite(const std::string& path, bool favorite) {
  std::lock_guard lock(mutex_);
  Load();
  meta_[Key(path)].favorite = favorite;
  Save();
}

void Library::SetTailMarkers(const std::string& path, std::vector<double> secondsBeforeEnd) {
  if (secondsBeforeEnd.empty()) return;
  std::lock_guard lock(mutex_);
  Load();
  meta_[Key(path)].tailMarkers = std::move(secondsBeforeEnd);
  Save();
}

void Library::SetMarkers(const std::string& path, std::vector<double> seconds) {
  if (seconds.empty()) return;
  std::lock_guard lock(mutex_);
  Load();
  meta_[Key(path)].markers = std::move(seconds);
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
  Save();
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

bool Library::Recycle(const std::string& path, std::string* error) {
  std::wstring from = Widen(path);
  from.push_back(L'\0'); // doble terminador
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  int r = SHFileOperationW(&op);
  if (r != 0 || op.fAnyOperationsAborted) {
    *error = "No se pudo mover a la papelera (¿está abierto en otro programa?).";
    return false;
  }
  std::lock_guard lock(mutex_);
  Load();
  if (meta_.erase(Key(path))) Save();
  return true;
}

int Library::EnforceLimit(const std::string& folder, uint64_t limitBytes, uint64_t* freedBytes) {
  if (freedBytes) *freedBytes = 0;
  if (limitBytes == 0) return 0;
  auto items = Scan(folder);
  uint64_t total = 0;
  for (const auto& item : items) total += item.size;
  if (total <= limitBytes) return 0;
  // Del más antiguo al más reciente. Lo de los últimos 2 minutos puede estar
  // escribiéndose todavía y no se toca.
  FILETIME nowFt;
  GetSystemTimeAsFileTime(&nowFt);
  int64_t now = int64_t((uint64_t(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime);
  const int64_t recent = int64_t(2) * 60 * 10000000;
  int deleted = 0;
  for (auto it = items.rbegin(); it != items.rend() && total > limitBytes; ++it) {
    if (!it->spectre || it->favorite || now - it->modified < recent) continue;
    if (!DeleteFileW(Widen(it->path).c_str())) continue; // abierto en otro programa
    total -= it->size;
    if (freedBytes) *freedBytes += it->size;
    ++deleted;
    Log("Límite de espacio: borrado %s (%s)", it->name.c_str(), FormatBytes(it->size).c_str());
    std::lock_guard lock(mutex_);
    meta_.erase(Key(it->path));
  }
  if (deleted > 0) {
    std::lock_guard lock(mutex_);
    Save();
  }
  return deleted;
}

// ------------------------------------------------------------------ utilidades

bool CopyFileToClipboard(HWND owner, const std::string& path) {
  std::wstring w = Widen(path);
  size_t bytes = sizeof(DROPFILES) + (w.size() + 2) * sizeof(wchar_t);
  HGLOBAL drop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!drop) return false;
  auto* df = static_cast<DROPFILES*>(GlobalLock(drop));
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), w.c_str(), w.size() * sizeof(wchar_t));
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

uint64_t FreeDiskBytes(const std::string& folder) {
  ULARGE_INTEGER avail{}, total{}, totalFree{};
  if (!GetDiskFreeSpaceExW(Widen(folder).c_str(), &avail, &total, &totalFree)) return 0;
  return avail.QuadPart;
}

// ------------------------------------------------------------------ trabajador

void MediaWorker::Start(HWND notifyWindow, UINT notifyMsg) {
  if (thread_.joinable()) return;
  notify_ = notifyWindow;
  msg_ = notifyMsg;
  stop_ = false;
  thread_ = std::thread([this] { Loop(); });
}

void MediaWorker::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  thread_.join();
  std::lock_guard lock(mutex_);
  results_.clear();
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
