#pragma once

// Interfaz de la ventana de Spectre: Inicio, Biblioteca (con visor y
// reproductor) y todos los Ajustes. App le pasa el estado en AppModel y
// ejecuta las acciones de AppActions después del frame.

#include "app/Config.h"
#include "app/Library.h"
#include "app/PhotoEdit.h"
#include "app/VideoEdit.h"
#include "core/Pipeline.h"
#include "ui/AppWindow.h"
#include "ui/EditorUi.h"
#include "ui/PhotoEditorUi.h"
#include "ui/Player.h"
#include "ui/SettingsPages.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spectre {

enum class AppPage { Home, Library, Viewer, Editor, Settings };

struct AppModel {
  Config config;          // configuración activa (Inicio, acciones rápidas)
  SettingsModel settings; // copia de trabajo de Ajustes
  PipelineStats stats;
  bool replayActive = false;
  bool gameDetected = false;
  std::string gameName;
  std::string outputDir;
  std::vector<MediaItem> items; // biblioteca, del más reciente al más antiguo
  bool itemsLoaded = false;
  uint64_t freeBytes = 0;       // libre en el disco de la carpeta de destino

  AppPage page = AppPage::Home;
  SettingsPage settingsPage = SettingsPage::Replay;
  std::string viewPath; // archivo abierto en el visor
  AppPage viewBack = AppPage::Library;
  // Exportación para Discord en marcha (una cada vez): archivo y porcentaje; -1 = ninguna.
  std::string exportPath;
  int exportPercent = -1;
  // «Para Discord» de varios: los que tiene la tanda y los que ya acabaron (0 = ninguna).
  int exportBatchTotal = 0;
  int exportBatchDone = 0;
  std::string editPath; // archivo abierto en «Editar» (al volver, el visor lo enseña)
  // Edición guardándose (una cada vez): archivo y porcentaje; -1 = ninguna.
  std::string editRenderPath;
  int editPercent = -1;
  std::string editSaved, editSavedFrom; // la última copia guardada y de qué archivo salió
  std::string photoSavePath;            // captura cuya copia editada se está guardando
  // Nube: estado de cada archivo de la biblioteca por ruta (el de la cola
  // entera está en settings.cloud).
  std::unordered_map<std::string, CloudFileState> cloud;
};

struct AppActions {
  std::function<void()> saveClip;
  std::function<void()> toggleRecording;
  std::function<void()> screenshot;
  std::function<void()> toggleReplay;
  std::function<void()> toggleMic;
  std::function<void()> toggleCamera;
  std::function<void(int)> setClipSeconds;
  std::function<void()> apply;   // aplica model.settings.config
  std::function<void()> discard; // descarta la copia de trabajo
  std::function<void()> loadDevices;
  std::function<void()> rescan;
  std::function<void(const std::string&)> openPath;
  std::function<void(const std::string&)> showInFolder;
  std::function<void(const std::string&, double, double)> trim;
  // Une los vídeos en ese orden (del más antiguo al más reciente) sin recodificar.
  std::function<void(const std::vector<std::string>&)> joinClips;
  // Copia a 1/factor de velocidad (2 o 4), sin audio.
  std::function<void(const std::string&, int)> slowMotion;
  // El fotograma del visor (BGR de arriba abajo) como captura junto al vídeo.
  std::function<void(const MediaItem&, std::vector<uint8_t>, int, int)> saveFrame;
  std::function<void(const std::string&)> copyToClipboard;
  std::function<void(const std::string&)> exportForDiscord;
  std::function<void()> cancelExport;
  // «Editar»: guarda una copia con esos cambios (en otro hilo) o cancela la que se está guardando.
  std::function<void(const std::string&, const EditSettings&)> saveEdit;
  std::function<void()> cancelEdit;
  // Editor de fotos: guarda una copia de la captura con esos cambios (en otro
  // hilo). target: 0 PNG, 1 JPG, 2 para Discord (JPG copiado al portapapeles).
  std::function<void(const std::string&, const PhotoSettings&, int)> savePhoto;
  // Parte el vídeo en dos copias sin recodificar: hasta ese segundo y desde él.
  std::function<void(const std::string&, double)> split;
  std::function<void(const std::string&, bool)> setFavorite;
  std::function<void(const MediaItem&, double)> setDuration;
  std::function<bool(const std::string&, const std::string&, std::string*, std::string*)> rename;
  // A la papelera después del frame (el Explorador puede tardar); un error llega como aviso.
  std::function<void(const std::string&)> recycle;
  // Selección de la biblioteca: varios a la papelera en una sola operación,
  // favoritos de una vez, «Para Discord» uno detrás de otro y copiar varios.
  std::function<void(const std::vector<std::string>&)> recycleMany;
  std::function<void(const std::vector<std::string>&, bool)> setFavorites;
  std::function<void(const std::vector<std::string>&)> exportManyForDiscord;
  std::function<void(const std::vector<std::string>&)> copyManyToClipboard;
  std::function<void(const std::string&, bool)> toast;
  // Nube: varios a la cola, compartir uno (subir y copiar el enlace), cancelar
  // y reintentar una subida, pausar todas.
  std::function<void(const std::vector<std::string>&)> uploadToCloud;
  std::function<void(const std::string&)> shareLink;
  std::function<void(const std::string&)> cancelUpload;
  std::function<void(const std::string&)> retryUpload;
  SettingsActions settings;
};

