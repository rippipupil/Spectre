#pragma once

#include "ui/AppWindow.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spectre {

// Reproductor del visor de la biblioteca y de la vista previa del panel:
// Media Foundation (IMFMediaEngine) en modo "frame server". Decodifica con la
// GPU y deja cada fotograma en una textura que la interfaz dibuja como una
// imagen más. Solo existe mientras hay un vídeo abierto; sin Media Foundation
// (Windows N) los clips se abren con el reproductor de Windows.
class Player {
public:
  Player();
  ~Player();
  static bool Available();

  bool Open(AppWindow& window, const std::string& path, bool autoplay) {
    return Open(window.VideoSupport() ? window.Device() : nullptr, path, autoplay);
  }
  // device: el de la ventana que dibuja los fotogramas, creado con
  // VIDEO_SUPPORT y protegido para varios hilos (el motor lo usa desde los suyos).
  bool Open(ID3D11Device* device, const std::string& path, bool autoplay);
  void Close();
  bool IsOpen() const;
  const std::string& Path() const;
  bool Ready() const;  // ya se conoce la duración y el tamaño
  bool Failed() const;
  std::string Error() const;

  void Play();
  void Pause();
  bool Playing() const;
  bool Ended() const;
  // exact=false salta al fotograma clave más cercano (arrastrando la barra).
  void Seek(double seconds, bool exact = true);
  double Position() const;
  double Duration() const;
  void SetVolume(float volume); // 0..1
  float Volume() const;
  void SetMuted(bool muted);
  bool Muted() const;
  // Velocidad de reproducción (vista previa del editor): 1 = normal.
  void SetRate(double rate);

  // En cada frame de la interfaz: copia el fotograma nuevo (si lo hay) a la
  // textura y la devuelve (nula hasta el primero).
  std::shared_ptr<UiTexture> Frame();
  std::shared_ptr<UiTexture> Frame(AppWindow&) { return Frame(); }
  // Fotogramas de este tamaño fijo en vez del del vídeo (la vista previa del
  // panel): la GPU los reduce al copiarlos, con bandas negras si no encajan.
  // 0 = el tamaño del vídeo.
  void SetFrameSize(int width, int height);
  int VideoWidth() const;
  int VideoHeight() const;
  // Cambia cada vez que Frame copia un fotograma nuevo a la textura.
  uint64_t FrameSerial() const;
  // Copia el último fotograma mostrado (BGR de 24 bits, de arriba abajo) para
  // guardarlo como imagen. Lee la textura de la GPU: solo al pulsar el botón.
  bool ReadFrame(std::vector<uint8_t>* bgr, int* width, int* height) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace spectre
