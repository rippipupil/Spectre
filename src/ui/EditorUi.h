#pragma once

// «Editar» de la biblioteca: una página con el vídeo, una línea de tiempo con
// asas y las herramientas en seis pestañas (Tiempo, Imagen, Color, Texto,
// Audio, Cámara). Nada se escribe hasta pulsar «Guardar copia»: entonces App lo pasa
// a EditWorker (app/VideoEdit.h) y el original no cambia.
//
// La vista previa no recodifica nada: el reproductor del visor enseña el
// original y encima se dibuja el formato, el giro, el zoom y el texto. En
// pausa, un hilo propio aplica todos los filtros de verdad al fotograma que se
// ve (RenderEditPreview), así el color y los difuminados se ven exactos.

#include "app/Library.h"
#include "app/VideoEdit.h"
#include "ui/AppWindow.h"
#include "ui/Player.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spectre {

struct AppModel;
struct AppActions;

class EditorUi {
public:
  EditorUi();
  ~EditorUi();

  // La ventana se abrió: los resultados del hilo de la vista previa se avisan
  // con notifyMsg (el hilo solo arranca al abrir el editor).
  void Start(HWND notifyWindow, UINT notifyMsg);
  // La ventana se cerró: se suelta todo, también los cambios sin guardar.
  void Stop();
  // Abre el archivo. Si es el mismo de la última vez, con los cambios que tenía.
  void Open(const std::string& path);
  // Se sale de la página: fuera el reproductor y las texturas; los cambios se
  // quedan por si se vuelve al mismo archivo.
  void Leave();
  bool Active() const { return active_; }
  const std::string& Path() const { return path_; }
  void Pause();
  bool Playing() const { return player_.Playing(); }

  struct Exit {
    bool back = false;   // Volver (Esc): al visor
    std::string view;    // abrir en el visor lo que se acaba de guardar
  };
  Exit Draw(AppWindow& w, AppModel& m, const AppActions& a, const MediaItem& item);
  // Llegaron resultados del hilo (análisis, vista previa, texto).
  void TakeResults(AppWindow& w);

private:
  struct Worker;

  // Una imagen de texto o marca de agua para la vista previa en movimiento:
  // posición y tamaño en fracciones de la imagen final.
  struct LiveOverlay {
    std::shared_ptr<UiTexture> texture;
    float x = 0, y = 0, w = 0, h = 0;
    double until = 0;
  };

  double Duration(const MediaItem& item) const;
  double End(const MediaItem& item) const;
  void Seek(double seconds, bool exact = true);
  void TogglePlay(const MediaItem& item);
  void SyncPlayback(const MediaItem& item);
  void SetStart(double t, const MediaItem& item);
  void SetEnd(double t, const MediaItem& item);

  void Header(const MediaItem& item, Exit* exit);
  void Preview(AppWindow& w, const MediaItem& item, ImVec2 a, ImVec2 b);
  void RequestPreview(AppWindow& w, ImVec2 area);
  void RequestCamFrame();
  void Transport(const MediaItem& item, float width);
  void Timeline(const MediaItem& item, float width);
  void Tools(const AppModel& m, const MediaItem& item, const AppActions& a, float height);
  void TabTime(const MediaItem& item, const AppActions& a);
  void TabPicture();
  void TabColor();
  void TabText();
  void TabAudio();
  void TabCamera(const AppModel& m);
  void Footer(AppModel& m, const AppActions& a, const MediaItem& item, Exit* exit);
  void Keyboard(const AppActions& a, const MediaItem& item, Exit* exit);
  void Commit();
  void Undo();
  void Redo();
  std::string Summary(const MediaItem& item) const;
  bool CanSave(const MediaItem& item, std::string* why) const;

  std::unique_ptr<Worker> worker_;
  HWND notify_ = nullptr;
  UINT notifyMsg_ = 0;
  bool active_ = false;
  std::string path_;
  Player player_;
  bool playerFailed_ = false;
  bool firstFrame_ = false;  // ya se pidió el primer fotograma (abierto en pausa)
  bool wantPlay_ = false;    // reproduciendo (también parado en la imagen congelada o al dar la vuelta)
  unsigned long long settleUntil_ = 0; // tras un salto: frames hasta que llegue el fotograma
  bool muted_ = false;       // solo la vista previa
  float volume_ = 1.0f;

  // Lo que se va a guardar, y deshacer/rehacer (una foto de los ajustes por cambio).
  EditSettings s_;
  EditSettings committed_;
  std::vector<EditSettings> undo_, redo_;

  // Análisis del original (pistas de audio, fps, tamaño), del hilo.
  bool probed_ = false;
  std::string probeError_;
  EditSource src_;

  int tab_ = 0;
  // Línea de tiempo: asa arrastrada (0 ninguna) y arrastre del cabezal.
  int drag_ = 0;
  bool scrubbing_ = false;
  bool wasPlaying_ = false;
  // Arrastre sobre la imagen: 1 encuadre (zoom o recorte), 2 mover la zona
  // tapada, 3 cambiar su tamaño, 4 un clic (reproducir o pausar).
  int areaMode_ = 0;
  ImVec2 dragFrom_{};
  float dragStart_[4] = {};

  // Reproducción que imita el resultado: tramo, trozo quitado, velocidades y
  // la imagen congelada.
  double lastPos_ = -1;
  unsigned long long frozenUntil_ = 0;
  bool frozen_ = false;

  // Vista previa exacta (en pausa): fotograma leído, petición y resultado.
  std::shared_ptr<const std::vector<uint8_t>> frameBgr_;
  int frameW_ = 0, frameH_ = 0;
  uint64_t frameSerial_ = ~0ull;  // fotograma del reproductor que hay en frameBgr_
  bool previewBusy_ = false;
  EditSettings askedSettings_;
  uint64_t askedFrame_ = ~0ull;
  unsigned long long askedAt_ = 0;
  std::shared_ptr<UiTexture> exact_;
  EditSettings exactSettings_;
  uint64_t exactFrame_ = ~0ull;
  uint64_t serial_ = 0;           // descarta resultados de otro archivo

  // Texto y marca de agua en movimiento.
  std::vector<LiveOverlay> overlays_;
  std::string overlayKey_, overlayAsked_;

  // Cámara cambiada en movimiento: el fondo de la pista aparte en el último
  // momento pedido (el hilo lo lee mientras se reproduce).
  std::shared_ptr<UiTexture> camBg_;
  bool camBusy_ = false;
  double camAsked_ = -1;
  bool camGap_ = false; // en ese momento no se veía la cámara: el fotograma se ve tal cual
};

} // namespace spectre
