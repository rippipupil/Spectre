#include "app/Updater.h"

#include "app/CrashHandler.h"
#include "common/Util.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifndef SPECTRE_VERSION
#define SPECTRE_VERSION "0.1.0"
#endif

namespace spectre {

namespace {

constexpr wchar_t kLatestApi[] = L"https://api.github.com/repos/rippipupil/Spectre/releases/latest";
constexpr char kReleasePage[] = "https://github.com/rippipupil/Spectre/releases/latest";
constexpr char kAssetName[] = "Spectre-Setup.exe";
constexpr uint64_t kMaxApiBytes = 1 << 20;        // la respuesta de la API ocupa unos KB
constexpr uint64_t kMaxInstallerBytes = 512u << 20; // tope de cordura para la descarga

const char* kNoNetwork = "No se pudo conectar con GitHub. Comprueba tu conexión a internet y vuelve a probar.";

struct WinHttpHandle {
  HINTERNET h = nullptr;
  WinHttpHandle() = default;
  explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
  ~WinHttpHandle() {
    if (h) WinHttpCloseHandle(h);
  }
  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;
  explicit operator bool() const { return h != nullptr; }
};

std::string NetworkError(DWORD code) {
  switch (code) {
  case ERROR_WINHTTP_TIMEOUT: return "GitHub no ha respondido a tiempo. Vuelve a probar en un momento.";
  case ERROR_WINHTTP_SECURE_FAILURE:
    return "No se pudo establecer una conexión segura con GitHub. Revisa la fecha y hora de Windows.";
  default: return kNoNetwork;
  }
}

// GET con redirecciones (GitHub manda las descargas a otro dominio). Llama a
// sink con cada trozo; si sink devuelve false, se aborta sin error.
bool HttpGet(const std::wstring& url, const wchar_t* accept, const std::function<bool()>& cancelled,
             const std::function<void(uint64_t)>& onLength, const std::function<bool(const char*, size_t)>& sink,
             std::string* error, DWORD* httpStatus = nullptr) {
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256]{}, path[2048]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = DWORD(std::size(path));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    *error = "Dirección de descarga no válida.";
    return false;
  }

  std::wstring agent = L"Spectre/" + Widen(SPECTRE_VERSION);
  // Proxy automático: el mismo que usa Windows (WPAD o el configurado).
  WinHttpHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    *error = kNoNetwork;
    return false;
  }
  WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 15000);
  WinHttpHandle connect(WinHttpConnect(session.h, host, parts.nPort, 0));
  WinHttpHandle request(connect ? WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr);
  if (!request) {
    *error = NetworkError(GetLastError());
    return false;
  }
  std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28";
  if (!WinHttpSendRequest(request.h, headers.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    DWORD code = GetLastError();
    Log("Actualizaciones: fallo de red %lu", code);
    *error = NetworkError(code);
    return false;
  }

  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (httpStatus) *httpStatus = status;
  if (status != 200) {
    Log("Actualizaciones: HTTP %lu", status);
    char buf[160];
    if (status == 403 || status == 429)
      snprintf(buf, sizeof(buf), "GitHub ha limitado las consultas desde tu red. Prueba de nuevo dentro de un rato.");
    else if (status == 404)
      snprintf(buf, sizeof(buf), "GitHub no encontró el archivo (código 404). Vuelve a buscar actualizaciones.");
    else
      snprintf(buf, sizeof(buf), "GitHub respondió con un error (código %lu). Prueba de nuevo más tarde.", status);
    *error = buf;
    return false;
  }

  wchar_t lengthText[32]{};
  size = sizeof(lengthText);
  if (onLength && WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                      lengthText, &size, WINHTTP_NO_HEADER_INDEX))
    onLength(_wcstoui64(lengthText, nullptr, 10));

  std::vector<char> chunk(64 * 1024);
  for (;;) {
    if (cancelled()) return false;
    DWORD read = 0;
    if (!WinHttpReadData(request.h, chunk.data(), DWORD(chunk.size()), &read)) {
      DWORD code = GetLastError();
      Log("Actualizaciones: la descarga se cortó (%lu)", code);
      *error = "La descarga se cortó. Comprueba tu conexión y vuelve a probar.";
      return false;
    }
    if (read == 0) return true;
    if (!sink(chunk.data(), read)) return false;
  }
}

// SHA-256 incremental con la API criptográfica de Windows.
class Sha256 {
public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
      BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
  }
  ~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  void Update(const char* data, size_t size) {
    if (hash_) BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), ULONG(size), 0);
  }
  std::string Hex() {
    unsigned char digest[32]{};
    if (!hash_ || BCryptFinishHash(hash_, digest, sizeof(digest), 0) != 0) return {};
    std::string out;
    char buf[3];
    for (unsigned char b : digest) {
      snprintf(buf, sizeof(buf), "%02x", b);
      out += buf;
    }
    return out;
  }

