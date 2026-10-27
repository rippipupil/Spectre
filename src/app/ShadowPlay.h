#pragma once

// Clips de NVIDIA ShadowPlay (GeForce Experience o la app de NVIDIA) en la
// biblioteca de Spectre. Se ven desde su carpeta, sin copiar nada ni
// recodificar; «Importar» los copia o los mueve a la carpeta de Spectre con
// su nombre y su fecha (ParseMediaName los sigue reconociendo: etiqueta NVIDIA
// y nunca se borran solos).

#include "app/Library.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace spectre {

// Dónde buscar: `custom` si no está vacía; si no, la carpeta que tiene
// configurada ShadowPlay (DefaultPathW en la clave NVSPCAPS de su registro),
// Vídeos\NVIDIA (la app de NVIDIA) y Vídeos (GeForce Experience). Solo las
// que existen, sin repetir.
std::vector<std::string> ShadowPlayFolders(const std::string& custom);

struct ShadowPlayImport {
  int copied = 0;  // copiados o movidos
  int skipped = 0; // ya estaban en la carpeta de Spectre
  int failed = 0;
  uint64_t bytes = 0;
  std::string error; // el primer fallo, en lenguaje claro
};

// Copia (o mueve, con move) los archivos a outputDir (a una subcarpeta por
// juego con perGame). Conserva el nombre y la fecha de modificación; lo que ya
// está allí con el mismo tamaño se salta. progress(hechos, total) va desde el
// hilo que llama; cancel corta entre archivos (y a mitad de una copia).
ShadowPlayImport ImportShadowPlay(Library& library, const std::vector<MediaItem>& items, const std::string& outputDir,
                                  bool perGame, bool move, const std::atomic<bool>& cancel,
                                  const std::function<void(int, int)>& progress);

} // namespace spectre
