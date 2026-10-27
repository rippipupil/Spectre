#pragma once

// Estado en Discord (Rich Presence): en el perfil sale «Jugando a Spectre»,
// con lo que se graba debajo («Clipeando Geometry Dash», «3 clips guardados»).
// Habla con el Discord de escritorio por su tubería local
// (\\.\pipe\discord-ipc-0 a 9), sin SDK ni DLL. Solo hay un hilo con la opción
// activada: espera a que algo cambie y, sin Discord abierto, mira cada 20 s si
// ya está. Al cerrarse la tubería (Spectre sale o se apaga la opción) Discord
// quita el estado solo.

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace spectre {

// Identificador de la aplicación de Discord que da el nombre («Spectre») y el
// icono. Lo pone CMake (SPECTRE_DISCORD_APP_ID); vacío = sin estado en Discord.
const char* DiscordAppId();

struct PresenceState {
  bool active = false;   // false: no se enseña nada de Spectre
  std::string details;   // primera línea: «Clipeando Geometry Dash»
  std::string state;     // segunda: «3 clips guardados»
  int64_t startUnix = 0; // Discord cuenta el tiempo desde aquí (segundos UNIX); 0 = sin tiempo
  bool operator==(const PresenceState&) const = default;
};

struct DiscordStatus {
  enum class State { Off, NoAppId, Waiting, Connected, Rejected };
  State state = State::Off;
  std::string user;  // con quién se conectó (Connected)
  std::string error; // lo que dijo Discord al rechazarlo (Rejected)
  bool operator==(const DiscordStatus&) const = default;
};

class DiscordPresence {
public:
  ~DiscordPresence() { Stop(); }
  // notifyMsg se publica en notifyWindow cuando cambia Status().
  void Start(HWND notifyWindow, UINT notifyMsg);
  void Stop();
  bool Running() const { return thread_.joinable(); }
  // Lo que se quiere enseñar. Barato si no cambia; el hilo lo manda como
  // mucho una vez cada pocos segundos (Discord limita las actualizaciones).
  void Set(const PresenceState& state);
  DiscordStatus Status() const;

private:
  void Loop();
  void SetStatus(DiscordStatus status);

  HWND notify_ = nullptr;
  UINT msg_ = 0;
  std::thread thread_;
  HANDLE wake_ = nullptr; // evento: hay estado nuevo o hay que parar
  mutable std::mutex mutex_;
  bool stop_ = false;
  PresenceState want_;
  DiscordStatus status_;
};

} // namespace spectre
