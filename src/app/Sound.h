#pragma once

namespace spectre {

// Sonidos cortos de confirmación (Ajustes > Avisos). Se generan en memoria al
// primer uso (unos pocos KB) y se reproducen con PlaySound, sin hilos propios.
enum class Cue { Saved, Marker };
void PlayCue(Cue cue);

} // namespace spectre
