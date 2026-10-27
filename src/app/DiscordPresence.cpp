#include "app/DiscordPresence.h"

#include "app/CrashHandler.h"
#include "common/Util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

#ifndef SPECTRE_DISCORD_APP_ID
#define SPECTRE_DISCORD_APP_ID ""
#endif

namespace spectre {

const char* DiscordAppId() { return SPECTRE_DISCORD_APP_ID; }

namespace {

// Protocolo IPC de Discord: cada mensaje lleva 4 bytes de operación y 4 de
// longitud (little-endian) y luego el JSON.
enum Op : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4 };

constexpr DWORD kRetryMs = 20000;  // sin Discord: cada cuánto se vuelve a mirar
constexpr DWORD kMinGapMs = 4000;  // entre dos estados (Discord admite 5 cada 20 s)
constexpr DWORD kReplyMs = 5000;   // espera a la respuesta del saludo
constexpr DWORD kWriteMs = 3000;
constexpr uint32_t kMaxFrame = 64 * 1024;

struct Frame {
  uint32_t op = 0;
  std::string json;
};

// Texto de un campo del JSON de Discord ("" si no está o no es texto).
std::string Field(const nlohmann::json& j, const char* key) {
  if (!j.is_object()) return {};
  auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Discord quiere de 2 a 128 caracteres: se corta sin partir una letra UTF-8.
std::string Fit(std::string text) {
  if (text.size() > 128) {
    size_t n = 128;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    text.resize(n);
  }
  return text.size() >= 2 ? text : std::string();
}

// La tubería con Discord en modo asíncrono: se espera a la vez a lo que diga
// Discord y al evento con el que App despierta el hilo.
class Pipe {
public:
  enum class Got { Frame, Woken, Timeout, Error };

  ~Pipe() { Close(); }
  bool Open();
  void Close();
  bool IsOpen() const { return h_ != INVALID_HANDLE_VALUE; }
  bool Write(uint32_t op, const std::string& json);
  // El siguiente mensaje de Discord, que se despertó al hilo (wake) o que pasó el tiempo.
  Got Next(Frame* out, DWORD timeoutMs, HANDLE wake);

private:
  bool TakeFrame(Frame* out, bool* bad);

  HANDLE h_ = INVALID_HANDLE_VALUE;
  HANDLE readEvent_ = nullptr, writeEvent_ = nullptr;
  OVERLAPPED read_{};
  bool reading_ = false;
  char buf_[4096];
  std::string inbox_;
};

bool Pipe::Open() {
  Close();
  // Discord abre la primera libre de discord-ipc-0 a 9 (hay una por cada
  // Discord abierto: estable, PTB, Canary). Con IDENTIFICATION, quien esté al
  // otro lado no puede hacerse pasar por Spectre.
  for (int i = 0; i < 10 && h_ == INVALID_HANDLE_VALUE; ++i) {
    const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
    h_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                     FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  }
  if (h_ == INVALID_HANDLE_VALUE) return false;
  readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!readEvent_ || !writeEvent_) {
    Close();
    return false;
  }
  return true;
}

void Pipe::Close() {
  if (h_ != INVALID_HANDLE_VALUE) {
    if (reading_) {
      // La lectura pendiente escribe en buf_: hay que esperar a que se cancele.
      CancelIoEx(h_, &read_);
      DWORD n = 0;
      GetOverlappedResult(h_, &read_, &n, TRUE);
      reading_ = false;
    }
    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }
  if (readEvent_) CloseHandle(readEvent_);
  if (writeEvent_) CloseHandle(writeEvent_);
  readEvent_ = writeEvent_ = nullptr;
  inbox_.clear();
}

bool Pipe::Write(uint32_t op, const std::string& json) {
  if (!IsOpen() || json.size() > kMaxFrame) return false;
  std::string data(8 + json.size(), '\0');
  const uint32_t len = uint32_t(json.size());
  std::memcpy(data.data(), &op, 4);
  std::memcpy(data.data() + 4, &len, 4);
  std::memcpy(data.data() + 8, json.data(), json.size());
  OVERLAPPED ov{};
  ov.hEvent = writeEvent_;
  ResetEvent(writeEvent_);
  DWORD n = 0;
  if (!WriteFile(h_, data.data(), DWORD(data.size()), nullptr, &ov)) {
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(writeEvent_, kWriteMs) != WAIT_OBJECT_0) {
      CancelIoEx(h_, &ov);
      GetOverlappedResult(h_, &ov, &n, TRUE);
      return false;
    }
  }
  return GetOverlappedResult(h_, &ov, &n, TRUE) && n == DWORD(data.size());
}

bool Pipe::TakeFrame(Frame* out, bool* bad) {
  if (inbox_.size() < 8) return false;
  uint32_t op = 0, len = 0;
  std::memcpy(&op, inbox_.data(), 4);
  std::memcpy(&len, inbox_.data() + 4, 4);
  if (len > kMaxFrame) {
    *bad = true;
    return false;
  }
  if (inbox_.size() < 8 + size_t(len)) return false;
  out->op = op;
  out->json.assign(inbox_, 8, len);
  inbox_.erase(0, 8 + size_t(len));
  return true;
}

