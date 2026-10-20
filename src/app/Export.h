#pragma once

// «Exportar para Discord»: una copia del vídeo en H.264 y AAC que cabe en el
// límite de subida de Discord sin Nitro y que Discord reproduce dentro del
// chat. Solo trabaja al pedirlo, en un hilo propio de prioridad baja, con el
// encoder de la GPU; el original no cambia.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

// Discord sin Nitro: 20 MB por archivo (desde el 13-08-2026).
constexpr uint64_t kDiscordLimitBytes = 20000000;

struct ExportOptions {
  uint64_t maxBytes = kDiscordLimitBytes;
  // Hay un juego abierto: se decodifica en un solo hilo, en modo de fondo.
  bool gentle = true;
  // Encoders a probar, en orden ("h264_mf:hw" = Media Foundation con la GPU).
  // Vacío: los de la GPU y, si ninguno abre, los de la CPU.
  std::vector<std::string> encoders;
};

struct ExportResult {
  bool ok = false;
  bool cancelled = false;
  bool alreadyFits = false; // ya era H.264 y cabía: no se recodificó
  std::string input;
  std::string output;  // la copia, o el original si ya cabía
  uint64_t bytes = 0;
  std::string encoder; // el que se usó, para el log
  std::string error;   // texto para el aviso
};

// Ruta libre junto al original: <nombre>_discord.mp4, <nombre>_discord_2.mp4...
std::string ExportPathFor(const std::string& input);

// Exporta de una vez (bloquea). progress recibe 0..1; cancel se mira en cada
// paquete. Si la copia sale grande, se repite con menos bitrate.
ExportResult ExportVideo(const std::string& input, const std::string& output, const ExportOptions& options,
                         const std::atomic<bool>& cancel, const std::function<void(float)>& progress);

// Una exportación cada vez, en su propio hilo.
class Exporter {
public:
  ~Exporter() { Stop(); }
  // false si ya hay una en marcha. onProgress (0..99, solo cuando cambia) y
  // onDone se llaman desde el hilo de la exportación.
  bool Start(const std::string& input, const ExportOptions& options, std::function<void(int)> onProgress,
             std::function<void()> onDone);
  void Cancel() { cancel_ = true; }
  // Cancela y espera al hilo (al salir de Spectre).
  void Stop();
  bool Busy() const { return busy_; }
  int Percent() const { return percent_; }
  std::string Input() const;
  // El resultado de la última exportación terminada, una sola vez.
  bool TakeResult(ExportResult* result);

private:
  std::thread thread_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> busy_{false};
  std::atomic<int> percent_{0};
  mutable std::mutex mutex_;
  std::string input_;
  bool haveResult_ = false;
  ExportResult result_;
};

} // namespace spectre