private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::filesystem::path DownloadDir() {
  wchar_t temp[MAX_PATH + 1]{};
  GetTempPathW(DWORD(std::size(temp)), temp);
  return std::filesystem::path(temp) / L"Spectre";
}

// Instaladores de actualizaciones anteriores (el último ya terminó de usarse).
void RemoveOldDownloads() {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(DownloadDir(), ec)) {
    auto name = entry.path().filename().wstring();
    if (name.rfind(L"Spectre-Setup-", 0) == 0) std::filesystem::remove(entry.path(), ec);
  }
}

bool IsPortableCopy() {
  std::error_code ec;
  return !std::filesystem::exists(ExecutablePath().parent_path() / L"unins000.exe", ec);
}

} // namespace

int CompareVersions(const std::string& a, const std::string& b) {
  auto parse = [](const std::string& text) {
    std::vector<long> parts;
    size_t i = (!text.empty() && (text[0] == 'v' || text[0] == 'V')) ? 1 : 0;
    while (i <= text.size()) {
      size_t dot = text.find('.', i);
      if (dot == std::string::npos) dot = text.size();
      parts.push_back(std::strtol(text.substr(i, dot - i).c_str(), nullptr, 10));
      i = dot + 1;
    }
    return parts;
  };
  auto x = parse(a), y = parse(b);
  for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
    long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}

struct Updater::Shared {
  mutable std::mutex mutex;
  UpdateStatus status;
  std::wstring url;   // descarga del instalador de la última release
  std::string sha256; // "digest" del asset, si GitHub lo publica
  // Cada comprobación o descarga lleva su número: Cancel() y el cierre de
  // Spectre lo suben, y el hilo antiguo se retira sin tocar el estado nuevo.
  std::atomic<unsigned> generation{0};

  void Set(const std::function<void(UpdateStatus&)>& change) {
    std::lock_guard lock(mutex);
    change(status);
  }
  void Fail(const std::string& message) {
    Set([&](UpdateStatus& s) {
      s.state = UpdateStatus::State::Error;
      s.message = message;
    });
  }
};

Updater::Updater() : shared_(std::make_shared<Shared>()) { shared_->status.portable = IsPortableCopy(); }

Updater::~Updater() { ++shared_->generation; }

UpdateStatus Updater::Status() const {
  std::lock_guard lock(shared_->mutex);
  return shared_->status;
}

void Updater::Check() {
  {
    std::lock_guard lock(shared_->mutex);
    auto st = shared_->status.state;
    if (st == UpdateStatus::State::Checking || st == UpdateStatus::State::Downloading ||
        st == UpdateStatus::State::Launching)
      return;
    shared_->status.state = UpdateStatus::State::Checking;
    shared_->status.message.clear();
  }
  const unsigned gen = ++shared_->generation;
  std::thread([shared = shared_, gen] {
    auto cancelled = [&] { return shared->generation != gen; };
    InstallThreadCrashHandler();
    RemoveOldDownloads();
    std::string body, error;
    DWORD status = 0;
    bool ok = HttpGet(
        kLatestApi, L"application/vnd.github+json", cancelled, nullptr,
        [&](const char* data, size_t size) {
          body.append(data, size);
          return body.size() <= kMaxApiBytes;
        },
        &error, &status);
    if (!ok && status == 404) error = "No hay ninguna versión publicada todavía.";
    if (cancelled()) return;
    if (!ok) {
      shared->Fail(error.empty() ? "GitHub envió una respuesta demasiado grande." : error);
      return;
    }
    try {
      auto json = nlohmann::json::parse(body);
      std::string tag = json.at("tag_name").get<std::string>();
      std::string latest = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
      std::string url, digest;
      uint64_t size = 0;
      for (const auto& asset : json.value("assets", nlohmann::json::array())) {
        if (asset.value("name", "") != kAssetName) continue;
        url = asset.value("browser_download_url", "");
        size = asset.value("size", uint64_t(0));
        digest = asset.contains("digest") && asset["digest"].is_string() ? asset["digest"].get<std::string>() : "";
      }
      bool newer = CompareVersions(SPECTRE_VERSION, latest) < 0;
      Log("Actualizaciones: instalada %s, última %s%s", SPECTRE_VERSION, latest.c_str(),
          newer ? " (hay versión nueva)" : "");
      if (newer && url.empty()) {
        shared->Fail("La versión " + latest + " aún no tiene instalador. Prueba de nuevo en unos minutos.");
        return;
      }
      std::lock_guard lock(shared->mutex);
      shared->url = Widen(url);
      shared->sha256 = digest.rfind("sha256:", 0) == 0 ? digest.substr(7) : "";
      // Sin huella no hay forma de saber que el archivo es el publicado: no se
      // instala en silencio, se ofrece la página de descarga.
      shared->status.unverified = newer && shared->sha256.empty();
      if (shared->status.unverified) Log("Actualizaciones: la release %s no publica SHA-256", latest.c_str());
      shared->status.latest = latest;
      shared->status.sizeBytes = size;
      shared->status.state = newer ? UpdateStatus::State::Available : UpdateStatus::State::UpToDate;
    } catch (const std::exception& e) {
      Log("Actualizaciones: respuesta de GitHub no válida: %s", e.what());
      shared->Fail("GitHub envió una respuesta que Spectre no entiende. Prueba de nuevo más tarde.");
    }
  }).detach();
}

