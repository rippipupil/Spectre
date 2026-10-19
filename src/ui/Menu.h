#pragma once

#include "app/Config.h"
#include "app/System.h"
#include "app/Updater.h"
#include "audio/AudioCapture.h"
#include "capture/Camera.h"
#include "capture/CaptureSource.h"
#include "core/Pipeline.h"
#include "encode/VideoEncoder.h"

#include <functional>
#include <string>
#include <vector>

namespace spectre {

class OverlayWindow;

// Datos que muestran el panel rápido (Alt+Z) y la ventana de ajustes. Las
// listas de dispositivos solo se cargan al abrir los ajustes: abrir el panel
// no enumera cámaras ni audio, así aparece al instante.
struct MenuModel {
  Config config; // panel: copia del config activo; ajustes: copia de trabajo
  bool dirty = false;
  bool devicesLoaded = false;
  PipelineStats stats;
  ProcessUsage usage;
  bool replayActive = false;
  bool micLive = false; // el micrófono se está capturando (se puede silenciar al vuelo)
  float systemLevel = 0, micLevel = 0; // pico reciente (0..1) de cada fuente, se refresca en cada frame
  bool gameDetected = false;
  std::string gameName;
  std::string hotkeyError;
  std::vector<MonitorInfo> monitors;
  std::vector<AudioDevice> outputs;
  std::vector<AudioDevice> inputs;
  std::vector<CameraDevice> cameras;
  std::vector<EncoderInfo> encoders;
  std::vector<ClipFile> clips;
  std::string outputDir;
  UpdateStatus update; // se refresca en cada frame de los ajustes

  // Estado de la interfaz
  unsigned long long openedAt = 0; // GetTickCount64 al abrir (animación de entrada)
  float panelWidth = 0, panelHeight = 0; // tamaño real del panel en el último frame
  int page = 0;                          // página activa de los ajustes
  bool busyLastFrame = false;            // había un campo o desplegable activo
  int selectedClip = -1;
  double clipDuration = 0;
  float trimStart = 0, trimEnd = 0;
};

// Todas las acciones se ejecutan después del frame de ImGui (App las aplaza):
// pueden cerrar o redimensionar la ventana sin romper el frame en curso.
struct MenuActions {
  std::function<void()> saveClip;
  std::function<void()> toggleRecording;
  std::function<void()> toggleReplay;
  std::function<void()> toggleMic;
  std::function<void()> toggleCamera;
  std::function<void(int)> setClipSeconds;
  std::function<void()> openSettings;
  std::function<void()> backToPanel;
  std::function<void()> close;
  std::function<void()> quit;
  std::function<void(const Config&)> apply;
  std::function<void()> refreshClips;
  std::function<void(const std::string&)> openPath;
  std::function<void(const std::string&)> showInFolder;
  std::function<void(const std::string&, double, double)> trim;
  std::function<double(const std::string&)> probeDuration;
  // Buscar actualizaciones: solo hay red cuando el usuario pulsa.
  std::function<void()> checkUpdates;
  std::function<void()> installUpdate;
  std::function<void()> cancelUpdate;
};

// Ancho del panel rápido en píxeles lógicos (se multiplica por la escala DPI).
constexpr float kPanelWidth = 320.0f;
// Cuadro de Ajustes, en píxeles lógicos: la ventana de Ajustes tiene este
// tamaño (menos si el monitor es más pequeño) y DrawSettings la llena entera.
constexpr float kSettingsWidth = 920.0f;
constexpr float kSettingsHeight = 660.0f;

void DrawPanel(OverlayWindow& window, MenuModel& model, const MenuActions& actions);
void DrawSettings(OverlayWindow& window, MenuModel& model, const MenuActions& actions);
void DrawToast(OverlayWindow& window, const std::string& text, bool error, float alpha);

} // namespace spectre
