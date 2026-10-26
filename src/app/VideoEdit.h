#pragma once

// Editor sencillo de la biblioteca: lo que se suele hacer con un clip antes de
// subirlo (recortar, acelerar, poner en vertical, un texto, bajar el micro...)
// sin las pistas, capas ni fotogramas clave de un editor de verdad. Todas las
// herramientas son una opción de EditSettings; al guardar se lee el original
// una vez, se pasa por los filtros de FFmpeg y se codifica con el encoder de
// la GPU, en un hilo propio de prioridad baja. El original no cambia.
//
// Portable (FFmpeg y std): lo de Windows (texto con GDI, COM) va aparte o
// dentro de _WIN32, así se prueba en Linux.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFrame;

namespace spectre {

enum class EditAspect { Original, Wide, Vertical, Square, Portrait }; // 16:9, 9:16, 1:1, 4:5
enum class EditFit { Crop, Blur, Bars };       // recortar, fondo difuminado o barras negras
enum class EditLook { None, Vivid, Mono, Sepia, Warm, Cold, Faded };
enum class EditDirection { Forward, Reverse, Boomerang };
enum class EditFormat { Video, Gif, Audio };    // MP4, GIF o solo el sonido (M4A)
enum class EditSize { Same, Light, Discord };  // Discord: después se exporta a menos de 20 MB
enum class EditPlace { Top, Middle, Bottom };
enum class EditCorner { TopLeft, TopRight, BottomLeft, BottomRight };
enum class EditCamShape { Original, Square, Circle }; // como se grabó, cuadrada o redonda

// Lo que se puede cambiar. Los tiempos son segundos del original.
struct EditSettings {
  // Tiempo
  double start = 0, end = 0; // recorte; end <= start = hasta el final
  bool cut = false;          // quitar un trozo del medio
  double cutStart = 0, cutEnd = 0;
  double speed = 1.0;        // de todo el vídeo: 0,25 a 3
  bool slow = false;         // cámara lenta solo en un tramo
  double slowStart = 0, slowEnd = 0;
  double slowSpeed = 0.5;
  bool freeze = false;       // congelar la imagen en un momento
  double freezeAt = 0;
  double freezeSeconds = 2;
  int repeat = 1;            // veces seguidas (1 a 3)
  EditDirection direction = EditDirection::Forward;

  // Imagen
  EditAspect aspect = EditAspect::Original;
  EditFit fit = EditFit::Crop;
  float zoom = 1.0f;         // 1 a 3
  float panX = 0.5f, panY = 0.5f; // qué parte se ve al recortar o con zoom (0..1)
  int rotate = 0;            // 0, 90, 180 o 270
  bool flipH = false, flipV = false;
  int shortSide = 0;         // resolución: lado corto de la salida (0 = el que salga)
  int fps = 0;               // 0 = los del original

  // Color (-100..100; 0 = sin cambios)
  int brightness = 0, contrast = 0, saturation = 0;
  EditLook look = EditLook::None;
  bool sharpen = false, vignette = false;
  bool fadeIn = false, fadeOut = false; // fundido a negro

  // Texto y zonas
  std::string text;          // título o comentario (UTF-8)
  EditPlace textPlace = EditPlace::Bottom;
  int textSize = 1;          // 0 pequeño, 1 mediano, 2 grande
  bool textBox = false;      // con fondo oscuro detrás (si no, con borde)
  double textSeconds = 0;    // 0 = todo el vídeo; si no, los primeros N segundos
  std::string watermark;     // tu nombre o canal, pequeño en una esquina
  EditCorner watermarkCorner = EditCorner::BottomRight;
  bool blur = false;         // difuminar una zona (nombres, chat)
  bool pixelate = false;     // en cuadraditos en vez de difuminado
  float blurX = 0.4f, blurY = 0.4f, blurW = 0.2f, blurH = 0.2f; // fracción de la imagen final

  // Cámara (solo si el clip la guardó aparte, ver EditSource::Webcam):
  // posición y ancho en fracciones de la imagen final.
  bool camEdit = false;      // cambiarla; si no, se queda como se grabó
  bool camHide = false;      // quitarla
  float camX = 0.72f, camY = 0.68f; // esquina de arriba a la izquierda
  float camW = 0.25f;
  EditCamShape camShape = EditCamShape::Original;
  int camBorder = 0;         // 0 sin borde, 1 fino, 2 grueso
  uint32_t camBorderColor = 0xFFFFFF; // 0xRRGGBB
  bool camMirror = false;

