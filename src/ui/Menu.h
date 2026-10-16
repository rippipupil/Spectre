#pragma once

#include "app/Config.h"
#include "app/System.h"
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

// Datos que el menú muestra. Las listas de dispositivos se rellenan al
// abrir el menú (no se consultan en cada frame).
struct MenuModel {
  Config config; // copia de trabajo, se aplica con "Aplicar"
  bool dirty = false;
  PipelineStats stats;
  ProcessUsage usage;
  bool replayActive = false;
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

  // Estado de la interfaz
  int selectedClip = -1;
  double clipDuration = 0;
  float trimStart = 0, trimEnd = 0;
};

struct MenuActions {
  std::function<void()> saveClip;
  std::function<void()> toggleRecording;
  std::function<void()> toggleReplay;
  std::function<void()> close;
  std::function<void()> quit;
  std::function<void(const Config&)> apply;
  std::function<void()> refreshClips;
  std::function<void(const std::string&, double, double)> trim;
  std::function<double(const std::string&)> probeDuration;
};

void DrawMenu(OverlayWindow& window, MenuModel& model, const MenuActions& actions);
void DrawToast(OverlayWindow& window, const std::string& text, bool error, float alpha);

} // namespace spectre