class AppUi {
public:
  void Draw(AppWindow& window, AppModel& model, const AppActions& actions);
  // La ventana se abrió (hilo de miniaturas) o se cerró (se suelta todo:
  // texturas, reproductor, hilo).
  void OnOpened(HWND notifyWindow, UINT notifyMsg);
  void OnClosed();
  // Llegaron miniaturas, duraciones o imágenes del hilo de la biblioteca.
  void TakeMediaResults(AppWindow& window, AppModel& model, const AppActions& actions);
  // Abre el visor con ese archivo (desde el panel o la biblioteca).
  void View(AppModel& model, const std::string& path, AppPage back);
  void PauseMedia();
  bool MediaPlaying() const { return player_.Playing() || editor_.Playing(); }

private:
  struct Thumb {
    std::shared_ptr<UiTexture> texture;
    bool pending = false;
    bool failed = false;
    unsigned long long lastUse = 0;
    int64_t modified = 0; // fecha del archivo de la miniatura
    int requested = 0;    // ancho pedido al hilo de la biblioteca (px)
  };

  // Navegación según el diseño (ui/Theme.h NavLayout).
  void Sidebar(AppWindow& w, AppModel& m, const AppActions& a);
  void NavRail(AppModel& m);
  void NavTopbar(AppModel& m);
  void SettingsNav(AppModel& m);
  void PageHome(AppWindow& w, AppModel& m, const AppActions& a);
  void PageLibrary(AppWindow& w, AppModel& m, const AppActions& a);
  void PageViewer(AppWindow& w, AppModel& m, const AppActions& a);
  void PageEditor(AppWindow& w, AppModel& m, const AppActions& a);
  // Pestaña Editor sin nada abierto: elegir un vídeo o una captura.
  void EditorPicker(AppWindow& w, AppModel& m);
  void LeaveEditors();
  // Abre «Editar» con ese vídeo o esa captura; Volver lleva a su visor (o a
  // la pestaña Editor si se abrió desde ella).
  void Edit(AppModel& m, const std::string& path);
  void PageSettings(AppWindow& w, AppModel& m, const AppActions& a);
  void StatusCard(AppModel& m, const AppActions& a);
  // selection: -1 sin casilla (Inicio), 0 o 1 en la biblioteca (elegido o no).
  // number: su posición, que Táctico escribe en el pie (#01).
  bool Card(AppWindow& w, const MediaItem& item, float width, bool* contextMenu, int selection = -1, int number = 0,
            const CloudFileState* cloud = nullptr);
  void StatsSection(const AppModel& m);
  void SaveCurrentFrame(const MediaItem& item, const AppActions& a);
  // Selección de la biblioteca: casillas, Ctrl+clic, Mayús+clic (un tramo) y «Seleccionar».
  bool Selected(const std::string& path) const;
  void ToggleSelected(const std::string& path);
  bool Selecting() const { return selectMode_ || !selected_.empty(); }
  void ClickSelect(const std::vector<const MediaItem*>& list, size_t index, bool range);
  void EndSelection();
  std::vector<const MediaItem*> SelectedItems(const AppModel& m) const;
  void SelectionBar(AppModel& m, const AppActions& a, const std::vector<const MediaItem*>& list);
  void SelectionMenu(AppModel& m, const AppActions& a);
  // Lo mismo desde la barra, el menú contextual y las teclas.
  void FavoriteSelection(AppModel& m, const AppActions& a);
  void ExportSelection(AppModel& m, const AppActions& a);
  void JoinSelection(AppModel& m, const AppActions& a);
  void CopySelection(AppModel& m, const AppActions& a);
  void AskDeleteSelection(AppModel& m);
  void ItemMenu(AppModel& m, const AppActions& a);
  void CloudMenuItems(AppModel& m, const AppActions& a, const std::string& path);
  void CloudLine(AppModel& m, const AppActions& a);
  void OpenCloudSettings(AppModel& m);
  void Dialogs(AppModel& m, const AppActions& a);
  void Timeline(AppWindow& w, const MediaItem& item, float width);
  // Miniatura de un archivo; width = ancho en píxeles al que se va a dibujar
  // (0 = la que haya). Si la que hay es más pequeña se pide otra más grande.
  UiTexture* ThumbFor(const MediaItem& item, float width = 0);
  void RequestDuration(const MediaItem& item);
  std::vector<const MediaItem*> Filtered(const AppModel& m) const;
  const MediaItem* Find(const AppModel& m, const std::string& path) const;
  void LeaveViewer();
  void Navigate(AppModel& m, AppPage page);

