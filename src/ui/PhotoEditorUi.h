#pragma once

// «Editar» de una captura (Alt+F1 o un fotograma guardado): la imagen a la
// izquierda, las herramientas a la derecha en cuatro pestañas (Recortar,
// Color, Marcar, Marco) y abajo cómo se guarda (PNG, JPG o para Discord).
// Se ve igual que el editor de vídeo y usa sus piezas (EditorWidgets.h).
// Leer la imagen y la vista previa van en un hilo propio; guardar la copia lo
// hace App en otro (SavePhotoEditAsync). El original no cambia.

#include "app/Library.h"
#include "app/PhotoEdit.h"
#include "ui/AppWindow.h"

#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

namespace spectre {

struct AppModel;
struct AppActions;

class PhotoEditorUi {
public:
  PhotoEditorUi();
  ~PhotoEditorUi();
  PhotoEditorUi(const PhotoEditorUi&) = delete;
  PhotoEditorUi& operator=(const PhotoEditorUi&) = delete;

  // La ventana se abrió o se cerró (el hilo se crea al abrir una imagen).
  void Start(HWND notifyWindow, UINT notifyMsg);
  void Stop();
  // Abre una captura; si es la misma de antes, los cambios siguen ahí.
  void Open(const std::string& path);
  // Se sale del editor: se suelta la imagen y la vista previa (los cambios de
  // la última se recuerdan mientras no se abra otra).
  void Leave();
  bool Active() const { return active_; }
  const std::string& Path() const { return path_; }

  struct Exit {
    bool back = false; // Volver (Esc)
    std::string view;  // abrir en el visor lo que se acaba de guardar
  };
  Exit Draw(AppWindow& w, AppModel& m, const AppActions& a, const MediaItem& item);
  // Llegaron resultados del hilo (imagen leída, vista previa).
  void TakeResults(AppWindow& w);

private:
  struct Worker;

  // Dónde se ve la imagen: la girada entera (Recortar) o el resultado, y
  // dentro de él la parte sin el marco, donde van las marcas.
  struct Layout {
    ImVec2 a, b;           // la imagen en pantalla
    ImVec2 ca, cb;         // Recortar: el recorte; si no, la imagen sin el marco
    int baseW = 0, baseH = 0; // tamaño de la imagen de trabajo
  };
  Layout ComputeLayout(ImVec2 va, ImVec2 vb) const;

  void Header(const MediaItem& item, Exit* exit);
  void Preview(AppWindow& w, ImVec2 va, ImVec2 vb);
  void CropArea(const Layout& l);
  void MarkArea(const Layout& l);
  void RequestRender(AppWindow& w);
  void Tools(const AppModel& m, float height);
  void TabCrop();
  void TabColor();
  void TabMarks(const AppModel& m);
  void TabFrame(const AppModel& m);
  void Footer(AppModel& m, const AppActions& a, Exit* exit);
  void Keyboard(AppModel& m, const AppActions& a, Exit* exit);
  void Save(AppModel& m, const AppActions& a);
  void Commit();
  void Undo();
  void Redo();
  std::string Summary() const;
  PhotoMark& NewMark(PhotoMarkKind kind);

  std::unique_ptr<Worker> worker_;
  HWND notify_ = nullptr;
  UINT notifyMsg_ = 0;
  uint64_t serial_ = 0; // cambia al abrir otra imagen: lo que llegue del hilo para la de antes no vale
  bool active_ = false;
  std::string path_;

  // Imagen
  bool loaded_ = false;
  std::string loadError_;
  int fullW_ = 0, fullH_ = 0; // el original
  int baseW_ = 0, baseH_ = 0; // la copia de trabajo de la vista previa (más pequeña)

  // Ajustes, deshacer y rehacer
  PhotoSettings s_, committed_;
  std::vector<PhotoSettings> undo_, redo_;
  int tab_ = 0;   // 0 Recortar, 1 Color, 2 Marcar, 3 Marco
  int target_ = 0; // guardar como: 0 PNG, 1 JPG, 2 Para Discord

  // Marcar: herramienta, color y grosor de la siguiente marca; la elegida.
  PhotoMarkKind tool_ = PhotoMarkKind::Arrow;
  uint32_t color_ = 0xFF3B30;
  int size_ = 1;
  int selected_ = -1;
  std::string text_; // lo que se escribe para el siguiente texto

  // Vista previa: la última que llegó y con qué se hizo.
  std::shared_ptr<UiTexture> preview_;
  bool previewCrop_ = false;
  PhotoMarkBoxes boxes_;
  bool busy_ = false;
  PhotoSettings asked_;
  bool askedCrop_ = false;
  bool askedOnce_ = false;
  unsigned long long askedAt_ = 0;

  // Arrastrar sobre la imagen: 0 nada, 1 mover el recorte, 2..5 sus esquinas,
  // 6 dibujar uno nuevo; 10 crear una marca, 11 mover la elegida, 12 su punta
  // o su esquina, 13 su origen.
  int drag_ = 0;
  ImVec2 dragFrom_{};
  PhotoSettings dragStart_;
  float pressX_ = 0, pressY_ = 0; // fracción donde se pulsó
};

} // namespace spectre
