#include "app/Config.h"

#include "common/Util.h"
#include "encode/Quality.h"
#include "ui/Theme.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace spectre {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HotkeyConfig, overlay, saveClip, toggleRecording, toggleReplay)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, replayEnabled, clipSeconds, onlyInGames, captureMode,
                                                monitorIndex, captureApi, captureCursor, fps, outputHeight,
                                                quality, bitrateKbps, codec, encoder, gpuPriority, captureSystemAudio,
                                                outputDeviceId, gameAudioOnly, captureMic, micDeviceId,
                                                systemVolume, micVolume, separateTracks, cameraEnabled, cameraId,
                                                cameraCorner, cameraSizePercent, outputDir, autostart,
                                                showNotifications, maxBufferMB, theme, themeMotion, hotkeys)

static std::filesystem::path ConfigPath() { return AppDataDir() / L"config.json"; }

static void Sanitize(Config& c) {
  c.clipSeconds = std::clamp(c.clipSeconds, 5, 600);
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

std::string ResolveOutputDir(const Config& config) {
  std::filesystem::path dir = config.outputDir.empty() ? DefaultOutputDir() : std::filesystem::path(Widen(config.outputDir));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return Narrow(dir.wstring());
}

} // namespace spectre