  MediaWorker worker_;
  std::unordered_map<std::string, Thumb> thumbs_;
  std::unordered_map<std::string, int64_t> durationsAsked_; // ruta -> fecha del archivo medido
  std::unordered_set<std::string> durationFailed_;          // no se pudo medir (archivo dañado)
  unsigned long long frame_ = 0;
  unsigned long long openedAt_ = 0;
  // Entrada de la página con la animación del diseño (Widgets EnterMotion).
  unsigned long long pageSince_ = 0;
  int shownPage_ = -1;
  Player player_;
  EditorUi editor_; // con su propio reproductor: el del visor se cierra al entrar
  PhotoEditorUi photo_;  // capturas
  int pickFilter_ = 0;   // Editor: 0 vídeos, 1 capturas
  std::string pickSearch_;
  std::string lastEdit_; // lo último abierto en un editor, para seguir con ello
  float volume_ = 1.0f;
  bool muted_ = false;
  std::string playerPath_;
  bool playerFailed_ = false;
  std::shared_ptr<UiTexture> image_; // captura abierta en el visor
  std::string imagePath_;
  bool imagePending_ = false;
  bool imageFailed_ = false;
  bool scrubbing_ = false;
  bool wasPlaying_ = false;
  bool trimMode_ = false;
  double trimStart_ = 0, trimEnd_ = 0;
  int dragHandle_ = 0; // 1 inicio, 2 fin

  // Biblioteca
  int filter_ = 0; // 0 todo, 1 clips, 2 grabaciones, 3 capturas, 4 favoritos
  std::string game_;
  std::string search_;
  int sort_ = 0; // 0 recientes, 1 antiguos, 2 más grandes, 3 más largos
  std::string menuPath_; // menú contextual abierto sobre este archivo
  std::string renamePath_, renameText_, renameError_;
  bool openRename_ = false;
  std::string deletePath_;
  bool openDelete_ = false;
  bool deleteHasBin_ = true; // la unidad del archivo tiene papelera
  std::string starToggle_; // estrella pulsada en una tarjeta este frame
  // Elegidos en la biblioteca (casilla, Ctrl+clic, Mayús+clic, «Seleccionar» o
  // «Unir con otros...»): se unen, se marcan como favoritos, se exportan para
  // Discord, se copian o van a la papelera a la vez. Un conjunto: con miles
  // elegidos (Ctrl+A) cada frame pregunta por cada tarjeta.
  std::unordered_set<std::string> selected_;
  std::string selectToggle_; // casilla pulsada en una tarjeta este frame
  std::string selectAnchor_; // el último pulsado: Mayús+clic elige desde ahí hasta el nuevo
  bool selectMode_ = false;  // «Seleccionar»: el clic elige en vez de abrir
  // «Mover a la papelera» de varios: los archivos, si se dejan fuera los
  // favoritos y cuántos están en una unidad sin papelera.
  std::vector<std::string> bulkDelete_;
  bool openBulkDelete_ = false;
  bool bulkKeepFavorites_ = false;
  size_t bulkNoBin_ = 0;
  unsigned randomSeed_ = 0;  // «Al azar»: no repetir el mismo vídeo dos veces seguidas
};

} // namespace spectre
