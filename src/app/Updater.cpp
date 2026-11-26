#include "app/Updater.h"

#include "app/CrashHandler.h"
#include "common/Util.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifndef SPECTRE_VERSION
#define SPECTRE_VERSION "0.1.0"
#endif
// Lo pone CMake (CI: el repositorio donde se publica cada versión).
#ifndef SPECTRE_UPDATE_REPO
#define SPECTRE_UPDATE_REPO ""
#endif

namespace spectre {

namespace {

constexpr char kRepo[] = SPECTRE_UPDATE_REPO;
constexpr char kAssetName[] = "Spectre-Setup.exe";
constexpr char kPortableName[] = "Spectre-Portable.zip";
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

// GET con redirecciones (GitHub manda las descargas a otro dominio). Con
// from > 0 pide el archivo desde ese byte: onResponse recibe el código (206 si
// el servidor lo respeta, 200 si manda el archivo entero) y el Content-Length
// (0 si no llega) antes del primer trozo. Llama a sink con cada trozo; si sink
// devuelve false, se aborta sin error.
bool HttpGet(const std::wstring& url, const wchar_t* accept, uint64_t from, const std::function<bool()>& cancelled,
             const std::function<void(DWORD, uint64_t)>& onResponse,
             const std::function<bool(const char*, size_t)>& sink, std::string* error, DWORD* httpStatus = nullptr) {
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
  if (from > 0) headers += L"\r\nRange: bytes=" + std::to_wstring(from) + L"-";
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
  if (status != 200 && !(from > 0 && status == 206)) {
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

  if (onResponse) {
    wchar_t lengthText[32]{};
    size = sizeof(lengthText);
    uint64_t length = WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                          lengthText, &size, WINHTTP_NO_HEADER_INDEX)
                          ? _wcstoui64(lengthText, nullptr, 10)
                          : 0;
    onResponse(status, length);
  }

  // Trozos grandes: menos vueltas por WinHTTP y por el disco con una conexión rápida.
  std::vector<char> chunk(256 * 1024);
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
  // Vuelve a empezar, como recién creado.
  void Reset() {
    if (hash_) BCryptDestroyHash(hash_);
    hash_ = nullptr;
    if (alg_) BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
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

std::wstring InstallerName(const std::string& version) {
  return L"Spectre-Setup-" + Widen(SanitizeFileName(version)) + L".exe";
}

// Instaladores y descargas a medias que ya no sirven. Se queda keep (el de la
// versión nueva que aún no se instaló) y su .part, para no bajarlo otra vez.
void RemoveOldDownloads(const std::wstring& keep) {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(DownloadDir(), ec), end; !ec && it != end; it.increment(ec)) {
    auto name = it->path().filename().wstring();
    if (name.rfind(L"Spectre-Setup-", 0) != 0) continue;
    if (!keep.empty() && (name == keep || name == keep + L".part")) continue;
    std::error_code removeError; // abierto por otra descarga o por el instalador: se queda para la próxima vez
    std::filesystem::remove(it->path(), removeError);
  }
}

// La descarga que escribe el .part de cada versión. Una cancelada puede
// seguir escribiendo unos segundos (hasta que vuelve su lectura de red): la
// nueva no lo toca y usa otro archivo. Nunca se destruye: un hilo suelto
// puede tenerlo al cerrar Spectre.
std::mutex& PartMutex() {
  static auto* mutex = new std::mutex;
  return *mutex;
}

// Pasa por el hash los primeros bytes de path. false si no se pudo leer todo.
bool HashFile(const std::filesystem::path& path, uint64_t bytes, Sha256& hash, const std::function<bool()>& cancelled) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<char> buf(1 << 20);
  while (bytes > 0) {
    if (cancelled()) return false;
    const size_t want = size_t(std::min<uint64_t>(bytes, buf.size()));
    in.read(buf.data(), std::streamsize(want));
    if (size_t(in.gcount()) != want) return false;
    hash.Update(buf.data(), want);
    bytes -= want;
  }
  return true;
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

void Updater::Check(bool background) {
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
  std::thread([shared = shared_, gen, background] {
    auto cancelled = [&] { return shared->generation != gen; };
    InstallThreadCrashHandler();
    // La comprobación automática no deja un error en Ajustes que nadie pidió
    // (sin red, GitHub caído): solo queda en el log.
    auto fail = [&](const std::string& message) {
      if (!background) {
        shared->Fail(message);
        return;
      }
      Log("Actualizaciones: la comprobación automática falló: %s", message.c_str());
      shared->Set([](UpdateStatus& s) { s.state = UpdateStatus::State::Idle; });
    };
    if (!kRepo[0]) {
      fail("Esta copia de Spectre no tiene de dónde descargar versiones nuevas.");
      return;
    }
    std::string body, error;
    DWORD status = 0;
    bool ok = HttpGet(
        L"https://api.github.com/repos/" + Widen(kRepo) + L"/releases/latest", L"application/vnd.github+json", 0,
        cancelled, nullptr,
        [&](const char* data, size_t size) {
          body.append(data, size);
          return body.size() <= kMaxApiBytes;
        },
        &error, &status);
    if (!ok && status == 404) error = "No hay ninguna versión publicada todavía.";
    if (cancelled()) return;
    if (!ok) {
      fail(error.empty() ? "GitHub envió una respuesta demasiado grande." : error);
      return;
    }
    try {
      auto json = nlohmann::json::parse(body);
      std::string tag = json.at("tag_name").get<std::string>();
      std::string latest = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
      std::string url, digest, portableUrl;
      uint64_t size = 0;
      for (const auto& asset : json.value("assets", nlohmann::json::array())) {
        if (asset.value("name", "") == kPortableName) portableUrl = asset.value("browser_download_url", "");
        if (asset.value("name", "") != kAssetName) continue;
        url = asset.value("browser_download_url", "");
        size = asset.value("size", uint64_t(0));
        digest = asset.contains("digest") && asset["digest"].is_string() ? asset["digest"].get<std::string>() : "";
      }
      bool newer = CompareVersions(SPECTRE_VERSION, latest) < 0;
      Log("Actualizaciones: instalada %s, última %s%s", SPECTRE_VERSION, latest.c_str(),
          newer ? " (hay versión nueva)" : "");
      // El instalador de la versión nueva, si ya se bajó en un intento que no
      // llegó a instalarse, se queda: Actualizar lo reutiliza.
      RemoveOldDownloads(newer ? InstallerName(latest) : std::wstring());
      if (newer && url.empty()) {
        fail("La versión " + latest + " aún no tiene instalador. Prueba de nuevo en unos minutos.");
        return;
      }
      std::lock_guard lock(shared->mutex);
      shared->url = Widen(url);
      shared->sha256 = digest.rfind("sha256:", 0) == 0 ? digest.substr(7) : "";
      // Sin huella no hay forma de saber que el archivo es el publicado: no se
      // instala en silencio, se ofrece la página de descarga.
      shared->status.unverified = newer && shared->sha256.empty();
      if (shared->status.unverified) Log("Actualizaciones: la release %s no publica SHA-256", latest.c_str());
      // Descarga a mano (portable o sin huella): el archivo directamente, no
      // la página del repositorio.
      shared->status.manualUrl = shared->status.portable && !portableUrl.empty() ? portableUrl : url;
      // Se abre con ShellExecute: solo https, como las descargas de HttpGet.
      if (shared->status.manualUrl.rfind("https://", 0) != 0) shared->status.manualUrl.clear();
      shared->status.latest = latest;
      shared->status.sizeBytes = size;
      shared->status.state = newer ? UpdateStatus::State::Available : UpdateStatus::State::UpToDate;
    } catch (const std::exception& e) {
      Log("Actualizaciones: respuesta de GitHub no válida: %s", e.what());
      fail("GitHub envió una respuesta que Spectre no entiende. Prueba de nuevo más tarde.");
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
    const auto target = dir / InstallerName(latest);
    // Listo para instalar: se avisa a la ventana, que cierra Spectre y lo lanza.
    auto ready = [&] {
      {
        std::lock_guard lock(shared->mutex);
        if (cancelled()) return; // cancelado en el último instante: el instalador queda para el próximo intento
        shared->status.state = UpdateStatus::State::Launching;
        shared->status.downloaded = 1;
      }
      auto* path = new std::wstring(target.wstring());
      if (!PostMessageW(hwnd, WM_SPECTRE_UPDATE_READY, 0, reinterpret_cast<LPARAM>(path))) delete path;
    };

    // Ya bajado en un intento que no llegó a instalarse: si es el publicado
    // (mismo tamaño y SHA-256), no se descarga otra vez.
    if (std::filesystem::exists(target, ec)) {
      const uint64_t size = std::filesystem::file_size(target, ec);
      Sha256 hash;
      if (!ec && (expected == 0 || size == expected) && HashFile(target, size, hash, cancelled) &&
          hash.Hex() == sha256) {
        Log("Actualizaciones: se usa el instalador ya descargado %s", Narrow(target.wstring()).c_str());
        ready();
        return;
      }
      if (cancelled()) return;
      Log("Actualizaciones: el instalador que había no coincide con el publicado, se descarga otra vez");
      std::filesystem::remove(target, ec);
    }

    // Una descarga cortada sigue donde se quedó: lo que ya hay en el .part
    // pasa por el hash y se piden solo los bytes que faltan.
    std::unique_lock partLock(PartMutex(), std::try_to_lock);
    const bool ownPart = partLock.owns_lock();
    auto part = target;
    part += ownPart ? std::wstring(L".part") : L"." + std::to_wstring(gen) + L".part";
    Sha256 hash;
    uint64_t offset = 0;
    if (ownPart && expected > 0) {
      const uint64_t have = std::filesystem::file_size(part, ec);
      if (!ec && have > 0 && have < expected) {
        if (HashFile(part, have, hash, cancelled)) offset = have;
        else hash.Reset();
      }
    }
    if (cancelled()) return;
    if (offset > 0)
      Log("Actualizaciones: la descarga sigue donde se cortó (%llu de %llu bytes)", (unsigned long long)offset,
          (unsigned long long)expected);

    uint64_t total = expected, received = offset; // Content-Length, si llega, manda
    std::string error;
    DWORD status = 0;
    bool ok, writeFailed = false;
    {
      std::ofstream file(part, std::ios::binary | (offset > 0 ? std::ios::app : std::ios::trunc));
      if (!file) {
        shared->Fail("No se pudo guardar la descarga en la carpeta temporal de Windows.");
        return;
      }
      ok = HttpGet(
          url, L"application/octet-stream", offset, cancelled,
          [&](DWORD code, uint64_t length) {
            if (offset > 0 && code != 206) {
              // El servidor no reanuda y manda el archivo entero: se empieza de cero.
              Log("Actualizaciones: el servidor no reanuda la descarga, empieza de cero");
              file.close();
              file.clear();
              file.open(part, std::ios::binary | std::ios::trunc);
              hash.Reset();
              offset = received = 0;
            }
            if (length > 0) total = offset + length;
          },
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
          &error, &status);
      file.close();
      // Un fallo de escritura corta la descarga desde el sink sin mensaje de red.
      if (!file) {
        ok = false;
        writeFailed = true;
        error = "No se pudo guardar la descarga: ¿queda espacio en el disco?";
      }
    }

    if (cancelled()) {
      // Lo bajado sirve para seguir en el próximo intento, salvo en un archivo de un solo uso.
      if (!ownPart) std::filesystem::remove(part, ec);
      return;
    }
    if (ok && total > 0 && received != total) {
      ok = false;
      error = "La descarga llegó incompleta. Vuelve a probar.";
    }
    bool hashMismatch = false;
    if (ok && hash.Hex() != sha256) {
      Log("Actualizaciones: SHA-256 no coincide (esperado %s)", sha256.c_str());
      ok = false;
      hashMismatch = true;
      error = "El instalador descargado no coincide con el publicado. Vuelve a probar.";
    }
    if (!ok) {
      // Un corte de red se reanuda en el próximo intento. Lo demás (huella
      // distinta, disco lleno, un error de GitHub, demasiado grande) empieza de cero.
      const bool resumable = ownPart && !writeFailed && !hashMismatch && received < expected &&
                             (status == 0 || status == 200 || status == 206);
      if (!resumable) std::filesystem::remove(part, ec);
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
    ready();
  }).detach();
}

void Updater::Cancel() {
  std::lock_guard lock(shared_->mutex);
  if (shared_->status.state != UpdateStatus::State::Downloading) return;
  ++shared_->generation;
  shared_->status.state = UpdateStatus::State::Available; // se puede reintentar al momento
}

std::filesystem::path Updater::InstallLogPath() { return AppDataDir() / L"actualizacion.log"; }

void Updater::RemoveDownloadedInstallers() { RemoveOldDownloads({}); }

bool Updater::LaunchInstaller(const std::wstring& path, bool relaunch, bool progress) {
  // /SILENT enseña solo la ventana de progreso del instalador (se pulsó
  // Actualizar sin juego delante); /VERYSILENT no abre nada, para cuando la
  // actualización llega al parar una grabación con el juego delante.
  // /RELAUNCH=1 hace que el instalador vuelva a abrir Spectre al terminar.
  // /SPECTREPID: el instalador espera a que este proceso termine (y, si se
  // queda colgado, lo cierra) antes de reemplazar nada. /LOG deja su registro
  // para explicar un fallo. Ver installer/spectre.iss.
  std::error_code ec;
  std::filesystem::remove(InstallLogPath(), ec); // que no se lea el de una vez anterior
  std::wstring cmd = L"\"" + path + L"\" " + (progress ? L"/SILENT" : L"/VERYSILENT") +
                     L" /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /SPECTREPID=" +
                     std::to_wstring(GetCurrentProcessId()) + L" /LOG=\"" + InstallLogPath().wstring() + L"\"";
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
  Log("Actualizaciones: instalador lanzado%s, Spectre se cierra", progress ? " con su ventana de progreso" : "");
  return true;
}

std::string Updater::FailedInstallReason() {
  std::ifstream in(InstallLogPath(), std::ios::binary);
  if (!in) {
    Log("Actualizaciones: el instalador no terminó y no dejó registro");
    return "El instalador no terminó.";
  }
  // El final del registro, a spectre.log: ahí está por qué se deshizo.
  std::deque<std::string> tail;
  bool inUse = false, diskFull = false;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find("in use") != std::string::npos || line.find("being used") != std::string::npos ||
        line.find("code 5.") != std::string::npos || line.find("code 32.") != std::string::npos ||
        line.find("Spectre sigue sin cerrarse") != std::string::npos)
      inUse = true;
    if (line.find("code 112.") != std::string::npos || line.find("not enough") != std::string::npos)
      diskFull = true;
    tail.push_back(std::move(line));
    if (tail.size() > 25) tail.pop_front();
  }
  for (const auto& line : tail) Log("Actualizaciones: instalador: %s", line.c_str());
  if (diskFull) return "No queda espacio en el disco.";
  if (inUse) return "Algún programa tenía abiertos los archivos de Spectre.";
  return "El instalador no terminó.";
}

} // namespace spectre
