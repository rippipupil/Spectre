#include "app/Cloud.h"

#include "app/CrashHandler.h"
#include "common/Util.h"
#include "core/ThreadPolicy.h"

#include <bcrypt.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>

#ifndef SPECTRE_VERSION
#define SPECTRE_VERSION "0.1.0"
#endif

namespace spectre {

namespace {

constexpr uint64_t kPartSize = 8ull << 20;      // partes de 8 MB (R2 y B2 piden 5 MB como mínimo)
constexpr uint64_t kSinglePutMax = 16ull << 20; // hasta aquí, de una vez
constexpr uint64_t kMaxParts = 9500;            // S3 admite 10 000: con archivos enormes las partes crecen
constexpr size_t kChunk = 64 * 1024;            // lo que se escribe en la conexión cada vez
constexpr size_t kMaxBody = 1 << 20;            // respuestas del bucket: unos KB
constexpr size_t kMaxHistory = 500;             // subidas terminadas que se recuerdan
constexpr int kMaxTries = 8;                    // fallos seguidos de una subida antes de darla por fallida
constexpr int kShareKBps = 1024;                // compartir con «Esperar a cerrar el juego»: sube igualmente, despacio
constexpr char kKeyPrefix[] = "spectre/";
constexpr char kExportPrefix[] = "spectre-nube:";
constexpr char kEntropy[] = "Spectre: claves de la nube";

// ------------------------------------------------------------------ cifrado

using Digest = std::array<uint8_t, 32>;

BCRYPT_ALG_HANDLE Provider(bool hmac) {
  static const BCRYPT_ALG_HANDLE sha = [] {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) h = nullptr;
    return h;
  }();
  static const BCRYPT_ALG_HANDLE mac = [] {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
      h = nullptr;
    return h;
  }();
  return hmac ? mac : sha;
}

// SHA-256 (key nula) o HMAC-SHA256.
Digest HashOrMac(const void* key, size_t keySize, const void* data, size_t size) {
  Digest out{};
  BCRYPT_ALG_HANDLE alg = Provider(key != nullptr);
  BCRYPT_HASH_HANDLE h = nullptr;
  if (!alg || !BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(key)),
                                               ULONG(keySize), 0)))
    return out;
  const auto* p = static_cast<const uint8_t*>(data);
  while (size > 0) {
    ULONG n = ULONG(std::min<size_t>(size, 1u << 30));
    BCryptHashData(h, const_cast<PUCHAR>(p), n, 0);
    p += n;
    size -= n;
  }
  BCryptFinishHash(h, out.data(), ULONG(out.size()), 0);
  BCryptDestroyHash(h);
  return out;
}

Digest Sha256(const void* data, size_t size) { return HashOrMac(nullptr, 0, data, size); }
Digest Sha256(const std::string& text) { return Sha256(text.data(), text.size()); }
Digest Hmac(const void* key, size_t keySize, const std::string& text) {
  return HashOrMac(key, keySize, text.data(), text.size());
}
Digest Hmac(const Digest& key, const std::string& text) { return Hmac(key.data(), key.size(), text); }

std::string Hex(const uint8_t* data, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 15]);
  }
  return out;
}
std::string Hex(const Digest& d) { return Hex(d.data(), d.size()); }

// Código aleatorio del enlace: letras y números, sin sesgo (se descartan los
// bytes de 248 en adelante).
std::string RandomCode(size_t length) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string out;
  for (int round = 0; out.size() < length && round < 16; ++round) {
    uint8_t buf[64];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) break;
    for (uint8_t b : buf)
      if (b < 248 && out.size() < length) out.push_back(kAlphabet[b % 62]);
  }
  return out.size() == length ? out : std::string();
}

uint64_t UnixMillis() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return (t - 116444736000000000ull) / 10000;
}

std::string Base36(uint64_t v) {
  static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::string out;
  do {
    out.insert(out.begin(), kDigits[v % 36]);
    v /= 36;
  } while (v);
  return out;
}

std::string Base64(const std::string& in) {
  static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
    for (int s = 18; s >= 0; s -= 6) out.push_back(kChars[(v >> s) & 63]);
  }
  if (i + 1 == in.size()) {
    uint32_t v = uint8_t(in[i]) << 16;
    out += {kChars[(v >> 18) & 63], kChars[(v >> 12) & 63], '=', '='};
  } else if (i + 2 == in.size()) {
    uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
    out += {kChars[(v >> 18) & 63], kChars[(v >> 12) & 63], kChars[(v >> 6) & 63], '='};
  }
  return out;
}

bool FromBase64(const std::string& in, std::string* out) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  out->clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || isspace((unsigned char)c)) continue;
    int v = value(c);
    if (v < 0) return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(char((acc >> bits) & 0xFF));
    }
  }
  return true;
}

// -------------------------------------------------------------- claves (DPAPI)

std::filesystem::path KeysPath() { return AppDataDir() / L"cloud-keys.bin"; }
std::filesystem::path QueuePath() { return AppDataDir() / L"cloud.json"; }

