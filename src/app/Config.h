#pragma once

#include <string>

namespace spectre {

struct HotkeyConfig {
  std::string overlay = "Alt+Z";
  std::string saveClip = "Alt+F10";
  std::string toggleRecording = "Alt+F9";
  std::string toggleReplay = "Alt+Shift+F10";
};

struct Config {
  // Clips (replay buffer)
  bool replayEnabled = true;
  int clipSeconds = 30;       // duración del clip = tamaño del buffer
  bool onlyInGames = false;   // activa el buffer solo con una app a pantalla completa

  // Vídeo
  std::string captureMode = "monitor"; // "monitor" | "game" (ventana en primer plano)
  int monitorIndex = 0;
  std::string captureApi = "auto";     // "auto" | "wgc" | "dda"
  bool captureCursor = true;
  int fps = 60;
  int outputHeight = 1080;             // 0 = resolución nativa
  std::string quality = "alta";        // baja | media | alta | maxima | personalizada
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

  // General
  std::string outputDir;               // vacío = Vídeos\Spectre
  bool autostart = false;
  bool showNotifications = true;
  int maxBufferMB = 1024;

  // Apariencia (ver ui/Theme.h)
  std::string theme = "negro";         // "negro" | "psiquico"
  bool themeMotion = true;             // animaciones propias del tema (solo las tiene Psíquico)

  HotkeyConfig hotkeys;
};

Config LoadConfig();
bool SaveConfig(const Config& config);
std::string ResolveOutputDir(const Config& config);

} // namespace spectre
