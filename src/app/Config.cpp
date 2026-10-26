#include "app/Config.h"

#include "common/Util.h"
#include "app/Sound.h"
#include "encode/Quality.h"
#include "ui/Theme.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace spectre {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HotkeyConfig, overlay, saveClip, toggleRecording, toggleReplay,
                                                screenshot, marker, shortClip)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, replayEnabled, clipSeconds, onlyInGames, folderPerGame,
                                                shortClipSeconds, recordLimitMinutes,
                                                captureMode, monitorIndex, captureApi, captureCursor, fps,
                                                outputHeight, quality, bitrateKbps, codec, encoder, gpuPriority,
                                                captureSystemAudio, outputDeviceId, gameAudioOnly, captureMic,
                                                micDeviceId, systemVolume, micVolume, separateTracks, cameraEnabled,
                                                cameraId, cameraCorner, cameraSizePercent, cameraEditable,
                                                screenshotFormat,
                                                outputDir, storageLimitGB, deleteAfterDays, showNotifications,
                                                notifyGame, notifyUpdates, soundOnSave, saveSound, copyClipOnSave,
                                                toastPosition, autostart, closeToTray, trayHintShown, maxBufferMB,
                                                theme, themeStyle, themeMotion, themeBackdrop, panelLayout, panelLateralX, panelCompactX,
                                                panelCompactY, hotkeys)

static std::filesystem::path ConfigPath() { return AppDataDir() / L"config.json"; }

static void Sanitize(Config& c) {
  c.clipSeconds = std::clamp(c.clipSeconds, 5, kMaxClipSeconds);
  c.shortClipSeconds = std::clamp(c.shortClipSeconds, 5, 120);
  c.recordLimitMinutes = std::clamp(c.recordLimitMinutes, 0, 24 * 60);
  c.deleteAfterDays = std::clamp(c.deleteAfterDays, 0, 3650);
  if (!IsKnownSaveSound(c.saveSound)) c.saveSound = "clasico";
  c.fps = std::clamp(c.fps, 15, 144);
  c.bitrateKbps = std::clamp(c.bitrateKbps, 1000, 150000);
  bool knownQuality = false;
  for (const auto& [value, label] : QualityOptions()) knownQuality |= value == c.quality;
  if (!knownQuality) c.quality = "alta";
  c.cameraSizePercent = std::clamp(c.cameraSizePercent, 10, 50);
  c.maxBufferMB = std::clamp(c.maxBufferMB, 64, 8192);
  c.systemVolume = std::clamp(c.systemVolume, 0.0f, 2.0f);
  c.micVolume = std::clamp(c.micVolume, 0.0f, 2.0f);
  if (c.outputHeight != 0) c.outputHeight = std::clamp(c.outputHeight, 360, 2160);
  if (c.monitorIndex < 0) c.monitorIndex = 0;
  if (!IsKnownTheme(c.theme)) c.theme = "negro";
  if (!c.themeStyle.empty() && !IsKnownStyle(c.themeStyle)) c.themeStyle.clear();
  if (c.themeBackdrop != "animado" && c.themeBackdrop != "quieto" && c.themeBackdrop != "no") c.themeBackdrop = "animado";
  if (c.panelLayout != "lateral" && c.panelLayout != "compacto") c.panelLayout = "lateral";
  // !(x >= 0) también descarta NaN (un config.json editado a mano).
  for (float* f : {&c.panelLateralX, &c.panelCompactX, &c.panelCompactY})
    *f = !(*f >= 0.0f) ? 0.0f : std::min(*f, 1.0f);
  if (c.screenshotFormat != "png" && c.screenshotFormat != "jpg") c.screenshotFormat = "png";
  c.storageLimitGB = std::clamp(c.storageLimitGB, 0, 100000);
  if (c.toastPosition != "top-right" && c.toastPosition != "top-left" && c.toastPosition != "bottom-right" &&
      c.toastPosition != "bottom-left")
    c.toastPosition = "top-right";
}

Config LoadConfig() {
  Config config;
  std::ifstream in(ConfigPath());
  if (in) {
    try {
      nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
      config = j.get<Config>();
    } catch (const std::exception& e) {
      Log("config.json inválido, se usan valores por defecto: %s", e.what());
      config = Config{};
    }
  }
  Sanitize(config);
  return config;
}

bool SaveConfig(const Config& config) {
  auto path = ConfigPath();
  auto tmp = path;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << nlohmann::json(config).dump(2);
    if (!out) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    Log("No se pudo guardar config.json: %s", ec.message().c_str());
    return false;
  }
  return true;
}

Config MergeEdits(const Config& current, const Config& base, const Config& edited) {
  nlohmann::json out = current, from = base, to = edited;
  for (auto it = to.begin(); it != to.end(); ++it) {
    const std::string& key = it.key();
    if (it->is_object() && from.contains(key) && from[key].is_object() && out.contains(key)) {
      for (auto sub = it->begin(); sub != it->end(); ++sub)
        if (!from[key].contains(sub.key()) || from[key][sub.key()] != *sub) out[key][sub.key()] = *sub;
    } else if (!from.contains(key) || from[key] != *it) {
      out[key] = *it;
    }
  }
  Config merged = out.get<Config>();
  Sanitize(merged);
  return merged;
}

bool SameConfig(const Config& a, const Config& b) { return nlohmann::json(a) == nlohmann::json(b); }

std::string ResolveOutputDir(const Config& config) {
  std::filesystem::path dir = config.outputDir.empty() ? DefaultOutputDir() : std::filesystem::path(Widen(config.outputDir));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return Narrow(dir.wstring());
}

} // namespace spectre
