#pragma once

// Miniaturas y vista previa de los clips del panel Alt+Z. Solo trabaja con el
// panel abierto: el hilo de las miniaturas arranca al abrirlo y se para al
// cerrarlo, y el vídeo de la vista previa se cierra al volver a la lista o al
// cerrar el panel. Entre aperturas solo quedan en RAM las miniaturas pequeñas
// ya leídas (unos KB cada una), para no esperar al Explorador cada vez.

#include "app/Library.h"
#include "ui/Player.h"
#include "ui/Texture.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spectre {

class OverlayWindow;

class PanelMedia {
public:
  // Panel abierto: el hilo de miniaturas avisa con notifyMsg a notifyWindow.
  void Begin(HWND notifyWindow, UINT notifyMsg);
  // Panel cerrado, antes de ocultar su ventana: el vídeo y las texturas son
  // de la GPU del panel.
  void End();
  // notifyMsg: recoge las miniaturas que llegaron.
  void TakeResults();
  // La ventana del panel se va a recrear (GPU reiniciada, DPI): fuera el vídeo
  // y las texturas, que eran de su GPU. Las miniaturas en RAM se quedan.
  void Release();

  // Al empezar cada frame del panel: suelta lo que se dibujó en el anterior.
  // Todo lo que se dibuja se guarda hasta entonces, porque ImGui lo usa al
  // pintar, después del frame, aunque entretanto se cierre el vídeo.
  void NewFrame();

  // Durante el frame. Miniatura del vídeo para verla a unos width x height
  // píxeles; nula hasta que llega (se pide al hilo la primera vez).
  UiTexture* Thumbnail(OverlayWindow& window, const MediaItem& item, int width, int height);

  // Vista previa: el reproductor con ese vídeo, abierto (en marcha) la primera
  // vez que se pide, con fotogramas de frameWidth x frameHeight. Nulo si aquí
  // no se puede reproducir (Windows N, GPU sin vídeo); si se abrió pero el
  // archivo falla, Failed() y Error() lo dicen.
  Player* Preview(OverlayWindow& window, const MediaItem& item, int frameWidth, int frameHeight);
  // El fotograma actual de la vista previa para dibujarlo en este frame.
  UiTexture* PreviewFrame();
  void ClosePreview();

  // Sin sonido de entrada: lo que suena en el PC también entra en el replay.
  // Se mantiene de un clip a otro mientras Spectre está abierto.
  bool muted = true;

private:
  // Lo de una GPU anterior del panel (se recreó) no se puede dibujar.
  void Sync(OverlayWindow& window);

  struct Slot {
    int requested = 0; // ancho pedido al Explorador
    bool pending = false;
    bool failed = false;
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;          // la copia en RAM
    std::shared_ptr<UiTexture> texture; // en la GPU del panel abierto
  };
  // Pequeña para las filas de recientes y grande para la vista del clip:
  // reducir mucho una imagen sin mipmaps la deja dentada.
  struct Thumb {
    int64_t modified = 0;
    unsigned long long lastUse = 0;
    Slot small, large;
  };
  // Pide la miniatura si hace falta (y mayRequest) y la sube a la GPU del panel.
  UiTexture* Use(OverlayWindow& window, const std::string& path, Slot& slot, int width, int height, bool mayRequest);

  std::map<std::string, Thumb> thumbs_;
  std::vector<std::shared_ptr<UiTexture>> drawn_; // lo dibujado en el frame en curso
  MediaWorker worker_;
  bool running_ = false;
  unsigned generation_ = 0; // OverlayWindow::Generation() de las texturas
  Player player_;
  std::string previewPath_;
  bool previewFailed_ = false;
};

} // namespace spectre