bool WriteFileAtomic(const std::filesystem::path& path, const void* data, size_t size) {
  auto tmp = path;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(static_cast<const char*>(data), std::streamsize(size));
    if (!out) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

void Wipe(std::string& s) {
  if (!s.empty()) SecureZeroMemory(s.data(), s.size());
  s.clear();
}

// Cifradas con la cuenta de Windows: otro usuario del PC (u otro PC) no las lee.
bool SaveKeysFile(const CloudKeys& keys) {
  std::string plain = nlohmann::json{{"id", keys.keyId}, {"secret", keys.secret}}.dump();
  DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
  DATA_BLOB entropy{DWORD(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
  DATA_BLOB out{};
  bool ok = CryptProtectData(&in, L"Spectre", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) != FALSE;
  Wipe(plain);
  if (!ok) {
    Log("Nube: no se pudieron cifrar las claves (%lu)", GetLastError());
    return false;
  }
  ok = WriteFileAtomic(KeysPath(), out.pbData, out.cbData);
  LocalFree(out.pbData);
  return ok;
}

bool LoadKeysFile(CloudKeys* keys) {
  std::ifstream file(KeysPath(), std::ios::binary);
  if (!file) return false;
  std::string blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (blob.empty() || blob.size() > 64 * 1024) return false;
  DATA_BLOB in{DWORD(blob.size()), reinterpret_cast<BYTE*>(blob.data())};
  DATA_BLOB entropy{DWORD(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    Log("Nube: las claves guardadas no se pueden descifrar (¿otro usuario de Windows?)");
    return false;
  }
  std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  bool ok = false;
  try {
    auto j = nlohmann::json::parse(plain);
    keys->keyId = j.value("id", "");
    keys->secret = j.value("secret", "");
    ok = !keys->Empty();
  } catch (const std::exception&) {
  }
  Wipe(plain);
  return ok;
}

void DeleteKeysFile() {
  std::error_code ec;
  std::filesystem::remove(KeysPath(), ec);
}

std::string KeyHint(const std::string& keyId) {
  if (keyId.size() <= 8) return keyId.empty() ? std::string() : keyId.substr(0, 2) + "...";
  return keyId.substr(0, 4) + "..." + keyId.substr(keyId.size() - 3);
}

// -------------------------------------------------------------------- destino

std::string Trim(std::string s) {
  while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
  size_t start = 0;
  while (start < s.size() && isspace((unsigned char)s[start])) ++start;
  return s.substr(start);
}

std::string LowerAscii(std::string s) {
  for (auto& c : s) c = char(tolower((unsigned char)c));
  return s;
}

bool EndsWith(const std::string& s, const char* suffix) {
  size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

struct Endpoint {
  std::string host; // en minúsculas
  int port = 443;
  std::string pathBucket; // la primera carpeta de la dirección, si la hay
};

bool ParseEndpoint(const std::string& text, Endpoint* out, std::string* error) {
  std::string s = Trim(text);
  if (s.empty()) {
    if (error) *error = "Escribe la dirección S3 (endpoint) de tu bucket.";
    return false;
  }
  std::string lower = LowerAscii(s);
  if (lower.rfind("http://", 0) == 0) {
    if (error) *error = "La dirección tiene que empezar por https://.";
    return false;
  }
  if (lower.rfind("https://", 0) == 0) s = s.substr(8);
  size_t slash = s.find('/');
  std::string hostPort = s.substr(0, slash);
  std::string path = slash == std::string::npos ? std::string() : s.substr(slash + 1);
  size_t colon = hostPort.find(':');
  out->host = LowerAscii(hostPort.substr(0, colon));
  out->port = 443;
  if (colon != std::string::npos) {
    out->port = atoi(hostPort.c_str() + colon + 1);
    if (out->port <= 0 || out->port > 65535) {
      if (error) *error = "El puerto de la dirección no es válido.";
      return false;
    }
  }
  if (out->host.empty() || out->host.find_first_of(" @?#") != std::string::npos) {
    if (error) *error = "La dirección S3 no es válida.";
    return false;
  }
  out->pathBucket = path.substr(0, path.find('/'));
  return true;
}

std::string RegionFor(const CloudConfig& c, const std::string& host) {
  if (!Trim(c.region).empty()) return Trim(c.region);
  if (EndsWith(host, ".r2.cloudflarestorage.com")) return "auto";
  // s3.<región>.backblazeb2.com
  if (host.rfind("s3.", 0) == 0 && EndsWith(host, ".backblazeb2.com")) return host.substr(3, host.size() - 3 - 16);
  // s3.<región>.amazonaws.com, s3-<región>.amazonaws.com
  if (EndsWith(host, ".amazonaws.com")) {
    size_t at = host.find("s3.");
    if (at == std::string::npos) at = host.find("s3-");
    if (at != std::string::npos) {
      std::string region = host.substr(at + 3, host.size() - 14 - (at + 3));
      if (!region.empty() && region != "amazonaws" && region.find('.') == std::string::npos) return region;
    }
    return "us-east-1";
  }
  return c.provider == "r2" ? "auto" : "us-east-1";
}

struct Target {
  std::wstring host;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
  std::string hostHeader; // host[:puerto], como lo firma S3
  std::string endpoint;   // tal cual en Ajustes (para recordar dónde se subió)
  std::string region, bucket;
  CloudKeys keys;
};

bool MakeTarget(const CloudConfig& c, const CloudKeys& keys, Target* t, std::string* error) {
  Endpoint ep;
  if (!ParseEndpoint(c.endpoint, &ep, error)) return false;
  t->host = Widen(ep.host);
  t->port = INTERNET_PORT(ep.port);
  t->hostHeader = ep.port == 443 ? ep.host : ep.host + ":" + std::to_string(ep.port);
  t->endpoint = Trim(c.endpoint);
  t->region = RegionFor(c, ep.host);
  t->bucket = CloudBucket(c);
  if (t->bucket.empty()) {
    if (error) *error = "Escribe el nombre del bucket.";
    return false;
  }
  if (keys.Empty()) {
    if (error) *error = "Faltan las claves de acceso (Access Key ID y Secret Access Key).";
    return false;
  }
  t->keys = keys;
  return true;
}

// ------------------------------------------------------------- firma (SigV4)

// RFC 3986: letras, números y - _ . ~ tal cual; lo demás %XX. S3 deja la / del
// nombre del objeto sin codificar.
std::string UriEncode(const std::string& s, bool keepSlash) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) {
      out.push_back(char(c));
    } else {
      out.push_back('%');
      out.push_back(kDigits[c >> 4]);
      out.push_back(kDigits[c & 15]);
    }
  }
  return out;
}

using Query = std::vector<std::pair<std::string, std::string>>;

std::string CanonicalQuery(Query q) {
  for (auto& [k, v] : q) {
    k = UriEncode(k, false);
    v = UriEncode(v, false);
  }
  std::sort(q.begin(), q.end());
  std::string out;
  for (const auto& [k, v] : q) {
    if (!out.empty()) out.push_back('&');
    out += k + "=" + v;
  }
  return out;
}

std::string ObjectUri(const Target& t, const std::string& key) {
  return "/" + UriEncode(t.bucket, false) + (key.empty() ? std::string() : "/" + UriEncode(key, true));
}

void AmzTimes(std::string* amzDate, std::string* day) {
  SYSTEMTIME st;
  GetSystemTime(&st);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond);
  *amzDate = buf;
  *day = amzDate->substr(0, 8);
}

Digest SigningKey(const Target& t, const std::string& day) {
  std::string secret = "AWS4" + t.keys.secret;
  Digest k = Hmac(secret.data(), secret.size(), day);
  Wipe(secret);
  k = Hmac(k, t.region);
  k = Hmac(k, "s3");
  return Hmac(k, "aws4_request");
}

// Cabeceras de una petición firmada: x-amz-date, x-amz-content-sha256 y
// Authorization (más las que van sin firmar, como Content-Type).
std::wstring SignedHeaders(const Target& t, const char* method, const std::string& uri, const std::string& query,
                           const std::string& payloadHash, const std::vector<std::string>& extra = {}) {
  std::string amzDate, day;
  AmzTimes(&amzDate, &day);
  const std::string scope = day + "/" + t.region + "/s3/aws4_request";
  const std::string signedNames = "host;x-amz-content-sha256;x-amz-date";
  const std::string canonical = std::string(method) + "\n" + uri + "\n" + query + "\nhost:" + t.hostHeader +
                                "\nx-amz-content-sha256:" + payloadHash + "\nx-amz-date:" + amzDate + "\n\n" +
                                signedNames + "\n" + payloadHash;
  const std::string toSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + Hex(Sha256(canonical));
  const std::string signature = Hex(Hmac(SigningKey(t, day), toSign));
  std::string headers = "x-amz-date: " + amzDate + "\r\nx-amz-content-sha256: " + payloadHash +
                        "\r\nAuthorization: AWS4-HMAC-SHA256 Credential=" + t.keys.keyId + "/" + scope +
                        ", SignedHeaders=" + signedNames + ", Signature=" + signature;
  for (const auto& h : extra) headers += "\r\n" + h;
  return Widen(headers);
}

// URL firmada para descargar: vale `seconds` (como mucho 7 días).
std::string Presign(const Target& t, const std::string& key, int seconds) {
  std::string amzDate, day;
  AmzTimes(&amzDate, &day);
  const std::string scope = day + "/" + t.region + "/s3/aws4_request";
  const std::string uri = ObjectUri(t, key);
  const std::string query = CanonicalQuery({{"X-Amz-Algorithm", "AWS4-HMAC-SHA256"},
                                            {"X-Amz-Credential", t.keys.keyId + "/" + scope},
                                            {"X-Amz-Date", amzDate},
                                            {"X-Amz-Expires", std::to_string(std::clamp(seconds, 60, 604800))},
                                            {"X-Amz-SignedHeaders", "host"}});
  const std::string canonical =
      "GET\n" + uri + "\n" + query + "\nhost:" + t.hostHeader + "\n\nhost\nUNSIGNED-PAYLOAD";
  const std::string toSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + Hex(Sha256(canonical));
  const std::string signature = Hex(Hmac(SigningKey(t, day), toSign));
  return "https://" + t.hostHeader + uri + "?" + query + "&X-Amz-Signature=" + signature;
}

std::string PublicLink(const std::string& base, const std::string& key) {
  std::string b = Trim(base);
  while (!b.empty() && b.back() == '/') b.pop_back();
  if (LowerAscii(b).rfind("https://", 0) != 0 && LowerAscii(b).rfind("http://", 0) != 0) b = "https://" + b;
  return b + "/" + UriEncode(key, true);
}

// ------------------------------------------------------------------- red

struct HttpResponse {
  DWORD status = 0;
  DWORD netError = 0; // fallo de WinHTTP (sin respuesta)
  std::string body;
  std::string etag;
};

class Http {
public:
  Http() {
    std::wstring agent = L"Spectre/" + Widen(SPECTRE_VERSION);
    // Proxy automático: el mismo que usa Windows.
    session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) WinHttpSetTimeouts(session_, 15000, 15000, 60000, 60000);
  }
  ~Http() {
    Disconnect();
    if (session_) WinHttpCloseHandle(session_);
  }
  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  // pace(enviados) se llama tras cada trozo del cuerpo: frena la subida y,
  // si devuelve false, la corta.
  bool Request(const std::wstring& host, INTERNET_PORT port, const wchar_t* method, const std::string& pathQuery,
               const std::wstring& headers, const uint8_t* body, size_t size,
               const std::function<bool(size_t)>& pace, HttpResponse* out) {
    *out = HttpResponse{};
    if (!session_) {
      out->netError = ERROR_WINHTTP_INTERNAL_ERROR;
      return false;
    }
    if (!connect_ || host != host_ || port != port_) {
      Disconnect();
      connect_ = WinHttpConnect(session_, host.c_str(), port, 0);
      if (!connect_) {
        out->netError = GetLastError();
        return false;
      }
      host_ = host;
      port_ = port;
    }
    std::wstring object = Widen(pathQuery);
    HINTERNET request = WinHttpOpenRequest(connect_, method, object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE | WINHTTP_FLAG_ESCAPE_DISABLE |
                                               WINHTTP_FLAG_ESCAPE_DISABLE_QUERY);
    if (!request) {
      out->netError = GetLastError();
      Disconnect();
      return false;
    }
    bool ok = Send(request, headers, body, size, pace, out);
    WinHttpCloseHandle(request);
    if (!ok) Disconnect(); // la próxima vez, conexión nueva
    return ok;
  }

private:
  bool Send(HINTERNET request, const std::wstring& headers, const uint8_t* body, size_t size,
            const std::function<bool(size_t)>& pace, HttpResponse* out) {
    if (!WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, DWORD(size), 0)) {
      out->netError = GetLastError();
      return false;
    }
    size_t sent = 0;
    while (sent < size) {
      DWORD n = DWORD(std::min(kChunk, size - sent)), written = 0;
      if (!WinHttpWriteData(request, body + sent, n, &written)) {
        out->netError = GetLastError();
        return false;
      }
      sent += written;
      if (pace && !pace(sent)) {
        out->netError = ERROR_WINHTTP_OPERATION_CANCELLED;
        return false;
      }
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
      out->netError = GetLastError();
      return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    out->status = status;
    wchar_t etag[256]{};
    len = sizeof(etag);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, etag, &len,
                            WINHTTP_NO_HEADER_INDEX))
      out->etag = Narrow(etag);
    char buf[16 * 1024];
    for (;;) {
      DWORD read = 0;
      if (!WinHttpReadData(request, buf, sizeof(buf), &read)) {
        out->netError = GetLastError();
        return false;
      }
      if (read == 0) break;
      if (out->body.size() < kMaxBody) out->body.append(buf, std::min<size_t>(read, kMaxBody - out->body.size()));
    }
    return true;
  }

  void Disconnect() {
    if (connect_) WinHttpCloseHandle(connect_);
    connect_ = nullptr;
    host_.clear();
  }

  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  std::wstring host_;
  INTERNET_PORT port_ = 0;
};

// GET de una URL entera (el enlace de la prueba).
bool HttpGetUrl(Http& http, const std::string& url, HttpResponse* out) {
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256]{}, path[4096]{}, extra[4096]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = DWORD(std::size(path));
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = DWORD(std::size(extra));
  std::wstring wide = Widen(url);
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    *out = HttpResponse{};
    out->netError = ERROR_WINHTTP_INVALID_URL;
    return false;
  }
  return http.Request(host, parts.nPort, L"GET", Narrow(std::wstring(path) + extra), L"", nullptr, 0, nullptr, out);
}

// ---------------------------------------------------------- respuestas de S3

std::string XmlUnescape(std::string s) {
  const std::pair<const char*, char> entities[] = {
      {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&#34;", '"'}, {"&amp;", '&'}};
  for (const auto& [entity, c] : entities)
    for (size_t at = s.find(entity); at != std::string::npos; at = s.find(entity, at + 1))
      s.replace(at, strlen(entity), 1, c);
  return s;
}

std::string XmlTag(const std::string& xml, const char* tag) {
  const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
  size_t a = xml.find(open);
  if (a == std::string::npos) return {};
  a += open.size();
  size_t b = xml.find(close, a);
  if (b == std::string::npos) return {};
  return XmlUnescape(xml.substr(a, b - a));
}

std::string XmlEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    default: out.push_back(c);
    }
  }
  return out;
}

