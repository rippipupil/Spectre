#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SwrContext;

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
// estéreo. El loopback se abre con el formato real del mezclador (cualquier
// frecuencia, canales o profundidad) y se convierte con libswresample, y si
// no se eligió un dispositivo concreto sigue al dispositivo por defecto de
// Windows cuando cambia (auriculares Bluetooth que pasan a manos libres,
// cascos USB recién conectados...). La línea de tiempo se ancla a startQpc
// con la marca de tiempo de cada paquete (la hora de captura de su primera
// muestra, descontado el retardo del resampler) y los huecos (nada sonando)
// se rellenan con silencio. El anillo sabe en qué posición de esa línea de
// tiempo está cada muestra, así que quien lee pide una posición exacta y un
// retraso del hilo de captura nunca desplaza el audio: como mucho se pierde.
class AudioCapture {
public:
  ~AudioCapture() { Stop(); }

  bool Start(AudioSourceKind kind, const std::string& deviceId, DWORD processId, int64_t startQpc) {
    Begin(kind, deviceId, processId, startQpc);
    return WaitStarted();
  }
  // Arranca el hilo sin esperar a que abra el dispositivo, para abrir varias
  // fuentes a la vez. WaitStarted espera el resultado (y para si falla).
  void Begin(AudioSourceKind kind, const std::string& deviceId, DWORD processId, int64_t startQpc);
  bool WaitStarted(DWORD timeoutMs = 6000);
  void Stop();

  // Rellena dst con las frames muestras estéreo intercaladas de la posición
  // pos (en muestras a 48 kHz desde startQpc). Lo que aún no ha llegado o ya
  // se perdió sale como silencio. Devuelve cuántas eran audio capturado.
  size_t ReadAt(int64_t pos, float* dst, size_t frames);
  // Cuánto (en muestras) puede ir el último audio escrito por detrás del
  // reloj: quien lee debe ir al menos así de retrasado para no perder nada.
  int64_t LagFrames() const { return lag_.load(); }
  bool Running() const { return running_; }
  // Pico (0..1+) de lo escrito desde la última consulta. Para el medidor y el log.
  float TakePeak() { return peak_.exchange(0.0f); }
  // Dispositivo y formato con el que se abrió (para el log y los ajustes).
  std::string Description();

private:
  void Run();
  bool Open();
  void CloseClient();
  void Write(const float* data, size_t frames);
  void WriteSilence(size_t frames);
  size_t AlignTo(int64_t targetFrames, size_t packetFrames, int64_t tolerance);
  void Compensate(int64_t targetFrames, int64_t deadband, int64_t tolerance);
  void UpdateLag(int64_t packetEndFrames, int64_t nowFrames);

  AudioSourceKind kind_ = AudioSourceKind::SystemLoopback;
  std::wstring deviceId_;
  DWORD processId_ = 0;
  int64_t startQpc_ = 0;

  std::atomic<bool> running_{false};
  std::thread thread_;
  HANDLE stopEvent_ = nullptr;
  HANDLE readyEvent_ = nullptr;
  std::atomic<bool> startOk_{false};
  std::atomic<float> peak_{0.0f};
  std::atomic<int64_t> lag_{0};

  // Estado del cliente WASAPI (solo lo toca el hilo de captura).
  struct Client;
  Client* client_ = nullptr;
  std::wstring openedId_;  // id del endpoint abierto (vacío en audio por proceso)
  bool followDefault_ = false;

  std::mutex mutex_;
  std::string description_;
  std::vector<float> fifo_; // ring buffer de muestras intercaladas
  size_t readPos_ = 0;
  size_t count_ = 0; // en frames
  // Posición en la línea de tiempo justo después de la última muestra
  // escrita; la primera del anillo está en framesWritten_ - count_.
  int64_t framesWritten_ = 0;
  // Diagnóstico para el log (en muestras): leídas sin haber llegado aún y
  // llegadas cuando ya se habían leído; ajustes del reloj del dispositivo.
  int64_t missingFrames_ = 0;
  int64_t lateFrames_ = 0;
  int64_t jumpFrames_ = 0;
  int discontinuities_ = 0;
  int stalls_ = 0;          // vueltas del hilo de captura de más de 50 ms
  int64_t maxStall_ = 0;    // la más larga, en ticks de QPC
  int64_t silentTail_ = 0;  // relleno de silencio al final del anillo (solo hilo de captura)
};

} // namespace spectre
