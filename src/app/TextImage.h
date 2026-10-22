#pragma once

#include "app/VideoEdit.h"

#include <string>

namespace spectre {

// Texto del editor (título, marca de agua) dibujado con GDI en Segoe UI
// negrita: blanco con un borde negro o sobre una caja oscura, en RGBA con
// transparencia, partido en líneas de como mucho maxWidth px y centrado.
// Sirve como TextRasterizer de VideoEdit. Se puede llamar desde cualquier hilo.
bool RasterizeText(const std::string& text, int pixelHeight, int maxWidth, bool box, EditOverlay* out);

} // namespace spectre
