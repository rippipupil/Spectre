#pragma once

#include "app/Cloud.h"
#include "app/Config.h"
#include "app/Export.h"
#include "app/Fullscreen.h"
#include "app/Library.h"
#include "app/ShadowPlay.h"
#include "app/System.h"
#include "app/Updater.h"
#include "app/VideoEdit.h"
#include "core/Pipeline.h"
#include "ui/AppUi.h"
#include "ui/AppWindow.h"
#include "ui/KeyboardHook.h"
#include "ui/Menu.h"
#include "ui/OverlayWindow.h"
#include "ui/Toast.h"

#include <shellapi.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace spectre {

class App {
public:
  int Run(HINSTANCE instance, bool startInTray, bool updated);

private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  // Acciones
  // shortClip: solo los últimos Config::shortClipSeconds (atajo de clip corto).
  void SaveClip(bool shortClip = false);
  void ToggleRecording();
  void ToggleReplay();
  void ToggleMicrophone();
  void ToggleCamera();
  void SetClipSeconds(int seconds);
  void SetQuality(const std::string& quality);
  void CaptureScreenshot();
  void AddMarker();
  void TrimClip(const std::string& path, double start, double end);
  // Biblioteca: unir clips y cámara lenta (app/Edit.h), en otro hilo como el recorte.
  void JoinClips(const std::vector<std::string>& paths);
  void SlowMotion(const std::string& path, int factor);
  // Fotograma del visor guardado como captura (PNG o JPG, como Alt+F1) junto al vídeo.
  void SaveFrame(const MediaItem& item, std::vector<uint8_t> bgr, int width, int height);
  // Copia de menos de 20 MB en H.264 para Discord, en otro hilo; al acabar se
  // copia al portapapeles.
  void ExportForDiscord(const std::string& path);
  void FinishExport();
  // Selección de la biblioteca: «Para Discord» de varios, uno detrás de otro
  // (al acabar la tanda se copian todos), y varios a la papelera de una vez.
  void ExportManyForDiscord(const std::vector<std::string>& paths);
  void StartExport(const std::string& path);
  void StartQueuedExport();
  void FinishBatchExport(const ExportResult& r);
  void EndExportBatch();
  // Hay una exportación en marcha o por recoger, o una tanda esperando turno.
  bool ExportBusy() const { return exportRunning_ || exporter_.Busy() || !exportQueue_.empty(); }
  void RecycleMany(std::vector<std::string> paths);
  // «Editar» de la biblioteca: guarda una copia con los cambios en otro hilo
  // (<nombre>_editado_<fecha>.mp4, .gif o .m4a junto al original).
  void SaveEdit(const std::string& path, const EditSettings& settings);
  void FinishEdit();
  // Editor de fotos: copia editada de una captura (<nombre>_editado_<fecha>.png
  // o .jpg) en otro hilo; target 2 = para Discord, que al acabar se copia.
  void SavePhoto(const std::string& path, const PhotoSettings& settings, int target);
  // Dos copias sin recodificar: hasta ese segundo y desde él.
  void SplitClip(const std::string& path, double at);
  void ToggleMenu();
  void OpenMenu();
  // restoreFocus=false: el programa que se acaba de abrir desde el panel
  // (reproductor, Explorador, la ventana de Spectre) se queda con el foco en
  // vez del juego.
  void CloseMenu(bool restoreFocus = true);
  void EndMenuSession(bool giveFocusBack); // la parte de CloseMenu que no depende de la ventana
  void ApplyConfig(const Config& next, bool quiet = false);
  void Quit();
  // Hay una actualización descargada esperando a que acabe la grabación.
  void InstallPendingUpdate();
  // «Salir de Spectre» con subidas a la nube a medias: se pregunta antes.
  void QuitAsked();

