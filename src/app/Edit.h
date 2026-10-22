#pragma once

// Ediciones de la biblioteca que no recodifican (copia de paquetes, como el
// recorte): unir clips y la cámara lenta. Tardan lo que tarda copiar el
// archivo y no tocan la GPU ni el encoder; se llaman desde un hilo propio.

#include <string>
#include <vector>

namespace spectre {

// Une los vídeos en ese orden en uno nuevo. Solo funciona si tienen las mismas
// pistas con el mismo códec, resolución y parámetros (los mismos ajustes de
// Spectre): si no, *error dice cuál no encaja y no se escribe nada.
bool JoinFiles(const std::vector<std::string>& inputs, const std::string& output, std::string* error);

// Copia el vídeo a 1/factor de velocidad estirando los tiempos de cada
// fotograma (factor 2 o 4): un clip a 120 fps se ve a 60 fps, sin saltos. El
// audio no se puede estirar sin recodificarlo y se quita.
bool SlowMotionFile(const std::string& input, const std::string& output, int factor, std::string* error);

} // namespace spectre
