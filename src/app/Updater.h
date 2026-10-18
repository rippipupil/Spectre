#pragma once

#include <windows.h>

#include <memory>
#include <string>

namespace spectre {

// Estado de "Buscar actualizaciones" que dibuja la página General.
struct UpdateStatus {
  enum class State { Idle, Checking, UpToDate, Available, Downloading, Launching, Error };
  State state = State::Idle;
  std::string latest;     // "0.1.20" (sin la v)
  std::string message;    // error en lenguaje claro
  double downloaded = 0;  // 0..1 durante la descarga
  uint64_t sizeBytes = 0; // tamaño del instalador
  bool portable = false;  // copia portable: no hay instalador que actualizar
  bool unverified = false; // GitHub no publicó la huella del instalador: solo descarga manual
};

// Comprueba la última release de GitHub y descarga Spectre-Setup.exe.
// Solo toca la red cuando el usuario lo pide: no hay sondeos en segundo
// plano ni hilos vivos fuera de una comprobación o una descarga.
class Updater {
public:
  Updater();
  // Los hilos de trabajo guardan su propia referencia al estado: si Spectre
  // se cierra a mitad de una descarga, se cancela y no tocan nada liberado.
  ~Updater();

  UpdateStatus Status() const;
  void Check();
  // Descarga el instalador y, al terminar, avisa a hwnd con
  // WM_SPECTRE_UPDATE_READY (lParam = new std::wstring* con la ruta).
  void Download(HWND hwnd);
  void Cancel();

  // Lanza el instalador en modo silencioso; vuelve a abrir Spectre al acabar.
  static bool LaunchInstaller(const std::wstring& path);
  // Página de la release (copia portable o instalador sin huella).
  static std::string ReleasePage();

private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
};

// "0.1.9" < "0.1.10"; acepta una "v" delante. Partes no numéricas cuentan como 0.
int CompareVersions(const std::string& a, const std::string& b);

} // namespace spectre