  // Nube (app/Cloud.h). Compartir: si ya está subido copia el enlace; si no,
  // lo sube el primero de la cola y lo copia al acabar. Sin nube configurada
  // solo avisa (nunca abre la ventana: puede llamarse desde el panel).
  void ShareLink(const std::string& path);
  CloudFileState CloudState(const std::string& path) const { return cloud_.State(path); }
  bool CloudConfigured() const { return cloud_.Configured(); }
  void UploadToCloud(const std::vector<std::string>& paths);
  // Algo nuevo guardado: con la subida automática o el modo cibercafé, a la cola.
  // forPaste: además se copió al portapapeles (en el cibercafé se borra al salir, no al subirse).
  void AutoUpload(const std::string& path, MediaKind kind, bool forPaste = false);
  // Modo cibercafé: lo nuevo de la carpeta que aún no va a la nube (recortes,
  // uniones, lo del editor, copias para Discord...) se pone en la cola.
  void CyberSweep();
  // Modo cibercafé: se copió al portapapeles para pegarlo; se sube y se borra al salir.
  void KeepForPaste(const std::vector<std::string>& paths);
  // Modo cibercafé: guardado después de activarlo (lo de antes no es de este cliente).
  bool CyberOwned(const std::string& path) const;
  // Qué hacer con el archivo del PC al subirlo a mano.
  CloudDelete ManualRemoval(const std::string& path) const;
  // WM_SPECTRE_CLOUD: avisos de lo que terminó y estado para la ventana.
  void OnCloudChanged();
  void CopyCloudLink(const std::string& path);
  void PasteCloudSettings(bool cyber);
  // Ajustes > Almacenamiento > ShadowPlay: copia o mueve sus clips a la carpeta de Spectre.
  void ImportFromShadowPlay(bool move);
  void ChooseShadowPlayFolder();

  // Estado
  bool WantPipeline() const;
  void UpdatePipeline(bool forceRestart = false);
  bool StartPipeline();
  PipelineSettings BuildSettings() const;
  bool GameActive() const;
  // Hay un juego abierto o se graba: guardar una edición o exportar va sin
  // prisa. También lo apunta en liveLoad_, para los trabajos que ya van.
  bool UpdateLiveLoad();
  // Ruta nueva en la carpeta de destino (o en la del juego): Clip_<juego>_<fecha>.mp4
  std::string OutputName(const char* prefix, const char* extension = ".mp4");
  bool RegisterHotkeys();
  void UnregisterHotkeys();
  // Mientras se graba un atajo en Ajustes, los globales no se quedan con la combinación.
  void SuspendHotkeys(bool suspend);
  void CheckGame();
  // Límite de espacio de Ajustes > Almacenamiento, en un hilo aparte.
  void EnforceStorageLimit();
  // Avisa de lo que borró el límite, o de que no puede borrar nada.
  void ReportStorageLimit(const LimitResult& result);
  // " Queda poco espacio..." para añadir al aviso de un clip o una grabación (vacío si sobra).
  std::string LowDiskNote();
  // La grabación terminó (atajo, panel o captura perdida): marcas, biblioteca y
  // límite de espacio. stoppedAt: QPC de justo antes de pararla.
  void OnRecordingStopped(const std::string& path, int64_t stoppedAt);

  // UI
  // Aviso a partir de una frase: título, detalle y tipo salen del texto
  // (ui/Toast.h MakeNotice) salvo que se diga el tipo.
  void Toast(const std::string& text, bool error = false, NoticeKind kind = NoticeKind::Auto);
  // continuation: es lo mismo que se ve (se añade algo, «Guardando clip» pasa a
  // «Clip guardado»): la tarjeta no vuelve a entrar.
  void Notify(Notice notice, bool continuation = false);
  // Lo añade al aviso que se está viendo (como «Clip guardado») en vez de taparlo.
  void ToastAfter(const std::string& text, bool error = false);
  // Coloca la ventana del aviso en su esquina; heightPx = 0 la estima.
  void PlaceToast(float heightPx = 0);
  // Ajustes > Avisos > Probar: «Guardando clip» y, al rato, «Clip guardado».
  void PreviewToast();
  // «Replay listo en <juego>» la primera vez que se ve cada juego.
  void AnnounceGame();
  // Ajustes > Avisos > Versiones nuevas: una comprobación al abrir y cada 12 h.
  void CheckUpdatesQuietly();
  void SyncModels(); // el config activo cambió: panel y ventana lo muestran
  void RefreshMenuModel();
  void RefreshPanelClips();
  void AddTrayIcon();
  void RemoveTrayIcon();
  void ShowTrayMenu();
  void RenderOverlays();
  // Dónde se mueve el panel: el monitor (o su área de trabajo), el ancho del
  // panel y la separación de los bordes, en píxeles.
  struct MenuArea {
    RECT area{};
    int width = 0;
    int margin = 0;
  };
  MenuArea MenuPlacement() const;
  RECT MenuRect() const;
  void PlaceMenu(bool activate);
  // Arrastre del panel: la ventana sigue al cursor; al soltar se guarda la posición.
  void UpdateMenuDrag();
  void EndMenuDrag();
  // Si el panel coge el foco al abrirse (escritorio, pantalla completa
  // exclusiva, juego que encierra el cursor en parte del monitor) o no (juego
  // a pantalla completa con el cursor libre, aunque esté escondido: el teclado
  // llega entonces por KeyboardHook).
  bool ShouldActivateMenu(const FullscreenInfo& screen);
  void SyncKeyboardHook();
  std::vector<KeyboardHook::Hotkey> PassThroughHotkeys() const;
  void WatchGameWhileOpen();
  void PollClickOutside();
  // Las acciones del menú y de la ventana se ejecutan después del frame de
  // ImGui: cerrar o redimensionar una ventana (o crear la de un aviso) en
  // mitad del frame tumbaba la app.
  void Defer(std::function<void()> action) { deferred_.push_back(std::move(action)); }
  void RunDeferred();

