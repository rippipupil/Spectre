#pragma once

#include <windows.h>

#include <filesystem>
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
  std::string manualUrl;   // descarga directa para portable o sin huella (zip o instalador)
};

// Comprueba la última release de GitHub y descarga Spectre-Setup.exe.
// Toca la red cuando el usuario lo pide y, con Ajustes > Avisos > Versiones
// nuevas, una vez al abrir Spectre y cada 12 h (App::CheckUpdatesQuietly).
// No hay hilos vivos fuera de una comprobación o una descarga, y nada se
// descarga sin pulsar Actualizar.
class Updater {
public:
  Updater();
  // Los hilos de trabajo guardan su propia referencia al estado: si Spectre
  // se cierra a mitad de una descarga, se cancela y no tocan nada liberado.
  ~Updater();

  UpdateStatus Status() const;
  // background: la comprobación automática; si falla, el estado vuelve a
  // Idle en vez de dejar un error en Ajustes.
  void Check(bool background = false);
  // Descarga el instalador y, al terminar, avisa a hwnd con
  // WM_SPECTRE_UPDATE_READY (lParam = new std::wstring* con la ruta). Si ya
  // está en la carpeta temporal y su SHA-256 coincide, no se vuelve a bajar;
  // una descarga cortada sigue donde se quedó.
  void Download(HWND hwnd);
  void Cancel();

  // Lanza el instalador sin preguntas. Con relaunch vuelve a abrir Spectre al
  // acabar; con progress enseña su ventana de progreso (no encima de un juego).
  // El instalador espera a que este proceso termine antes de tocar nada.
  static bool LaunchInstaller(const std::wstring& path, bool relaunch, bool progress);
  // El registro del último instalador lanzado desde Spectre.
  static std::filesystem::path InstallLogPath();
  // Tras una actualización que no se pudo instalar (--update-failed): copia el
  // final del registro del instalador a spectre.log y devuelve el motivo en
  // lenguaje claro.
  static std::string FailedInstallReason();
  // Borra los instaladores descargados (tras actualizar ya no sirven).
  static void RemoveDownloadedInstallers();

private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
};

// "0.1.9" < "0.1.10"; acepta una "v" delante. Partes no numéricas cuentan como 0.
int CompareVersions(const std::string& a, const std::string& b);

} // namespace spectre
