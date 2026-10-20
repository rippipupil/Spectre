#pragma once

// Biblioteca de clips, grabaciones y capturas de la carpeta de destino.
// El nombre del archivo ya dice el tipo, el juego y la fecha
// (Clip_GeometryDash_2026-10-16_05-30-12.mp4); lo que no dice (favoritos,
// marcas, duración) se guarda en %APPDATA%\Spectre\library.json. Nada de esto
// trabaja en segundo plano: se escanea al abrir la app o el panel y al guardar.

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

enum class MediaKind { Clip, Recording, Trim, Screenshot };

struct MediaItem {
  std::string path; // UTF-8, ruta completa
  std::string name; // nombre del archivo con extensión
  std::string stem; // sin extensión (lo que se puede renombrar)
  std::string game; // del nombre del archivo; vacío si no lo dice
  MediaKind kind = MediaKind::Clip;
  bool spectre = false; // el nombre sigue el formato de Spectre (solo esos se borran solos)
  uint64_t size = 0;
  int64_t modified = 0; // FILETIME en 100 ns, para ordenar
  std::string when;     // "16/10 05:30"
  std::string date;     // "16 oct 2026, 05:30"
  // Metadatos de library.json
  bool favorite = false;
  double duration = 0;          // segundos; 0 = todavía no se sabe
  std::vector<double> markers;  // segundos desde el inicio (ya resueltos)
  bool Video() const { return kind != MediaKind::Screenshot; }
};

struct StorageInfo {
  uint64_t usedBytes = 0;  // lo que ocupa lo que hay en la biblioteca
  uint64_t freeBytes = 0;  // libre en el disco de la carpeta
  uint64_t totalBytes = 0;
  size_t videos = 0, screenshots = 0, favorites = 0;
  bool valid = false;
};

class Library {
public:
  // Archivos de la carpeta y de sus subcarpetas de primer nivel (una por
  // juego), del más reciente al más antiguo. max = 0: todos.
  std::vector<MediaItem> Scan(const std::string& folder, size_t max = 0, bool videosOnly = false);
  StorageInfo Storage(const std::string& folder);

  void SetFavorite(const std::string& path, bool favorite);
  // Marcas de un clip del replay: segundos antes del final (el clip acaba
  // justo al pulsar el atajo). Se resuelven al conocer la duración.
  void SetTailMarkers(const std::string& path, std::vector<double> secondsBeforeEnd);
  // Marcas de una grabación: segundos desde que empezó.
  void SetMarkers(const std::string& path, std::vector<double> seconds);
  void SetDuration(const std::string& path, uint64_t size, int64_t modified, double seconds);
  // Vuelve a aplicar favorito, duración y marcas a un elemento ya escaneado
  // (las marcas de un clip se colocan cuando se sabe su duración).
  void Annotate(MediaItem& item);
  // Renombra el archivo (y conserva sus metadatos). newStem sin extensión.
  bool Rename(const std::string& path, const std::string& newStem, std::string* newPath, std::string* error);
  // A la papelera de reciclaje (se puede recuperar).
  bool Recycle(const std::string& path, std::string* error);

  // Límite de espacio: borra del todo los clips, grabaciones y capturas más
  // antiguos con nombre de Spectre hasta caber en limitBytes. Nunca toca los
  // favoritos ni archivos que no haya creado Spectre. Devuelve cuántos borró.
  int EnforceLimit(const std::string& folder, uint64_t limitBytes, uint64_t* freedBytes);

private:
  struct Meta {
    bool favorite = false;
    double duration = 0;
    uint64_t size = 0;      // tamaño y fecha del archivo cuando se midió la duración
    int64_t modified = 0;
    std::vector<double> markers;
    std::vector<double> tailMarkers;
    bool Empty() const {
      return !favorite && duration <= 0 && markers.empty() && tailMarkers.empty();
    }
  };
  void Load();
  void Save();
  static std::string Key(const std::string& path);
  void Apply(MediaItem& item);

  std::mutex mutex_;
  bool loaded_ = false;
  std::map<std::string, Meta> meta_; // clave: ruta en minúsculas
};

// Analiza un nombre de archivo de Spectre ("Clip_<juego>_<fecha>...").
void ParseMediaName(const std::string& fileName, MediaItem* item);
const char* MediaKindName(MediaKind kind); // "Clip", "Grabación"...

// Copia el archivo al portapapeles como archivo (pegar en Discord, WhatsApp, el Explorador).
bool CopyFileToClipboard(HWND owner, const std::string& path);
// Espacio libre en el disco de una carpeta (0 si no se sabe).
uint64_t FreeDiskBytes(const std::string& folder);

// Trabajo lento de la biblioteca en un hilo propio, solo mientras la app está
// abierta: miniaturas del Explorador (ya cacheadas por Windows), duración de
// cada vídeo e imágenes a tamaño completo para el visor de capturas.
class MediaWorker {
public:
  enum class Job { Thumbnail, Duration, Image };
  struct Result {
    Job job = Job::Thumbnail;
    std::string path;
    bool ok = false;
    int width = 0, height = 0;
    std::vector<uint8_t> bgra; // miniatura o imagen, BGRA de arriba abajo
    double duration = 0;
  };

  ~MediaWorker() { Stop(); }
  // notifyMsg se publica en notifyWindow cuando hay resultados nuevos.
  void Start(HWND notifyWindow, UINT notifyMsg);
  void Stop();
  bool Running() const { return thread_.joinable(); }
  // Repetidos se ignoran; los nuevos van delante (lo que se ve ahora antes).
  void Request(Job job, const std::string& path, int width = 0, int height = 0);
  void CancelAll();
  std::vector<Result> Take();

private:
  struct Request_ {
    Job job;
    std::string path;
    int width, height;
  };
  void Loop();
  Result Run(const Request_& request);

  HWND notify_ = nullptr;
  UINT msg_ = 0;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::deque<Request_> queue_;
  std::vector<Result> results_;
};

} // namespace spectre
