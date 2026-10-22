#pragma once

#include "capture/CaptureSource.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spectre {

// Captura de pantalla de un solo fotograma, en un hilo propio: abre una
// captura (Windows.Graphics.Capture, o Desktop Duplication si no hay), copia
// el primer fotograma, la cierra y lo guarda en PNG o JPG con WIC. No depende
// de que el replay esté encendido y no cuesta nada mientras no se usa.
struct ScreenshotRequest {
  CaptureTarget target;
  bool cursor = true;
  std::string path; // .png o .jpg
  bool jpeg = false;
};

// done se llama desde el hilo de la captura: ok y la ruta, o el error.
void TakeScreenshot(ScreenshotRequest request, std::function<void(bool ok, const std::string& text)> done);

// Guarda una imagen BGR de 24 bits (de arriba abajo) en PNG o JPG con WIC, en
// un hilo propio y con prioridad baja (el fotograma del visor). done, como en
// TakeScreenshot.
void SaveImageAsync(std::string path, bool jpeg, std::vector<uint8_t> bgr, unsigned width, unsigned height,
                    std::function<void(bool ok, const std::string& text)> done);

} // namespace spectre
