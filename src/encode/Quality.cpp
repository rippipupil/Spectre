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
  double pixelRate = double(std::max(width, 16)) * double(std::max(height, 16)) * double(std::max(fps, 1));
  double ratio = pixelRate / (1920.0 * 1080.0 * 60.0);
  double cap = 30000.0 * std::pow(ratio, 0.85);
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