// InUse: el archivo se está escribiendo o está abierto en otro programa; se espera sin contar como fallo.
// Gone: ya no está en el PC (lo subido solo, como una copia intermedia del editor, se olvida sin avisar).
enum class Outcome { Done, Retry, Offline, Failed, Restart, Cancelled, InUse, Gone };

std::string NetworkText(DWORD code) {
  switch (code) {
  case ERROR_WINHTTP_NAME_NOT_RESOLVED:
  case ERROR_WINHTTP_CANNOT_CONNECT:
  case ERROR_WINHTTP_CONNECTION_ERROR:
    return "Sin conexión con el bucket. Se reintenta sola.";
  case ERROR_WINHTTP_TIMEOUT: return "El bucket tarda demasiado en responder. Se reintenta sola.";
  case ERROR_WINHTTP_SECURE_FAILURE:
    return "No se pudo establecer una conexión segura con el bucket. Revisa la fecha y hora de Windows.";
  default: return "La conexión con el bucket se cortó (error " + std::to_string(code) + "). Se reintenta sola.";
  }
}

// Qué hacer con una respuesta: seguir, reintentar más tarde o rendirse con un
// mensaje que se entienda.
Outcome Classify(const HttpResponse& r, const Target& t, std::string* message) {
  if (r.netError == ERROR_WINHTTP_OPERATION_CANCELLED) return Outcome::Cancelled;
  if (r.netError != 0) {
    *message = NetworkText(r.netError);
    if (r.netError == ERROR_WINHTTP_NAME_NOT_RESOLVED || r.netError == ERROR_WINHTTP_CANNOT_CONNECT)
      return Outcome::Offline;
    return Outcome::Retry;
  }
  if (r.status >= 200 && r.status < 300) return Outcome::Done;
  const std::string code = XmlTag(r.body, "Code");
  const std::string text = XmlTag(r.body, "Message");
  Log("Nube: HTTP %lu %s %s", r.status, code.c_str(), text.c_str());
  if (code == "NoSuchUpload") {
    *message = "La subida por partes caducó en el bucket: se empieza de nuevo.";
    return Outcome::Restart;
  }
  if (r.status == 408 || r.status == 429 || r.status >= 500 || code == "SlowDown" || code == "RequestTimeout" ||
      code == "InternalError") {
    *message = "El bucket está ocupado (código " + std::to_string(r.status) + "). Se reintenta sola.";
    return Outcome::Retry;
  }
  if (code == "InvalidAccessKeyId")
    *message = "Esa clave de acceso no existe: revisa el Access Key ID en Ajustes > Nube.";
  else if (code == "SignatureDoesNotMatch")
    *message = "La clave secreta no corresponde a la de acceso: vuelve a copiar el Secret Access Key.";
  else if (code == "RequestTimeTooSkewed")
    *message = "La hora de Windows está desajustada y el bucket rechaza la firma: sincronízala en Configuración > "
               "Hora e idioma.";
  else if (code == "NoSuchBucket")
    *message = "El bucket «" + t.bucket + "» no existe en esa cuenta: revisa el nombre y la dirección S3.";
  else if (code == "AccessDenied" && (LowerAscii(text).find("storage cap") != std::string::npos ||
                                      LowerAscii(text).find("cap exceeded") != std::string::npos))
    *message = "Tu cuenta llegó a su tope de almacenamiento: libera espacio o súbelo en la web del proveedor.";
  else if (code == "AccessDenied" || r.status == 403)
    *message = "Las claves no tienen permiso para subir a este bucket: dales permiso de escritura (lectura y "
               "escritura de objetos en R2, writeFiles en B2).";
  else if (code == "EntityTooLarge")
    *message = "El archivo es demasiado grande para el bucket.";
  else if (r.status == 404)
    *message = "El bucket no se encuentra (código 404): revisa la dirección S3 y el nombre del bucket.";
  else
    *message = "El bucket respondió con un error (código " + std::to_string(r.status) +
               (code.empty() ? std::string() : ", " + code) + ").";
  return Outcome::Failed;
}

// ".mp4" en minúsculas (path::string() puede lanzar con letras fuera de la página de códigos).
std::string Extension(const std::string& name) {
  return LowerAscii(Narrow(std::filesystem::path(Widen(name)).extension().wstring()));
}

std::string ContentType(const std::string& name) {
  std::string ext = Extension(name);
  if (ext == ".mp4") return "video/mp4";
  if (ext == ".mkv") return "video/x-matroska";
  if (ext == ".mov") return "video/quicktime";
  if (ext == ".m4a") return "audio/mp4";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

// Que el navegador lo enseñe (en vez de descargarlo) y, si se descarga, con su
// nombre. Las cabeceras van en ASCII: el nombre en UTF-8 va codificado aparte.
std::string ContentDisposition(const std::string& name) {
  // El nombre en ASCII para los navegadores viejos: vocales sin tilde, ñ -> n
  // y un _ por cada otro carácter que no lo sea.
  static const std::pair<const char*, char> kPlain[] = {
      {"á", 'a'}, {"é", 'e'}, {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'}, {"ü", 'u'}, {"ñ", 'n'}, {"ç", 'c'},
      {"Á", 'A'}, {"É", 'E'}, {"Í", 'I'}, {"Ó", 'O'}, {"Ú", 'U'}, {"Ü", 'U'}, {"Ñ", 'N'}, {"Ç", 'C'}};
  std::string ascii;
  for (size_t i = 0; i < name.size();) {
    const unsigned char c = (unsigned char)name[i];
    if (c < 0x80) {
      ascii.push_back(c < 32 || c == 127 || c == '"' || c == '\\' ? '_' : char(c));
      ++i;
      continue;
    }
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    char plain = '_';
    for (const auto& [utf8, letter] : kPlain)
      if (name.compare(i, strlen(utf8), utf8) == 0) plain = letter;
    ascii.push_back(plain);
    i += len;
  }
  return "Content-Disposition: inline; filename=\"" + ascii + "\"; filename*=UTF-8''" + UriEncode(name, false);
}

std::string LowerPath(const std::string& path) {
  std::wstring w = Widen(path);
  if (!w.empty()) CharLowerBuffW(w.data(), DWORD(w.size()));
  return Narrow(w);
}

std::string NowText() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char buf[32];
  snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
  return buf;
}

} // namespace

// ----------------------------------------------------------------- estado

namespace {

struct Entry {
  uint64_t id = 0; // solo en memoria: el hilo encuentra la suya aunque cambie la cola
  std::string path, name, lower;
  uint64_t size = 0;
  int64_t mtime = 0;
  std::string key;                      // spectre/<id>-<código>.<ext>
  std::string endpoint, bucket, region; // dónde se subió
  enum class St { Queued, Done, Failed } st = St::Queued;
  std::string uploadId;
  uint64_t partSize = 0;
  std::vector<std::string> etags;
  std::string error;
  std::string when; // fecha de la subida, para Ajustes > Nube
  int64_t at = 0;   // ms Unix de la subida
  bool copyLink = false, automatic = false;
  bool deleteAfter = false;   // se borra del PC en cuanto se sube
  bool deleteAtExit = false;  // se borra al salir de Spectre (modo cibercafé: lo copiado para pegarlo)
  bool urgent = false;        // compartir ahora: no espera a que se cierre el juego
  bool local = true;          // sigue en el PC
  bool deletePending = false; // subido, pero no se pudo borrar del PC todavía
  // Solo en memoria
  bool thisRun = false; // se puso en la cola (o se reintentó) desde que se abrió Spectre
  float progress = 0;
  int tries = 0;
  ULONGLONG retryAt = 0; // tras un fallo pasajero no se vuelve a probar hasta entonces (las demás siguen)
};

const char* StateName(Entry::St st) {
  switch (st) {
  case Entry::St::Done: return "done";
  case Entry::St::Failed: return "failed";
  default: return "queued";
  }
}

} // namespace

struct CloudUploader::Shared {
  mutable std::mutex mutex;
  std::condition_variable cv;
  HWND notify = nullptr;
  CloudConfig config;
  CloudKeys keys;
  bool keysStored = false;
  std::deque<Entry> entries; // en cola, fallidas y ya subidas; las de la cola se suben en este orden
  uint64_t nextId = 1;
  bool running = false; // hay un hilo de subida
  bool stop = true; // hasta Start: sin arrancar, Stop no escribe una cola vacía encima de la guardada
  bool paused = false;
  bool gameActive = false;
  bool offline = false;
  uint64_t currentId = 0;
  double rate = 0;
  std::string lastError;
  std::vector<CloudEvent> events;
  uint64_t version = 1;
  ULONGLONG lastPost = 0;
  ULONGLONG lastSave = 0;
  CloudStatus::Test test = CloudStatus::Test::Idle;
  std::string testMessage;
  unsigned testGen = 0;
  uint64_t wake = 0;     // sube al cambiar algo que corta las esperas del hilo (reintentar, ajustes nuevos)
  std::mutex saveMutex;  // cloud.json lo escriben el hilo de subida y el de la interfaz