Pipe::Got Pipe::Next(Frame* out, DWORD timeoutMs, HANDLE wake) {
  const ULONGLONG until = timeoutMs == INFINITE ? 0 : GetTickCount64() + timeoutMs;
  for (;;) {
    bool bad = false;
    if (TakeFrame(out, &bad)) return Got::Frame;
    if (bad) return Got::Error;
    if (!reading_) {
      std::memset(&read_, 0, sizeof(read_));
      read_.hEvent = readEvent_;
      ResetEvent(readEvent_);
      // Aunque termine al momento, el evento queda señalado y se recoge abajo.
      if (!ReadFile(h_, buf_, DWORD(sizeof(buf_)), nullptr, &read_) && GetLastError() != ERROR_IO_PENDING &&
          GetLastError() != ERROR_MORE_DATA)
        return Got::Error;
      reading_ = true;
    }
    DWORD wait = INFINITE;
    if (timeoutMs != INFINITE) {
      const ULONGLONG now = GetTickCount64();
      wait = now >= until ? 0 : DWORD(until - now);
    }
    HANDLE handles[2] = {readEvent_, wake};
    const DWORD r = WaitForMultipleObjects(wake ? 2 : 1, handles, FALSE, wait);
    if (r == WAIT_OBJECT_0) {
      DWORD n = 0;
      const BOOL ok = GetOverlappedResult(h_, &read_, &n, FALSE);
      reading_ = false;
      // ERROR_MORE_DATA: un mensaje más largo que buf_, el resto en la siguiente lectura.
      if (!ok && GetLastError() != ERROR_MORE_DATA) return Got::Error; // Discord se cerró
      inbox_.append(buf_, n);
      continue;
    }
    if (wake && r == WAIT_OBJECT_0 + 1) return Got::Woken;
    if (r == WAIT_TIMEOUT) return Got::Timeout;
    return Got::Error;
  }
}

std::string ActivityJson(const PresenceState& s, int nonce) {
  nlohmann::json args = {{"pid", GetCurrentProcessId()}};
  if (s.active) {
    nlohmann::json activity = nlohmann::json::object();
    if (std::string d = Fit(s.details); !d.empty()) activity["details"] = d;
    if (std::string t = Fit(s.state); !t.empty()) activity["state"] = t;
    if (s.startUnix > 0) activity["timestamps"] = {{"start", s.startUnix}};
    // El icono subido en la aplicación de Discord con el nombre «spectre».
    activity["assets"] = {{"large_image", "spectre"}, {"large_text", "Spectre"}};
    activity["instance"] = false;
    args["activity"] = activity;
  } else {
    args["activity"] = nullptr; // quita el estado
  }
  nlohmann::json cmd = {{"cmd", "SET_ACTIVITY"}, {"args", args}, {"nonce", std::to_string(nonce)}};
  return cmd.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

void DiscordPresence::Start(HWND notifyWindow, UINT notifyMsg) {
  if (thread_.joinable()) return;
  notify_ = notifyWindow;
  msg_ = notifyMsg;
  if (!*DiscordAppId()) {
    SetStatus({DiscordStatus::State::NoAppId, {}, {}});
    return;
  }
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!wake_) return;
  {
    std::lock_guard lock(mutex_);
    stop_ = false;
  }
  SetStatus({DiscordStatus::State::Waiting, {}, {}});
  thread_ = std::thread([this] { Loop(); });
}

void DiscordPresence::Stop() {
  if (thread_.joinable()) {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    SetEvent(wake_);
    thread_.join();
  }
  if (wake_) {
    CloseHandle(wake_);
    wake_ = nullptr;
  }
  SetStatus({});
}

void DiscordPresence::Set(const PresenceState& state) {
  std::lock_guard lock(mutex_);
  if (want_ == state) return;
  want_ = state;
  if (wake_) SetEvent(wake_);
}

DiscordStatus DiscordPresence::Status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

void DiscordPresence::SetStatus(DiscordStatus status) {
  {
    std::lock_guard lock(mutex_);
    if (status_ == status) return;
    status_ = std::move(status);
  }
  if (notify_) PostMessageW(notify_, msg_, 0, 0);
}

