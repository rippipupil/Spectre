#pragma once

#include <string>
#include <vector>

namespace spectre {

// Preajustes de calidad de vídeo. "personalizada" usa el bitrate que fije el
// usuario; los demás calculan el bitrate según resolución, fps y códec.
// "fuente" graba siempre a la resolución nativa con lo máximo que da el
// encoder sin dejar de ir en tiempo real (ver PlanQuality y VideoEncoder).
inline const std::vector<std::pair<std::string, const char*>>& QualityOptions() {
  static const std::vector<std::pair<std::string, const char*>> options = {
      {"baja", "Baja, archivos pequeños"},
      {"media", "Media"},
      {"alta", "Alta (recomendada)"},
      {"maxima", "Máxima, como ShadowPlay en Alta"},
      {"fuente", "Fuente, lo máximo que da tu PC"},
      {"personalizada", "Personalizada (bitrate manual)"},
  };
  return options;
}

// 0 = baja ... 3 = máxima, 4 = fuente; -1 = personalizada.
int QualityLevel(const std::string& quality);
constexpr int kSourceLevel = 4;
inline bool IsSourceQuality(const std::string& quality) { return quality == "fuente"; }

struct QualityPlan {
  int targetKbps = 20000; // bitrate medio al que apunta el encoder
  int maxKbps = 30000;    // tope: lo que como mucho ocupa un segundo (y la RAM del replay)
  int cq = 21;            // calidad objetivo (menor = mejor) para los encoders que la admiten
  int level = 2;          // nivel 0..4 (-1 = personalizada)
};

// Fps de Fuente: los Hz del monitor (59,94 Hz, que Windows da como 59, cuenta
// como 60), como mucho 144. Si la resolución es grande se baja a 120 o a 60
// para que el encoder siga en tiempo real con el juego usando la GPU: hasta
// unas 5 veces el trabajo de 1080p60 (1440p a 144 sí, 4K a 144 no). Sin Hz
// conocidos, los fps elegidos.
constexpr int kSourceMaxFps = 144;
int SourceFps(int refreshHz, int width, int height, int fallbackFps);

// Calcula el plan para una salida concreta. Para 1080p60 en H.264: baja 15,
// media 22, alta 30, máxima 45 y fuente 90 Mbps de tope; se escala con los
// píxeles por segundo y HEVC/AV1 necesitan un tercio menos. En Fuente los
// fps pesan menos (152 Mbps en 1080p144) y el tope llega a 240 Mbps, lo que
// admite H.264 nivel 5.1; el resto, a 150.
QualityPlan PlanQuality(const std::string& quality, int customKbps, int width, int height, int fps,
                        const std::string& codec);

} // namespace spectre