  // Ventana de Spectre (la app). Solo existe mientras está abierta.
  void OpenApp(std::optional<AppPage> page = std::nullopt);
  void CloseApp();
  void AppCloseRequested(); // X o Alt+F4: a la bandeja o salir, según Ajustes > General
  void RenderApp();
  void RefreshAppModel();
  void RescanLibrary();
  void BeginSettingsEdit(); // copia de trabajo de Ajustes = config activo
  void ApplySettings();
  void LoadDevices();
  void ChooseOutputFolder();

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  UINT taskbarCreated_ = 0;
  NOTIFYICONDATAW tray_{};
  bool quitting_ = false;

  Config config_;
  Pipeline pipeline_;
  std::optional<ForegroundApp> game_;
  ULONGLONG lastGameSeen_ = 0;

  OverlayWindow menu_;
  OverlayWindow toast_;
  MenuModel menuModel_;
  MenuActions menuActions_;
  std::vector<std::function<void()>> deferred_;
  bool inFrame_ = false;        // dentro de un frame de ImGui (ver Toast)
  HWND previousForeground_ = nullptr;
  HMONITOR menuMonitor_ = nullptr;
  FullscreenInfo menuScreen_;   // lo que había debajo al abrir el panel (juego a pantalla completa, su monitor)
  UINT menuGeneration_ = 0;     // descarta avisos de cierre de un menú anterior
  ULONGLONG menuClosedAt_ = 0;
  ULONGLONG startFailedAt_ = 0; // la captura falló al arrancar: no reintentar en bucle
  DWORD pipelinePid_ = 0;       // proceso cuyo audio se captura (0 = todo el sistema)
  HWND pipelineWindow_ = nullptr; // ventana pedida al arrancar (nula = monitor)
  std::deque<std::string> recentNames_; // nombres ya usados aunque el archivo aún no exista
  bool restartPending_ = false; // cambios de captura a aplicar al terminar la grabación
  Notice toastNotice_;          // lo que dice el aviso visible
  HMONITOR toastMonitor_ = nullptr;
  ULONGLONG toastShownAt_ = 0;  // entrada de la tarjeta (animación)
  ULONGLONG toastPhaseAt_ = 0;  // último cambio de contenido (guardando -> guardado)
  ULONGLONG toastUntil_ = 0;
  ULONGLONG toastTotalMs_ = 1;
  int toastPlacedHeight_ = 0;   // alto medido con el que ya se recolocó
  // Clips pedidos que aún se escriben: duración y si es el corto, para el aviso.
  // Clips que se están escribiendo, por nombre de archivo: duración y si es
  // el corto. Pueden terminar en otro orden que el de las teclas.
  std::unordered_map<std::string, std::pair<int, bool>> pendingClips_;
  bool previewing_ = false; // «Probar aviso»: se ve aunque los avisos estén apagados
  DWORD announcedGamePid_ = 0;  // juego del último «Replay listo»
  ULONGLONG nextUpdateCheck_ = 0;
  bool quietCheckPending_ = false;
  std::string foundVersion_;     // versión nueva que encontró la comprobación automática
  std::string announcedVersion_; // versión nueva ya avisada
  ULONGLONG lastStats_ = 0;
  std::string hotkeyError_;     // atajos que no se pudieron registrar
  unsigned hotkeyFailedMask_ = 0; // bit i: el atajo i de HotkeyConfig no se pudo registrar
  bool hotkeysSuspended_ = false;
  Updater updater_;
  std::wstring installerPath_; // actualización descargada: se lanza al terminar de cerrar
  bool relaunchAfterUpdate_ = false; // la salida la pidió la actualización, no el usuario

