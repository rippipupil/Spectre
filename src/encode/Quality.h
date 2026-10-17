#pragma once

#include <string>
#include <vector>

namespace spectre {

// Preajustes de calidad de vídeo. "personalizada" usa el bitrate que fije el
// usuario; los demás calculan el bitrate según resolución, fps y códec.
inline const std::vector<std::pair<std::string, const char*>>& QualityOptions() {
  static const std::vector<std::pair<std::string, const char*>> options = {
      {"baja", "Baja, archivos pequeños"},
      {"media", "Media"},
      {"alta", "Alta (recomendada)"},
      {"maxima", "Máxima, como ShadowPlay en Alta"},
      {"personalizada", "Personalizada (bitrate manual)"},
  };
  return options;
}

// 0 = baja ... 3 = máxima; -1 = personalizada.
int QualityLevel(const std::string& quality);

struct QualityPlan {
  int targetKbps = 20000; // bitrate medio al que apunta el encoder
  int maxKbps = 30000;    // tope: lo que como mucho ocupa un segundo (y la RAM del replay)
  int cq = 21;            // calidad objetivo (menor = mejor) para los encoders que la admiten
  int level = 2;          // nivel 0..3 (-1 = personalizada)
};

// Calcula el plan para una salida concreta. Para 1080p60 en H.264: baja 15,
// media 22, alta 30 y máxima 45 Mbps de tope; se escala con los píxeles por
// segundo y HEVC/AV1 necesitan un tercio menos.
QualityPlan PlanQuality(const std::string& quality, int customKbps, int width, int height, int fps,
                        const std::string& codec);

} // namespace spectre
