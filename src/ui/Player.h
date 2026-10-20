#pragma once

#include "ui/AppWindow.h"

#include <memory>
#include <string>

namespace spectre {

// Reproductor del visor de la biblioteca: Media Foundation (IMFMediaEngine)
// en modo "frame server". Decodifica con la GPU y deja cada fotograma en una
// textura que la interfaz dibuja como una imagen más. Solo existe mientras el
// visor tiene un vídeo abierto; sin Media Foundation (Windows N) los clips se
// abren con el reproductor de Windows.
class Player {
public:
  Player();
  ~Player();
  static bool Available();

  bool Open(AppWindow& window, const std::string& path, bool autoplay);
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

  // En cada frame de la interfaz: copia el fotograma nuevo (si lo hay) a la
  // textura y la devuelve (nula hasta el primero).
  std::shared_ptr<UiTexture> Frame(AppWindow& window);
  int VideoWidth() const;
  int VideoHeight() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace spectre
