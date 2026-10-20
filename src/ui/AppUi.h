#pragma once

// Interfaz de la ventana de Spectre: Inicio, Biblioteca (con visor y
// reproductor) y todos los Ajustes. App le pasa el estado en AppModel y
// ejecuta las acciones de AppActions después del frame.

#include "app/Config.h"
#include "app/Library.h"
#include "core/Pipeline.h"
#include "ui/AppWindow.h"
#include "ui/Player.h"
#include "ui/SettingsPages.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spectre {

enum class AppPage { Home, Library, Viewer, Settings };

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
  std::function<void(const std::string&)> copyToClipboard;
  std::function<void(const std::string&, bool)> setFavorite;
  std::function<void(const MediaItem&, double)> setDuration;
  std::function<bool(const std::string&, const std::string&, std::string*, std::string*)> rename;
  // A la papelera después del frame (el Explorador puede tardar); un error llega como aviso.
  std::function<void(const std::string&)> recycle;
  std::function<void(const std::string&, bool)> toast;
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
  bool MediaPlaying() const { return player_.Playing(); }

private:
  struct Thumb {
    std::shared_ptr<UiTexture> texture;
    bool pending = false;
    bool failed = false;
    unsigned long long lastUse = 0;
    int64_t modified = 0; // fecha del archivo de la miniatura
  };

  void Sidebar(AppWindow& w, AppModel& m, const AppActions& a);
  void PageHome(AppWindow& w, AppModel& m, const AppActions& a);
  void PageLibrary(AppWindow& w, AppModel& m, const AppActions& a);
  void PageViewer(AppWindow& w, AppModel& m, const AppActions& a);
  void PageSettings(AppWindow& w, AppModel& m, const AppActions& a);
  void StatusCard(AppModel& m, const AppActions& a);
  bool Card(AppWindow& w, const MediaItem& item, float width, bool* contextMenu);
  void ItemMenu(AppModel& m, const AppActions& a);
  void Dialogs(AppModel& m, const AppActions& a);
  void Timeline(AppWindow& w, const MediaItem& item, float width);
  UiTexture* ThumbFor(const MediaItem& item);
  void RequestDuration(const MediaItem& item);
  std::vector<const MediaItem*> Filtered(const AppModel& m) const;
  const MediaItem* Find(const AppModel& m, const std::string& path) const;
  void LeaveViewer();
  void Navigate(AppModel& m, AppPage page);

  MediaWorker worker_;
  std::unordered_map<std::string, Thumb> thumbs_;
  std::unordered_map<std::string, int64_t> durationsAsked_; // ruta -> fecha del archivo medido
  unsigned long long frame_ = 0;
  unsigned long long openedAt_ = 0;
  Player player_;
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
};

} // namespace spectre