  // Con el mutex cogido. progress: solo cambió el porcentaje (como mucho 4 avisos por segundo).
  void Changed(bool progress = false) {
    ++version;
    ULONGLONG now = GetTickCount64();
    if (progress && now - lastPost < 250) return;
    lastPost = now;
    if (notify) PostMessageW(notify, WM_SPECTRE_CLOUD, 0, 0);
  }
  Entry* Find(uint64_t id) {
    for (auto& e : entries)
      if (e.id == id) return &e;
    return nullptr;
  }
  std::deque<Entry>::iterator At(const Entry* e) {
    return std::find_if(entries.begin(), entries.end(), [&](const Entry& x) { return &x == e; });
  }
  Entry* FindPath(const std::string& path) {
    const std::string lower = LowerPath(path);
    // La última que sigue en el PC o en la cola: si un archivo se subió dos
    // veces, vale lo más reciente. Si no, la última ya subida (el enlace de
    // algo que se borró del PC).
    Entry* uploaded = nullptr;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if (it->lower != lower) continue;
      if (it->local || it->st != Entry::St::Done) return &*it;
      if (!uploaded) uploaded = &*it;
    }
    return uploaded;
  }
  const Entry* FindPath(const std::string& path) const { return const_cast<Shared*>(this)->FindPath(path); }
  bool HaveTarget() const { return !keys.Empty() && !Trim(config.endpoint).empty() && !CloudBucket(config).empty(); }
  // Con un juego abierto y «Esperar a cerrarlo», solo sube lo que se compartió a mano.
  bool Waits(const Entry& e) const { return gameActive && config.gameLimitKBps == 0 && !e.urgent; }
  // Modo cibercafé: cada archivo va a la nube que había al guardarlo y nunca
  // cambia de bucket. Lo de otra configuración (otro cliente) espera, y lo que
  // quedó en la cola sin destino de otra vez no se sube solo.
  bool OtherCloud(const Entry& e) const {
    if (!config.cyberMode) return false;
    if (e.endpoint.empty()) return !e.thisRun;
    return e.endpoint != Trim(config.endpoint) || e.bucket != CloudBucket(config);
  }
  // Modo cibercafé: nada de lo ya subido (nombres, enlaces) se queda en la
  // lista; solo lo que aún hay que borrar del PC.
  void CyberCleanup() {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const Entry& e) { return e.st == Entry::St::Done && !e.deletePending; }),
                  entries.end());
  }
  // Hay algo que subir ahora o tras la espera de un reintento.
  bool HasWork() const {
    if (!HaveTarget() || paused || stop) return false;
    for (const auto& e : entries)
      if (e.st == Entry::St::Queued && !Waits(e) && !OtherCloud(e)) return true;
    return false;
  }
  // La siguiente que toca; si todas esperan un reintento, *waitMs dice cuánto falta.
  Entry* Next(ULONGLONG* waitMs) {
    *waitMs = 0;
    if (!HasWork()) return nullptr;
    const ULONGLONG now = GetTickCount64();
    for (auto& e : entries) {
      if (e.st != Entry::St::Queued || Waits(e) || OtherCloud(e)) continue;
      if (e.retryAt <= now) return &e;
      const ULONGLONG wait = e.retryAt - now;
      if (*waitMs == 0 || wait < *waitMs) *waitMs = wait;
    }
    return nullptr;
  }
  // KB/s para la subida en curso; -1 = sin límite.
  int LimitKBps(bool urgent) const {
    if (!gameActive) return -1;
    if (config.gameLimitKBps == 0) return urgent ? kShareKBps : 256; // termina la parte despacio
    return config.gameLimitKBps;
  }

  nlohmann::json ToJson() const {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& e : entries) {
      nlohmann::json v = {{"path", e.path},     {"name", e.name},   {"size", e.size},         {"mtime", e.mtime},
                          {"key", e.key},       {"endpoint", e.endpoint}, {"bucket", e.bucket}, {"region", e.region},
                          {"state", StateName(e.st)}, {"at", e.at}, {"when", e.when},         {"local", e.local}};
      if (!e.uploadId.empty()) {
        v["uploadId"] = e.uploadId;
        v["partSize"] = e.partSize;
        v["etags"] = e.etags;
      }
      if (!e.error.empty()) v["error"] = e.error;
      if (e.copyLink) v["copy"] = true;
      if (e.deleteAfter) v["delete"] = true;
      if (e.deleteAtExit) v["deleteAtExit"] = true;
      if (e.automatic) v["auto"] = true;
      if (e.urgent) v["urgent"] = true;
      if (e.deletePending) v["deletePending"] = true;
      items.push_back(std::move(v));
    }
    return {{"version", 1}, {"paused", paused}, {"items", std::move(items)}};
  }

  void Load() {
    std::ifstream in(QueuePath());
    if (!in) return;
    try {
      auto j = nlohmann::json::parse(in, nullptr, true, true);
      paused = j.value("paused", false); // «Pausar» sigue tras reiniciar
      for (const auto& v : j.value("items", nlohmann::json::array())) {
        Entry e;
        e.id = nextId++;
        e.path = v.value("path", "");
        e.name = v.value("name", "");
        e.lower = LowerPath(e.path);
        e.size = v.value("size", uint64_t(0));
        e.mtime = v.value("mtime", int64_t(0));
        e.key = v.value("key", "");
        e.endpoint = v.value("endpoint", "");
        e.bucket = v.value("bucket", "");
        e.region = v.value("region", "");
        const std::string st = v.value("state", "queued");
        e.st = st == "done" ? Entry::St::Done : st == "failed" ? Entry::St::Failed : Entry::St::Queued;
        e.at = v.value("at", int64_t(0));
        e.when = v.value("when", "");
        e.local = v.value("local", true);
        e.uploadId = v.value("uploadId", "");
        e.partSize = v.value("partSize", uint64_t(0));
        if (v.contains("etags")) e.etags = v["etags"].get<std::vector<std::string>>();
        e.error = v.value("error", "");
        e.copyLink = v.value("copy", false);
        e.deleteAfter = v.value("delete", false);
        e.deleteAtExit = v.value("deleteAtExit", false);
        e.automatic = v.value("auto", false);
        e.urgent = v.value("urgent", false);
        e.deletePending = v.value("deletePending", false);
        if (e.path.empty() || e.key.empty()) continue;
        entries.push_back(std::move(e));
      }
    } catch (const std::exception& ex) {
      Log("cloud.json inválido, se ignora: %s", ex.what());
      entries.clear();
    }
  }
};

namespace {

using Shared = CloudUploader::Shared;

// Escribe cloud.json; force = false: como mucho cada 5 s (progreso de las partes).
void SaveQueue(Shared& sh, bool force) {
  // saveMutex antes de la foto: si no, dos hilos pueden escribir sus fotos al
  // revés y ganar la más vieja. Nadie coge saveMutex con mutex cogido.
  std::lock_guard save(sh.saveMutex);
  std::string text;
  {
    std::lock_guard lock(sh.mutex);
    ULONGLONG now = GetTickCount64();
    if (!force && now - sh.lastSave < 5000) return;
    sh.lastSave = now;
    text = sh.ToJson().dump();
  }
  if (!WriteFileAtomic(QueuePath(), text.data(), text.size())) Log("Nube: no se pudo guardar cloud.json");
}

// Enlace de algo subido: la dirección pública si es el mismo bucket; si no, firmado ahora.
std::string LinkFor(const Shared& sh, const Entry& e) {
  const CloudConfig& c = sh.config;
  if (!Trim(c.publicUrl).empty() && Trim(c.endpoint) == e.endpoint && CloudBucket(c) == e.bucket)
    return PublicLink(c.publicUrl, e.key);
  CloudConfig at = c;
  at.endpoint = e.endpoint;
  at.bucket = e.bucket;
  at.region = e.region;
  Target t;
  if (!MakeTarget(at, sh.keys, &t, nullptr)) return {};
  return Presign(t, e.key, c.linkDays * 86400);
}

// Lo que el hilo necesita de la entrada, copiado fuera del mutex.
struct Job {
  uint64_t id = 0;
  std::string path, name, key;
  uint64_t size = 0;
  int64_t mtime = 0;
  std::string uploadId;
  uint64_t partSize = 0;
  std::vector<std::string> etags;
  bool urgent = false;
};

// Frena la subida al límite que toque en cada momento (con un juego abierto) y
// mide la velocidad para Ajustes y la biblioteca.
class Pacer {
public:
  // false: hay que cortar (salir de Spectre, subida cancelada).
  bool Sent(Shared& sh, uint64_t id, uint64_t doneBefore, uint64_t total, size_t sentNow, bool urgent) {
    const ULONGLONG now = GetTickCount64();
    const size_t delta = sentNow - lastSent_;
    lastSent_ = sentNow;
    rateBytes_ += delta;
    int limit;
    {
      std::lock_guard lock(sh.mutex);
      Entry* e = sh.Find(id);
      if (sh.stop || !e || e->st != Entry::St::Queued) return false;
      e->progress = total ? float(double(doneBefore + sentNow) / double(total)) : 0.0f;
      if (now - rateAt_ >= 1000) {
        sh.rate = double(rateBytes_) * 1000.0 / double(now - rateAt_);
        rateAt_ = now;
        rateBytes_ = 0;
      }
      sh.Changed(true);
      limit = sh.LimitKBps(urgent);
    }
    if (limit != limit_ || now - windowAt_ > 3000) {
      // Límite nuevo (o una pausa larga): la cuenta empieza de cero, sin ráfaga.
      limit_ = limit;
      windowAt_ = now;
      windowBytes_ = 0;
    }
    windowBytes_ += delta;
    if (limit_ > 0) {
      const double due = double(windowBytes_) * 1000.0 / (double(limit_) * 1024.0);
      const double elapsed = double(GetTickCount64() - windowAt_);
      if (due > elapsed) Sleep(DWORD(std::min(due - elapsed, 500.0)));
    }
    return true;
  }
  void NewRequest() { lastSent_ = 0; }

private:
  int limit_ = -2;
  ULONGLONG windowAt_ = 0;
  uint64_t windowBytes_ = 0;
  size_t lastSent_ = 0;
  ULONGLONG rateAt_ = GetTickCount64();
  uint64_t rateBytes_ = 0;
};

bool ReadAt(HANDLE file, uint64_t offset, uint8_t* data, size_t size) {
  LARGE_INTEGER pos;
  pos.QuadPart = LONGLONG(offset);
  if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) return false;
  while (size > 0) {
    DWORD n = DWORD(std::min<size_t>(size, 1u << 30)), read = 0;
    if (!ReadFile(file, data, n, &read, nullptr) || read == 0) return false;
    data += read;
    size -= read;
  }
  return true;
}

// Guarda en la entrada lo que el hilo sabe de la subida por partes.
void StoreProgress(Shared& sh, const Job& job, bool force) {
  {
    std::lock_guard lock(sh.mutex);
    Entry* e = sh.Find(job.id);
    if (!e) return;
    e->size = job.size;
    e->mtime = job.mtime;
    e->uploadId = job.uploadId;
    e->partSize = job.partSize;
    e->etags = job.etags;
  }
  SaveQueue(sh, force);
}