  // Marcas (atajo de marca): momentos de la partida en QPC. Al guardar un clip
  // entran las de sus últimos segundos; las de una grabación, al detenerla.
  std::deque<int64_t> markers_;
  int64_t recordStartQpc_ = 0;
  std::vector<int64_t> recordMarkers_; // QPC de las marcas de la grabación en curso
  ULONGLONG lowDiskWarnedAt_ = 0;
  std::string limitWarned_; // el último «el límite no puede borrar nada» avisado

  // Panel sobre el juego. Abierto sin foco (juego a pantalla completa), el
  // teclado llega por el gancho; con foco, el gancho no se instala.
  KeyboardHook keyboard_;
  bool menuActivated_ = false;    // el panel cogió el foco al abrirse; se repite al recrearlo
  ULONGLONG menuOpenedAt_ = 0;
  bool menuFocusChecked_ = false; // ya se comprobó que el foco llegó al panel tras abrirlo
  bool gameRestored_ = false;     // el juego minimizado al perder el foco ya se restauró
  bool mouseWasDown_ = false;     // sondeo del clic fuera del panel
  RECT menuHitScreen_{};          // zona del panel que recibe clics, en pantalla
  bool menuHitValid_ = false;
  bool menuDragActive_ = false;   // arrastrando el panel (UpdateMenuDrag)
  POINT menuDragFrom_{};          // cursor al pulsar, en pantalla
  LONG menuDragLeft_ = 0;         // borde izquierdo de la ventana al pulsar
  LONG menuDragTop_ = 0;          // borde superior del panel al pulsar, en pantalla

  // Ventana de Spectre
  std::shared_ptr<Library> library_ = std::make_shared<Library>(); // también la usa el hilo del límite de espacio
  AppWindow appWindow_;
  AppUi appUi_;
  AppModel appModel_;
  AppActions appActions_;
  ULONGLONG lastAppStats_ = 0;
  ULONGLONG lastAppUsage_ = 0;
  ULONGLONG lastAppFrame_ = 0;
  Exporter exporter_;
  EditWorker editWorker_;
  // Lo que da UpdateLiveLoad(), para el editor y Exportar mientras trabajan
  // (con un juego o una grabación van como mucho a tiempo real).
  std::shared_ptr<std::atomic<bool>> liveLoad_ = std::make_shared<std::atomic<bool>>(false);
  // Edición guardada «Para Discord»: se exporta después y, si sale una copia
  // aparte, esta (la intermedia) se borra.
  std::string discordAfterEdit_;
  // «Para Discord» de varios: los que esperan turno, el de la tanda que se
  // está exportando, las copias ya hechas y lo que falló.
  std::deque<std::string> exportQueue_;
  // Desde StartExport hasta recoger el resultado: el hilo de Exporter deja de
  // estar ocupado un poco antes de que llegue el aviso de que acabó.
  bool exportRunning_ = false;
  bool exportBatchCancelled_ = false;
  std::string exportBatchInput_;
  std::vector<std::string> exportBatchOutputs_;
  int exportBatchTotal_ = 0;
  int exportBatchFailed_ = 0;
  std::string exportBatchError_;

  // Nube y ShadowPlay
  CloudUploader cloud_;
  uint64_t cloudVersion_ = 0; // la de appModel_.cloud
  uint64_t cyberSince_ = 0;   // FILETIME de cuando se activó el modo cibercafé (0 = apagado)
  std::unordered_set<std::string> cyberCancelled_; // en minúsculas: canceladas a mano en esta sesión
  std::shared_ptr<std::atomic<bool>> importCancel_;
  std::shared_ptr<std::atomic<int>> importDone_ = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<std::atomic<int>> importTotal_ = std::make_shared<std::atomic<int>>(0);
  bool importing_ = false;
};

} // namespace spectre
