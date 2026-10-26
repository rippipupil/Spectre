#pragma once

// Editor de capturas (Alt+F1 y fotogramas guardados): lo que se suele hacer
// con una imagen antes de mandarla (recortar, girar, color, una flecha, un
// texto, tapar un nombre) sin capas ni pinceles. Todo es una opción de
// PhotoSettings y se aplica en la CPU sobre BGRA, en este orden: girar,
// recortar, color, marcas, tamaño y marco. El original no cambia.
//
// Portable (solo std): el texto llega ya dibujado (TextRasterizer, con GDI en
// Windows) y leer o guardar el archivo va aparte (WIC), así se prueba en Linux.

#include "app/VideoEdit.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace spectre {

// Libre, la de la imagen, 16:9, 9:16, 1:1, 4:3 y 4:5.
enum class PhotoAspect { Free, Original, Wide, Vertical, Square, Classic, Portrait };
// La de la imagen, 1920 o 1280 px de lado largo como mucho, o la mitad.
enum class PhotoSize { Same, Large, Medium, Half };
enum class PhotoMarkKind { Text, Arrow, Box, Blur, Pixelate };

struct PhotoMark {
  PhotoMarkKind kind = PhotoMarkKind::Box;
  // Fracción de la imagen recortada. Flecha: de (x0, y0) a la punta (x1, y1);
  // texto: su centro en (x0, y0); el resto, el rectángulo entre los dos puntos.
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t color = 0xFF3B30; // 0xRRGGBB
  int size = 1;              // 0 fino o pequeño, 1 medio, 2 grueso o grande
  std::string text;          // UTF-8
  bool box = false;          // texto sobre una caja oscura (si no, con borde)
  bool operator==(const PhotoMark&) const = default;
};

struct PhotoSettings {
  int rotate = 0; // 0, 90, 180 o 270, en el sentido de las agujas del reloj
  bool flipH = false, flipV = false;
  // Recorte en fracción de la imagen ya girada (0, 0, 1, 1 = entera).
  float cropX = 0, cropY = 0, cropW = 1, cropH = 1;
  PhotoAspect aspect = PhotoAspect::Free;
  int brightness = 0, contrast = 0, saturation = 0; // -100..100, como en el editor de vídeo
  EditLook look = EditLook::None;
  bool sharpen = false, vignette = false;
  std::vector<PhotoMark> marks;
  int border = 0; // marco alrededor: 0 sin, 1 fino, 2 grueso
  uint32_t borderColor = 0xFFFFFF;
  PhotoSize size = PhotoSize::Same;
  bool operator==(const PhotoSettings&) const = default;
};

struct PhotoImage {
  std::vector<uint8_t> bgra; // de arriba abajo, opaca
  int width = 0, height = 0;
  bool Empty() const { return width <= 0 || height <= 0 || bgra.size() < size_t(width) * size_t(height) * 4; }
};

struct PhotoRect {
  int x = 0, y = 0, w = 0, h = 0;
};

// Tamaño de la imagen girada.
void PhotoRotatedSize(const PhotoSettings& s, int width, int height, int* rw, int* rh);
// El recorte en píxeles de la imagen girada.
PhotoRect PhotoCrop(const PhotoSettings& s, int width, int height);
// Ancho / alto de una proporción; 0 = libre.
double PhotoAspectRatio(PhotoAspect aspect, int rotatedW, int rotatedH);
// El recorte más grande con esa proporción, centrado en el de ahora.
void ApplyPhotoAspect(PhotoSettings* s, int width, int height);
// Tamaño final (con «Tamaño» y el marco) de una imagen de width x height.
void PhotoOutputSize(const PhotoSettings& s, int width, int height, int* outW, int* outH);
// Hay algo que cambiar (si no, guardar daría la misma imagen).
bool PhotoTouches(const PhotoSettings& s);

struct PhotoRender {
  bool finalSize = false; // aplicar «Tamaño» (al guardar; la vista previa no)
  bool cropView = false;  // la imagen girada entera con su color: sin recortar, sin marcas ni marco
};
// Dónde quedó cada marca, en fracción de la imagen recortada (el texto ocupa
// lo que ocupe ya dibujado): para tocarlas en la vista previa.
using PhotoMarkBoxes = std::vector<std::array<float, 4>>;
bool RenderPhoto(const PhotoSettings& s, const PhotoImage& source, const TextRasterizer& raster,
                 const PhotoRender& options, PhotoImage* out, PhotoMarkBoxes* boxes = nullptr);

// Reduce (media de área) o amplía (bilineal) a w x h.
PhotoImage ScalePhoto(const PhotoImage& in, int w, int h);
// BGRA -> BGR de 24 bits para guardar con WIC.
std::vector<uint8_t> PhotoToBgr(const PhotoImage& image);

} // namespace spectre