Outcome Upload(Shared& sh, Http& http, const Target& t, Job& job, std::vector<uint8_t>& buffer, std::string* error) {
  // Sin FILE_SHARE_WRITE: si algo lo sigue escribiendo (una grabación que
  // termina de vaciarse al disco), no se abre y se prueba más tarde en vez de
  // subir medio archivo. Con FILE_SHARE_DELETE se puede renombrar mientras sube.
  HANDLE file = CreateFileW(Widen(job.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
      *error = "El archivo ya no está en el PC.";
      if (!job.uploadId.empty()) { // que el bucket suelte las partes ya subidas
        const std::string uri = ObjectUri(t, job.key), query = CanonicalQuery({{"uploadId", job.uploadId}});
        HttpResponse r;
        http.Request(t.host, t.port, L"DELETE", uri + "?" + query,
                     SignedHeaders(t, "DELETE", uri, query, Hex(Sha256("", 0))), nullptr, 0, nullptr, &r);
      }
      return Outcome::Gone;
    }
    if (e == ERROR_SHARING_VIOLATION) return Outcome::InUse; // se sube cuando lo suelten
    *error = "No se pudo abrir el archivo (error " + std::to_string(e) + ").";
    return Outcome::Failed;
  }
  struct Closer {
    HANDLE h;
    ~Closer() { CloseHandle(h); }
  } closer{file};
  LARGE_INTEGER size{};
  FILETIME written{};
  GetFileSizeEx(file, &size);
  GetFileTime(file, nullptr, nullptr, &written);
  const int64_t mtime = int64_t((uint64_t(written.dwHighDateTime) << 32) | written.dwLowDateTime);
  if (size.QuadPart <= 0) {
    *error = "El archivo está vacío.";
    return Outcome::Failed;
  }
  if (uint64_t(size.QuadPart) != job.size || mtime != job.mtime) {
    // Cambió desde que se puso en la cola: se sube entero otra vez.
    if (!job.uploadId.empty()) {
      const std::string uri = ObjectUri(t, job.key), query = CanonicalQuery({{"uploadId", job.uploadId}});
      HttpResponse r;
      http.Request(t.host, t.port, L"DELETE", uri + "?" + query,
                   SignedHeaders(t, "DELETE", uri, query, Hex(Sha256("", 0))), nullptr, 0, nullptr, &r);
    }
    job.size = uint64_t(size.QuadPart);
    job.mtime = mtime;
    job.uploadId.clear();
    job.etags.clear();
    job.partSize = 0;
    StoreProgress(sh, job, true);
  }

  const std::string uri = ObjectUri(t, job.key);
  const std::vector<std::string> meta = {"Content-Type: " + ContentType(job.name), ContentDisposition(job.name)};
  Pacer pacer;
  HttpResponse r;

  if (job.size <= kSinglePutMax) {
    buffer.resize(size_t(job.size));
    if (!ReadAt(file, 0, buffer.data(), buffer.size())) {
      *error = "No se pudo leer el archivo.";
      return Outcome::Retry;
    }
    const std::string hash = Hex(Sha256(buffer.data(), buffer.size()));
    http.Request(t.host, t.port, L"PUT", uri, SignedHeaders(t, "PUT", uri, "", hash, meta), buffer.data(),
                 buffer.size(), [&](size_t sent) { return pacer.Sent(sh, job.id, 0, job.size, sent, job.urgent); },
                 &r);
    return Classify(r, t, error);
  }

  // Por partes: se crea la subida (una vez) y cada parte se guarda al llegar.
  if (job.uploadId.empty()) {
    const std::string query = CanonicalQuery({{"uploads", ""}});
    http.Request(t.host, t.port, L"POST", uri + "?" + query,
                 SignedHeaders(t, "POST", uri, query, Hex(Sha256("", 0)), meta), nullptr, 0, nullptr, &r);
    Outcome o = Classify(r, t, error);
    if (o != Outcome::Done) return o;
    job.uploadId = XmlTag(r.body, "UploadId");
    if (job.uploadId.empty()) {
      *error = "El bucket no devolvió el número de la subida por partes.";
      return Outcome::Retry;
    }
    job.partSize = std::max<uint64_t>(kPartSize, ((job.size + kMaxParts - 1) / kMaxParts + (1 << 20) - 1) &
                                                     ~uint64_t((1 << 20) - 1));
    job.etags.clear();
    StoreProgress(sh, job, true);
  }
  if (job.partSize == 0) job.partSize = kPartSize;
  const uint64_t parts = (job.size + job.partSize - 1) / job.partSize;
  for (uint64_t i = job.etags.size(); i < parts; ++i) {
    // Con un juego abierto y «Esperar a cerrarlo», entre parte y parte se para.
    {
      std::lock_guard lock(sh.mutex);
      Entry* e = sh.Find(job.id);
      if (sh.stop || !e || e->st != Entry::St::Queued) return Outcome::Cancelled;
      if (sh.paused || sh.Waits(*e)) return Outcome::Cancelled; // sigue en la cola: el hilo se va y vuelve luego
    }
    const uint64_t offset = i * job.partSize;
    const size_t n = size_t(std::min(job.partSize, job.size - offset));
    buffer.resize(n);
    if (!ReadAt(file, offset, buffer.data(), n)) {
      *error = "No se pudo leer el archivo.";
      return Outcome::Retry;
    }
    const std::string hash = Hex(Sha256(buffer.data(), n));
    const std::string query = CanonicalQuery({{"partNumber", std::to_string(i + 1)}, {"uploadId", job.uploadId}});
    pacer.NewRequest();
    http.Request(t.host, t.port, L"PUT", uri + "?" + query, SignedHeaders(t, "PUT", uri, query, hash), buffer.data(),
                 n, [&](size_t sent) { return pacer.Sent(sh, job.id, offset, job.size, sent, job.urgent); }, &r);
    Outcome o = Classify(r, t, error);
    if (o == Outcome::Restart) {
      job.uploadId.clear();
      job.etags.clear();
      StoreProgress(sh, job, true);
      return Outcome::Retry;
    }
    if (o != Outcome::Done) return o;
    if (r.etag.empty()) {
      *error = "El bucket no devolvió la huella (ETag) de una parte.";
      return Outcome::Retry;
    }
    job.etags.push_back(r.etag);
    StoreProgress(sh, job, false);
    // Una parte bien: los fallos de antes ya no cuentan.
    std::lock_guard lock(sh.mutex);
    if (Entry* e = sh.Find(job.id)) e->tries = 0;
  }
  buffer.clear();
  buffer.shrink_to_fit();

  std::string xml = "<CompleteMultipartUpload>";
  for (size_t i = 0; i < job.etags.size(); ++i)
    xml += "<Part><PartNumber>" + std::to_string(i + 1) + "</PartNumber><ETag>" + XmlEscape(job.etags[i]) +
           "</ETag></Part>";
  xml += "</CompleteMultipartUpload>";
  const std::string query = CanonicalQuery({{"uploadId", job.uploadId}});
  http.Request(t.host, t.port, L"POST", uri + "?" + query,
               SignedHeaders(t, "POST", uri, query, Hex(Sha256(xml)), {"Content-Type: application/xml"}),
               reinterpret_cast<const uint8_t*>(xml.data()), xml.size(), nullptr, &r);
  Outcome o = Classify(r, t, error);
  // S3 puede responder 200 con un error dentro.
  if (o == Outcome::Done && r.body.find("<Error>") != std::string::npos) {
    const std::string code = XmlTag(r.body, "Code");
    Log("Nube: CompleteMultipartUpload devolvió %s", code.c_str());
    if (code == "NoSuchUpload" || code == "InvalidPart" || code == "InvalidPartOrder") {
      job.uploadId.clear();
      job.etags.clear();
      StoreProgress(sh, job, true);
    }
    *error = "El bucket no pudo juntar las partes. Se reintenta.";
    return Outcome::Retry;
  }
  if (o == Outcome::Restart) {
    job.uploadId.clear();
    job.etags.clear();
    StoreProgress(sh, job, true);
    return Outcome::Retry;
  }
  return o;
}

// Espera `ms`, hasta que haya que salir o hasta que algo cambie (reintentar, ajustes nuevos).
void WaitFor(Shared& sh, std::unique_lock<std::mutex>& lock, DWORD ms) {
  const uint64_t wake = sh.wake;
  sh.cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return sh.stop || sh.wake != wake; });
}

