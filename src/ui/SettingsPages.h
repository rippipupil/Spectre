#pragma once

// Páginas de Ajustes de la ventana de Spectre. Se editan sobre una copia de
// la configuración (model.config) que App aplica al pulsar Aplicar.

#include "app/Cloud.h"
#include "app/Config.h"
#include "app/DiscordPresence.h"
#include "app/Library.h"
#include "app/System.h"
#include "app/Updater.h"
#include "app/Voice.h"
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
  Replay, Video, Audio, Camera, Hotkeys, Voice, Storage, Cloud, Notifications, General, Performance, Projects, Count
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
  // Calidad que había antes de elegir Fuente, mientras se muestra su aviso
  // (Cancelar la devuelve).
  std::string qualityBeforeSource;
  DiscordStatus discord; // conexión del estado en Discord (Ajustes > General)
  // Ajustes > Voz: lo que hace el oyente, si espera a un juego («Solo con un
  // juego abierto») y los reconocedores de Windows instalados.
  VoiceStatus voice;
  bool voiceWaiting = false;
  std::vector<VoiceRecognizer> recognizers;
  // Nube: estado de la cola y de «Probar», y las claves que se están
  // escribiendo (no van en config: se guardan cifradas al pulsar Guardar).
  CloudStatus cloud;
  std::string cloudKeyId, cloudSecret;
  bool cloudEditKeys = false; // con claves ya guardadas: «Cambiar» abre los campos
  std::string cloudMessage;   // resultado de Guardar o Pegar
  bool cloudMessageError = false;
  // ShadowPlay: importar copiando o moviendo (esperando confirmación: 0 copiar,
  // 1 mover; -1 nada) y cuántos van mientras se importa (total 0 = nada en marcha).
  int shadowPlayConfirm = -1;
  int shadowPlayDone = 0, shadowPlayTotal = 0;
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
  std::function<void(const std::string&)> playSound; // escuchar un sonido de guardar
  std::function<void()> previewNotice; // un aviso de clip de prueba con el tema actual
  // Nube
  std::function<void()> saveCloudKeys;   // las de model.cloudKeyId/cloudSecret
  std::function<void()> forgetCloudKeys;
  std::function<void()> testCloud;       // con model.config.cloud y las claves escritas (o las guardadas)
  std::function<void()> copyCloudSettings;
  std::function<void(bool)> pasteCloudSettings; // true: en el modo cibercafé
  std::function<void(const std::string&)> copyLink;
  std::function<void(bool)> pauseUploads;
  std::function<void()> retryUploads;
  std::function<void()> clearCloudHistory;
  // ShadowPlay
  std::function<void(bool)> importShadowPlay; // true: mover en vez de copiar
  std::function<void()> cancelImport;
  std::function<void()> chooseShadowPlayFolder;
};

// Dibuja el contenido de la página (sin título ni pie: los pone la ventana).
void DrawSettingsPage(SettingsPage page, SettingsModel& model, const SettingsActions& actions);
// Termina la grabación de un atajo a medias (cambio de página, ventana cerrada).
void CancelHotkeyCapture(SettingsModel& model, const SettingsActions& actions);

} // namespace spectre
