#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spectre {

constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;

struct AudioDevice {
  std::string id;   // id de IMMDevice (UTF-8)
  std::string name;
};
std::vector<AudioDevice> EnumerateAudioDevices(bool capture);

enum class AudioSourceKind {
  SystemLoopback,  // todo lo que suena por un dispositivo de salida
  ProcessLoopback, // solo el audio de un proceso (Windows 10 2004+)
  Microphone,
};

// Captura WASAPI en modo compartido, siempre convertida a float 48 kHz
// estéreo. La línea de tiempo se ancla a startQpc y los huecos (nada
// sonando) se rellenan con silencio para que el audio no se desincronice.
class AudioCapture {
public:
  ~AudioCapture() { Stop(); }

  bool Start(AudioSourceKind kind, const std::string& deviceId, DWORD processId, int64_t startQpc);
  void Stop();

  size_t Available();
  // Copia hasta frames muestras estéreo intercaladas. Devuelve las copiadas.
  size_t Read(float* dst, size_t frames);
  bool Running() const { return running_; }

private:
  void Run();
  void Write(const float* data, size_t frames);
  void WriteSilence(size_t frames);

  AudioSourceKind kind_ = AudioSourceKind::SystemLoopback;
  std::wstring deviceId_;
  DWORD processId_ = 0;
  int64_t startQpc_ = 0;

  std::atomic<bool> running_{false};
  std::thread thread_;
  HANDLE stopEvent_ = nullptr;
  HANDLE readyEvent_ = nullptr;
  bool startOk_ = false;

  std::mutex mutex_;
  std::vector<float> fifo_; // ring buffer de muestras intercaladas
  size_t readPos_ = 0;
  size_t count_ = 0; // en frames
  uint64_t framesWritten_ = 0;
};

} // namespace spectre
