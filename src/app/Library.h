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
#include <utility>
#include <vector>

namespace spectre {

// Export: la copia de menos de 20 MB para Discord (<nombre>_discord.mp4).
// Edit: clips unidos, en cámara lenta, divididos o del editor (<nombre>_union_<fecha>,
// <nombre>_lenta<n>_<fecha>, <nombre>_parte1_<fecha>, <nombre>_editado_<fecha>).
enum class MediaKind { Clip, Recording, Trim, Export, Edit, Screenshot };

struct MediaItem {
  std::string path; // UTF-8, ruta completa
  std::string name; // nombre del archivo con extensión
  std::string stem; // sin extensión (lo que se puede renombrar)
  std::string game; // del nombre del archivo; vacío si no lo dice
  MediaKind kind = MediaKind::Clip;
  bool spectre = false; // el nombre sigue el formato de Spectre (solo esos se borran solos)
  // De NVIDIA ShadowPlay («Juego 2026.10.16 - 05.30.12.02.DVR.mp4»): en su
  // carpeta (se ve sin copiarlo) o ya importado a la de Spectre. Nunca se
  // borra solo.
  bool nvidia = false;
  bool audioOnly = false; // .m4a («Solo audio» del editor): se reproduce sin imagen
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
  uint64_t usedBytes = 0;     // lo que lleva el nombre de Spectre: lo que cuenta el límite de espacio
  uint64_t favoriteBytes = 0; // la parte de usedBytes que son favoritos (el límite nunca los borra)
  uint64_t otherBytes = 0;    // renombrados o de otros programas: ni cuentan ni se borran
  uint64_t freeBytes = 0;     // libre en el disco de la carpeta
  uint64_t totalBytes = 0;
  size_t videos = 0, screenshots = 0, others = 0;
  bool valid = false;
  // Lo de ShadowPlay que se ve desde sus carpetas (app/ShadowPlay.h): no está
  // en la de Spectre y no cuenta arriba.
  size_t shadowPlayFiles = 0;
  uint64_t shadowPlayBytes = 0;
  std::vector<std::string> shadowPlayFolders; // las carpetas en las que se buscó
  // Fecha (FILETIME UTC) y tamaño de lo que el borrado por antigüedad podría
  // quitar (de Spectre y sin estrella): Ajustes dice cuánto se iría antes de aplicar.
  std::vector<std::pair<int64_t, uint64_t>> deletable;
};

// Una pasada del límite de espacio.
struct LimitResult {
  uint64_t limitBytes = 0;
  int deleted = 0;
  uint64_t freedBytes = 0;
  MediaKind kind = MediaKind::Clip; // de lo borrado, si todo es del mismo tipo
  bool mixed = false;
  // Lo que no se puede borrar ya pasa del tope: no se borró nada.
  bool blocked = false;
  uint64_t usedBytes = 0;     // lo que cuenta, después de borrar
  uint64_t keptBytes = 0;     // favoritos, lo de los últimos minutos y la grabación en curso
  uint64_t favoriteBytes = 0; // la parte de keptBytes que son favoritos
  // Borrado de lo antiguo (Ajustes > Almacenamiento): cuántos de los borrados
  // lo fueron por tener más de maxAgeDays días.
  int maxAgeDays = 0;
  int agedDeleted = 0;
};

class Library {
public:
  // Archivos de la carpeta y de sus subcarpetas de primer nivel (una por
  // juego), del más reciente al más antiguo. max = 0: todos.
  std::vector<MediaItem> Scan(const std::string& folder, size_t max = 0, bool videosOnly = false);

  // Lo de NVIDIA ShadowPlay en sus carpetas (y en una subcarpeta por juego),
  // sin lo que haya en skipFolder (la de Spectre, que ya escanea Scan).
  std::vector<MediaItem> ScanShadowPlay(const std::vector<std::string>& folders, const std::string& skipFolder);

