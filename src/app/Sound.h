#pragma once

#include <string>
#include <utility>
#include <vector>

namespace spectre {

// Sonidos cortos de confirmación (Ajustes > Avisos). Se generan en memoria al
// primer uso (unos pocos KB) y se reproducen con PlaySound, sin hilos propios.
// Voice: orden de voz entendida (Ajustes > Voz), más grave que el de guardar.
enum class Cue { Saved, Marker, Voice };
// style: el sonido de guardar elegido en Ajustes (Config::saveSound).
void PlayCue(Cue cue, const std::string& style = "clasico");

// Sonidos de guardar que se pueden elegir: id (config) y nombre.
const std::vector<std::pair<std::string, const char*>>& SaveSounds();
bool IsKnownSaveSound(const std::string& id);

} // namespace spectre