  // Audio
  float volume = 1.0f;       // juego (o todo, si no hay pista del micro aparte): 0 a 2
  float micVolume = 1.0f;    // micrófono, si el clip lo tiene en su propia pista
  bool mute = false;
  bool audioFadeIn = false, audioFadeOut = false;
  bool normalize = false;    // igualar el volumen
  bool denoise = false;      // quitar ruido (al micro, si está aparte)

  // Guardar
  EditFormat format = EditFormat::Video;
  EditSize size = EditSize::Same;

  bool operator==(const EditSettings&) const = default;
};

// Lo que el editor necesita saber del original.
struct EditSource {
  double duration = 0;
  int width = 0, height = 0;
  int fpsNum = 60, fpsDen = 1;
  double fps = 60;
  int64_t videoBps = 0; // 0 = no se sabe
  bool audio = false;
  // Pistas de audio: la primera (la mezcla en los clips de Spectre) y, si el
  // clip se grabó con pistas separadas, las del juego y el micrófono.
  int mixTrack = -1, gameTrack = -1, micTrack = -1;
  bool SeparateMic() const { return gameTrack >= 0 && micTrack >= 0; }
  // Cámara guardada aparte (Ajustes > Cámara > «Poder editar la cámara
  // después»): la pista camTrack tiene lo que había del juego debajo de la
  // cámara, que el vídeo lleva pegada en este recuadro (píxeles del original).
  int camTrack = -1;
  int camX = 0, camY = 0, camW = 0, camH = 0;
  bool Webcam() const { return camTrack >= 0 && camW >= 16 && camH >= 16; }
};
bool ProbeEditSource(const std::string& path, EditSource* source, std::string* error);

// Título de la pista con el fondo de la cámara: lo escribe la captura (con el
// recuadro de la cámara en píxeles del vídeo) y lo lee ProbeEditSource.
std::string CamTrackTitle(int x, int y, int w, int h);
bool ParseCamTrackTitle(const std::string& title, int* x, int* y, int* w, int* h);

// Un tramo del resultado: un trozo del original a una velocidad, al revés o
// una imagen congelada (from = to, hold segundos).
struct EditSegment {
  enum class Kind { Forward, Reverse, Freeze };
  Kind kind = Kind::Forward;
  double from = 0, to = 0; // segundos del original
  double speed = 1;
  double hold = 0;         // Freeze: lo que dura
  double Length() const { return kind == Kind::Freeze ? hold : (to - from) / speed; }
};
std::vector<EditSegment> BuildSegments(const EditSettings& settings, const EditSource& source);
double SegmentsLength(const std::vector<EditSegment>& segments);

// Un GIF dura como mucho esto.
constexpr double kMaxGifSeconds = 30;
// «Al revés» y «Bumerán» guardan el sonido del tramo entero y decodifican cada
// trozo varias veces: como mucho este tramo del original (sin contar las
// repeticiones).
constexpr double kMaxReverseSeconds = 60;
double ReversedSeconds(const EditSettings& settings, const EditSource& source);
// Momento del resultado en el que se ve ese segundo del original (el primero,
// si se ve varias veces); -1 si no sale.
double OutputTimeOf(const std::vector<EditSegment>& segments, double sourceSeconds);

// Tamaños de cada paso de la imagen, en píxeles.
struct EditGeometry {
  int rotatedW = 0, rotatedH = 0;   // después de girar
  int cropX = 0, cropY = 0, cropW = 0, cropH = 0; // ventana del zoom o del recorte
  int canvasW = 0, canvasH = 0;     // la imagen con su formato (9:16...) antes de escalar
  int fgW = 0, fgH = 0;             // fondo difuminado o barras: el vídeo dentro del lienzo
  int outW = 0, outH = 0;           // tamaño final
  double fps = 60;
  int fpsNum = 60, fpsDen = 1;
};
EditGeometry ComputeGeometry(const EditSettings& settings, const EditSource& source);

// Recuadro de la cámara en el lienzo (píxeles de canvasW x canvasH); w = 0
// si no se pone encima (sin cambiar, quitada o el clip no la tiene aparte).
struct EditCamBox {
  int x = 0, y = 0, w = 0, h = 0;
};
EditCamBox ComputeCamBox(const EditSettings& settings, const EditSource& source, const EditGeometry& g);
// Para empezar a cambiar la cámara desde donde se ve ahora: su sitio y su
// ancho en la imagen final (o una esquina si el formato ya la ha movido).
void DefaultCamPlacement(EditSettings* settings, const EditSource& source);

// Lee la pista con el fondo de la cámara de un clip, en orden o saltando (la
// vista previa y el guardado). Solo un hilo a la vez.
class CamBackgroundReader {
public:
  CamBackgroundReader();
  ~CamBackgroundReader();
  CamBackgroundReader(const CamBackgroundReader&) = delete;
  CamBackgroundReader& operator=(const CamBackgroundReader&) = delete;
  bool Open(const std::string& path, int track);
  void Close();
  bool IsOpen() const;
  const std::string& Path() const { return path_; }
  // El fotograma de la pista con el mismo tiempo que el del vídeo en el
  // segundo t del original; al principio del clip, antes de que empiece la
  // pista, su primer fotograma. nullptr si en ese fotograma no se veía la
  // cámara (no hay que tocarlo). Vale hasta la siguiente llamada.
  const AVFrame* At(double t);
  // Lo mismo en BGRA para la vista previa (t es el momento del reproductor:
  // vale el fotograma que se está viendo).
  bool ReadBgra(double t, std::vector<uint8_t>* bgra, int* width, int* height);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
};

// Imagen RGBA (de arriba abajo) que se pone encima del vídeo: el texto y la
// marca de agua. La dibuja quien llama (en Windows, con GDI): así el motor no
// depende de fuentes del sistema.
struct EditOverlay {
  std::vector<uint8_t> rgba;
  int width = 0, height = 0;
  int x = 0, y = 0;     // posición en la imagen final
  double until = 0;     // segundos del resultado; 0 = hasta el final
};
// Pide el texto dibujado con letra de pixelHeight px, partido en líneas de
// como mucho maxWidth px; box = con fondo oscuro detrás (si no, con borde).
using TextRasterizer =
    std::function<bool(const std::string& text, int pixelHeight, int maxWidth, bool box, EditOverlay* out)>;
// Texto y marca de agua ya colocados para una imagen de outW x outH.
std::vector<EditOverlay> BuildOverlays(const EditSettings& settings, int outW, int outH, const TextRasterizer& raster);

struct EditJob {
  std::string input, output;
  EditSettings settings;
  bool gentle = true; // hay un juego abierto o se está grabando: un hilo, modo de fondo
  // Lo mismo, pero al momento (App lo actualiza): mientras está activo, el
  // MP4 se escribe como mucho a tiempo real (LivePacer). nullptr: sin tope.
  std::shared_ptr<const std::atomic<bool>> live;
  std::vector<std::string> encoders; // vacío: los de la GPU y luego los de la CPU
  TextRasterizer raster;
};

struct EditResult {
  bool ok = false;
  bool cancelled = false;
  std::string input, output;
  EditSettings settings;
  uint64_t bytes = 0;
  double seconds = 0;   // duración del resultado
  std::string encoder;
  std::string error;    // texto para el aviso
};

// Escribe el resultado (bloquea). progress recibe 0..1; cancel se mira en
// cada fotograma.
EditResult RenderEdit(const EditJob& job, const std::atomic<bool>& cancel, const std::function<void(float)>& progress);

// Vista previa: aplica la imagen y el color a un fotograma del original
// (BGR de 24 bits, de arriba abajo, el que enseña el reproductor) visto en el
// segundo sourceSeconds. Devuelve BGRA del tamaño final, reducido para que
// quepa en maxW x maxH. camBackground: el fotograma de la pista del fondo de
// la cámara en ese momento (BGRA), si se cambia la cámara.
struct EditImage {
  const uint8_t* data = nullptr;
  int width = 0, height = 0;
};
bool RenderEditPreview(const EditSettings& settings, const EditSource& source, const uint8_t* bgr, int width,
                       int height, double sourceSeconds, const TextRasterizer& raster, int maxW, int maxH,
                       std::vector<uint8_t>* bgra, int* outW, int* outH, std::string* error,
                       EditImage camBackground = {});

// Hay algo que cambiar en la imagen (si no, la vista previa es el vídeo tal cual).
bool EditTouchesPicture(const EditSettings& settings);

// Un trabajo cada vez, en su propio hilo (como Exporter).
class EditWorker {
public:
  ~EditWorker() { Stop(); }
  bool Start(EditJob job, std::function<void(int)> onProgress, std::function<void()> onDone);
  void Cancel() { cancel_ = true; }
  void Stop();
  bool Busy() const { return busy_; }
  int Percent() const { return percent_; }
  std::string Input() const;
  bool TakeResult(EditResult* result);

private:
  std::thread thread_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> busy_{false};
  std::atomic<int> percent_{0};
  mutable std::mutex mutex_;
  std::string input_;
  bool haveResult_ = false;
  EditResult result_;
};

} // namespace spectre