  void SetFavorite(const std::string& path, bool favorite);
  // Varios de una vez (selección de la biblioteca): library.json se escribe una sola vez.
  void SetFavorite(const std::vector<std::string>& paths, bool favorite);
  // Marcas de un clip o una grabación: segundos antes del final. Se cuentan
  // desde ahí porque el clip acaba justo al pulsar el atajo y la grabación al
  // detenerla, mientras que los dos empiezan en el keyframe anterior (hasta
  // 2 s antes). Se resuelven al conocer la duración.
  void SetTailMarkers(const std::string& path, std::vector<double> secondsBeforeEnd);
  // No guarda library.json: lo hace Flush, una vez por tanda de duraciones.
  void SetDuration(const std::string& path, uint64_t size, int64_t modified, double seconds);
  void Flush();
  // Vuelve a aplicar favorito, duración y marcas a un elemento ya escaneado
  // (las marcas de un clip se colocan cuando se sabe su duración).
  void Annotate(MediaItem& item);
  // Renombra el archivo (y conserva sus metadatos). newStem sin extensión.
  bool Rename(const std::string& path, const std::string& newStem, std::string* newPath, std::string* error);
  // El archivo se copió (keepSource) o se movió a `to`: favorito, duración y
  // marcas van con él.
  void CarryMeta(const std::string& from, const std::string& to, bool keepSource);
  // Se borró por otro camino (subido a la nube en el modo cibercafé): fuera de library.json.
  void Forget(const std::vector<std::string>& paths);
  // A la papelera de reciclaje (se puede recuperar). Si no puede ir allí
  // (memoria USB, carpeta de red, archivo más grande que la papelera),
  // Windows pregunta antes de borrarlo para siempre; si se le dice que no,
  // *cancelled = true.
  bool Recycle(HWND owner, const std::string& path, std::string* error, bool* cancelled);
  // Varios en una sola operación del Explorador (selección de la biblioteca):
  // Windows pregunta una vez por los que no caben en la papelera («Sí a
  // todo») y por los que estén abiertos en otro programa. Devuelve los que ya
  // no están; si alguno sigue, *error lo explica y *cancelled = true si fue
  // porque se dijo que no.
  std::vector<std::string> Recycle(HWND owner, const std::vector<std::string>& paths, std::string* error,
                                   bool* cancelled);

  // Límite de espacio: borra del todo los clips, grabaciones y capturas más
  // antiguos con nombre de Spectre hasta caber en limitBytes. Solo cuenta lo
  // que lleva ese nombre (lo renombrado o de otros programas no es suyo).
  // Los favoritos, lo de los últimos minutos y keepPaths (la grabación en
  // curso, lo que espera para subirse a la nube) cuentan, pero nunca se
  // borran; si ellos solos ya pasan del tope, no borra nada (blocked). Antes,
  // si maxAgeDays > 0, borra lo de Spectre con más días que eso, con las
  // mismas excepciones.
  LimitResult EnforceLimit(const std::string& folder, uint64_t limitBytes, const std::vector<std::string>& keepPaths,
                           int maxAgeDays = 0);

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
  bool dirty_ = false; // hay duraciones sin guardar
  std::map<std::string, Meta> meta_; // clave: ruta en minúsculas
};

// Analiza un nombre de archivo de Spectre ("Clip_<juego>_<fecha>...") o de
// NVIDIA ShadowPlay ("<juego> 2026.10.16 - 05.30.12.02.DVR.mp4").
void ParseMediaName(const std::string& fileName, MediaItem* item);
const char* MediaKindName(MediaKind kind); // "Clip", "Grabación"...

// Lo que ocupa la biblioteca, contado como el límite de espacio, y el disco de la carpeta.
StorageInfo SummarizeStorage(const std::vector<MediaItem>& items, const std::string& folder);
// Copia el archivo al portapapeles como archivo (pegar en Discord, WhatsApp, el Explorador).
bool CopyFileToClipboard(HWND owner, const std::string& path);
// Varios archivos de una vez (Discord deja pegar hasta 10).
bool CopyFilesToClipboard(HWND owner, const std::vector<std::string>& paths);
// Texto (un enlace). private: no entra en el historial del portapapeles de
// Windows (Win+V) ni se sincroniza con otros PC (la configuración de la nube).
bool CopyTextToClipboard(HWND owner, const std::string& text, bool privateText = false);
std::string ClipboardText(HWND owner);
// Espacio libre en el disco de una carpeta (0 si no se sabe).
uint64_t FreeDiskBytes(const std::string& folder);
// La unidad del archivo tiene papelera de reciclaje (las memorias USB y las
// carpetas de red no la tienen).
bool HasRecycleBin(const std::string& path);

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
  // wait = false: no espera al trabajo en curso (una miniatura de una carpeta
  // de red puede tardar). El hilo acaba solo después y lo recogen el
  // siguiente Start o el destructor.
  void Stop(bool wait = true);
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
