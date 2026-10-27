#pragma once

// Comandos de voz (Ajustes > Voz): decir «Spectre clip» guarda un clip, como
// Alt+F10. Usa el reconocedor de voz de Windows (SAPI) dentro del proceso, sin
// conexión y con una gramática cerrada de frases: no hay modelo que descargar
// ni nada sale del PC, y entre frase y frase apenas gasta CPU. Solo existe
// mientras los comandos están activados; apagados no se abre el micrófono.
//
// Windows.Media.SpeechRecognition no sirve: exige que la app esté empaquetada
// (MSIX) y Spectre se instala como un programa normal.

#include "app/Config.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

// En el orden de Ajustes > Voz.
enum class VoiceCommand { SaveClip, ShortClip, StartRecording, StopRecording, Screenshot, Marker, Count };
constexpr int kVoiceCommandCount = int(VoiceCommand::Count);

const char* VoiceCommandName(VoiceCommand command);
std::string& VoicePhrases(VoiceConfig& config, VoiceCommand command);
const std::string& VoicePhrases(const VoiceConfig& config, VoiceCommand command);
// Las frases de una orden, separadas por comas, sin espacios de sobra.
std::vector<std::string> SplitPhrases(const std::string& text);
// Una frase repetida en dos órdenes (solo cuenta la primera), para avisar en
// Ajustes; vacío si no hay ninguna.
std::string RepeatedPhrase(const VoiceConfig& config);
// Lo que hay que decir: la palabra de activación y la frase.
std::string SpokenPhrase(const VoiceConfig& config, const std::string& phrase);

// Reconocedores de voz instalados en Windows (los de SAPI: español de España,
// inglés, francés, alemán, japonés y chino).
struct VoiceRecognizer {
  std::string id;   // id del token de SAPI (Config::voice.recognizer)
  std::string name; // "Microsoft Speech Recognizer 8.0 for Windows (Spanish - Spain)"
  bool spanish = false;
};
std::vector<VoiceRecognizer> InstalledRecognizers();

struct VoiceStatus {
  enum class State { Off, Starting, Listening, Error };
  State state = State::Off;
  std::string recognizer; // con qué reconocedor escucha
  std::string note;       // escuchando, pero con un pero (no es español, micrófono que no está)
  std::string error;      // por qué no escucha
  // Lo último que oyó: la frase (vacía si no se parecía a ninguna), la
  // confianza del reconocedor (0..1) y si se aceptó como orden.
  std::string heard;
  float confidence = 0;
  bool accepted = false;
  ULONGLONG heardAt = 0; // GetTickCount64; 0 = todavía nada
};

// Orden reconocida, para la ventana de App (WM_SPECTRE_VOICE, lParam = new VoiceEvent*).
struct VoiceEvent {
  VoiceCommand command = VoiceCommand::SaveClip;
  std::string phrase;
  int64_t spokenQpc = 0; // cuándo se empezó a decir (QPC); 0 = no se sabe
};

class VoiceListener {
public:
  ~VoiceListener() { Stop(); }

  // Empieza a escuchar en su hilo (vuelve enseguida). notify recibe
  // WM_SPECTRE_VOICE con cada orden y WM_SPECTRE_VOICE_STATUS cuando cambia
  // lo que enseña Ajustes. micDeviceId: el micrófono de Ajustes > Audio
  // (vacío = el predeterminado de Windows).
  void Start(const VoiceConfig& config, const std::string& micDeviceId, HWND notify);
  // Deja de escuchar y suelta el micrófono. Espera como mucho waitMs: si el
  // driver no lo suelta, el hilo termina solo más tarde.
  void Stop(DWORD waitMs = 2000);
  bool Running() const { return thread_.joinable(); }
  // Con qué se arrancó, para saber si hay que volver a arrancar.
  const VoiceConfig& Settings() const { return config_; }
  const std::string& MicDeviceId() const { return micDeviceId_; }
  VoiceStatus Status() const;

  struct Shared;

private:
  VoiceConfig config_;
  std::string micDeviceId_;
  std::shared_ptr<Shared> shared_;
  std::thread thread_;
};

} // namespace spectre
