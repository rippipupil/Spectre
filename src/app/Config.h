#pragma once

#include <string>

namespace spectre {

struct HotkeyConfig {
  std::string overlay = "Alt+Z";
  std::string saveClip = "Alt+F10";
  std::string toggleRecording = "Alt+F9";
  std::string toggleReplay = "Alt+Shift+F10";
  std::string screenshot = "Alt+F1"; // captura de pantalla (PNG o JPG)
  std::string marker = "Alt+F8";     // marca un momento de la partida en el clip o la grabación
  std::string shortClip = "Alt+F11"; // clip corto: solo los últimos Config::shortClipSeconds
};

// Comandos de voz (Ajustes > Voz). Usa el reconocimiento de voz de Windows sin
// conexión, con una lista cerrada de frases: no se envía nada a internet y
// solo se escucha mientras están activados. Cada orden admite varias frases
// separadas por comas; se dicen después de la palabra de activación.
struct VoiceConfig {
  bool enabled = false;
  std::string wakeWord = "Spectre";      // vacío = las frases solas
  std::string recognizer;                // id del reconocedor de Windows; vacío = el de español si lo hay
  std::string sensitivity = "normal";    // "estricta" | "normal" | "permisiva"
  bool onlyInGames = false;              // escucha solo con un juego a pantalla completa (o grabando)
  bool sound = true;                     // pitido corto al reconocer una orden
  std::string saveClip = "clip, guarda el clip";
  std::string shortClip = "clip corto";
  std::string startRecording = "graba, empieza a grabar";
  std::string stopRecording = "para, para de grabar";
  std::string screenshot = "captura, haz una captura";
  std::string marker = "marca, pon una marca";
};

// Duraciones del clip que ofrecen el panel, Inicio y Ajustes (segundos). Las
// largas solo caben si la RAM da para ellas con la calidad elegida: la
// interfaz lo avisa (PipelineStats::replayFitSeconds).
inline constexpr int kClipLengths[] = {10, 15, 20, 30, 45, 60, 120, 180, 300, 600, 900, 1200};
inline constexpr int kMaxClipSeconds = 1200;

struct Config {
  // Clips (replay buffer)
  bool replayEnabled = true;
  int clipSeconds = 30;       // duración del clip = tamaño del buffer
  bool onlyInGames = false;   // activa el buffer solo con una app a pantalla completa
  bool folderPerGame = false; // guarda cada juego en su propia subcarpeta
  int shortClipSeconds = 15;  // atajo de clip corto (como mucho, clipSeconds)
  int recordLimitMinutes = 0; // la grabación se detiene sola tras estos minutos; 0 = sin límite

  // Vídeo
  std::string captureMode = "monitor"; // "monitor" | "game" (ventana en primer plano)
  int monitorIndex = 0;
  std::string captureApi = "auto";     // "auto" | "wgc" | "dda"
  bool captureCursor = true;
  int fps = 60;
  int outputHeight = 1080;             // 0 = resolución nativa
  std::string quality = "alta";        // baja | media | alta | maxima | fuente | personalizada
  int bitrateKbps = 20000;             // solo con quality = personalizada
  std::string codec = "h264";          // "h264" | "hevc" | "av1"
  std::string encoder = "auto";        // "auto" o nombre de FFmpeg (h264_nvenc...)
  bool gpuPriority = true;

  // Audio
  bool captureSystemAudio = true;
  std::string outputDeviceId;          // vacío = dispositivo por defecto
  bool gameAudioOnly = false;          // en modo juego: solo el audio de ese proceso
  bool captureMic = false;
  std::string micDeviceId;
  float systemVolume = 1.0f;
  float micVolume = 1.0f;
  bool separateTracks = true;          // pista de mezcla + una por fuente

  // Cámara
  bool cameraEnabled = false;
  std::string cameraId;
  std::string cameraCorner = "bottom-right"; // top-left | top-right | bottom-left | bottom-right
  int cameraSizePercent = 25;          // ancho de la cámara respecto al vídeo
  bool cameraEditable = true;          // guardar aparte lo que hay debajo para moverla o quitarla en el editor

  // Capturas de pantalla
  std::string screenshotFormat = "png"; // "png" (sin pérdida) | "jpg" (ocupa menos)

  // Almacenamiento
  std::string outputDir;               // vacío = Vídeos\Spectre
  int storageLimitGB = 0;              // 0 = sin límite; si se pasa, se borran los más antiguos (nunca los favoritos)
  int deleteAfterDays = 0;             // 0 = nunca; borra lo de Spectre con más días (nunca los favoritos)

  // Avisos
  bool showNotifications = true;
  bool notifyGame = true;              // "Replay listo en <juego>" al detectar un juego nuevo
  bool notifyUpdates = true;           // mira si hay versión nueva al abrir Spectre (y cada 12 h) y avisa
  bool soundOnSave = false;            // pitido corto al guardar un clip o una captura
  std::string saveSound = "clasico";   // clasico | suave | moneda | arcade | campana (app/Sound.h)
  bool copyClipOnSave = false;         // el clip recién guardado va al portapapeles (pegar en Discord)
  std::string toastPosition = "top-right"; // top-right | top-left | bottom-right | bottom-left

  // General
  bool autostart = false;
  bool closeToTray = true;             // cerrar la ventana deja Spectre en la bandeja
  bool trayHintShown = false;          // ya se avisó de que cerrar la ventana no cierra Spectre
  int maxBufferMB = 1024;

  // Apariencia (ver ui/Theme.h)
  std::string theme = "negro";         // id de ui/Theme.h: "negro", "psiquico" o uno de RoCer0
  std::string themeStyle;              // diseño (ui/Theme.h Style::id); vacío = el que trae el tema
  bool themeMotion = true;             // animaciones de la interfaz (ratón encima, pulsar, avisos)
  // Fondo ilustrado del tema (ui/Backdrop.h): "animado", "quieto" o "no".
  std::string themeBackdrop = "animado";

  // Panel Alt+Z (ver ui/Menu.h). Se arrastra y se vuelve a abrir donde se
  // dejó: la posición se guarda como fracción del hueco libre del monitor
  // (0 = izquierda o arriba, 1 = derecha o abajo), así sirve en cualquier
  // monitor, resolución y escala.
  std::string panelLayout = "lateral"; // "lateral" (todo el alto, como NVIDIA) | "compacto" (el de antes)
  float panelLateralX = 0.0f;          // lateral: pegado a la izquierda
  float panelCompactX = 1.0f;          // compacto: a la derecha y centrado en vertical
  float panelCompactY = 0.5f;

  HotkeyConfig hotkeys;
  VoiceConfig voice;
};

Config LoadConfig();
bool SaveConfig(const Config& config);
// Aplica sobre `current` solo lo que se cambió en `edited` respecto a `base`
// (la configuración que había al empezar a editar los ajustes): lo que cambió
// mientras tanto por otro lado (un atajo, el panel Alt+Z) no se pisa.
Config MergeEdits(const Config& current, const Config& base, const Config& edited);
bool SameConfig(const Config& a, const Config& b);
bool SameVoiceConfig(const VoiceConfig& a, const VoiceConfig& b);
std::string ResolveOutputDir(const Config& config);

} // namespace spectre