void DiscordPresence::Loop() {
  InstallThreadCrashHandler();
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  using State = DiscordStatus::State;
  auto stopping = [this] {
    std::lock_guard lock(mutex_);
    return stop_;
  };
  Pipe pipe;
  PresenceState sent;
  bool synced = false;      // Discord ya tiene el último estado mandado
  ULONGLONG lastSend = 0;
  ULONGLONG nextTry = 0;
  int nonce = 0;
  bool reportedWaiting = false;
  while (!stopping()) {
    if (!pipe.IsOpen()) {
      const ULONGLONG now = GetTickCount64();
      if (now >= nextTry) {
        nextTry = now + kRetryMs;
        DiscordStatus status{State::Waiting, {}, {}};
        if (pipe.Open()) {
          // Saludo: Discord contesta READY (con el usuario) o cierra con el motivo.
          const nlohmann::json hello = {{"v", 1}, {"client_id", DiscordAppId()}};
          bool ready = false;
          if (pipe.Write(kHandshake, hello.dump())) {
            const ULONGLONG until = GetTickCount64() + kReplyMs;
            while (!ready && !stopping() && pipe.IsOpen()) {
              const ULONGLONG t = GetTickCount64();
              if (t >= until) break;
              Frame f;
              const Pipe::Got got = pipe.Next(&f, DWORD(until - t), wake_);
              if (got == Pipe::Got::Woken) continue; // un estado nuevo (se manda al conectar) o Stop()
              if (got != Pipe::Got::Frame) break;
              const nlohmann::json j = nlohmann::json::parse(f.json, nullptr, false);
              if (f.op == kPing) {
                pipe.Write(kPong, f.json);
              } else if (f.op == kClose || Field(j, "evt") == "ERROR") {
                const nlohmann::json& data = j.is_object() && j.contains("data") ? j["data"] : j;
                status = {State::Rejected, {}, Field(data, "message")};
                break;
              } else if (Field(j, "evt") == "READY") {
                ready = true;
                const nlohmann::json& data = j.is_object() && j.contains("data") ? j["data"] : j;
                const nlohmann::json& user = data.is_object() && data.contains("user") ? data["user"] : data;
                std::string name = Field(user, "global_name");
                if (name.empty()) name = Field(user, "username");
                status = {State::Connected, name, {}};
              }
            }
          }
          if (!ready && stopping()) break; // Stop() durante el saludo
          if (ready) {
            Log("Discord: conectado"); // sin el nombre: el log se comparte
            synced = false;
            reportedWaiting = false;
          } else {
            pipe.Close();
            if (status.state == State::Rejected) {
              // No va a cambiar de opinión enseguida (identificador de la app mal puesto).
              nextTry = GetTickCount64() + 5 * 60 * 1000;
              if (Status().state != State::Rejected)
                Log("Discord: rechazó el estado: %s", status.error.empty() ? "sin motivo" : status.error.c_str());
            }
          }
        }
        if (status.state == State::Waiting && !reportedWaiting) {
          Log("Discord: no está abierto; se vuelve a mirar cada %u s", unsigned(kRetryMs / 1000));
          reportedWaiting = true;
        }
        SetStatus(status);
      }
      if (!pipe.IsOpen()) {
        // El saludo pudo comerse el aviso de Stop() (el evento se reinicia solo).
        if (stopping()) break;
        const ULONGLONG t = GetTickCount64();
        WaitForSingleObject(wake_, nextTry > t ? DWORD(nextTry - t) : 0);
        continue;
      }
    }

    // Conectado: el estado nuevo, como mucho uno cada kMinGapMs.
    PresenceState want;
    {
      std::lock_guard lock(mutex_);
      want = want_;
    }
    DWORD timeout = INFINITE;
    if (!synced || !(want == sent)) {
      const ULONGLONG since = GetTickCount64() - lastSend;
      if (lastSend != 0 && since < kMinGapMs) {
        timeout = DWORD(kMinGapMs - since);
      } else if (pipe.Write(kFrame, ActivityJson(want, ++nonce))) {
        sent = want;
        synced = true;
        lastSend = GetTickCount64();
      } else {
        Log("Discord: se cerró la conexión");
        pipe.Close();
        SetStatus({State::Waiting, {}, {}});
        nextTry = GetTickCount64() + kRetryMs;
        continue;
      }
    }
    Frame f;
    switch (pipe.Next(&f, timeout, wake_)) {
    case Pipe::Got::Frame:
      if (f.op == kPing) {
        pipe.Write(kPong, f.json);
      } else if (f.op == kClose) {
        Log("Discord: cerró la conexión");
        pipe.Close();
        SetStatus({State::Waiting, {}, {}});
        nextTry = GetTickCount64() + kRetryMs;
      } else {
        // Respuesta a SET_ACTIVITY: solo interesa si es un error (se queda en el log).
        const nlohmann::json j = nlohmann::json::parse(f.json, nullptr, false);
        if (Field(j, "evt") == "ERROR") {
          const nlohmann::json& data = j.contains("data") ? j["data"] : j;
          Log("Discord: no aceptó el estado: %s", Field(data, "message").c_str());
        }
      }
      break;
    case Pipe::Got::Woken:
    case Pipe::Got::Timeout:
      break;
    case Pipe::Got::Error:
      Log("Discord: se cerró la conexión");
      pipe.Close();
      SetStatus({State::Waiting, {}, {}});
      nextTry = GetTickCount64() + kRetryMs;
      break;
    }
  }
  // Al cerrar la tubería Discord quita el estado de Spectre.
  pipe.Close();
}

} // namespace spectre
