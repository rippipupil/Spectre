#include "encode/Quality.h"

#include <algorithm>
#include <cmath>

namespace spectre {

int QualityLevel(const std::string& quality) {
  if (quality == "baja") return 0;
  if (quality == "media") return 1;
  if (quality == "maxima") return 3;
  if (IsSourceQuality(quality)) return kSourceLevel;
  if (quality == "personalizada") return -1;
  return 2;
}

int NormalizedRefreshHz(int refreshHz) {
  for (int rate : {30, 60, 75, 90, 100, 120, 144, 165, 240, 360})
    if (refreshHz == rate - 1) return rate;
  return refreshHz;
}

int SourceFps(int refreshHz, int width, int height, int fallbackFps) {
  if (refreshHz < 24) return fallbackFps;
  const int hz = NormalizedRefreshHz(refreshHz);
  const double frame = double(std::max(width, 16)) * double(std::max(height, 16)) / (1920.0 * 1080.0);
  auto fits = [&](int fps) { return fps <= kSourceMaxFps && frame * fps / 60.0 <= 5.0; };
  // Lo más alto que quepa: los Hz, o si no 144, 120 o 60.
  int fps = std::min(hz, kSourceMaxFps);
  for (int lower : {120, 60})
    if (fps > lower && !fits(fps)) fps = lower;
  if (fps == hz) return fps;
  // Si no son los Hz, mejor una fracción exacta de ellos cuando no pierde fps
  // de verdad: a 144 fps, un monitor de 240 Hz da uno o dos frames por cada
  // uno (5:3) y el scroll sale a saltitos; a 120, uno de cada dos, siempre
  // igual. Igual con 4K a 144 Hz, 72 antes que 60. Pero 180 Hz sigue a 144 y
  // no a 90: sería un 40 % menos de fps en el modo de lo máximo.
  for (int d = 2; hz / d >= 60; ++d)
    if (hz % d == 0 && fits(hz / d)) return hz / d >= 120 || hz / d > fps ? hz / d : fps;
  return fps;
}

QualityPlan PlanQuality(const std::string& quality, int customKbps, int width, int height, int fps,
                        const std::string& codec) {
  QualityPlan plan;
  plan.level = QualityLevel(quality);
  if (plan.level < 0) {
    plan.targetKbps = std::max(customKbps, 1000);
    plan.maxKbps = plan.targetKbps;
    plan.cq = 21;
    return plan;
  }
  // Tope de referencia: 30 Mbps para 1080p60 en H.264, escalado con la tasa
  // de píxeles (con un exponente < 1: a más resolución, menos bits por píxel).
  double frame = double(std::max(width, 16)) * double(std::max(height, 16)) / (1920.0 * 1080.0);
  double rate = double(std::max(fps, 1)) / 60.0;
  double cap = 30000.0 * std::pow(frame * rate, 0.85);
  // Fuente llega a 144 fps, y doblar los fps no pide doblar los bits: cada
  // frame se parece más al anterior. Ahí los fps pesan con 0,6
  // (investigacion/calidad-video.md, 4.2).
  if (plan.level == kSourceLevel) cap = 30000.0 * std::pow(frame, 0.85) * std::pow(rate, 0.6);
  // Fuente: el doble que Máxima y cq 15, que en juegos ya no se distingue del
  // original. El tope de RAM del replay (Pipeline::BufferLimit) sale de aquí.
  static const double kMultiplier[] = {0.5, 0.75, 1.0, 1.5, 3.0};
  cap *= kMultiplier[plan.level];
  if (codec == "hevc" || codec == "av1") cap *= 0.65;
  const double ceiling = plan.level == kSourceLevel ? 240000.0 : 150000.0;
  plan.maxKbps = int(std::clamp(cap, 4000.0, ceiling));
  plan.targetKbps = int(std::clamp(cap * 0.7, 3000.0, ceiling));
  static const int kCq[] = {27, 24, 21, 18, 15};
  plan.cq = kCq[plan.level] + ((codec == "hevc" || codec == "av1") ? 3 : 0);
  return plan;
}

} // namespace spectre