void Worker(std::shared_ptr<Shared> sh) {
  InstallThreadCrashHandler();
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  SetThreadEcoQoS(true);
  BackgroundMode background; // leer del disco a prioridad muy baja: el juego primero
  Http http;
  std::vector<uint8_t> buffer;
  for (;;) {
    Job job;
    Target target;
    {
      std::unique_lock lock(sh->mutex);
      Entry* e = nullptr;
      for (;;) {
        ULONGLONG wait = 0;
        e = sh->stop ? nullptr : sh->Next(&wait);
        if (e || wait == 0) break;
        WaitFor(*sh, lock, DWORD(std::min<ULONGLONG>(wait, 60000)));
      }
      if (!e) {
        sh->running = false;
        sh->currentId = 0;
        sh->rate = 0;
        sh->Changed();
        return;
      }
      CloudConfig at = sh->config;
      if (!e->endpoint.empty() && !e->uploadId.empty()) {
        // Una subida por partes empezada en otro bucket se termina allí.
        at.endpoint = e->endpoint;
        at.bucket = e->bucket;
        at.region = e->region;
      }
      std::string error;
      if (!MakeTarget(at, sh->keys, &target, &error)) {
        e->st = Entry::St::Failed;
        e->error = error;
        sh->lastError = error;
        CloudEvent ev;
        ev.path = e->path;
        ev.name = e->name;
        ev.error = error;
        ev.automatic = e->automatic;
        sh->events.push_back(std::move(ev));
        sh->Changed();
        continue;
      }
      if (e->key.empty() || e->uploadId.empty()) {
        e->endpoint = target.endpoint;
        e->bucket = target.bucket;
        e->region = target.region;
      }
      sh->currentId = e->id;
      e->progress = 0;
      job.id = e->id;
      job.path = e->path;
      job.name = e->name;
      job.key = e->key;
      job.size = e->size;
      job.mtime = e->mtime;
      job.uploadId = e->uploadId;
      job.partSize = e->partSize;
      job.etags = e->etags;
      job.urgent = e->urgent;
      sh->Changed();
    }

    std::string error;
    Outcome o = Outcome::Failed;
    try {
      o = Upload(*sh, http, target, job, buffer, &error);
    } catch (const std::exception& ex) {
      Log("Nube: excepción subiendo %s: %s", job.name.c_str(), ex.what());
      error = "Error inesperado al subir.";
    }
    buffer.clear();
    buffer.shrink_to_fit(); // nada de memoria retenida entre archivos

    bool deleted = false;
    if (o == Outcome::Done) {
      // Sin la clave: con ella y la dirección pública, el log abriría el enlace.
      Log("Nube: subido %s (%s)", job.name.c_str(), FormatBytes(job.size).c_str());
      std::string removePath; // la ruta de ahora: pudo renombrarse mientras subía
      {
        std::lock_guard lock(sh->mutex);
        Entry* e = sh->Find(job.id);
        // Tras Stop (al salir) también lo que se borraba al salir: esa limpieza ya pasó.
        if (e && (e->deleteAfter || (sh->stop && e->deleteAtExit))) removePath = e->path;
      }
      if (!removePath.empty()) {
        deleted = DeleteFileW(Widen(removePath).c_str()) != FALSE;
        if (!deleted) Log("Nube: subido, pero no se pudo borrar del PC (%lu): %s", GetLastError(), job.name.c_str());
      }
    }

    std::unique_lock lock(sh->mutex);
    sh->currentId = 0;
    Entry* e = sh->Find(job.id);
    if (!e) continue; // se canceló mientras subía
    if (o == Outcome::Gone) {
      if (e->automatic) {
        Log("Nube: %s ya no está en el PC: no se sube", e->name.c_str());
        sh->entries.erase(sh->At(e));
        sh->Changed();
        lock.unlock();
        SaveQueue(*sh, true);
        continue;
      }
      o = Outcome::Failed;
    }
    switch (o) {
    case Outcome::Done: {
      e->st = Entry::St::Done;
      e->uploadId.clear();
      e->etags.clear();
      e->partSize = 0;
      e->error.clear();
      e->tries = 0;
      e->progress = 1;
      e->at = int64_t(UnixMillis());
      e->when = NowText();
      e->local = !deleted;
      e->deletePending = !deleted && (e->deleteAfter || (sh->stop && e->deleteAtExit));
      sh->offline = false;
      CloudEvent ev;
      ev.path = e->path;
      ev.name = e->name;
      ev.ok = true;
      ev.link = LinkFor(*sh, *e);
      ev.copyLink = e->copyLink;
      ev.automatic = e->automatic;
      ev.deleted = deleted;
      sh->events.push_back(std::move(ev));
      // La más reciente al final: el orden de la lista es el de las subidas.
      // Tras salir en el modo cibercafé no se apunta (salvo lo que falta borrar).
      Entry done = std::move(*e);
      sh->entries.erase(sh->At(e));
      if (!(sh->stop && sh->config.cyberMode && !done.deletePending)) sh->entries.push_back(std::move(done));
      size_t history = 0;
      for (const auto& x : sh->entries) history += x.st == Entry::St::Done ? 1 : 0;
      for (auto it = sh->entries.begin(); history > kMaxHistory && it != sh->entries.end();) {
        if (it->st == Entry::St::Done) {
          it = sh->entries.erase(it);
          --history;
        } else {
          ++it;
        }
      }
      sh->Changed();
      lock.unlock();
      SaveQueue(*sh, true);
      break;
    }
    case Outcome::Cancelled:
      e->progress = 0;
      sh->Changed();
      break;
    case Outcome::InUse:
      // Se está escribiendo (una grabación, una exportación) o está abierto en
      // otro programa: se vuelve a probar sin contarlo como fallo.
      e->progress = 0;
      e->retryAt = GetTickCount64() + 15000;
      sh->Changed();
      break;
    case Outcome::Offline:
      // Sin red: no cuenta como fallo, se vuelve a probar cada 30 s.
      sh->offline = true;
      sh->lastError = error;
      e->progress = 0;
      e->retryAt = GetTickCount64() + 30000;
      sh->Changed();
      break;
    case Outcome::Retry:
    case Outcome::Restart:
      sh->offline = false;
      sh->lastError = error;
      e->progress = 0;
      if (++e->tries < kMaxTries) {
        // Mientras espera, sube las demás (un archivo abierto en otro programa no frena la cola).
        Log("Nube: %s, reintento %d: %s", e->name.c_str(), e->tries, error.c_str());
        e->retryAt = GetTickCount64() + ULONGLONG(std::min(60000, 2000 << std::min(e->tries - 1, 5)));
        sh->Changed();
        break;
      }
      [[fallthrough]];
    case Outcome::Gone: // a mano: se avisa como un fallo
    case Outcome::Failed:
      Log("Nube: no se pudo subir %s: %s", e->name.c_str(), error.c_str());
      e->st = Entry::St::Failed;
      e->error = error;
      e->tries = 0;
      e->progress = 0;
      sh->offline = false;
      sh->lastError = error;
      {
        CloudEvent ev;
        ev.path = e->path;
        ev.name = e->name;
        ev.error = error;
        ev.automatic = e->automatic;
        sh->events.push_back(std::move(ev));
      }
      sh->Changed();
      lock.unlock();
      SaveQueue(*sh, true);
      break;
    }
  }
}

// Ajustes > Nube > Probar.
void RunTest(std::shared_ptr<Shared> sh, unsigned gen, CloudConfig config, CloudKeys keys) {
  InstallThreadCrashHandler();
  auto finish = [&](CloudStatus::Test result, const std::string& message) {
    std::lock_guard lock(sh->mutex);
    if (sh->testGen != gen) return;
    sh->test = result;
    sh->testMessage = message;
    sh->Changed();
  };
  Target t;
  std::string error;
  if (!MakeTarget(config, keys, &t, &error)) {
    finish(CloudStatus::Test::Failed, error);
    return;
  }
  Http http;
  const std::string code = RandomCode(16);
  if (code.empty()) {
    finish(CloudStatus::Test::Failed, "Windows no pudo generar un código aleatorio.");
    return;
  }
  const std::string key = std::string(kKeyPrefix) + "prueba-" + code + ".txt";
  const std::string body = "Spectre puede subir clips a este bucket.\n";
  const std::string uri = ObjectUri(t, key);
  HttpResponse r;
  http.Request(t.host, t.port, L"PUT", uri,
               SignedHeaders(t, "PUT", uri, "", Hex(Sha256(body)), {"Content-Type: text/plain; charset=utf-8"}),
               reinterpret_cast<const uint8_t*>(body.data()), body.size(), nullptr, &r);
  if (Classify(r, t, &error) != Outcome::Done) {
    finish(CloudStatus::Test::Failed, "No se pudo subir: " + error);
    return;
  }
  const bool isPublic = !Trim(config.publicUrl).empty();
  const std::string link = isPublic ? PublicLink(config.publicUrl, key) : Presign(t, key, 3600);
  HttpResponse g;
  bool linkOk = HttpGetUrl(http, link, &g) && g.status == 200 && g.body == body;
  const std::string delUri = ObjectUri(t, key);
  HttpResponse d;
  http.Request(t.host, t.port, L"DELETE", delUri, SignedHeaders(t, "DELETE", delUri, "", Hex(Sha256("", 0))),
               nullptr, 0, nullptr, &d);
  const bool removed = d.netError == 0 && d.status >= 200 && d.status < 300;
  std::string tail = removed ? "" : " (Las claves no pueden borrar: el archivo de prueba se queda en spectre/.)";
  if (linkOk) {
    finish(CloudStatus::Test::Ok,
           std::string("Todo bien: Spectre sube al bucket y ") +
               (isPublic ? "los enlaces públicos se abren." : "los enlaces firmados se abren.") + tail);
    return;
  }
  std::string why = g.netError ? NetworkText(g.netError) : "código " + std::to_string(g.status);
  if (isPublic)
    finish(CloudStatus::Test::Warning,
           "Spectre sube al bucket, pero la dirección pública no abre el archivo (" + why +
               "). Revisa que el acceso público esté activado y que la dirección sea la del bucket, o déjala vacía "
               "para usar enlaces firmados." + tail);
  else
    finish(CloudStatus::Test::Warning,
           "Spectre sube al bucket, pero el enlace firmado no se abre (" + why +
               "). Si las claves solo pueden escribir, pon la dirección pública del bucket." + tail);
}

} // namespace

// ---------------------------------------------------------------- CloudUploader

CloudUploader::CloudUploader() : shared_(std::make_shared<Shared>()) {}

CloudUploader::~CloudUploader() { Stop(); }

void CloudUploader::Start(HWND notify, const CloudConfig& config) {
  {
    std::lock_guard lock(shared_->mutex);
    shared_->notify = notify;
    shared_->config = config;
    shared_->stop = false;
    shared_->Load();
    // Lo que se estaba subiendo al cerrar sigue en la cola; lo que falló, también fallado.
    if (!config.cyberMode && LoadKeysFile(&shared_->keys)) shared_->keysStored = true;
    if (config.cyberMode) DeleteKeysFile(); // en un PC compartido nunca quedan en disco
    shared_->Changed();
  }
  // Subidas terminadas que no se pudieron borrar del PC (estaba abierto en otro programa).
  RemoveUploaded(false);
  {
    std::lock_guard lock(shared_->mutex);
    Shared& sh = *shared_;
    if (sh.config.cyberMode) {
      // Otra sesión (quizá de otro cliente) pudo acabar sin Stop: Windows
      // cerró la sesión o mataron el proceso. Lo subido no se enseña, y lo que
      // no llegó a subirse no va solo a la nube de quien pegue ahora la suya.
      sh.CyberCleanup();
      sh.paused = false; // la pausa era del cliente anterior
      for (auto& e : sh.entries) {
        if (e.st == Entry::St::Done) continue;
        e.st = Entry::St::Failed;
        e.error = "Quedó de otra sesión del modo cibercafé y no se sube sola. «Reintentar» la sube a la nube que "
                  "esté configurada ahora.";
        e.endpoint.clear();
        e.bucket.clear();
        e.region.clear();
        e.uploadId.clear();
        e.etags.clear();
        e.partSize = 0;
      }
      sh.Changed();
    }
  }
  SaveQueue(*shared_, true);
  Kick();
}

