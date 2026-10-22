#pragma once

// Piezas comunes de «Exportar para Discord» y del editor: abrir el decoder de
// una pista y los encoders H.264 y AAC, con el mismo orden de encoders (los de
// la GPU primero) y las mismas opciones de calidad. No es para el replay ni la
// grabación (encode/VideoEncoder), que trabajan en directo con la GPU.

#include "encode/Media.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace spectre {

// Por GPU primero (NVIDIA, AMD, Intel y Media Foundation con el encoder de la
// tarjeta); por CPU solo si ninguno abre.
extern const char* const kH264Encoders[6];

// Tamaño, fps y bitrate de la copia que se codifica.
struct VideoTarget {
  int width = 0, height = 0;
  AVRational rate{60, 1};
  int64_t bps = 0;
  // Hay un juego abierto o se graba: el replay usa el mismo encoder de la
  // GPU, así que se abre en sus modos rápidos (como el directo) y no en los
  // de más calidad.
  bool background = false;
};

// Con un juego abierto o grabando (live, que App actualiza mientras dura el
// trabajo), la copia va como mucho a tiempo real: así no le pide al encoder de
// la GPU más que una segunda grabación y el replay no se queda sin hueco. La
// primera vez que live se activa, el hilo pasa también a EcoQoS y al modo de
// fondo, por si el juego se abrió después de empezar. Se usa en el hilo del
// trabajo; sin live (nullptr) no frena nunca.
class LivePacer {
public:
  explicit LivePacer(std::shared_ptr<const std::atomic<bool>> live);
  ~LivePacer();
  LivePacer(const LivePacer&) = delete;
  LivePacer& operator=(const LivePacer&) = delete;
  // seconds: segundos del resultado ya entregados al encoder. Si van por
  // delante del reloj, espera (y deja de esperar si se cancela).
  void Wait(double seconds, const std::atomic<bool>& cancel);

private:
  struct Calm;
  std::shared_ptr<const std::atomic<bool>> live_;
  std::unique_ptr<Calm> calm_;
  bool pacing_ = false;
  double from_ = 0; // segundos del resultado al empezar a ir a tiempo real
  std::chrono::steady_clock::time_point start_;
};

// h264_mf, como en VideoEncoder::TryOpen: si no encuentra el MFT, la limpieza
// de FFmpeg 8.1 hace un MFShutdown() y un CoUninitialize() de más, que se
// llevarían el MFStartup de main (webcam y listas de audio) y el COM de este
// hilo. Antes de cada intento se toma uno de cada. Si el intento falla, o los
// gastó FFmpeg o se quedan sin soltar, que no molesta; si abre, se sueltan
// después de cerrar el encoder (por eso se declara antes que él).
struct MfHold {
  bool held = false;
  static bool Take();
  ~MfHold();
};

bool SupportsPixFmt(const AVCodec* codec, AVPixelFormat fmt);

// Decoder por software de la pista (threads: 1 con un juego abierto).
bool OpenStreamDecoder(const AVStream* st, int threads, AVCodecContext** out);

// id: nombre de FFmpeg, con ":hw" para pedir el encoder de la GPU a Media
// Foundation. simple: sin las opciones de calidad, por si el driver no las
// admite. Los colores se copian de dec. tag va delante de los mensajes del log.
bool OpenH264Encoder(const std::string& id, const VideoTarget& target, const AVCodecContext* dec, bool globalHeader,
                     bool simple, AVCodecContext** out, const char* tag);

// El primero que abra de order (vacío: kH264Encoders), con opciones y sin
// ellas. *used = el que abrió; mf se queda con lo que tomó Media Foundation.
bool OpenFirstH264Encoder(const std::vector<std::string>& order, const VideoTarget& target, const AVCodecContext* dec,
                          bool globalHeader, AVCodecContext** out, std::string* used, MfHold* mf, const char* tag);

// AAC estéreo (o mono si lo es el original) a 48 kHz, o 44,1 si lo era.
bool OpenAacEncoder(int64_t bps, const AVCodecContext* dec, bool globalHeader, AVCodecContext** out, const char* tag);
// Igual, sin decoder de referencia: estéreo a 48 kHz.
bool OpenAacEncoder(int64_t bps, int channels, int sampleRate, bool globalHeader, AVCodecContext** out,
                    const char* tag);

} // namespace spectre
