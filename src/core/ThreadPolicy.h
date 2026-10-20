#pragma once

#include <windows.h>

namespace spectre {

// Cómo se reparten la CPU, el disco y la memoria los hilos de Spectre frente
// al juego. Lo que tiene plazo (cada frame de vídeo, la captura de audio) va
// con prioridad alta; lo que no lo tiene (escribir un clip o una grabación)
// cede todo lo posible.

// eco = true: EcoQoS. El planificador lleva el hilo a los núcleos de
// eficiencia (Intel de 12.ª generación en adelante) y a la frecuencia más
// eficiente, lejos de los núcleos en los que corre el juego. En Windows 10
// equivale a la QoS baja. eco = false: lo contrario, Windows no lo frena
// aunque Spectre no tenga ninguna ventana a la vista. No hace nada donde la
// función no existe.
void SetThreadEcoQoS(bool eco);

// Modo de fondo del hilo actual (THREAD_MODE_BACKGROUND_BEGIN): E/S de
// prioridad muy baja y páginas de memoria de prioridad baja, así el juego lee
// del disco y conserva su caché de archivos antes que nosotros. También baja
// la CPU: solo para trabajo que puede esperar.
class BackgroundMode {
public:
  BackgroundMode() { Enter(); }
  ~BackgroundMode() { Leave(); }
  BackgroundMode(const BackgroundMode&) = delete;
  BackgroundMode& operator=(const BackgroundMode&) = delete;

  void Enter() {
    if (!active_) active_ = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
  }
  // Recupera la prioridad que tenía el hilo antes de Enter().
  void Leave() {
    if (active_) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    active_ = false;
  }
  bool Active() const { return active_; }

private:
  bool active_ = false;
};

} // namespace spectre
