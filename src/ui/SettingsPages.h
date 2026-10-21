#pragma once

// Páginas de Ajustes de la ventana de Spectre. Se editan sobre una copia de
// la configuración (model.config) que App aplica al pulsar Aplicar.

#include "app/Config.h"
#include "app/Library.h"
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

enum class SettingsPage {
  Replay, Video, Audio, Camera, Hotkeys, Storage, Notifications, General, Performance, Projects, Count
};
const char* SettingsPageName(SettingsPage page);

struct SettingsModel {
  Config config;       // copia de trabajo
  Config base;         // la configuración activa cuando empezó la edición (App::MergeEdits)
  bool dirty = false;
  bool devicesLoaded = false;
  PipelineStats stats;
  ProcessUsage usage;
  float systemLevel = 0, micLevel = 0; // pico reciente de cada fuente (0..1)
  std::string hotkeyError;
  std::vector<MonitorInfo> monitors;
  std::vector<AudioDevice> outputs;
  std::vector<AudioDevice> inputs;
  std::vector<CameraDevice> cameras;
  std::vector<EncoderInfo> encoders;
  std::string outputDir;
  UpdateStatus update;
  StorageInfo storage;
  // Grabación de un atajo: índice del atajo que espera la combinación (-1 = ninguno).
  int capturing = -1;
  std::string captureHint;
};

struct SettingsActions {
  std::function<void()> checkUpdates;
  std::function<void()> installUpdate;
  std::function<void()> cancelUpdate;
  std::function<void()> quit;
  std::function<void(const std::string&)> openPath;
  // Mientras se graba un atajo, los atajos globales se sueltan: si no, pulsar
  // Alt+Z abriría el panel en vez de llegar a la ventana.
  std::function<void(bool)> suspendHotkeys;
  std::function<void()> chooseFolder; // diálogo de Windows para elegir la carpeta de destino
  std::function<void()> refreshStorage;
};

// Dibuja el contenido de la página (sin título ni pie: los pone la ventana).
void DrawSettingsPage(SettingsPage page, SettingsModel& model, const SettingsActions& actions);
// Termina la grabación de un atajo a medias (cambio de página, ventana cerrada).
void CancelHotkeyCapture(SettingsModel& model, const SettingsActions& actions);

} // namespace spectre