void Updater::Download(HWND hwnd) {
  std::wstring url;
  std::string latest, sha256;
  uint64_t expected = 0;
  {
    std::lock_guard lock(shared_->mutex);
    if (shared_->status.state != UpdateStatus::State::Available || shared_->url.empty() || shared_->sha256.empty())
      return;
    url = shared_->url;
    latest = shared_->status.latest;
    sha256 = shared_->sha256;
    expected = shared_->status.sizeBytes;
    shared_->status.state = UpdateStatus::State::Downloading;
    shared_->status.downloaded = 0;
  }
  const unsigned gen = ++shared_->generation;
  std::thread([shared = shared_, gen, hwnd, url, latest, sha256, expected] {
    InstallThreadCrashHandler();
    auto cancelled = [&] { return shared->generation != gen; };
    std::error_code ec;
    auto dir = DownloadDir();
    std::filesystem::create_directories(dir, ec);
    auto target = dir / (L"Spectre-Setup-" + Widen(latest) + L".exe");
    auto part = target;
    part += L"." + std::to_wstring(gen) + L".part"; // una descarga cancelada aún puede estar escribiendo la suya

    uint64_t total = expected, received = 0; // Content-Length, si llega, manda
    Sha256 hash;
    std::string error;
    bool ok;
    {
      std::ofstream file(part, std::ios::binary | std::ios::trunc);
      if (!file) {
        shared->Fail("No se pudo guardar la descarga en la carpeta temporal de Windows.");
        return;
      }
      ok = HttpGet(
          url, L"application/octet-stream", cancelled,
          [&](uint64_t length) { total = length; },
          [&](const char* data, size_t size) {
            received += size;
            if (received > kMaxInstallerBytes) return false;
            file.write(data, std::streamsize(size));
            hash.Update(data, size);
            if (total > 0) {
              double progress = double(received) / double(total);
              std::lock_guard lock(shared->mutex);
              if (!cancelled()) shared->status.downloaded = progress;
            }
            return bool(file);
          },
          &error);
      file.close();
      // Un fallo de escritura corta la descarga desde el sink sin mensaje de red.
      if (!file) {
        ok = false;
        error = "No se pudo guardar la descarga: ¿queda espacio en el disco?";
      }
    }

    if (cancelled()) {
      std::filesystem::remove(part, ec);
      return;
    }
    if (ok && total > 0 && received != total) {
      ok = false;
      error = "La descarga llegó incompleta. Vuelve a probar.";
    }
    if (ok && hash.Hex() != sha256) {
      Log("Actualizaciones: SHA-256 no coincide (esperado %s)", sha256.c_str());
      ok = false;
      error = "El instalador descargado no coincide con el publicado. Vuelve a probar.";
    }
    if (!ok) {
      std::filesystem::remove(part, ec);
      shared->Fail(error.empty() ? "El instalador descargado es demasiado grande." : error);
      return;
    }
    std::filesystem::rename(part, target, ec);
    if (ec) {
      std::filesystem::remove(part, ec);
      shared->Fail("No se pudo preparar el instalador en la carpeta temporal de Windows.");
      return;
    }
    Log("Actualizaciones: descargado %s (%llu bytes)", Narrow(target.wstring()).c_str(),
        (unsigned long long)received);
    {
      std::lock_guard lock(shared->mutex);
      if (cancelled()) return; // cancelado en el último instante: el archivo se borra en la próxima comprobación
      shared->status.state = UpdateStatus::State::Launching;
      shared->status.downloaded = 1;
    }
    auto* path = new std::wstring(target.wstring());
    if (!PostMessageW(hwnd, WM_SPECTRE_UPDATE_READY, 0, reinterpret_cast<LPARAM>(path))) delete path;
  }).detach();
}

void Updater::Cancel() {
  std::lock_guard lock(shared_->mutex);
  if (shared_->status.state != UpdateStatus::State::Downloading) return;
  ++shared_->generation;
  shared_->status.state = UpdateStatus::State::Available; // se puede reintentar al momento
}

bool Updater::LaunchInstaller(const std::wstring& path, bool relaunch) {
  // /VERYSILENT no abre ninguna ventana: la actualización suele llegar al
  // parar una grabación, con el juego delante. /RELAUNCH=1 hace que el
  // instalador vuelva a abrir Spectre al terminar (ver installer/spectre.iss).
  std::wstring cmd = L"\"" + path + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
  if (relaunch) cmd += L" /RELAUNCH=1";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    Log("Actualizaciones: no se pudo abrir el instalador (%lu)", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  Log("Actualizaciones: instalador lanzado, Spectre se cierra");
  return true;
}

std::string Updater::ReleasePage() { return kReleasePage; }

} // namespace spectre