std::vector<std::string> CloudUploader::RemoveUploaded(bool atExit) {
  std::vector<std::string> paths;
  {
    std::lock_guard lock(shared_->mutex);
    for (const auto& e : shared_->entries)
      if (e.st == Entry::St::Done && e.local && (e.deletePending || (atExit && e.deleteAtExit)))
        paths.push_back(e.path);
  }
  std::vector<std::string> left;
  for (const auto& path : paths) {
    const bool gone = DeleteFileW(Widen(path).c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    if (!gone) left.push_back(path);
    std::lock_guard lock(shared_->mutex);
    const std::string lower = LowerPath(path);
    for (auto& e : shared_->entries)
      if (e.lower == lower && e.st == Entry::St::Done && e.local) {
        e.local = !gone;
        e.deletePending = !gone;
      }
  }
  if (!paths.empty()) {
    std::lock_guard lock(shared_->mutex);
    shared_->Changed();
  }
  return left;
}

void CloudUploader::Configure(const CloudConfig& config) {
  {
    std::lock_guard lock(shared_->mutex);
    shared_->config = config;
    ++shared_->wake;
    shared_->Changed();
  }
  shared_->cv.notify_all();
  Kick();
}

std::vector<std::string> CloudUploader::Stop() {
  {
    std::lock_guard lock(shared_->mutex);
    if (shared_->stop) return {};
    shared_->stop = true;
    ++shared_->testGen;
  }
  shared_->cv.notify_all();
  // Lo que había que borrar del PC: lo que no se pudo al subirse (la ventana o
  // el panel lo tenían abierto, y ya se cerraron) y lo que se borraba al salir.
  std::vector<std::string> left = RemoveUploaded(true);
  {
    std::lock_guard lock(shared_->mutex);
    // PC compartido: nada de lo ya subido (nombres, enlaces) se queda aquí.
    if (shared_->config.cyberMode) shared_->CyberCleanup();
    Wipe(shared_->keys.secret);
    shared_->keys = {};
  }
  SaveQueue(*shared_, true);
  return left;
}

bool CloudUploader::Configured() const {
  std::lock_guard lock(shared_->mutex);
  return shared_->HaveTarget();
}

bool CloudUploader::SetKeys(const CloudKeys& keys, bool persist, std::string* error) {
  CloudKeys k{Trim(keys.keyId), Trim(keys.secret)};
  if (k.Empty()) {
    if (error) *error = "Escribe las dos claves.";
    return false;
  }
  bool stored = false;
  if (persist) {
    stored = SaveKeysFile(k);
    if (!stored && error) *error = "No se pudieron guardar las claves: siguen en memoria hasta que salgas de Spectre.";
  } else {
    DeleteKeysFile();
  }
  {
    std::lock_guard lock(shared_->mutex);
    Wipe(shared_->keys.secret);
    shared_->keys = k;
    shared_->keysStored = stored;
    // Con claves nuevas, lo que falló por ellas se vuelve a intentar (en el
    // cibercafé, lo de otra sesión solo con «Reintentar»).
    for (auto& e : shared_->entries)
      if (e.st == Entry::St::Failed && (!shared_->config.cyberMode || e.thisRun)) {
        e.st = Entry::St::Queued;
        e.error.clear();
      }
    ++shared_->wake;
    shared_->Changed();
  }
  shared_->cv.notify_all();
  Kick();
  return stored || !persist;
}

void CloudUploader::PersistKeys(bool persist) {
  CloudKeys keys;
  {
    std::lock_guard lock(shared_->mutex);
    keys = shared_->keys;
  }
  bool stored = false;
  if (persist && !keys.Empty()) stored = SaveKeysFile(keys);
  else if (!persist) DeleteKeysFile();
  Wipe(keys.secret);
  std::lock_guard lock(shared_->mutex);
  shared_->keysStored = stored;
  shared_->Changed();
}

void CloudUploader::ForgetKeys() {
  DeleteKeysFile();
  std::lock_guard lock(shared_->mutex);
  Wipe(shared_->keys.secret);
  shared_->keys = {};
  shared_->keysStored = false;
  shared_->Changed();
}

CloudKeys CloudUploader::Keys() const {
  std::lock_guard lock(shared_->mutex);
  return shared_->keys;
}

void CloudUploader::Enqueue(const std::string& path, bool front, bool copyLink, CloudDelete removal, bool automatic) {
  const bool deleteAfter = removal == CloudDelete::AfterUpload, deleteAtExit = removal == CloudDelete::AtExit;
  if (path.empty()) return;
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (!GetFileAttributesExW(Widen(path).c_str(), GetFileExInfoStandard, &fa)) {
    Log("Nube: no se puede subir %s: no existe", path.c_str());
    return;
  }
  const uint64_t size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
  const int64_t mtime = int64_t((uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime);
  {
    std::lock_guard lock(shared_->mutex);
    Shared& sh = *shared_;
    Entry* existing = sh.FindPath(path);
    if (existing && existing->st == Entry::St::Done && existing->size == size && existing->mtime == mtime) {
      // Ya está subido tal cual: solo el aviso con el enlace.
      CloudEvent ev;
      ev.path = existing->path;
      ev.name = existing->name;
      ev.ok = true;
      ev.link = LinkFor(sh, *existing);
      ev.copyLink = copyLink;
      ev.automatic = automatic;
      sh.events.push_back(std::move(ev));
      sh.Changed();
      return;
    }
    if (existing && existing->st != Entry::St::Done) {
      // Ya en la cola (o fallado): se mueve delante si corre prisa y se vuelve a intentar.
      existing->st = Entry::St::Queued;
      existing->error.clear();
      existing->tries = 0;
      existing->retryAt = 0;
      existing->copyLink |= copyLink;
      existing->urgent |= front;
      existing->deleteAfter |= deleteAfter;
      existing->deleteAtExit |= deleteAtExit;
      if (deleteAtExit) existing->deleteAfter = false; // se copió para pegarlo: aguanta hasta salir
      existing->automatic &= automatic;
      existing->thisRun = true;
      // Pedirlo a mano lo lleva a la nube de ahora (si no había empezado en otra).
      if (sh.config.cyberMode && existing->uploadId.empty()) existing->endpoint.clear();
      if (front && existing->id != sh.currentId) {
        Entry moved = std::move(*existing);
        sh.entries.erase(sh.At(existing));
        sh.entries.push_front(std::move(moved));
      }
      sh.Changed();
    } else {
      // Lo subido era otra versión del archivo: su enlace sigue en la lista,
      // pero la biblioteca ya enseña esta.
      if (existing) existing->local = false;
      Entry e;
      e.id = sh.nextId++;
      e.path = path;
      e.lower = LowerPath(path);
      e.name = Narrow(std::filesystem::path(Widen(path)).filename().wstring());
      e.size = size;
      e.mtime = mtime;
      const std::string code = RandomCode(24);
      std::string ext = Extension(path);
      if (ext.size() > 8 || ext.find_first_not_of(".abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos)
        ext.clear();
      if (code.empty()) {
        e.st = Entry::St::Failed;
        e.error = "Windows no pudo generar el código del enlace.";
      }
      e.key = std::string(kKeyPrefix) + Base36(UnixMillis()) + "-" + code + ext;
      e.copyLink = copyLink;
      e.urgent = front;
      e.deleteAfter = deleteAfter;
      e.deleteAtExit = deleteAtExit;
      e.automatic = automatic;
      e.thisRun = true;
      if (sh.config.cyberMode && !Trim(sh.config.endpoint).empty()) {
        // Modo cibercafé: a la nube de quien lo guardó, aunque luego se pegue otra.
        e.endpoint = Trim(sh.config.endpoint);
        e.bucket = CloudBucket(sh.config);
      }
      // Compartir: la siguiente en subir (la que ya sube no se interrumpe).
      if (front) sh.entries.push_front(std::move(e));
      else sh.entries.push_back(std::move(e));
      sh.Changed();
    }
  }
  SaveQueue(*shared_, true);
  shared_->cv.notify_all();
  Kick();
}

namespace {

// Subida por partes que ya no se va a terminar: se avisa al bucket para que
// suelte las partes (B2 las cobra como espacio usado). Sin esperar respuesta.
struct PendingAbort {
  CloudConfig config;
  std::string key, uploadId;
};

PendingAbort AbortFor(const Shared& sh, const Entry& e) {
  PendingAbort a;
  a.config = sh.config;
  a.config.endpoint = e.endpoint;
  a.config.bucket = e.bucket;
  a.config.region = e.region;
  a.key = e.key;
  a.uploadId = e.uploadId;
  return a;
}

void AbortUploads(std::vector<PendingAbort> aborts, CloudKeys keys) {
  if (aborts.empty() || keys.Empty()) return;
  std::thread([aborts = std::move(aborts), keys] {
    InstallThreadCrashHandler();
    Http http;
    for (const auto& a : aborts) {
      Target t;
      if (!MakeTarget(a.config, keys, &t, nullptr)) continue;
      const std::string uri = ObjectUri(t, a.key), query = CanonicalQuery({{"uploadId", a.uploadId}});
      HttpResponse r;
      http.Request(t.host, t.port, L"DELETE", uri + "?" + query,
                   SignedHeaders(t, "DELETE", uri, query, Hex(Sha256("", 0))), nullptr, 0, nullptr, &r);
      Log("Nube: subida por partes cancelada en el bucket (HTTP %lu)", r.status);
    }
  }).detach();
}

} // namespace

void CloudUploader::Cancel(const std::string& path) {
  std::vector<PendingAbort> aborts;
  CloudKeys keys;
  {
    std::lock_guard lock(shared_->mutex);
    Shared& sh = *shared_;
    Entry* e = sh.FindPath(path);
    if (!e || e->st == Entry::St::Done) return;
    if (!e->uploadId.empty()) aborts.push_back(AbortFor(sh, *e));
    keys = sh.keys;
    sh.entries.erase(sh.At(e));
    sh.Changed();
  }
  SaveQueue(*shared_, true);
  shared_->cv.notify_all();
  AbortUploads(std::move(aborts), std::move(keys));
}

void CloudUploader::Retry(const std::string& path) {
  {
    std::lock_guard lock(shared_->mutex);
    const std::string lower = path.empty() ? std::string() : LowerPath(path);
    for (auto& e : shared_->entries)
      if ((e.st == Entry::St::Failed || (e.st == Entry::St::Queued && e.retryAt)) &&
          (lower.empty() || e.lower == lower)) {
        e.st = Entry::St::Queued;
        e.error.clear();
        e.tries = 0;
        e.retryAt = 0;
        e.thisRun = true;
        if (shared_->config.cyberMode && e.uploadId.empty()) e.endpoint.clear(); // a la nube de ahora
      }
    shared_->offline = false;
    ++shared_->wake;
    shared_->Changed();
  }
  shared_->cv.notify_all();
  Kick();
}

void CloudUploader::Renamed(const std::string& from, const std::string& to) {
  {
    std::lock_guard lock(shared_->mutex);
    const std::string lower = LowerPath(from);
    for (auto& e : shared_->entries)
      if (e.lower == lower) {
        e.path = to;
        e.lower = LowerPath(to);
        e.name = Narrow(std::filesystem::path(Widen(to)).filename().wstring());
      }
    shared_->Changed();
  }
  SaveQueue(*shared_, true);
}

void CloudUploader::Removed(const std::vector<std::string>& paths) {
  if (paths.empty()) return;
  std::vector<PendingAbort> aborts;
  CloudKeys keys;
  {
    std::lock_guard lock(shared_->mutex);
    Shared& sh = *shared_;
    keys = sh.keys;
    for (const auto& path : paths) {
      const std::string lower = LowerPath(path);
      for (auto it = sh.entries.begin(); it != sh.entries.end();) {
        if (it->lower != lower) {
          ++it;
        } else if (it->st == Entry::St::Done) {
          it->local = false; // ya está en la nube: el enlace sigue sirviendo
          it->deletePending = false;
          ++it;
        } else {
          if (!it->uploadId.empty()) aborts.push_back(AbortFor(sh, *it));
          it = sh.entries.erase(it);
        }
      }
    }
    sh.Changed();
  }
  SaveQueue(*shared_, true);
  shared_->cv.notify_all();
  AbortUploads(std::move(aborts), std::move(keys));
}

void CloudUploader::SetPaused(bool paused) {
  {
    std::lock_guard lock(shared_->mutex);
    if (shared_->paused == paused) return;
    shared_->paused = paused;
    shared_->Changed();
  }
  SaveQueue(*shared_, true); // sigue en pausa tras reiniciar
  shared_->cv.notify_all();
  Kick();
}

void CloudUploader::SetGameActive(bool active) {
  {
    std::lock_guard lock(shared_->mutex);
    if (shared_->gameActive == active) return;
    shared_->gameActive = active;
    shared_->Changed();
  }
  Kick();
}

CloudFileState CloudUploader::State(const std::string& path) const {
  CloudFileState s;
  std::lock_guard lock(shared_->mutex);
  const Entry* e = shared_->FindPath(path);
  if (!e) return s;
  switch (e->st) {
  case Entry::St::Done:
    s.phase = CloudFileState::Phase::Done;
    s.progress = 1;
    break;
  case Entry::St::Failed:
    s.phase = CloudFileState::Phase::Failed;
    s.error = e->error;
    break;
  case Entry::St::Queued:
    s.phase = e->id == shared_->currentId ? CloudFileState::Phase::Uploading : CloudFileState::Phase::Queued;
    s.progress = e->progress;
    break;
  }
  return s;
}

std::string CloudUploader::Link(const std::string& path) const {
  std::lock_guard lock(shared_->mutex);
  const Entry* e = shared_->FindPath(path);
  return e && e->st == Entry::St::Done ? LinkFor(*shared_, *e) : std::string();
}

CloudStatus CloudUploader::Status() const {
  CloudStatus s;
  std::lock_guard lock(shared_->mutex);
  const Shared& sh = *shared_;
  s.configured = sh.HaveTarget();
  s.hasKeys = !sh.keys.Empty();
  s.keysStored = sh.keysStored;
  s.keyIdHint = KeyHint(sh.keys.keyId);
  s.paused = sh.paused;
  s.offline = sh.offline;
  s.lastError = sh.lastError;
  s.bytesPerSecond = sh.rate;
  for (auto it = sh.entries.rbegin(); it != sh.entries.rend(); ++it) {
    const Entry& e = *it;
    if (e.st == Entry::St::Failed) {
      ++s.failed;
    } else if (e.st == Entry::St::Queued) {
      if (e.id == sh.currentId) {
        s.current = e.name;
        s.progress = e.progress;
      } else {
        ++s.queued;
        if (sh.Waits(e)) s.waitingGame = true;
      }
    } else {
      ++s.doneCount;
      if (s.recent.size() < 20) s.recent.push_back({e.path, e.name, e.when, e.local});
    }
  }
  s.test = sh.test;
  s.testMessage = sh.testMessage;
  s.version = sh.version;
  return s;
}

std::unordered_map<std::string, CloudFileState> CloudUploader::Snapshot() const {
  std::unordered_map<std::string, CloudFileState> out;
  std::lock_guard lock(shared_->mutex);
  for (const auto& e : shared_->entries) {
    if (!e.local && e.st == Entry::St::Done) continue;
    CloudFileState s;
    switch (e.st) {
    case Entry::St::Done:
      s.phase = CloudFileState::Phase::Done;
      s.progress = 1;
      break;
    case Entry::St::Failed:
      s.phase = CloudFileState::Phase::Failed;
      s.error = e.error;
      break;
    case Entry::St::Queued:
      s.phase = e.id == shared_->currentId ? CloudFileState::Phase::Uploading : CloudFileState::Phase::Queued;
      s.progress = e.progress;
      break;
    }
    out[e.path] = std::move(s); // la última de un mismo archivo gana
  }
  return out;
}

uint64_t CloudUploader::Version() const {
  std::lock_guard lock(shared_->mutex);
  return shared_->version;
}

std::vector<std::string> CloudUploader::PendingPaths() const {
  std::vector<std::string> out;
  std::lock_guard lock(shared_->mutex);
  for (const auto& e : shared_->entries)
    if (e.st != Entry::St::Done || e.deletePending) out.push_back(e.path);
  return out;
}

bool CloudUploader::Busy() const {
  std::lock_guard lock(shared_->mutex);
  for (const auto& e : shared_->entries)
    if (e.st == Entry::St::Queued) return true;
  return false;
}

std::vector<CloudEvent> CloudUploader::TakeEvents() {
  std::lock_guard lock(shared_->mutex);
  return std::exchange(shared_->events, {});
}

void CloudUploader::Test(const CloudConfig& config, const CloudKeys& keys) {
  CloudKeys k{Trim(keys.keyId), Trim(keys.secret)};
  unsigned gen;
  {
    std::lock_guard lock(shared_->mutex);
    if (k.Empty()) k = shared_->keys;
    gen = ++shared_->testGen;
    shared_->test = CloudStatus::Test::Running;
    shared_->testMessage.clear();
    shared_->Changed();
  }
  std::thread([sh = shared_, gen, config, k] { RunTest(sh, gen, config, k); }).detach();
}

void CloudUploader::ClearHistory() {
  {
    std::lock_guard lock(shared_->mutex);
    auto& entries = shared_->entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const Entry& e) { return e.st == Entry::St::Done && !e.deletePending; }),
                  entries.end());
    shared_->Changed();
  }
  SaveQueue(*shared_, true);
}

void CloudUploader::Kick() {
  std::lock_guard lock(shared_->mutex);
  Shared& sh = *shared_;
  if (sh.running || !sh.HasWork()) return;
  sh.running = true;
  std::thread([sh = shared_] { Worker(sh); }).detach();
}

// ------------------------------------------------------------------ utilidades

std::string CloudBucket(const CloudConfig& config) {
  std::string bucket = Trim(config.bucket);
  if (!bucket.empty()) return bucket;
  Endpoint ep;
  return ParseEndpoint(config.endpoint, &ep, nullptr) ? ep.pathBucket : std::string();
}

std::string ExportCloudSettings(const CloudConfig& config, const CloudKeys& keys) {
  nlohmann::json j = {{"v", 1},
                      {"provider", config.provider},
                      {"endpoint", config.endpoint},
                      {"region", config.region},
                      {"bucket", config.bucket},
                      {"publicUrl", config.publicUrl},
                      {"linkDays", config.linkDays},
                      {"id", keys.keyId},
                      {"secret", keys.secret}};
  std::string plain = j.dump();
  std::string out = kExportPrefix + Base64(plain);
  Wipe(plain);
  return out;
}

bool ImportCloudSettings(const std::string& text, CloudConfig* config, CloudKeys* keys) {
  size_t at = text.find(kExportPrefix);
  if (at == std::string::npos) return false;
  size_t start = at + strlen(kExportPrefix), end = start;
  while (end < text.size() && !isspace((unsigned char)text[end])) ++end;
  std::string plain;
  if (!FromBase64(text.substr(start, end - start), &plain)) return false;
  bool ok = false;
  try {
    auto j = nlohmann::json::parse(plain);
    config->provider = j.value("provider", config->provider);
    config->endpoint = j.value("endpoint", "");
    config->region = j.value("region", "");
    config->bucket = j.value("bucket", "");
    config->publicUrl = j.value("publicUrl", "");
    config->linkDays = std::clamp(j.value("linkDays", 7), 1, 7);
    keys->keyId = j.value("id", "");
    keys->secret = j.value("secret", "");
    ok = !config->endpoint.empty() && !keys->Empty();
  } catch (const std::exception&) {
  }
  Wipe(plain);
  return ok;
}

} // namespace spectre
