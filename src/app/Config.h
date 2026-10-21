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
};

struct Config {
  // Clips (replay buffer)
  bool replayEnabled = true;
  int clipSeconds = 30;       // duración del clip = tamaño del buffer
  bool onlyInGames = false;   // activa el buffer solo con una app a pantalla completa
  bool folderPerGame = false; // guarda cada juego en su propia subcarpeta

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

  // Capturas de pantalla
  std::string screenshotFormat = "png"; // "png" (sin pérdida) | "jpg" (ocupa menos)

  // Almacenamiento
  std::string outputDir;               // vacío = Vídeos\Spectre
  int storageLimitGB = 0;              // 0 = sin límite; si se pasa, se borran los más antiguos (nunca los favoritos)

  // Avisos
  bool showNotifications = true;
  bool soundOnSave = false;            // pitido corto al guardar un clip o una captura
  std::string toastPosition = "top-right"; // top-right | top-left | bottom-right | bottom-left

  // General
  bool autostart = false;
  bool closeToTray = true;             // cerrar la ventana deja Spectre en la bandeja
  bool trayHintShown = false;          // ya se avisó de que cerrar la ventana no cierra Spectre
  int maxBufferMB = 1024;

  // Apariencia (ver ui/Theme.h)
  std::string theme = "negro";         // "negro" | "psiquico"
  bool themeMotion = true;             // animaciones propias del tema (solo las tiene Psíquico)

  HotkeyConfig hotkeys;
};

Config LoadConfig();
bool SaveConfig(const Config& config);
// Aplica sobre `current` solo lo que se cambió en `edited` respecto a `base`
// (la configuración que había al empezar a editar los ajustes): lo que cambió
// mientras tanto por otro lado (un atajo, el panel Alt+Z) no se pisa.
Config MergeEdits(const Config& current, const Config& base, const Config& edited);
bool SameConfig(const Config& a, const Config& b);
std::string ResolveOutputDir(const Config& config);

} // namespace spectre
