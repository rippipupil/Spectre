#include "app/App.h"
#include "app/CrashHandler.h"
#include "app/Edit.h"
#include "app/Screenshot.h"
#include "app/Sound.h"
#include "app/TextImage.h"

#include "common/Util.h"
#include "core/Mp4Writer.h"
#include "core/ThreadPolicy.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <shellscalingapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>

#ifndef SPECTRE_VERSION
#define SPECTRE_VERSION "0.1.0"
#endif

namespace spectre {

namespace {
constexpr wchar_t kMainClass[] = L"SpectreMain";

// En el mismo orden que HotkeyText.
enum HotkeyId {
  kHotkeyOverlay = 1, kHotkeySaveClip, kHotkeyRecord, kHotkeyReplay, kHotkeyScreenshot, kHotkeyMarker, kHotkeyShortClip
};
constexpr int kHotkeyCount = 7;
enum TimerId { kTimerGame = 1, kTimerHealth, kTimerPreview };
enum TrayCommand {
  kCmdOpenApp = 100,
  kCmdOpenPanel,
  kCmdSaveClip,
  kCmdShortClip,
  kCmdRecord,
  kCmdScreenshot,
  kCmdReplay,
  kCmdFolder,
  kCmdQuit = 199,
};

constexpr ULONGLONG kGameGraceMs = 15000; // el juego sigue "activo" 15 s después de perder el foco
constexpr ULONGLONG kToastMs = 2600;
constexpr size_t kPanelClips = 8; // el compacto enseña 3; el lateral, los que quepan
constexpr double kMaxMarkerAge = kMaxClipSeconds + 10; // segundos: el clip más largo y un margen
constexpr uint64_t kLowDiskBytes = uint64_t(1) << 30; // menos de 1 GB libre: se avisa al guardar

const std::string& HotkeyText(const HotkeyConfig& h, int index) {
  switch (index) {
  case 0: return h.overlay;
  case 1: return h.saveClip;
  case 2: return h.toggleRecording;
  case 3: return h.toggleReplay;
  case 4: return h.screenshot;
  case 5: return h.marker;
  default: return h.shortClip;
  }
}

std::string& HotkeyText(HotkeyConfig& h, int index) {
  return const_cast<std::string&>(HotkeyText(std::as_const(h), index));
}

bool SameHotkeys(const HotkeyConfig& a, const HotkeyConfig& b) {
  for (int i = 0; i < kHotkeyCount; ++i)
    if (HotkeyText(a, i) != HotkeyText(b, i)) return false;
  return true;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

const char* QualityName(const std::string& quality) {
  if (quality == "baja") return "Baja";
  if (quality == "media") return "Media";
  if (quality == "maxima") return "Máxima";
  if (IsSourceQuality(quality)) return "Fuente";
  if (quality == "personalizada") return "Personalizada";
  return "Alta";
}

// «45 min», «1 h», «1 h 30 min»
std::string MinutesText(int minutes) {
  if (minutes < 60) return std::to_string(minutes) + " min";
  std::string text = std::to_string(minutes / 60) + " h";
  if (minutes % 60) text += " " + std::to_string(minutes % 60) + " min";
  return text;
}

std::string FileNameOf(const std::string& path) { return Narrow(std::filesystem::path(Widen(path)).filename().wstring()); }

// «1 archivo», «12 archivos»
std::string CountText(size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

float MonitorScale(HMONITOR monitor) {
  UINT dpiX = 96, dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = GetDpiForSystem();
  return float(dpiX) / 96.0f;
}

bool AnyMouseButtonDown() {
  for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
    if (GetAsyncKeyState(vk) & 0x8000) return true;
  return false;
}

bool SameCaptureSettings(const Config& a, const Config& b) {
  return a.captureMode == b.captureMode && a.monitorIndex == b.monitorIndex && a.captureApi == b.captureApi &&
         a.captureCursor == b.captureCursor && a.fps == b.fps && a.outputHeight == b.outputHeight &&
         a.quality == b.quality && a.bitrateKbps == b.bitrateKbps && a.codec == b.codec && a.encoder == b.encoder &&
         a.gpuPriority == b.gpuPriority && a.captureSystemAudio == b.captureSystemAudio &&
         a.outputDeviceId == b.outputDeviceId && a.gameAudioOnly == b.gameAudioOnly && a.captureMic == b.captureMic &&
         a.micDeviceId == b.micDeviceId && a.systemVolume == b.systemVolume && a.micVolume == b.micVolume &&
         a.separateTracks == b.separateTracks && a.maxBufferMB == b.maxBufferMB;
}

// La webcam no está aquí arriba: se cambia con la captura en marcha
// (Pipeline::SetCamera), sin vaciar el replay ni esperar a que abra.
bool SameCameraSettings(const Config& a, const Config& b) {
  return a.cameraEnabled == b.cameraEnabled && a.cameraId == b.cameraId && a.cameraCorner == b.cameraCorner &&
         a.cameraSizePercent == b.cameraSizePercent;
}
} // namespace

int App::Run(HINSTANCE instance, bool startInTray, bool updated) {
  instance_ = instance;
  config_ = LoadConfig();
  // El instalador puede haber activado el inicio con Windows: el registro manda.
  if (config_.autostart != AutostartEnabled()) {
    config_.autostart = AutostartEnabled();
    SaveConfig(config_);
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kMainClass;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(0, kMainClass, L"Spectre", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
  if (!hwnd_) return 1;
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  AddTrayIcon();

  menu_.Init(instance, OverlayWindow::Kind::Menu);
  toast_.Init(instance, OverlayWindow::Kind::Toast);
  appWindow_.Init(instance);
  // Alt+F4 cierra el menú, siempre a través de la cola de mensajes (nunca
  // dentro del WndProc ni del frame de la ventana). Perder el foco no lo
  // cierra: el panel se queda abierto hasta que el usuario lo cierra con
  // Alt+Z, Esc, el botón o un clic fuera (PollClickOutside).
  menu_.onCloseRequested = [this] { PostMessageW(hwnd_, WM_SPECTRE_CLOSE_MENU, menuGeneration_, 0); };
  keyboard_.onCloseRequested = menu_.onCloseRequested;

  // Panel (Alt+Z)
  menuActions_.saveClip = [this] { Defer([this] { SaveClip(); }); };
  menuActions_.toggleRecording = [this] { Defer([this] { ToggleRecording(); }); };
  menuActions_.screenshot = [this] { Defer([this] { CaptureScreenshot(); }); };
  menuActions_.toggleReplay = [this] { Defer([this] { ToggleReplay(); }); };
  menuActions_.toggleMic = [this] { Defer([this] { ToggleMicrophone(); }); };
  menuActions_.toggleCamera = [this] { Defer([this] { ToggleCamera(); }); };
  menuActions_.setClipSeconds = [this](int seconds) { Defer([this, seconds] { SetClipSeconds(seconds); }); };
  menuActions_.setQuality = [this](const std::string& quality) { Defer([this, quality] { SetQuality(quality); }); };
  // La ventana de Spectre se abre delante: el panel se cierra sin devolver el
  // foco al juego (si no, la ventana se quedaría detrás).
  menuActions_.openApp = [this] {
    Defer([this] {
      CloseMenu(false);
      OpenApp();
    });
  };
  menuActions_.openLibrary = [this] {
    Defer([this] {
      CloseMenu(false);
      OpenApp(AppPage::Library);
    });
  };
  menuActions_.openClip = [this](const std::string& path) {
    Defer([this, path] {
      CloseMenu(false);
      OpenApp();
      if (appWindow_.Visible()) appUi_.View(appModel_, path, AppPage::Library);
    });
  };
  menuActions_.close = [this] { Defer([this] { CloseMenu(); }); };
  // Abrir la carpeta lleva a otra ventana: el panel se cierra, como hacía al
  // perder el foco (si no, se quedaría encima del Explorador). Sin devolver el
  // foco al juego: ShellExecute vuelve antes de que exista la ventana nueva y
  // esta se abriría detrás.
  menuActions_.openPath = [this](const std::string& path) {
    Defer([this, path] {
      OpenPath(path);
      CloseMenu(false);
    });
  };
  menuActions_.showInFolder = [this](const std::string& path) {
    Defer([this, path] {
      ShowInFolder(path);
      CloseMenu(false);
    });
  };

  // Ventana de Spectre
  appWindow_.onCloseRequested = [this] { Defer([this] { AppCloseRequested(); }); };
  appWindow_.onModalFrame = [this] { RenderApp(); };
  appWindow_.onMinimized = [this] { appUi_.PauseMedia(); };
  appActions_.saveClip = menuActions_.saveClip;
  appActions_.toggleRecording = menuActions_.toggleRecording;
  appActions_.screenshot = menuActions_.screenshot;
  appActions_.toggleReplay = menuActions_.toggleReplay;
  appActions_.toggleMic = menuActions_.toggleMic;
  appActions_.toggleCamera = menuActions_.toggleCamera;
  appActions_.setClipSeconds = menuActions_.setClipSeconds;
  appActions_.apply = [this] { Defer([this] { ApplySettings(); }); };
  appActions_.discard = [this] { Defer([this] { BeginSettingsEdit(); }); };
  appActions_.loadDevices = [this] { LoadDevices(); };
  appActions_.rescan = [this] { Defer([this] { RescanLibrary(); }); };
  appActions_.openPath = [this](const std::string& path) { Defer([path] { OpenPath(path); }); };
  appActions_.showInFolder = [this](const std::string& path) { Defer([path] { ShowInFolder(path); }); };
  appActions_.trim = [this](const std::string& path, double start, double end) {
    Defer([this, path, start, end] { TrimClip(path, start, end); });
  };
  appActions_.copyToClipboard = [this](const std::string& path) {
    Defer([this, path] {
      if (CopyFileToClipboard(appWindow_.Visible() ? appWindow_.Hwnd() : hwnd_, path))
        Toast("Copiado. Pégalo con Ctrl+V en Discord, WhatsApp o una carpeta");
      else
        Toast("No se pudo copiar el archivo al portapapeles", true);
    });
  };
  appActions_.exportForDiscord = [this](const std::string& path) { Defer([this, path] { ExportForDiscord(path); }); };
  appActions_.joinClips = [this](const std::vector<std::string>& paths) { Defer([this, paths] { JoinClips(paths); }); };
  appActions_.slowMotion = [this](const std::string& path, int factor) {
    Defer([this, path, factor] { SlowMotion(path, factor); });
  };
  appActions_.saveFrame = [this](const MediaItem& item, std::vector<uint8_t> bgr, int width, int height) {
    SaveFrame(item, std::move(bgr), width, height); // solo lanza el hilo: puede ir dentro del frame
  };
  appActions_.cancelExport = [this] {
    exporter_.Cancel();
    exportQueue_.clear(); // con varios en cola se cancela la tanda entera
  };
  appActions_.saveEdit = [this](const std::string& path, const EditSettings& settings) {
    Defer([this, path, settings] { SaveEdit(path, settings); });
  };
  appActions_.cancelEdit = [this] { editWorker_.Cancel(); };
  appActions_.split = [this](const std::string& path, double at) { Defer([this, path, at] { SplitClip(path, at); }); };
  appActions_.setFavorite = [this](const std::string& path, bool favorite) {
    library_->SetFavorite(path, favorite);
    for (auto& item : appModel_.items)
      if (item.path == path) item.favorite = favorite;
    appWindow_.Invalidate();
  };
  appActions_.setDuration = [this](const MediaItem& item, double seconds) {
    library_->SetDuration(item.path, item.size, item.modified, seconds);
    // Con la duración ya se pueden colocar las marcas de un clip (se guardaron contando desde el final).
    for (auto& it : appModel_.items)
      if (it.path == item.path) library_->Annotate(it);
  };
  appActions_.rename = [this](const std::string& path, const std::string& stem, std::string* newPath,
                              std::string* error) {
    if (!library_->Rename(path, stem, newPath, error)) return false;
    RescanLibrary();
    return true;
  };
  appActions_.recycle = [this](const std::string& path) {
    Defer([this, path] {
      std::string error;
      bool cancelled = false;
      bool bin = HasRecycleBin(path);
      if (library_->Recycle(appWindow_.Visible() ? appWindow_.Hwnd() : hwnd_, path, &error, &cancelled))
        Toast((bin ? "Movido a la papelera: " : "Borrado: ") + FileNameOf(path));
      else if (!cancelled)
        Toast(error, true);
      RescanLibrary();
    });
  };
  appActions_.recycleMany = [this](const std::vector<std::string>& paths) {
    Defer([this, paths] { RecycleMany(paths); });
  };
  appActions_.setFavorites = [this](const std::vector<std::string>& paths, bool favorite) {
    library_->SetFavorite(paths, favorite);
    for (auto& item : appModel_.items)
      if (std::find(paths.begin(), paths.end(), item.path) != paths.end()) item.favorite = favorite;
    appWindow_.Invalidate();
  };
  appActions_.exportManyForDiscord = [this](const std::vector<std::string>& paths) {
    Defer([this, paths] { ExportManyForDiscord(paths); });
  };
  appActions_.copyManyToClipboard = [this](const std::vector<std::string>& paths) {
    Defer([this, paths] {
      if (CopyFilesToClipboard(appWindow_.Visible() ? appWindow_.Hwnd() : hwnd_, paths))
        Toast(CountText(paths.size(), "archivo copiado", "archivos copiados") +
                  (paths.size() > 10 ? ": pégalos con Ctrl+V en una carpeta (Discord acepta 10 por mensaje)"
                                     : ": pégalos con Ctrl+V en Discord, WhatsApp o una carpeta"),
              false, NoticeKind::Copied);
      else
        Toast("No se pudieron copiar los archivos al portapapeles", true);
    });
  };
  appActions_.toast = [this](const std::string& text, bool error) { Toast(text, error); };
  SettingsActions& settings = appActions_.settings;
  settings.checkUpdates = [this] { updater_.Check(); };
  settings.installUpdate = [this] { updater_.Download(hwnd_); };
  settings.cancelUpdate = [this] { updater_.Cancel(); };
  settings.quit = [this] { Defer([this] { Quit(); }); };
  settings.openPath = appActions_.openPath;
  settings.suspendHotkeys = [this](bool suspend) { SuspendHotkeys(suspend); };
  settings.chooseFolder = [this] { Defer([this] { ChooseOutputFolder(); }); };
  settings.refreshStorage = appActions_.rescan;
  settings.playSound = [](const std::string& style) { PlayCue(Cue::Saved, style); };
  settings.previewNotice = [this] { Defer([this] { PreviewToast(); }); };

  if (!RegisterHotkeys()) Toast(hotkeyError_, true);
  SetTimer(hwnd_, kTimerGame, 2000, nullptr);
  SetTimer(hwnd_, kTimerHealth, 1000, nullptr);
  CheckGame();
  UpdatePipeline();
  EnforceStorageLimit();
  if (updated)
    Notify({NoticeKind::Update, "Spectre actualizado", "Ya tienes la versión " SPECTRE_VERSION ". Tus ajustes se conservan"});
  // La primera comprobación de versiones, un minuto después de arrancar: no
  // compite con el arranque de la captura.
  nextUpdateCheck_ = GetTickCount64() + 60 * 1000;
  // Abrir Spectre (menú Inicio, el exe, el instalador) abre su ventana. Con
  // Windows (--tray) arranca solo en la bandeja, y tras una actualización
  // tampoco se abre encima de un juego a pantalla completa.
  if (!startInTray && !(updated && GameActive())) OpenApp();

  // Bucle de mensajes: bloqueante cuando no hay nada en pantalla (0 % CPU).
  // El menú y los avisos van a ~60 fps mientras se ven; la ventana de Spectre
  // solo dibuja cuando algo cambia (ratón, teclado, un vídeo, un reloj), como
  // mucho a ~60 fps, y nunca minimizada.
  MSG msg;
  ULONGLONG lastFrame = 0;
  while (!quitting_) {
    ULONGLONG now = GetTickCount64();
    DWORD wait = INFINITE;
    if (menu_.Visible() || toast_.Visible()) wait = now - lastFrame >= 16 ? 0 : DWORD(16 - (now - lastFrame));
    if (appWindow_.Visible()) {
      DWORD appWait = appWindow_.WaitMs(now);
      if (appWait != INFINITE) {
        ULONGLONG since = now - lastAppFrame_;
        appWait = std::max<DWORD>(appWait, since >= 16 ? 0 : DWORD(16 - since));
      }
      wait = std::min(wait, appWait);
    }
    if (!deferred_.empty()) wait = 0;
    if (wait == INFINITE) {
      if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    } else if (wait > 0) {
      MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) quitting_ = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (quitting_) break;
    now = GetTickCount64();
    if ((menu_.Visible() || toast_.Visible()) && now - lastFrame >= 16) {
      lastFrame = now;
      RenderOverlays();
    }
    if (appWindow_.Visible() && appWindow_.NeedsFrame(now) && now - lastAppFrame_ >= 16) {
      lastAppFrame_ = now;
      RenderApp();
    }
    RunDeferred();
  }

  CloseApp();
  menu_.Hide();
  toast_.Hide();
  exporter_.Stop(); // una exportación a medias se cancela y se borra
  editWorker_.Stop(); // y lo mismo una edición
  pipeline_.Stop();
  // Clips o recortes que se están escribiendo. Antes de actualizar se espera
  // lo que haga falta: el instalador no debe encontrar un MP4 a medias.
  WaitForPendingWrites(installerPath_.empty() ? 5000 : 10 * 60 * 1000);
  UnregisterHotkeys();
  RemoveTrayIcon();
  if (hwnd_) DestroyWindow(hwnd_);
  // Spectre ya soltó la captura y terminó de escribir: el instalador puede
  // reemplazar los archivos y vuelve a abrir la app al acabar.
  // Con "Salir de Spectre" se instala igualmente, pero sin volver a abrir la app.
  if (!installerPath_.empty() && !Updater::LaunchInstaller(installerPath_, relaunchAfterUpdate_)) {
    std::wstring text = L"No se pudo abrir el instalador de la actualización. Puedes abrirlo a mano desde:\n" +
                        installerPath_;
    MessageBoxW(nullptr, text.c_str(), L"Spectre", MB_OK | MB_ICONWARNING);
  }
  return 0;
}

// ---------------------------------------------------------------- acciones

bool App::GameActive() const { return game_ && GetTickCount64() - lastGameSeen_ < kGameGraceMs; }

bool App::UpdateLiveLoad() {
  const bool busy = GameActive() || pipeline_.Recording();
  liveLoad_->store(busy);
  return busy;
}

std::string App::OutputName(const char* prefix, const char* extension) {
  bool gameActive = GameActive();
  std::string app = gameActive && !game_->exeName.empty() ? game_->exeName : "Escritorio";
  std::string base = SanitizeFileName(std::string(prefix) + "_" + app + "_" + TimestampForFile());
  auto dir = std::filesystem::path(Widen(ResolveOutputDir(config_)));
  // Ajustes > Replay > Una carpeta por juego (lo del escritorio va a la principal).
  if (config_.folderPerGame && gameActive && !game_->exeName.empty()) {
    dir /= Widen(SanitizeFileName(game_->exeName));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
  }
  // Dos clips en el mismo segundo no pueden compartir archivo: el segundo
  // todavía se está escribiendo cuando se pide el tercero, así que también se
  // recuerdan los nombres entregados.
  std::string path;
  for (int i = 1;; ++i) {
    std::string name = base + (i > 1 ? "_" + std::to_string(i) : "") + extension;
    path = Narrow((dir / Widen(name)).wstring());
    std::error_code ec;
    bool used = std::filesystem::exists(Widen(path), ec) ||
                std::find(recentNames_.begin(), recentNames_.end(), path) != recentNames_.end();
    if (!used) break;
  }
  recentNames_.push_back(path);
  if (recentNames_.size() > 32) recentNames_.pop_front();
  return path;
}

std::string App::LowDiskNote() {
  uint64_t free = FreeDiskBytes(ResolveOutputDir(config_));
  if (free == 0 || free >= kLowDiskBytes) return {};
  return ". Queda poco espacio en el disco (" + FormatBytes(free) + ")";
}

void App::SaveClip(bool shortClip) {
  if (!pipeline_.Running() || !config_.replayEnabled) {
    Toast("El replay no está activo", true);
    return;
  }
  // El clip corto no puede pasar de lo que guarda el replay.
  const int seconds = shortClip ? std::min(config_.shortClipSeconds, config_.clipSeconds) : config_.clipSeconds;
  std::string path = OutputName("Clip");
  // Las marcas de los últimos segundos van con el clip, que acaba justo ahora.
  int64_t now = QpcNow();
  std::vector<double> tail;
  for (int64_t at : markers_) {
    double ago = QpcToSeconds(now - at);
    if (ago >= 0 && ago <= double(seconds)) tail.push_back(ago);
  }
  library_->SetTailMarkers(path, std::move(tail));
  // El clip acaba al pedirlo: antes que el aviso, que puede tener que crear
  // su ventana. «Guardando clip» sale enseguida; el resultado
  // (WM_SPECTRE_NOTIFY, siempre después) lo cambia por «Clip guardado» en la
  // misma tarjeta.
  pendingClips_[FileNameOf(path)] = {seconds, shortClip};
  pipeline_.SaveClip(path, double(seconds));
  Notify({NoticeKind::ClipSaving, shortClip ? "Guardando clip corto" : "Guardando clip",
          "Los últimos " + ui::FormatClipLength(seconds)});
}

void App::OnRecordingStopped(const std::string& path, int64_t stoppedAt) {
  if (!path.empty() && !recordMarkers_.empty()) {
    // Desde el final, como en los clips: la grabación acaba al detenerla, pero
    // empieza en el keyframe anterior a pulsar el atajo (hasta 2 s antes).
    // Se cuenta desde antes de parar: con un disco lento, parar puede tardar.
    std::vector<double> tail;
    for (int64_t at : recordMarkers_) tail.push_back(QpcToSeconds(stoppedAt - at));
    library_->SetTailMarkers(path, std::move(tail));
  }
  recordMarkers_.clear();
  recordStartQpc_ = 0;
  if (appWindow_.Visible()) RescanLibrary();
  if (menu_.Visible()) RefreshPanelClips();
  EnforceStorageLimit();
}

void App::ToggleRecording() {
  if (pipeline_.Recording()) {
    PipelineStats stats = pipeline_.Stats();
    std::string path = stats.recordPath;
    int64_t stoppedAt = QpcNow();
    pipeline_.StopRecording();
    if (config_.soundOnSave) PlayCue(Cue::Saved, config_.saveSound);
    Notice notice{NoticeKind::RecordingSaved, "Grabación guardada",
                  FileNameOf(path) + " (" + ui::FormatDuration(stats.recordSeconds) + ")"};
    std::string note = LowDiskNote();
    if (!note.empty()) {
      notice.detail += note;
      notice.warning = true; // se ve aunque los avisos estén apagados
    }
    Notify(std::move(notice));
    OnRecordingStopped(path, stoppedAt);
    UpdatePipeline(std::exchange(restartPending_, false));
    InstallPendingUpdate();
    return;
  }
  if (!pipeline_.Running() && !StartPipeline()) {
    Toast("No se pudo iniciar la captura. Prueba otro método en Ajustes > Vídeo o mira spectre.log", true);
    return;
  }
  if (pipeline_.StartRecording(OutputName("Grabacion"))) {
    recordStartQpc_ = QpcNow();
    recordMarkers_.clear();
    const std::string& key = config_.hotkeys.toggleRecording;
    Notice notice{NoticeKind::Recording, "Grabando",
                  key.empty() ? "Detenla desde el panel (Alt+Z)" : "Pulsa " + key + " para detener"};
    std::string note = LowDiskNote();
    if (!note.empty()) {
      notice.detail += note;
      notice.warning = true;
    }
    Notify(std::move(notice));
  } else {
    Toast("No se pudo iniciar la grabación", true);
  }
}

void App::ToggleReplay() {
  config_.replayEnabled = !config_.replayEnabled;
  SaveConfig(config_);
  SyncModels();
  if (pipeline_.Running()) pipeline_.SetReplaySeconds(config_.replayEnabled ? config_.clipSeconds : 3.0);
  UpdatePipeline();
  const std::string& key = config_.hotkeys.saveClip;
  if (config_.replayEnabled)
    Notify({NoticeKind::ReplayOn, "Replay activado",
            (key.empty() ? std::string("Guarda") : key + " guarda") + " los últimos " +
                ui::FormatClipLength(config_.clipSeconds)});
  else
    Notify({NoticeKind::ReplayOff, "Replay desactivado", "No se guarda nada hasta que lo vuelvas a activar"});
}

void App::ToggleMicrophone() {
  Config next = config_;
  next.captureMic = !next.captureMic;
  if (pipeline_.MicCaptured()) {
    // El micrófono ya se está capturando: se silencia al vuelo y el replay no se pierde.
    config_.captureMic = next.captureMic;
    SaveConfig(config_);
    SyncModels();
    pipeline_.SetMicMuted(!config_.captureMic);
    Toast(config_.captureMic ? "Micrófono activado" : "Micrófono silenciado");
  } else {
    bool restarts = pipeline_.Running() && !pipeline_.Recording();
    ApplyConfig(next, true);
    if (!restartPending_)
      Toast(std::string(config_.captureMic ? "Micrófono activado" : "Micrófono desactivado") +
            (restarts ? ". El replay empieza de cero." : ""));
  }
}

void App::ToggleCamera() {
  // Antes reiniciaba toda la captura aquí, en el hilo del panel, y abría la
  // cámara con él: el panel se quedaba congelado lo que tardara (o para
  // siempre si el driver no volvía). Ahora la cámara se abre y se cierra en su
  // hilo y el replay sigue; si no abre, llega otro aviso con el motivo.
  Config next = config_;
  next.cameraEnabled = !next.cameraEnabled;
  ULONGLONG t0 = GetTickCount64();
  ApplyConfig(next, true);
  Log("Cámara %s desde el panel o la ventana (%llu ms)", config_.cameraEnabled ? "activada" : "desactivada",
      GetTickCount64() - t0);
  if (!config_.cameraEnabled) Toast("Cámara desactivada");
  else if (pipeline_.Running()) Toast("Cámara activada");
  else Toast("Cámara activada: se verá en cuanto empiece la captura");
}

void App::SetClipSeconds(int seconds) {
  Config next = config_;
  next.clipSeconds = seconds;
  ApplyConfig(next, true);
  // Una duración que no cabe en la RAM del replay se puede elegir, pero se
  // avisa: el anillo tiraría lo más antiguo y el clip saldría más corto.
  PipelineStats stats = pipeline_.Stats();
  if (stats.running && stats.replayFitSeconds > 0 && double(seconds) > stats.replayFitSeconds + 0.5)
    Toast("Con la calidad " + std::string(QualityName(config_.quality)) + ", " + ui::FormatClipLength(seconds) +
              " no caben enteros en la memoria del replay: en escenas de mucho movimiento el clip saldría más corto. "
              "Caben unos " + ui::FormatClipLength(std::max(5, int(stats.replayFitSeconds) / 5 * 5)) +
              ". Baja la calidad o sube el límite en Ajustes > Replay.",
          true);
}

void App::SetQuality(const std::string& quality) {
  if (quality == config_.quality) return;
  Config next = config_;
  next.quality = quality;
  bool restarts = pipeline_.Running() && !pipeline_.Recording();
  ApplyConfig(next, true);
  if (!restartPending_)
    Toast(std::string("Calidad ") + QualityName(config_.quality) + (restarts ? ". El replay empieza de cero." : ""));
}

void App::CaptureScreenshot() {
  ScreenshotRequest request;
  if (config_.captureMode == "game" && GameActive() && IsWindow(game_->window)) {
    request.target.window = game_->window;
  } else {
    // El monitor de lo que se está mirando: el juego (también con el panel
    // abierto encima) o la ventana activa; desde la bandeja, el del cursor.
    HWND looking = menu_.Visible() && previousForeground_ ? previousForeground_ : GetForegroundWindow();
    HMONITOR monitor = looking && looking != hwnd_ ? MonitorFromWindow(looking, MONITOR_DEFAULTTONULL) : nullptr;
    if (!monitor) {
      POINT cursor{};
      GetCursorPos(&cursor);
      monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    }
    request.target.monitor = monitor;
  }
  request.cursor = config_.captureCursor;
  request.jpeg = config_.screenshotFormat == "jpg";
  request.path = OutputName("Captura", request.jpeg ? ".jpg" : ".png");
  HWND hwnd = hwnd_;
  TakeScreenshot(std::move(request), [hwnd](bool ok, const std::string& text) {
    auto* msg = new std::string(ok ? "Captura guardada: " + FileNameOf(text) : text);
    if (!PostMessageW(hwnd, WM_SPECTRE_NOTIFY, ok ? 0 : 1, reinterpret_cast<LPARAM>(msg))) delete msg;
  });
}

void App::AddMarker() {
  if (!pipeline_.Running()) {
    Toast("No se está capturando nada: la marca no iría a ningún clip", true);
    return;
  }
  int64_t now = QpcNow();
  markers_.push_back(now);
  while (!markers_.empty() && QpcToSeconds(now - markers_.front()) > kMaxMarkerAge) markers_.pop_front();
  if (pipeline_.Recording() && recordStartQpc_) recordMarkers_.push_back(now);
  if (config_.soundOnSave) PlayCue(Cue::Marker, config_.saveSound);
  Notify({NoticeKind::Marker, "Marca añadida",
          pipeline_.Recording() ? "En la grabación y en el replay" : "Saldrá en el próximo clip"});
}

void App::TrimClip(const std::string& path, double start, double end) {
  HWND hwnd = hwnd_;
  auto pending = std::make_shared<PendingWrite>();
  std::thread([hwnd, path, start, end, pending] {
    InstallThreadCrashHandler();
    std::string error;
    bool ok = false;
    std::filesystem::path out;
    try {
      auto in = std::filesystem::path(Widen(path));
      std::wstring base = in.stem().wstring() + L"_recorte_" + Widen(TimestampForFile());
      out = in.parent_path() / (base + L".mp4");
      std::error_code ec;
      for (int i = 2; std::filesystem::exists(out, ec); ++i)
        out = in.parent_path() / (base + L"_" + std::to_wstring(i) + L".mp4");
      ok = TrimFile(path, Narrow(out.wstring()), start, end, &error);
    } catch (const std::exception& e) {
      error = std::string("No se pudo recortar: ") + e.what();
      Log("Recorte: excepción: %s", e.what());
    }
    auto* msg = new std::string(ok ? "Recorte guardado: " + Narrow(out.filename().wstring()) : error);
    if (!PostMessageW(hwnd, WM_SPECTRE_NOTIFY, ok ? 0 : 1, reinterpret_cast<LPARAM>(msg))) delete msg;
  }).detach();
  Toast("Recortando el clip...");
}

namespace {

// <carpeta>\<base>.<ext> o, si ya existe, <base>_2, <base>_3...
std::filesystem::path FreePath(const std::filesystem::path& dir, const std::wstring& base, const wchar_t* ext) {
  std::filesystem::path out = dir / (base + ext);
  std::error_code ec;
  for (int i = 2; std::filesystem::exists(out, ec); ++i) out = dir / (base + L"_" + std::to_wstring(i) + ext);
  return out;
}

// Ediciones de la biblioteca (unir, cámara lenta): en un hilo en modo de fondo,
// como la escritura de clips: unir grabaciones largas lee y escribe GB, y el
// juego va antes en el disco. work devuelve la ruta escrita o el error; al
// acabar, aviso.
void RunEdit(HWND hwnd, std::string doneText, std::function<bool(std::string* out, std::string* error)> work) {
  auto pending = std::make_shared<PendingWrite>();
  std::thread([hwnd, doneText = std::move(doneText), work = std::move(work), pending] {
    InstallThreadCrashHandler();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    BackgroundMode background;
    std::string out, error;
    bool ok = false;
    try {
      ok = work(&out, &error);
    } catch (const std::exception& e) {
      error = std::string("No se pudo editar el vídeo: ") + e.what();
      Log("Edición: excepción: %s", e.what());
    }
    auto* msg = new std::string(ok ? doneText + FileNameOf(out) : error);
    if (!PostMessageW(hwnd, WM_SPECTRE_NOTIFY, ok ? 0 : 1, reinterpret_cast<LPARAM>(msg))) delete msg;
  }).detach();
}

} // namespace

void App::JoinClips(const std::vector<std::string>& paths) {
  if (paths.size() < 2) return;
  RunEdit(hwnd_, "Unión guardada: ", [paths](std::string* out, std::string* error) {
    auto first = std::filesystem::path(Widen(paths.front()));
    auto path = FreePath(first.parent_path(), first.stem().wstring() + L"_union_" + Widen(TimestampForFile()), L".mp4");
    *out = Narrow(path.wstring());
    return JoinFiles(paths, *out, error);
  });
  Toast("Uniendo " + std::to_string(paths.size()) + " vídeos...");
}

void App::SlowMotion(const std::string& path, int factor) {
  RunEdit(hwnd_, "Cámara lenta guardada: ", [path, factor](std::string* out, std::string* error) {
    auto in = std::filesystem::path(Widen(path));
    auto dest = FreePath(in.parent_path(), in.stem().wstring() + L"_lenta_" + Widen(TimestampForFile()), L".mp4");
    *out = Narrow(dest.wstring());
    return SlowMotionFile(path, *out, factor, error);
  });
  Toast(factor >= 4 ? "Haciendo la copia a un cuarto de velocidad..." : "Haciendo la copia a mitad de velocidad...");
}

void App::SplitClip(const std::string& path, double at) {
  HWND hwnd = hwnd_;
  auto pending = std::make_shared<PendingWrite>();
  std::thread([hwnd, path, at, pending] {
    InstallThreadCrashHandler();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    BackgroundMode background;
    std::string error, names;
    bool ok = false;
    try {
      auto in = std::filesystem::path(Widen(path));
      const std::wstring stamp = Widen(TimestampForFile());
      auto first = FreePath(in.parent_path(), in.stem().wstring() + L"_parte1_" + stamp, L".mp4");
      auto second = FreePath(in.parent_path(), in.stem().wstring() + L"_parte2_" + stamp, L".mp4");
      ok = TrimFile(path, Narrow(first.wstring()), 0, at, &error) &&
           TrimFile(path, Narrow(second.wstring()), at, 1e9, &error);
      if (!ok) {
        std::error_code ec;
        std::filesystem::remove(first, ec);
        std::filesystem::remove(second, ec);
      }
      names = Narrow(first.filename().wstring()) + " y " + Narrow(second.filename().wstring());
    } catch (const std::exception& e) {
      ok = false;
      error = std::string("No se pudo dividir el vídeo: ") + e.what();
      Log("Dividir: excepción: %s", e.what());
    }
    auto* msg = new std::string(ok ? "Dividido en dos: " + names : error.empty() ? "No se pudo dividir el vídeo" : error);
    if (!PostMessageW(hwnd, WM_SPECTRE_NOTIFY, ok ? 0 : 1, reinterpret_cast<LPARAM>(msg))) delete msg;
  }).detach();
  Toast("Dividiendo el vídeo en dos...");
}

void App::SaveEdit(const std::string& path, const EditSettings& settings) {
  if (editWorker_.Busy()) {
    Toast("Ya se está guardando otra edición: espera a que termine", true);
    return;
  }
  EditJob job;
  job.input = path;
  job.settings = settings;
  // Con un juego abierto o grabando se guarda sin prisa (un hilo, en modo de
  // fondo, como mucho a tiempo real), para no quitarle CPU, disco ni el
  // encoder de la GPU al replay.
  job.gentle = UpdateLiveLoad();
  job.live = liveLoad_;
  job.raster = RasterizeText;
  const wchar_t* ext = settings.format == EditFormat::Gif     ? L".gif"
                       : settings.format == EditFormat::Audio ? L".m4a"
                                                              : L".mp4";
  auto in = std::filesystem::path(Widen(path));
  job.output =
      Narrow(FreePath(in.parent_path(), in.stem().wstring() + L"_editado_" + Widen(TimestampForFile()), ext).wstring());
  HWND hwnd = hwnd_;
  if (!editWorker_.Start(
          std::move(job), [hwnd](int percent) { PostMessageW(hwnd, WM_SPECTRE_EDIT, WPARAM(percent), 0); },
          [hwnd] { PostMessageW(hwnd, WM_SPECTRE_EDIT, kEditDone, 0); })) {
    Toast("Ya se está guardando otra edición: espera a que termine", true);
    return;
  }
  appModel_.editRenderPath = path;
  appModel_.editPercent = 0;
  appWindow_.Invalidate();
  Toast(settings.format == EditFormat::Gif     ? "Haciendo el GIF..."
        : settings.format == EditFormat::Audio ? "Guardando el sonido..."
                                               : "Guardando la edición...");
}

void App::FinishEdit() {
  EditResult r;
  if (!editWorker_.TakeResult(&r)) return;
  appModel_.editRenderPath.clear();
  appModel_.editPercent = -1;
  appWindow_.Invalidate();
  if (r.cancelled) {
    Toast("Edición cancelada");
    return;
  }
  if (!r.ok) {
    Toast(r.error.empty() ? "No se pudo guardar la edición" : r.error, true);
    return;
  }
  appModel_.editSaved = r.output;
  appModel_.editSavedFrom = r.input;
  if (r.settings.format == EditFormat::Video && r.settings.size == EditSize::Discord) {
    // Después, la copia de menos de 20 MB (la edición sola se borra si sale aparte).
    if (ExportBusy()) {
      Toast("Edición guardada: " + FileNameOf(r.output) +
                ". Ya se está exportando otro vídeo: expórtala para Discord desde su menú cuando acabe",
            true);
    } else {
      discordAfterEdit_ = r.output;
      ExportForDiscord(r.output);
    }
  } else {
    const std::string note = LowDiskNote();
    Toast("Edición guardada: " + FileNameOf(r.output) + " (" + FormatBytes(r.bytes) + ")" + note, !note.empty());
  }
  if (menu_.Visible()) RefreshPanelClips();
  if (appWindow_.Visible()) RescanLibrary();
  EnforceStorageLimit(); // algo nuevo en la carpeta
}

void App::SaveFrame(const MediaItem& item, std::vector<uint8_t> bgr, int width, int height) {
  if (bgr.empty() || width <= 0 || height <= 0) {
    Toast("Aún no hay fotograma que guardar: espera a que se vea el vídeo", true);
    return;
  }
  // Como una captura de Alt+F1 de ese juego, en la carpeta del vídeo.
  const bool jpeg = config_.screenshotFormat == "jpg";
  auto dir = std::filesystem::path(Widen(item.path)).parent_path();
  std::string game = item.spectre && !item.game.empty() ? item.game : "Escritorio";
  auto dest = FreePath(dir, Widen(SanitizeFileName("Captura_" + game + "_" + TimestampForFile())), jpeg ? L".jpg" : L".png");
  HWND hwnd = hwnd_;
  SaveImageAsync(Narrow(dest.wstring()), jpeg, std::move(bgr), unsigned(width), unsigned(height),
                 [hwnd](bool ok, const std::string& text) {
                   auto* msg = new std::string(ok ? "Fotograma guardado: " + FileNameOf(text) : text);
                   if (!PostMessageW(hwnd, WM_SPECTRE_NOTIFY, ok ? 0 : 1, reinterpret_cast<LPARAM>(msg))) delete msg;
                 });
}

void App::ExportForDiscord(const std::string& path) {
  if (ExportBusy()) {
    Toast("Ya se está exportando otro vídeo: espera a que termine", true);
    return;
  }
  StartExport(path);
  Toast("Exportando para Discord...");
}

void App::StartExport(const std::string& path) {
  ExportOptions options;
  // Con un juego abierto o grabando se exporta sin prisa, para no quitarle
  // CPU ni el encoder de la GPU al replay.
  options.gentle = UpdateLiveLoad();
  options.live = liveLoad_;
  HWND hwnd = hwnd_;
  exporter_.Start(
      path, options, [hwnd](int percent) { PostMessageW(hwnd, WM_SPECTRE_EXPORT, WPARAM(percent), 0); },
      [hwnd] { PostMessageW(hwnd, WM_SPECTRE_EXPORT, kExportDone, 0); });
  exportRunning_ = true;
  appModel_.exportPath = path;
  appModel_.exportPercent = 0;
  appWindow_.Invalidate();
}

void App::ExportManyForDiscord(const std::vector<std::string>& paths) {
  const std::string current = exportRunning_ ? exporter_.Input() : std::string();
  size_t added = 0;
  const bool batch = exportBatchTotal_ > 0;
  for (const auto& path : paths) {
    if (path == current || std::find(exportQueue_.begin(), exportQueue_.end(), path) != exportQueue_.end())
      continue;
    exportQueue_.push_back(path);
    ++added;
  }
  if (added == 0) {
    Toast("Exportando para Discord: ya estaban en la cola");
    return;
  }
  if (!batch) {
    exportBatchOutputs_.clear();
    exportBatchFailed_ = 0;
    exportBatchError_.clear();
    exportBatchCancelled_ = false;
  }
  exportBatchTotal_ += int(added);
  const std::string videos = CountText(added, "vídeo", "vídeos");
  if (batch) Toast("Exportando para Discord: " + videos + " más en la cola");
  else if (exportRunning_) Toast("Exportando para Discord: " + videos + " al acabar el que está en marcha");
  else Toast("Exportando para Discord: " + videos + ", uno detrás de otro");
  StartQueuedExport();
}

void App::StartQueuedExport() {
  if (exportRunning_) return;
  while (!exportQueue_.empty()) {
    std::string path = exportQueue_.front();
    exportQueue_.pop_front();
    // Borrado o renombrado mientras esperaba su turno: ya no cuenta.
    if (GetFileAttributesW(Widen(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
      --exportBatchTotal_;
      continue;
    }
    exportBatchInput_ = path;
    StartExport(path);
    return;
  }
  if (exportBatchTotal_ > 0) EndExportBatch();
}

void App::FinishBatchExport(const ExportResult& r) {
  if (r.cancelled) {
    exportBatchCancelled_ = true;
    exportQueue_.clear();
  } else if (!r.ok) {
    ++exportBatchFailed_;
    exportBatchError_ = r.error;
    Log("Exportar para Discord: %s: %s", r.input.c_str(), r.error.c_str());
  } else {
    exportBatchOutputs_.push_back(r.output);
    Log("Exportado para Discord: %s, %s (%s)", r.output.c_str(), FormatBytes(r.bytes).c_str(),
        r.alreadyFits ? "ya cabía, sin recodificar" : r.encoder.c_str());
    // La copia aparece en la biblioteca según se hace.
    if (!r.alreadyFits && appWindow_.Visible()) RescanLibrary();
  }
  appWindow_.Invalidate();
}

void App::EndExportBatch() {
  const int failed = exportBatchFailed_;
  const bool cancelled = exportBatchCancelled_;
  const std::string error = exportBatchError_;
  std::vector<std::string> outputs = std::move(exportBatchOutputs_);
  exportBatchOutputs_.clear();
  exportBatchTotal_ = exportBatchFailed_ = 0;
  exportBatchError_.clear();
  exportBatchCancelled_ = false;
  exportBatchInput_.clear();
  appWindow_.Invalidate();
  const size_t done = outputs.size();
  // Todas las copias al portapapeles: se pegan juntas en Discord.
  const bool copied = done > 0 && CopyFilesToClipboard(appWindow_.Visible() ? appWindow_.Hwnd() : hwnd_, outputs);
  if (done > 0) {
    if (menu_.Visible()) RefreshPanelClips();
    if (appWindow_.Visible()) RescanLibrary();
    EnforceStorageLimit(); // algo nuevo en la carpeta
  }
  const std::string copiedText = !copied ? "" : done == 1 ? " y copiado" : " y copiados";
  if (cancelled) {
    Toast(done == 0 ? std::string("Exportación cancelada")
                    : "Exportación cancelada: " + CountText(done, "vídeo listo", "vídeos listos") + copiedText +
                          (copied ? " para pegar con Ctrl+V" : ""));
  } else if (failed == 0 && done > 0) {
    // Discord acepta 10 archivos por mensaje.
    Toast("Listos para Discord: " + CountText(done, "vídeo", "vídeos") +
              (!copied     ? " junto a los originales"
               : done > 10 ? ", copiados (Discord acepta 10 por mensaje: pégalos en tandas)"
                           : ", copiados para pegar con Ctrl+V"),
          false, NoticeKind::Export);
  } else if (done > 0) {
    Toast("Listos para Discord " + std::to_string(done) + " de " + std::to_string(done + size_t(failed)) +
              (copied ? ", copiados. " : ". ") + (failed == 1 ? "El otro: " : "Los otros: ") + error,
          true);
  } else if (failed > 0) {
    Toast(error, true);
  }
}

void App::RecycleMany(std::vector<std::string> paths) {
  // Lo que se está exportando o guardando como edición sigue abierto: se queda.
  const std::string exporting = exportRunning_ ? exporter_.Input() : std::string();
  const std::string editing = editWorker_.Busy() ? editWorker_.Input() : std::string();
  const size_t asked = paths.size();
  paths.erase(std::remove_if(paths.begin(), paths.end(),
                             [&](const std::string& path) { return path == exporting || path == editing; }),
              paths.end());
  const size_t inUse = asked - paths.size();
  bool bin = true;
  for (const auto& path : paths) bin = bin && HasRecycleBin(path);
  std::string error;
  bool cancelled = false;
  std::vector<std::string> gone;
  if (!paths.empty())
    gone = library_->Recycle(appWindow_.Visible() ? appWindow_.Hwnd() : hwnd_, paths, &error, &cancelled);
  const std::string kept = inUse == 0   ? std::string()
                           : inUse == 1 ? std::string(". El que se está exportando o editando se queda")
                                        : std::string(". Los que se están exportando o editando se quedan");
  if (!gone.empty() && gone.size() == paths.size()) {
    if (gone.size() == 1)
      Toast((bin ? "Movido a la papelera: " : "Borrado: ") + FileNameOf(gone[0]) + kept, false, NoticeKind::Storage);
    else
      Toast((bin ? "Movidos a la papelera: " : "Borrados: ") + CountText(gone.size(), "archivo", "archivos") + kept,
            false, NoticeKind::Storage);
  } else if (!gone.empty()) {
    Toast("Movidos a la papelera " + std::to_string(gone.size()) + " de " + std::to_string(asked) + ". " +
              (cancelled ? "Los demás no se han borrado" : "Los demás están abiertos en otro programa"),
          !cancelled, NoticeKind::Storage);
  } else if (paths.empty()) {
    Toast("No se ha borrado: se está exportando o editando", true);
  } else if (!cancelled) {
    Toast(error, true);
  }
  RescanLibrary();
}

void App::FinishExport() {
  ExportResult r;
  if (!exporter_.TakeResult(&r)) return;
  exportRunning_ = false;
  appModel_.exportPath.clear();
  appModel_.exportPercent = -1;
  appWindow_.Invalidate();
  if (!exportBatchInput_.empty() && r.input == exportBatchInput_) {
    exportBatchInput_.clear();
    FinishBatchExport(r);
    return;
  }
  const bool afterEdit = !discordAfterEdit_.empty() && r.input == discordAfterEdit_;
  if (afterEdit) discordAfterEdit_.clear();
  if (r.cancelled) {
    Toast(afterEdit ? "Exportación cancelada: la edición se quedó guardada" : "Exportación cancelada");
    return;
  }
  if (!r.ok) {
    Toast(r.error, true);
    return;
  }
  if (afterEdit && !r.alreadyFits) {
    // De «Editar» con «Para Discord»: se queda solo la copia que cabe.
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(Widen(r.input)), ec);
    if (ec) Log("Editor: no se pudo borrar la copia intermedia %s", r.input.c_str());
    if (appModel_.editSaved == r.input) appModel_.editSaved = r.output;
  }
  Log("Exportado para Discord: %s, %s (%s)", r.output.c_str(), FormatBytes(r.bytes).c_str(),
      r.alreadyFits ? "ya cabía, sin recodificar" : r.encoder.c_str());
  const bool copied = CopyFileToClipboard(appWindow_.Visible() ? appWindow_.Hwnd() : hwnd_, r.output);
  const std::string size = FormatBytes(r.bytes);
  if (r.alreadyFits) {
    Toast(copied ? "Ya cabe en Discord (" + size + "): copiado, pégalo con Ctrl+V"
                 : "Ya cabe en Discord (" + size + "): no hace falta exportarlo");
    return;
  }
  Toast(copied ? "Lista para Discord (" + size + ") y copiada: pégala con Ctrl+V"
               : "Copia para Discord guardada (" + size + ")");
  if (menu_.Visible()) RefreshPanelClips();
  if (appWindow_.Visible()) RescanLibrary();
  EnforceStorageLimit(); // algo nuevo en la carpeta
}

void App::ToggleMenu() {
  if (menu_.Visible()) CloseMenu();
  else OpenMenu();
}

App::MenuArea App::MenuPlacement() const {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(menuMonitor_, &mi)) {
    mi.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    mi.rcWork = mi.rcMonitor;
  }
  const bool lateral = IsLateralPanel(menuModel_.config);
  const float scale = MonitorScale(menuMonitor_);
  MenuArea a;
  // Lateral: el monitor entero, de arriba abajo, como el overlay de NVIDIA.
  // Compacto: sobre un juego a pantalla completa la barra de tareas está
  // tapada y también se usa el monitor entero; en el escritorio, el área de
  // trabajo, con un margen.
  a.area = lateral || menuScreen_.fullscreen ? mi.rcMonitor : mi.rcWork;
  a.width = int(std::ceil(kPanelWidth * scale));
  a.margin = lateral ? 0 : int(kPanelMargin * scale);
  return a;
}

RECT App::MenuRect() const {
  // La ventana es una franja vertical del ancho del panel y del alto de la
  // zona; solo el panel recibe clics (SetHitRegion) y el resto de la franja es
  // transparente. Va donde se dejó al arrastrarlo: la fracción guardada del
  // hueco libre, que sirve igual con otra resolución, escala o monitor.
  const MenuArea a = MenuPlacement();
  const Config& c = menuModel_.config;
  const float fx = IsLateralPanel(c) ? c.panelLateralX : c.panelCompactX;
  const LONG free = std::max<LONG>(0, a.area.right - a.area.left - 2 * a.margin - a.width);
  const LONG left = a.area.left + a.margin + LONG(std::lround(fx * float(free)));
  return RECT{left, a.area.top + a.margin, left + a.width, a.area.bottom - a.margin};
}

void App::PlaceMenu(bool activate) {
  // Las letras del diseño del tema, para que el primer frame ya las tenga.
  const Style& style = StyleFor(FindTheme(config_.theme), config_.themeStyle);
  menu_.SetFonts(style.body, style.display);
  if (!menu_.Show(MenuRect(), activate)) {
    Toast("No se pudo abrir el panel", true);
    return;
  }
  // Un juego topmost (GLFW, Unity...) vuelve a subir al activarse: el panel se
  // mantiene por encima mientras esté abierto.
  menu_.StayAbove(menuScreen_.fullscreen ? menuScreen_.window : nullptr);
}

void App::OpenMenu() {
  if (menu_.Visible()) return;
  ++menuGeneration_;
  previousForeground_ = GetForegroundWindow();
  if (previousForeground_ == hwnd_) previousForeground_ = nullptr; // ventana oculta (tras el menú de la bandeja)
  menuOpenedAt_ = GetTickCount64();
  menuFocusChecked_ = false;
  gameRestored_ = false;
  menuHitValid_ = false;
  mouseWasDown_ = AnyMouseButtonDown();
  menuModel_ = MenuModel{};
  menuModel_.config = config_;
  menuModel_.openedAt = GetTickCount64();
  RefreshPanelClips();
  RefreshMenuModel();
  // El panel va al monitor de la ventana activa (el juego), con su DPI; sin
  // ventana, al del cursor. Sobre un juego a pantalla completa se decide si
  // coge el foco o se abre sin activarse (ShouldActivateMenu).
  menuScreen_ = QueryFullscreen(previousForeground_);
  menuMonitor_ = menuScreen_.monitor;
  if (menuScreen_.fullscreen) Log("Panel sobre %s", menuScreen_.Describe().c_str());
  menuActivated_ = ShouldActivateMenu(menuScreen_);
  PlaceMenu(menuActivated_);
  SyncKeyboardHook();
}

void App::CloseMenu(bool restoreFocus) {
  if (!menu_.Visible()) return;
  // Si el panel tiene el foco, se le devuelve al juego antes de destruir la
  // ventana (si no, Windows activaría cualquier otra). Abierto sin foco no hay
  // nada que devolver, y si el usuario se fue a otra ventana se respeta.
  EndMenuSession(restoreFocus && GetForegroundWindow() == menu_.Hwnd());
  menu_.Hide();
}

// Lo que cierra el panel aparte de su ventana: también hace falta cuando la
// ventana ya no existe (no se pudo recrear tras perder la GPU). Un juego que se
// minimizó al perder el foco se restaura: al activarse vuelve a su pantalla
// completa.
void App::EndMenuSession(bool giveFocusBack) {
  EndMenuDrag(); // cerrado a mitad de un arrastre: se guarda donde iba
  ++menuGeneration_;
  menuClosedAt_ = GetTickCount64();
  keyboard_.Uninstall();
  HWND previous = previousForeground_;
  previousForeground_ = nullptr;
  if (giveFocusBack && previous && previous != hwnd_ && IsWindow(previous)) {
    if (IsIconic(previous)) ShowWindow(previous, SW_RESTORE);
    SetForegroundWindow(previous);
  }
}

void App::ApplyConfig(const Config& next, bool quiet) {
  Config old = config_;
  config_ = next;
  for (int i = 0; i < kHotkeyCount; ++i)
    HotkeyText(config_.hotkeys, i) = NormalizeHotkey(HotkeyText(config_.hotkeys, i));
  SaveConfig(config_);
  SyncModels();

  if (old.autostart != config_.autostart && !SetAutostart(config_.autostart))
    Toast("No se pudo cambiar el inicio con Windows", true);

  bool hotkeysOk = true;
  if (!SameHotkeys(old.hotkeys, config_.hotkeys)) {
    UnregisterHotkeys();
    if (!RegisterHotkeys()) {
      // Un atajo nuevo que no se pudo registrar vuelve al anterior: mejor eso
      // que quedarse sin Alt+Z. Los que ya fallaban antes se quedan como están.
      std::string error = hotkeyError_;
      bool reverted = false;
      for (int i = 0; i < kHotkeyCount; ++i) {
        if (!(hotkeyFailedMask_ & (1u << i)) || HotkeyText(config_.hotkeys, i) == HotkeyText(old.hotkeys, i)) continue;
        HotkeyText(config_.hotkeys, i) = HotkeyText(old.hotkeys, i);
        reverted = true;
      }
      if (reverted) {
        UnregisterHotkeys();
        RegisterHotkeys();
        SaveConfig(config_);
        SyncModels();
      }
      hotkeyError_ = error;
      hotkeysOk = false;
    }
    SyncKeyboardHook(); // el gancho deja pasar los atajos nuevos
  }
  startFailedAt_ = 0; // con ajustes nuevos se puede volver a intentar ya

  bool deferred = false;
  if (!SameCaptureSettings(old, config_) && pipeline_.Running()) {
    if (pipeline_.Recording()) {
      restartPending_ = true;
      deferred = true;
    } else {
      UpdatePipeline(true);
    }
  } else if (pipeline_.Running()) {
    pipeline_.SetReplaySeconds(config_.replayEnabled ? config_.clipSeconds : 3.0);
  }
  // También grabando: la cámara solo se compone encima, no corta nada.
  if (!SameCameraSettings(old, config_) && pipeline_.Running()) pipeline_.SetCamera(BuildSettings());
  UpdatePipeline();

  const bool folderChanged = old.outputDir != config_.outputDir || old.folderPerGame != config_.folderPerGame;
  if (folderChanged && appWindow_.Visible()) RescanLibrary();
  if (old.storageLimitGB != config_.storageLimitGB || old.deleteAfterDays != config_.deleteAfterDays ||
      old.outputDir != config_.outputDir)
    EnforceStorageLimit();

  if (!hotkeysOk) Toast(hotkeyError_, true);
  else if (deferred) Toast("Se aplicará al terminar la grabación");
  else if (!quiet) Toast("Cambios aplicados");
}

void App::InstallPendingUpdate() {
  // Ni a mitad de una grabación ni de una edición o una exportación, que se
  // cancelarían al cerrar: el temporizador de cada segundo vuelve a probar.
  if (installerPath_.empty() || pipeline_.Recording() || editWorker_.Busy() || ExportBusy()) return;
  relaunchAfterUpdate_ = true;
  Quit();
}

void App::Quit() {
  quitting_ = true;
  PostQuitMessage(0);
}

// ------------------------------------------------------------------ estado

bool App::WantPipeline() const {
  if (pipeline_.Recording()) return true;
  if (!config_.replayEnabled) return false;
  if (!config_.onlyInGames) return true;
  return GameActive();
}

PipelineSettings App::BuildSettings() const {
  PipelineSettings s;
  bool gameActive = GameActive() && IsWindow(game_->window);
  if (config_.captureMode == "game" && gameActive) {
    s.target.window = game_->window;
  } else {
    auto monitors = EnumerateMonitors();
    if (!monitors.empty())
      s.target.monitor = monitors[size_t(std::min<int>(config_.monitorIndex, int(monitors.size()) - 1))].handle;
  }
  s.captureApi = config_.captureApi;
  s.cursor = config_.captureCursor;
  s.fps = config_.fps;
  s.outputHeight = config_.outputHeight;
  s.quality = config_.quality;
  s.bitrateKbps = config_.bitrateKbps;
  s.codec = config_.codec;
  s.encoder = config_.encoder;
  s.gpuPriority = config_.gpuPriority;
  s.audio.system = config_.captureSystemAudio;
  s.audio.outputDeviceId = config_.outputDeviceId;
  s.audio.processOnly = config_.gameAudioOnly && gameActive;
  s.audio.processId = gameActive ? game_->processId : 0;
  s.audio.mic = config_.captureMic;
  s.audio.micDeviceId = config_.micDeviceId;
  s.audio.systemVolume = config_.systemVolume;
  s.audio.micVolume = config_.micVolume;
  s.audio.separateTracks = config_.separateTracks;
  s.camera = config_.cameraEnabled;
  s.cameraId = config_.cameraId;
  s.cameraCorner = config_.cameraCorner;
  s.cameraSizePercent = config_.cameraSizePercent;
  s.replaySeconds = config_.replayEnabled ? config_.clipSeconds : 3.0;
  // 1024 MB es el valor por defecto de siempre: con él, el tope lo calcula el
  // pipeline según calidad, duración y RAM (con 5 min en Alta se quedaba corto).
  s.maxBufferBytes = config_.maxBufferMB == 1024 ? 0 : size_t(config_.maxBufferMB) << 20;
  return s;
}

bool App::StartPipeline() {
  if (startFailedAt_ && GetTickCount64() - startFailedAt_ < 10000) return false; // no reintentar en bucle
  HWND hwnd = hwnd_;
  PipelineSettings settings = BuildSettings();
  pipelinePid_ = settings.audio.processOnly ? settings.audio.processId : 0;
  pipelineWindow_ = settings.target.window;
  bool ok = pipeline_.Start(settings, [hwnd](const std::string& message, bool error) {
    auto* text = new std::string(message);
    if (!PostMessageW(hwnd, WM_SPECTRE_NOTIFY, error ? 1 : 0, reinterpret_cast<LPARAM>(text))) delete text;
  });
  if (!ok) {
    startFailedAt_ = GetTickCount64();
    Toast("No se pudo iniciar la captura. Revisa spectre.log en %APPDATA%\\Spectre", true);
  } else {
    startFailedAt_ = 0;
  }
  return ok;
}

void App::UpdatePipeline(bool forceRestart) {
  bool want = WantPipeline();
  if (pipeline_.Running() && (!want || forceRestart) && !pipeline_.Recording()) pipeline_.Stop();
  if (want && !pipeline_.Running()) StartPipeline();
}

void App::CheckGame() {
  if (menu_.Visible()) return; // el panel tapa el juego: se mantiene lo último detectado
  ULONGLONG now = GetTickCount64();
  if (auto detected = DetectFullscreenApp()) {
    game_ = detected;
    lastGameSeen_ = now;
    // Un juego (o un vídeo) a pantalla completa delante: el visor de la
    // biblioteca no se queda sonando detrás.
    if (appWindow_.Visible()) appUi_.PauseMedia();
  }
  bool gameActive = game_ && now - lastGameSeen_ < kGameGraceMs && IsWindow(game_->window);
  if (pipeline_.Running() && !pipeline_.Recording()) {
    // Modo "ventana del juego": la captura sigue al juego activo. Se comprueba
    // en cada pasada (no solo cuando cambia), por si la primera vez se estaba
    // grabando o la captura no pudo arrancar.
    // Se compara con la ventana pedida, no con la efectiva: si WGC no pudo y se
    // cayó a DDA (siempre en Windows 10), la efectiva es el monitor y se
    // reiniciaba el pipeline cada 2 s, vaciando el replay.
    bool retarget = config_.captureMode == "game" && gameActive && pipelineWindow_ != game_->window;
    // "Solo el audio del juego": el proceso capturado tiene que seguir al juego,
    // también en modo pantalla completa y cuando el juego se cierra.
    bool processAudio = config_.captureSystemAudio && config_.gameAudioOnly;
    DWORD wantPid = processAudio && gameActive ? game_->processId : 0;
    bool reaudio = processAudio && wantPid != pipelinePid_;
    if (retarget || reaudio) {
      Log("Juego %s: se reinicia la captura", gameActive ? game_->exeName.c_str() : "cerrado");
      UpdatePipeline(true);
      AnnounceGame();
      return;
    }
  }
  UpdatePipeline();
  AnnounceGame();
}

bool App::RegisterHotkeys() {
  if (hotkeysSuspended_) return true; // se registran al terminar de grabar el atajo en Ajustes
  std::string failed;
  hotkeyFailedMask_ = 0;
  for (int i = 0; i < kHotkeyCount; ++i) {
    const std::string& text = HotkeyText(config_.hotkeys, i);
    if (text.empty()) continue;
    UINT mods = 0, vk = 0;
    if (!ParseHotkey(text, &mods, &vk) || !RegisterHotKey(hwnd_, kHotkeyOverlay + i, mods, vk)) {
      failed += (failed.empty() ? "" : ", ") + text;
      hotkeyFailedMask_ |= 1u << i;
    }
  }
  if (!failed.empty()) {
    hotkeyError_ = "No se pudo registrar " + failed + " (puede que otra app ya lo use, p. ej. GeForce Experience)";
    Log("%s", hotkeyError_.c_str());
    return false;
  }
  hotkeyError_.clear();
  return true;
}

void App::UnregisterHotkeys() {
  for (int id = kHotkeyOverlay; id < kHotkeyOverlay + kHotkeyCount; ++id) UnregisterHotKey(hwnd_, id);
}

void App::SuspendHotkeys(bool suspend) {
  if (suspend == hotkeysSuspended_) return;
  if (suspend) {
    UnregisterHotkeys();
    hotkeysSuspended_ = true;
  } else {
    hotkeysSuspended_ = false;
    RegisterHotkeys(); // si alguno falla, Ajustes > Atajos lo dice
  }
}

void App::EnforceStorageLimit() {
  if (config_.storageLimitGB <= 0) limitWarned_.clear();
  if (config_.storageLimitGB <= 0 && config_.deleteAfterDays <= 0) return;
  // Una pasada cada vez: el hilo escanea la carpeta y borra lo más antiguo.
  static std::atomic<bool> running{false};
  if (running.exchange(true)) return;
  std::string folder = ResolveOutputDir(config_);
  uint64_t limit = uint64_t(std::max(config_.storageLimitGB, 0)) << 30;
  int days = config_.deleteAfterDays;
  // La grabación en curso cuenta, pero no se toca.
  std::string recording = pipeline_.Recording() ? pipeline_.Stats().recordPath : std::string();
  std::shared_ptr<Library> library = library_;
  HWND hwnd = hwnd_;
  auto pending = std::make_shared<PendingWrite>(); // al salir se espera a que acabe
  std::thread([folder, limit, days, recording, library, hwnd, pending] {
    InstallThreadCrashHandler();
    auto result = std::make_unique<LimitResult>(library->EnforceLimit(folder, limit, recording, days));
    if (result->deleted > 0)
      Log("Límite de espacio: %d archivos borrados (%d por antigüedad), %s liberados", result->deleted,
          result->agedDeleted, FormatBytes(result->freedBytes).c_str());
    if (result->blocked)
      Log("Límite de espacio: lo que no se puede borrar (%s, %s en favoritos) ya pasa de %s: no se borra nada",
          FormatBytes(result->keptBytes).c_str(), FormatBytes(result->favoriteBytes).c_str(),
          FormatBytes(limit).c_str());
    running = false;
    if (PostMessageW(hwnd, WM_SPECTRE_LIBRARY, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
  }).detach();
}

namespace {

// «se borraron 3 clips antiguos (2.10 GB)»
std::string DeletedText(const LimitResult& r) {
  struct Noun {
    const char *one, *many;
    bool female;
  };
  Noun n{"archivo", "archivos", false};
  if (!r.mixed) switch (r.kind) {
    case MediaKind::Clip: n = {"clip", "clips", false}; break;
    case MediaKind::Recording: n = {"grabación", "grabaciones", true}; break;
    case MediaKind::Trim: n = {"recorte", "recortes", false}; break;
    case MediaKind::Export: n = {"vídeo para Discord", "vídeos para Discord", false}; break;
    case MediaKind::Edit: n = {"vídeo editado", "vídeos editados", false}; break;
    case MediaKind::Screenshot: n = {"captura", "capturas", true}; break;
    }
  std::string text = r.deleted == 1 ? std::string("se borró ") + (n.female ? "una " : "un ") + n.one +
                                           (n.female ? " antigua" : " antiguo")
                                     : "se borraron " + std::to_string(r.deleted) + " " + n.many +
                                           (n.female ? " antiguas" : " antiguos");
  return text + " (" + FormatBytes(r.freedBytes) + ")";
}

} // namespace

void App::ReportStorageLimit(const LimitResult& r) {
  std::string limit = std::to_string(r.limitBytes >> 30) + " GB";
  const std::string old = "Más de " + std::to_string(r.maxAgeDays) + (r.maxAgeDays == 1 ? " día" : " días");
  if (r.blocked) {
    // Lo que se borró por antiguo antes de ver que el límite no puede borrar más.
    if (r.agedDeleted > 0) ToastAfter(old + ": " + DeletedText(r));
    // Una vez: sale otra vez si cambia algo (el límite, los favoritos, lo guardado).
    bool favorites = r.favoriteBytes > r.limitBytes;
    std::string key = (favorites ? "fav " : "new ") + std::to_string(r.limitBytes) + " " +
                      std::to_string(favorites ? r.favoriteBytes : r.keptBytes);
    if (key == limitWarned_) return;
    limitWarned_ = key;
    if (favorites)
      ToastAfter("Tus favoritos ya ocupan " + FormatBytes(r.favoriteBytes) + ", más que el límite de espacio (" +
                     limit + "), así que no se borra nada. Sube el límite en Ajustes > Almacenamiento o quita "
                     "alguna estrella.",
                 true);
    else if (r.favoriteBytes == 0)
      ToastAfter("Lo más reciente ya ocupa más que el límite de espacio (" + limit +
                     "), así que por ahora no se borra nada. Si quieres conservarlo, márcalo como favorito.",
                 true);
    else
      ToastAfter("Tus favoritos y lo más reciente ya ocupan más que el límite de espacio (" + limit +
                     "), así que por ahora no se borra nada. Sube el límite en Ajustes > Almacenamiento o quita "
                     "alguna estrella.",
                 true);
    return;
  }
  limitWarned_.clear();
  if (r.deleted == 0) return;
  if (r.agedDeleted == r.deleted) ToastAfter(old + ": " + DeletedText(r));
  else if (r.agedDeleted > 0) ToastAfter(old + " y límite de espacio (" + limit + "): " + DeletedText(r));
  else ToastAfter("Límite de espacio (" + limit + "): " + DeletedText(r));
}

// ---------------------------------------------------------------------- UI

void App::Toast(const std::string& text, bool error, NoticeKind kind) { Notify(MakeNotice(text, error, kind)); }

void App::Notify(Notice notice, bool continuation) {
  // Crear la ventana del aviso cambia el contexto de ImGui: nunca en mitad
  // del frame de otra ventana.
  if (inFrame_) {
    Defer([this, notice, continuation] { Notify(notice, continuation); });
    return;
  }
  // Los errores y lo de poco disco se ven aunque los avisos estén apagados.
  if (!notice.error && !notice.warning && !config_.showNotifications && !previewing_) return;
  const ULONGLONG now = GetTickCount64();
  const bool visible = toast_.Visible() && now < toastUntil_;
  // «Guardando clip» pasa a «Clip guardado» (o al error) en la misma tarjeta:
  // no vuelve a entrar, solo cambia lo de dentro y suena su animación.
  const bool same = visible && (continuation || (toastNotice_.kind == NoticeKind::ClipSaving &&
                                                 (notice.kind == NoticeKind::Clip || notice.error)));
  if (!same) {
    toastShownAt_ = now;
    // En el monitor donde se está jugando (o donde está el panel), no siempre
    // en el principal.
    toastMonitor_ = menu_.Visible() ? menuMonitor_ : MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
  }
  if (!continuation || toastNotice_.kind != notice.kind) toastPhaseAt_ = now;
  toastNotice_ = std::move(notice);
  // Más tiempo para leer los largos y para los errores. «Guardando clip» se
  // queda hasta que el archivo está escrito (lo sustituye «Clip guardado»).
  size_t chars = 0;
  for (unsigned char ch : toastNotice_.title + toastNotice_.detail) chars += (ch & 0xC0) != 0x80;
  ULONGLONG ms = kToastMs + ULONGLONG(chars > 90 ? (chars - 90) / 40 : 0) * 1200 + (toastNotice_.error ? 1500 : 0);
  if (toastNotice_.kind == NoticeKind::ClipSaving) ms = 10000;
  toastUntil_ = now + ms;
  toastTotalMs_ = ms;
  toastPlacedHeight_ = 0;
  PlaceToast();
}

void App::PlaceToast(float heightPx) {
  HMONITOR monitor = toastMonitor_ ? toastMonitor_ : MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi)) {
    monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(monitor, &mi);
  }
  float scale = MonitorScale(monitor);
  // Alto estimado por letras hasta que el primer frame mide el de verdad.
  int w = int(kToastWidth * scale), margin = int(20 * scale);
  int h = heightPx > 0 ? int(heightPx) : int(std::ceil(EstimateToastHeight(config_, toastNotice_) * scale));
  h = std::min<int>(h, (mi.rcWork.bottom - mi.rcWork.top) / 2);
  // Esquina de Ajustes > Avisos.
  const RECT& work = mi.rcWork;
  const std::string& corner = config_.toastPosition;
  bool left = corner == "top-left" || corner == "bottom-left";
  bool bottom = corner == "bottom-left" || corner == "bottom-right";
  LONG x = left ? work.left + margin : work.right - margin - w;
  LONG y = bottom ? work.bottom - margin - h : work.top + margin;
  // Con el panel abierto en esa esquina (se puede mover), el aviso se pone a
  // su lado en vez de taparlo.
  RECT r{x, y, x + w, y + h}, overlap{};
  if (menu_.Visible() && menuHitValid_ && IntersectRect(&overlap, &r, &menuHitScreen_)) {
    const bool panelOnLeft = menuHitScreen_.left + menuHitScreen_.right < work.left + work.right;
    x = panelOnLeft ? menuHitScreen_.right + margin : menuHitScreen_.left - margin - w;
    r = {x, y, x + w, y + h};
  }
  const Style& style = StyleFor(FindTheme(config_.theme), config_.themeStyle);
  toast_.SetFonts(style.body, style.display);
  if (!toast_.Show(r, false)) return;
  // "Clip guardado" tiene que verse también encima de un juego topmost.
  FullscreenInfo under = menu_.Visible() ? menuScreen_ : QueryFullscreen(GetForegroundWindow(), false);
  toast_.StayAbove(under.fullscreen ? under.window : nullptr);
}

void App::ToastAfter(const std::string& text, bool error) {
  if (!toast_.Visible() || GetTickCount64() >= toastUntil_ || toastNotice_.kind == NoticeKind::ClipSaving) {
    Toast(text, error);
    return;
  }
  Notice joined = toastNotice_;
  std::string& detail = joined.detail;
  if (!detail.empty() && detail.back() != '.') detail += '.';
  detail += (detail.empty() ? "" : " ") + text;
  joined.error = joined.error || error;
  Notify(std::move(joined), true);
}

void App::PreviewToast() {
  previewing_ = true;
  Notify({NoticeKind::ClipSaving, "Guardando clip", "Los últimos " + ui::FormatClipLength(config_.clipSeconds)});
  previewing_ = false;
  // El archivo «tarda» lo que suele tardar uno de verdad.
  SetTimer(hwnd_, kTimerPreview, 900, nullptr);
}

void App::AnnounceGame() {
  if (!game_ || !GameActive() || game_->processId == announcedGamePid_) return;
  if (!config_.notifyGame || !config_.replayEnabled || !pipeline_.Running()) return;
  // Navegadores y reproductores a pantalla completa no son juegos.
  std::string exe = game_->exeName;
  for (auto& ch : exe) ch = char(tolower(static_cast<unsigned char>(ch)));
  static const char* kMediaApps[] = {"chrome",   "msedge",       "firefox",     "opera",  "brave",   "vivaldi",
                                     "iexplore", "vlc",          "mpc-hc",      "mpc-hc64", "mpc-be", "mpc-be64",
                                     "mpv",      "potplayermini", "potplayermini64", "wmplayer", "kodi", "spotify",
                                     "discord",  "powerpnt",     "netflix",     "obs64"};
  announcedGamePid_ = game_->processId;
  for (const char* app : kMediaApps)
    if (exe == app) return;
  const std::string& key = config_.hotkeys.saveClip;
  Notify({NoticeKind::Game, "Replay listo en " + game_->exeName,
          (key.empty() ? std::string("Guarda") : key + " guarda") + " los últimos " +
              ui::FormatClipLength(config_.clipSeconds)});
}

void App::CheckUpdatesQuietly() {
  const ULONGLONG now = GetTickCount64();
  const UpdateStatus status = updater_.Status();
  if (quietCheckPending_) {
    if (status.state == UpdateStatus::State::Checking) return;
    quietCheckPending_ = false;
    // Lo que encuentra el botón de Ajustes ya se ve allí: solo se avisa de
    // lo que encontró la comprobación automática.
    if (status.state == UpdateStatus::State::Available) foundVersion_ = status.latest;
  }
  // Una versión nueva se avisa una vez, y no en mitad de una partida: espera
  // a que no haya juego delante ni grabación.
  if (!foundVersion_.empty() && foundVersion_ != announcedVersion_) {
    if (status.state != UpdateStatus::State::Available) {
      foundVersion_.clear(); // ya se está descargando o instalando desde Ajustes
      return;
    }
    if (GameActive() || pipeline_.Recording()) return;
    announcedVersion_ = foundVersion_;
    Notify({NoticeKind::Update, "Versión nueva: " + status.latest,
            status.portable || status.unverified ? "Descárgala desde Ajustes > General > Actualizaciones"
                            : "Instálala desde Ajustes > General > Actualizaciones. Tus ajustes se conservan"});
    return;
  }
  // Sin avisos no hay a quién contárselo: tampoco se consulta GitHub.
  if (!config_.showNotifications || !config_.notifyUpdates || now < nextUpdateCheck_) return;
  nextUpdateCheck_ = now + 12ull * 60 * 60 * 1000;
  // Lo que el usuario ya está haciendo en Ajustes manda.
  if (status.state != UpdateStatus::State::Idle && status.state != UpdateStatus::State::UpToDate &&
      status.state != UpdateStatus::State::Error)
    return;
  updater_.Check(true);
  quietCheckPending_ = true;
}

void App::SyncModels() {
  menuModel_.config = config_;
  appModel_.config = config_;
  if (appWindow_.Visible()) appWindow_.Invalidate();
}

void App::RefreshMenuModel() {
  menuModel_.stats = pipeline_.Stats();
  menuModel_.replayActive = pipeline_.Running() && config_.replayEnabled;
  menuModel_.gameDetected = GameActive();
  menuModel_.gameName = game_ ? game_->exeName : "";
}

// Los últimos clips y grabaciones para el panel (también los de las carpetas
// de cada juego). La grabación en curso todavía no es un archivo terminado.
void App::RefreshPanelClips() {
  menuModel_.outputDir = ResolveOutputDir(config_);
  menuModel_.clips.clear();
  std::string recording = pipeline_.Recording() ? pipeline_.Stats().recordPath : std::string();
  for (const auto& item : library_->Scan(menuModel_.outputDir, kPanelClips + 1, true)) {
    if (item.path == recording) continue;
    if (menuModel_.clips.size() >= kPanelClips) break;
    menuModel_.clips.push_back(ClipFile{item.path, item.name, item.size, item.when});
  }
}

void App::RenderOverlays() {
  ULONGLONG now = GetTickCount64();
  // GPU reiniciada o DPI cambiado con el menú abierto: se recrea en el mismo
  // sitio, con el mismo modo de foco y otra vez sobre el juego (PlaceMenu
  // llama a StayAbove). Se sube la generación para que un aviso de cierre
  // publicado para la ventana anterior no cierre la nueva.
  if (menu_.Visible() && menu_.Lost()) {
    const bool hadFocus = GetForegroundWindow() == menu_.Hwnd();
    menu_.Hide();
    ++menuGeneration_;
    // Para el foco es como abrirlo otra vez: el juego pudo volver a minimizarse.
    menuOpenedAt_ = GetTickCount64();
    menuFocusChecked_ = false;
    gameRestored_ = false;
    PlaceMenu(menuActivated_);
    if (menu_.Visible()) SyncKeyboardHook(); // la ventana es otra
    else EndMenuSession(hadFocus);           // no se pudo recrear: se cierra del todo
  }
  if (toast_.Visible() && toast_.Lost()) toast_.Hide();
  if (menu_.Visible()) {
    if (now - lastStats_ > 1000) {
      lastStats_ = now;
      RefreshMenuModel();
    }
    UpdateMenuDrag();
    inFrame_ = true;
    menu_.Render([this] { DrawPanel(menu_, menuModel_, menuActions_); });
    inFrame_ = false;
    if (menuModel_.panelHeight > 0) {
      // Donde DrawPanel puso el panel en este frame (el lateral llena la franja).
      LONG top = std::max<LONG>(0, LONG(std::floor(menuModel_.panelTop)));
      LONG bottom = std::min<LONG>(LONG(menu_.Height()), top + LONG(std::ceil(menuModel_.panelHeight)));
      RECT hit{0, top, LONG(menu_.Width()), bottom};
      menu_.SetHitRegion(&hit);
      RECT win{};
      if (GetWindowRect(menu_.Hwnd(), &win)) {
        menuHitScreen_ = {win.left + hit.left, win.top + hit.top, win.left + hit.right, win.top + hit.bottom};
        menuHitValid_ = true;
      }
    }
  }
  if (menu_.Visible()) {
    // Un frame largo (disco lento) puede pasar del tiempo que Windows da a un
    // gancho de bajo nivel, y entonces lo quita sin avisar: se vuelve a poner.
    ULONGLONG frameMs = GetTickCount64() - now;
    if (frameMs >= 200 && keyboard_.Installed()) {
      Log("Panel: frame de %llu ms; se reinstala el gancho de teclado", frameMs);
      keyboard_.Reinstall();
    }
    WatchGameWhileOpen();
    PollClickOutside();
  }
  if (toast_.Visible()) {
    if (now >= toastUntil_) {
      toast_.Hide();
    } else {
      float remaining = float(toastUntil_ - now);
      ToastFrame frame;
      frame.notice = &toastNotice_;
      frame.age = float(now - toastShownAt_) / 1000.0f;
      frame.phaseAge = float(now - toastPhaseAt_) / 1000.0f;
      frame.life = remaining / float(toastTotalMs_);
      frame.fade = remaining < 400 ? remaining / 400.0f : 1.0f;
      frame.fromRight = config_.toastPosition == "top-right" || config_.toastPosition == "bottom-right";
      frame.fromBottom = config_.toastPosition == "bottom-right" || config_.toastPosition == "bottom-left";
      float needed = 0;
      inFrame_ = true;
      toast_.Render([&] { needed = DrawToast(toast_, config_, frame); });
      inFrame_ = false;
      // El alto estimado por letras no era el bueno: se recoloca con el medido.
      if (needed > 0 && std::fabs(needed - float(toast_.Height())) > 1.5f && int(needed) != toastPlacedHeight_) {
        toastPlacedHeight_ = int(needed);
        PlaceToast(needed);
      }
    }
  }
  RunDeferred();
}

void App::RunDeferred() {
  if (deferred_.empty()) return;
  KeyboardHook::Pause pause(keyboard_); // las acciones pueden tardar (reiniciar la captura)
  auto actions = std::move(deferred_);
  deferred_.clear();
  for (auto& action : actions) action();
}

// ------------------------------------------------ el panel sobre el juego

bool App::ShouldActivateMenu(const FullscreenInfo& screen) {
  // Sin ventana delante (menú de la bandeja, escritorio) o con una ventana
  // normal: el panel coge el foco como cualquier aplicación.
  if (!screen.window || !screen.fullscreen) return true;
  // Pantalla completa exclusiva de Direct3D (según el shell): mientras el juego
  // tenga el foco no se dibuja nada encima. Al perderlo sale del modo y al
  // recuperarlo vuelve.
  if (screen.exclusive) {
    Log("Panel: el juego está en pantalla completa exclusiva (D3D); el panel coge el foco para verse");
    return true;
  }
  // El juego encierra el cursor en una parte del monitor (ClipCursor): el panel
  // quedaría fuera de su alcance y sin quitarle el foco no se podría hacer
  // clic. Encerrarlo en el monitor entero no cuenta: el panel está dentro.
  // Un cursor escondido tampoco: el juego lo esconde solo dentro de su ventana
  // (GLFW con SetCursor(NULL) desde su WM_SETCURSOR; el contador de ShowCursor
  // es del hilo que lo llama) y sobre el panel Windows pone la flecha. Geometry
  // Dash lo esconde durante un nivel, y ahí el panel no debe quitarle el foco.
  RECT clip{};
  if (GetClipCursor(&clip)) {
    const RECT& m = screen.bounds;
    if (clip.right - clip.left < m.right - m.left || clip.bottom - clip.top < m.bottom - m.top) {
      Log("Panel: el juego encierra el cursor en %ldx%ld; el panel coge el foco para que se pueda usar",
          clip.right - clip.left, clip.bottom - clip.top);
      return true;
    }
  }
  // Juego a pantalla completa con el cursor libre (Geometry Dash y la mayoría):
  // el panel se abre sin quitarle el foco. Si se lo quitara, los juegos hechos
  // con GLFW se minimizan y devuelven la resolución del escritorio.
  Log("Panel: abierto sobre el juego sin quitarle el foco");
  return false;
}

std::vector<KeyboardHook::Hotkey> App::PassThroughHotkeys() const {
  std::vector<KeyboardHook::Hotkey> keys;
  for (int i = 0; i < kHotkeyCount; ++i) {
    UINT mods = 0, vk = 0;
    if (ParseHotkey(HotkeyText(config_.hotkeys, i), &mods, &vk)) keys.push_back({mods, vk});
  }
  return keys;
}

void App::SyncKeyboardHook() {
  if (menu_.Visible() && !menuActivated_) keyboard_.Install(menu_.Hwnd(), previousForeground_, PassThroughHotkeys());
  else keyboard_.Uninstall();
}

// Con el panel abierto con foco: el foco pudo irse a otra ventana en el cambio
// (se vuelve a pedir una vez) y el juego pudo minimizarse al perderlo (GLFW):
// se restaura detrás del panel sin activarlo, solo mientras el foco lo tenga
// el panel (si el usuario se fue a otra aplicación, no se toca nada).
void App::WatchGameWhileOpen() {
  HWND menu = menu_.Hwnd();
  ULONGLONG since = GetTickCount64() - menuOpenedAt_;
  if (menuActivated_ && !menuFocusChecked_ && since >= 100) {
    menuFocusChecked_ = true;
    HWND fg = GetForegroundWindow();
    if (fg != menu && fg != previousForeground_) {
      Log("Panel: otra ventana cogió el foco al abrir; se vuelve a pedir");
      SetForegroundWindow(menu);
    }
  }
  if (!gameRestored_ && previousForeground_ && IsWindow(previousForeground_) && IsIconic(previousForeground_) &&
      GetForegroundWindow() == menu) {
    gameRestored_ = true;
    Log("Panel: el juego se minimizó al perder el foco; se restaura detrás del panel");
    ShowWindow(previousForeground_, SW_SHOWNOACTIVATE);
  }
}

// El panel sigue al cursor mientras se arrastra (lo empieza DrawPanel). Se
// mide en coordenadas de pantalla: las del ratón de ImGui son de la ventana,
// que se está moviendo. Lateral: solo en horizontal; compacto: también en
// vertical dentro de la franja. A menos de 16 px de un borde se pega a él.
// La ventana se mueve sin activarse (Move), así que el juego no pierde el foco.
void App::UpdateMenuDrag() {
  if (!menuModel_.dragging && !menuDragActive_) return;
  // Soltado. También si ImGui no se enteró: sin activar, la captura del ratón
  // solo vale sobre la ventana, y soltando fuera (el compacto se para a 18 px
  // del borde, el cursor no) el WM_LBUTTONUP va al juego. Entonces se le dice
  // a ImGui o el siguiente clic en el panel se perdería.
  if (!menuModel_.dragging || !AnyMouseButtonDown()) {
    if (menuModel_.dragging) menu_.ReleaseMouseButtons();
    EndMenuDrag();
    return;
  }
  RECT win{};
  POINT p{};
  if (!GetWindowRect(menu_.Hwnd(), &win) || !GetCursorPos(&p)) return;
  if (!menuDragActive_) {
    menuDragActive_ = true;
    menuDragFrom_ = {win.left + LONG(menuModel_.dragFromX), win.top + LONG(menuModel_.dragFromY)};
    menuDragLeft_ = win.left;
    menuDragTop_ = win.top + LONG(std::floor(menuModel_.panelTop));
  }
  if (p.x == menuDragFrom_.x && p.y == menuDragFrom_.y) return; // un clic sin mover no cambia nada
  const MenuArea a = MenuPlacement();
  const LONG snap = LONG(16 * MonitorScale(menuMonitor_));
  auto place = [snap](LONG want, LONG lo, LONG hi) {
    want = std::clamp(want, lo, hi);
    if (want - lo < snap) return lo;
    if (hi - want < snap) return hi;
    return want;
  };
  Config& c = menuModel_.config;
  const bool lateral = IsLateralPanel(c);
  const LONG minX = a.area.left + a.margin;
  const LONG freeX = std::max<LONG>(0, a.area.right - a.area.left - 2 * a.margin - a.width);
  if (freeX > 0) {
    const LONG left = place(menuDragLeft_ + p.x - menuDragFrom_.x, minX, minX + freeX);
    (lateral ? c.panelLateralX : c.panelCompactX) = float(left - minX) / float(freeX);
  }
  if (!lateral) {
    // La franja ya descuenta el margen: el panel va de su borde de arriba al de abajo.
    const LONG freeY = std::max<LONG>(0, LONG(menu_.Height()) - LONG(std::ceil(menuModel_.panelHeight)));
    if (freeY > 0) {
      const LONG top = place(menuDragTop_ + p.y - menuDragFrom_.y, win.top, win.top + freeY);
      c.panelCompactY = float(top - win.top) / float(freeY);
    }
  }
  RECT to = MenuRect();
  if (to.left != win.left || to.top != win.top) menu_.Move(to);
}

// Fin del arrastre: la posición nueva queda en config.json y el panel se abre
// ahí la próxima vez.
void App::EndMenuDrag() {
  menuModel_.dragging = false;
  if (!menuDragActive_) return;
  menuDragActive_ = false;
  const Config& m = menuModel_.config;
  if (m.panelLateralX == config_.panelLateralX && m.panelCompactX == config_.panelCompactX &&
      m.panelCompactY == config_.panelCompactY)
    return;
  config_.panelLateralX = m.panelLateralX;
  config_.panelCompactX = m.panelCompactX;
  config_.panelCompactY = m.panelCompactY;
  // Con Ajustes a medio editar, su copia de trabajo también la recibe si no
  // la había tocado: así «Devolverlo a su sitio» se activa y lo que pone se
  // aplica (MergeEdits solo aplica lo que difiere de base).
  SettingsModel& s = appModel_.settings;
  auto follow = [](float& edited, float& base, float now) {
    if (edited == base) edited = base = now;
  };
  follow(s.config.panelLateralX, s.base.panelLateralX, config_.panelLateralX);
  follow(s.config.panelCompactX, s.base.panelCompactX, config_.panelCompactX);
  follow(s.config.panelCompactY, s.base.panelCompactY, config_.panelCompactY);
  SaveConfig(config_);
  SyncModels();
  Log("Panel %s movido: x %.2f, y %.2f", IsLateralPanel(config_) ? "lateral" : "compacto",
      double(IsLateralPanel(config_) ? config_.panelLateralX : config_.panelCompactX),
      double(IsLateralPanel(config_) ? 0.0f : config_.panelCompactY));
}

// Un clic fuera del panel lo cierra; el clic llega también a lo que haya
// debajo. Se sondea el botón en cada frame en vez de usar un gancho de ratón,
// que metería la latencia de nuestro frame a todo el ratón del sistema.
void App::PollClickOutside() {
  const bool down = AnyMouseButtonDown();
  const bool pressed = down && !mouseWasDown_;
  mouseWasDown_ = down;
  if (!pressed || !menuHitValid_) return;
  POINT p{};
  if (!GetCursorPos(&p) || PtInRect(&menuHitScreen_, p)) return;
  CloseMenu();
}

// ------------------------------------------------------ la ventana de Spectre

void App::OpenApp(std::optional<AppPage> page) {
  const bool wasOpen = appWindow_.Visible();
  // Las letras del diseño del tema antes de crearla: el atlas se hace una vez.
  const Style& style = StyleFor(FindTheme(config_.theme), config_.themeStyle);
  appWindow_.SetFonts(style.body, style.display);
  if (!appWindow_.Open()) {
    Toast("No se pudo abrir la ventana de Spectre. Mira spectre.log", true);
    return;
  }
  if (!wasOpen) {
    appUi_.OnOpened(hwnd_, WM_SPECTRE_MEDIA);
    appModel_.page = AppPage::Home;
    BeginSettingsEdit();
    RefreshAppModel();
    RescanLibrary();
  }
  if (page) appModel_.page = *page;
  appWindow_.Invalidate();
}

void App::CloseApp() {
  if (!appWindow_.Visible()) return;
  CancelHotkeyCapture(appModel_.settings, appActions_.settings);
  appUi_.OnClosed();
  appWindow_.Close();
  // Cerrada no se queda con nada: ni la lista de la biblioteca ni los dispositivos.
  appModel_.items.clear();
  appModel_.items.shrink_to_fit();
  appModel_.itemsLoaded = false;
  appModel_.settings = SettingsModel{};
  appModel_.page = AppPage::Home;
}

void App::AppCloseRequested() {
  if (!config_.closeToTray) {
    Quit();
    return;
  }
  const bool discarded = appModel_.settings.dirty;
  CloseApp();
  if (!config_.trayHintShown) {
    config_.trayHintShown = true;
    SaveConfig(config_);
    SyncModels();
    Toast("Spectre sigue en la bandeja y el replay sigue funcionando. Para salir: clic derecho en el icono, "
          "Salir de Spectre.");
  } else if (discarded) {
    Toast("Ajustes: cambios sin aplicar descartados");
  }
}

void App::RenderApp() {
  if (!appWindow_.Visible()) return;
  if (appWindow_.Lost()) {
    // La GPU se reinició (driver nuevo, TDR): se recrea la ventana donde
    // estaba, en la misma página y sin robar el foco si no lo tenía. No en
    // mitad de un arrastre: destruirla dentro del bucle modal de Windows es
    // buscarse problemas; al soltar llega otro frame.
    if (appWindow_.Sizing()) return;
    const bool focused = appWindow_.Focused();
    const AppPage page = appModel_.page == AppPage::Viewer ? appModel_.viewBack : appModel_.page;
    CancelHotkeyCapture(appModel_.settings, appActions_.settings);
    appUi_.OnClosed();
    appWindow_.Close();
    if (!appWindow_.Open(focused)) {
      Toast("No se pudo volver a abrir la ventana de Spectre tras reiniciarse la GPU", true);
      CloseApp();
      return;
    }
    appUi_.OnOpened(hwnd_, WM_SPECTRE_MEDIA);
    appModel_.page = page;
    return;
  }
  ULONGLONG now = GetTickCount64();
  if (now - lastAppStats_ >= 250) {
    lastAppStats_ = now;
    RefreshAppModel();
  }
  SettingsModel& s = appModel_.settings;
  if (appModel_.page == AppPage::Settings) {
    s.update = updater_.Status();
    if (appModel_.settingsPage == SettingsPage::Audio) pipeline_.AudioLevels(&s.systemLevel, &s.micLevel);
  }
  // Grabando un atajo y el usuario se va a otra ventana: se cancela (si no,
  // los atajos globales se quedarían sin registrar).
  if (s.capturing >= 0 && !appWindow_.Focused()) CancelHotkeyCapture(s, appActions_.settings);
  inFrame_ = true;
  appWindow_.Render([this] { appUi_.Draw(appWindow_, appModel_, appActions_); });
  inFrame_ = false;
  // El tema no espera a Aplicar: se guarda al elegirlo y lo usan enseguida el
  // panel (Alt+Z) y los avisos, no solo esta ventana. Tampoco cuenta como
  // cambio sin aplicar.
  if (s.config.theme != config_.theme || s.config.themeMotion != config_.themeMotion ||
      s.config.themeStyle != config_.themeStyle || s.config.themeBackdrop != config_.themeBackdrop) {
    config_.theme = s.base.theme = s.config.theme;
    config_.themeMotion = s.base.themeMotion = s.config.themeMotion;
    config_.themeStyle = s.base.themeStyle = s.config.themeStyle;
    config_.themeBackdrop = s.base.themeBackdrop = s.config.themeBackdrop;
    SaveConfig(config_);
    SyncModels();
  }
  // "Sin aplicar" solo si de verdad difiere de lo que había (cambiar algo y
  // volverlo a dejar como estaba no cuenta).
  if (appModel_.page == AppPage::Settings || s.dirty) s.dirty = !SameConfig(s.config, s.base);
}

void App::RefreshAppModel() {
  appModel_.config = config_;
  appModel_.stats = pipeline_.Stats();
  appModel_.replayActive = pipeline_.Running() && config_.replayEnabled;
  appModel_.gameDetected = GameActive();
  appModel_.gameName = game_ ? game_->exeName : "";
  appModel_.exportPath = exporter_.Busy() ? exporter_.Input() : std::string();
  appModel_.exportPercent = exporter_.Busy() ? exporter_.Percent() : -1;
  appModel_.exportBatchTotal = exportBatchTotal_;
  appModel_.exportBatchDone = int(exportBatchOutputs_.size()) + exportBatchFailed_;
  appModel_.editRenderPath = editWorker_.Busy() ? editWorker_.Input() : std::string();
  appModel_.editPercent = editWorker_.Busy() ? editWorker_.Percent() : -1;
  SettingsModel& s = appModel_.settings;
  s.stats = appModel_.stats;
  s.hotkeyError = hotkeyError_;
  ULONGLONG now = GetTickCount64();
  if (appModel_.page == AppPage::Settings && appModel_.settingsPage == SettingsPage::Performance &&
      now - lastAppUsage_ >= 1000) {
    lastAppUsage_ = now;
    s.usage = SampleProcessUsage();
  }
  // Sin cambios propios, Ajustes enseña lo que hay ahora (un atajo o el panel
  // pudieron cambiar el micrófono, la duración...).
  if (!s.dirty && s.capturing < 0 && !SameConfig(s.base, config_)) {
    s.config = config_;
    s.base = config_;
  }
}

void App::RescanLibrary() {
  // La lista nueva sustituye a la que está dibujando el frame en curso: nunca
  // en mitad de uno (renombrar se llama desde dentro).
  if (inFrame_) {
    Defer([this] { RescanLibrary(); });
    return;
  }
  std::string dir = ResolveOutputDir(config_);
  std::vector<MediaItem> items = library_->Scan(dir);
  // La grabación en curso todavía no es un archivo terminado.
  if (pipeline_.Recording()) {
    std::string recording = pipeline_.Stats().recordPath;
    items.erase(std::remove_if(items.begin(), items.end(), [&](const MediaItem& i) { return i.path == recording; }),
                items.end());
  }
  StorageInfo storage = SummarizeStorage(items, dir);
  appModel_.items = std::move(items);
  appModel_.itemsLoaded = true;
  appModel_.outputDir = dir;
  appModel_.freeBytes = storage.freeBytes;
  appModel_.settings.storage = storage;
  appModel_.settings.outputDir = dir;
  appWindow_.Invalidate();
}

void App::BeginSettingsEdit() {
  SettingsModel& s = appModel_.settings;
  CancelHotkeyCapture(s, appActions_.settings);
  // La lista de encoders es la del códec elegido: si se descarta otro códec, vuelve.
  if (s.devicesLoaded && s.config.codec != config_.codec) s.encoders = CandidateEncoders(config_.codec);
  s.config = config_;
  s.base = config_;
  s.dirty = false;
  s.hotkeyError = hotkeyError_;
  s.outputDir = ResolveOutputDir(config_);
}

void App::ApplySettings() {
  SettingsModel& s = appModel_.settings;
  CancelHotkeyCapture(s, appActions_.settings);
  // Solo lo que se cambió en Ajustes: lo que cambió mientras tanto desde el
  // panel o un atajo (el micrófono, la duración) no se pisa.
  ApplyConfig(MergeEdits(config_, s.base, s.config));
  BeginSettingsEdit();
}

void App::LoadDevices() {
  SettingsModel& s = appModel_.settings;
  s.monitors = EnumerateMonitors();
  s.outputs = EnumerateAudioDevices(false);
  s.inputs = EnumerateAudioDevices(true);
  s.cameras = EnumerateCameras();
  s.encoders = CandidateEncoders(s.config.codec);
  s.devicesLoaded = true;
}

void App::ChooseOutputFolder() {
  using Microsoft::WRL::ComPtr;
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    Toast("No se pudo abrir el selector de carpetas", true);
    return;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dialog->SetTitle(L"Carpeta de los clips, grabaciones y capturas");
  SettingsModel& s = appModel_.settings;
  std::wstring current = Widen(s.config.outputDir.empty() ? ResolveOutputDir(config_) : s.config.outputDir);
  ComPtr<IShellItem> start;
  if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(&start))))
    dialog->SetFolder(start.Get());
  if (dialog->Show(appWindow_.Visible() ? appWindow_.Hwnd() : hwnd_) != S_OK) return; // cancelado
  ComPtr<IShellItem> item;
  PWSTR path = nullptr;
  if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
  s.config.outputDir = Narrow(path);
  CoTaskMemFree(path);
  appModel_.page = AppPage::Settings;
  appModel_.settingsPage = SettingsPage::Storage;
  appWindow_.Invalidate();
}

// ---------------------------------------------------------------- bandeja

void App::AddTrayIcon() {
  tray_ = {};
  tray_.cbSize = sizeof(tray_);
  tray_.hWnd = hwnd_;
  tray_.uID = 1;
  tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  tray_.uCallbackMessage = WM_SPECTRE_TRAY;
  tray_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
  if (!tray_.hIcon) tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(tray_.szTip, L"Spectre");
  Shell_NotifyIconW(NIM_ADD, &tray_);
}

void App::RemoveTrayIcon() { Shell_NotifyIconW(NIM_DELETE, &tray_); }

void App::ShowTrayMenu() {
  auto withKey = [](const wchar_t* text, const std::string& key) {
    std::wstring s = text;
    if (!key.empty()) s += L"\t" + Widen(key);
    return s;
  };
  HMENU menu = CreatePopupMenu();
  std::wstring panel = withKey(L"Abrir panel", config_.hotkeys.overlay);
  std::wstring clip = withKey(L"Guardar clip", config_.hotkeys.saveClip);
  std::wstring shortClip = withKey((L"Guardar clip corto (" +
                                    Widen(ui::FormatClipLength(std::min(config_.shortClipSeconds, config_.clipSeconds))) +
                                    L")").c_str(),
                                   config_.hotkeys.shortClip);
  std::wstring rec = withKey(pipeline_.Recording() ? L"Detener grabación" : L"Iniciar grabación",
                             config_.hotkeys.toggleRecording);
  std::wstring shot = withKey(L"Captura de pantalla", config_.hotkeys.screenshot);
  AppendMenuW(menu, MF_STRING, kCmdOpenApp, L"Abrir Spectre");
  AppendMenuW(menu, MF_STRING, kCmdOpenPanel, panel.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  const UINT replayOn = pipeline_.Running() && config_.replayEnabled ? 0 : MF_GRAYED;
  AppendMenuW(menu, MF_STRING | replayOn, kCmdSaveClip, clip.c_str());
  AppendMenuW(menu, MF_STRING | replayOn, kCmdShortClip, shortClip.c_str());
  AppendMenuW(menu, MF_STRING, kCmdRecord, rec.c_str());
  AppendMenuW(menu, MF_STRING, kCmdScreenshot, shot.c_str());
  AppendMenuW(menu, MF_STRING | (config_.replayEnabled ? MF_CHECKED : 0), kCmdReplay, L"Replay activado");
  AppendMenuW(menu, MF_STRING, kCmdFolder, L"Abrir carpeta de clips");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdQuit, L"Salir de Spectre");
  SetMenuDefaultItem(menu, kCmdOpenApp, FALSE);
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd_);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

// ------------------------------------------------------------- mensajes

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* self = nullptr;
  if (msg == WM_NCCREATE) {
    self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self) return self->HandleMessage(hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == taskbarCreated_ && taskbarCreated_ != 0) {
    AddTrayIcon(); // explorer se reinició
    return 0;
  }
  switch (msg) {
  case WM_HOTKEY: {
    KeyboardHook::Pause pause(keyboard_); // guardar un clip o grabar pueden tardar
    switch (int(wp)) {
    case kHotkeyOverlay: ToggleMenu(); break;
    case kHotkeySaveClip: SaveClip(); break;
    case kHotkeyShortClip: SaveClip(true); break;
    case kHotkeyRecord: ToggleRecording(); break;
    case kHotkeyReplay: ToggleReplay(); break;
    case kHotkeyScreenshot: CaptureScreenshot(); break;
    case kHotkeyMarker: AddMarker(); break;
    default: break;
    }
    return 0;
  }
  case WM_TIMER:
    // Un juego que se abre o una grabación que empieza frenan la edición o la
    // exportación que ya va.
    UpdateLiveLoad();
    // La grabación pudo acabar sola (disco lento o con errores): ni los ajustes ni la actualización pendientes se
    // quedan colgados.
    if (wp == kTimerHealth && restartPending_ && !pipeline_.Recording())
      UpdatePipeline(std::exchange(restartPending_, false));
    if (wp == kTimerHealth) InstallPendingUpdate();
    if (wp == kTimerHealth) CheckUpdatesQuietly();
    if (wp == kTimerHealth) {
      std::string stall = pipeline_.TakeCameraStall();
      if (!stall.empty()) Toast(stall, true);
    }
    if (wp == kTimerPreview) {
      KillTimer(hwnd_, kTimerPreview);
      previewing_ = true;
      Notify({NoticeKind::Clip, "Clip guardado", "Así se verá cuando guardes un clip"});
      previewing_ = false;
      if (config_.soundOnSave) PlayCue(Cue::Saved, config_.saveSound);
      return 0;
    }
    // Ajustes > Replay > Detener la grabación tras: se para sola y se guarda.
    if (wp == kTimerHealth && config_.recordLimitMinutes > 0 && pipeline_.Recording() &&
        pipeline_.Stats().recordSeconds >= double(config_.recordLimitMinutes) * 60.0) {
      KeyboardHook::Pause pause(keyboard_);
      Log("Grabación: llegó al límite de %d min, se detiene", config_.recordLimitMinutes);
      ToggleRecording();
      ToastAfter("Se detuvo sola al llegar a " + MinutesText(config_.recordLimitMinutes) +
                 " (Ajustes > Replay)");
      return 0;
    }
    if (wp == kTimerGame) {
      CheckGame();
    } else if (wp == kTimerHealth && pipeline_.Running() && pipeline_.CaptureLost()) {
      KeyboardHook::Pause pause(keyboard_); // reiniciar la captura tarda
      bool wasRecording = pipeline_.Recording();
      std::string path = wasRecording ? pipeline_.Stats().recordPath : std::string();
      int64_t stoppedAt = QpcNow();
      if (wasRecording) pipeline_.StopRecording();
      Log("Captura perdida: se reinicia");
      pipeline_.Stop();
      restartPending_ = false;
      if (wasRecording) {
        Notify({NoticeKind::RecordingSaved, "Grabación guardada", "La ventana capturada se cerró: " + FileNameOf(path)});
        OnRecordingStopped(path, stoppedAt);
      }
      UpdatePipeline();
      InstallPendingUpdate();
    }
    return 0;
  case WM_SPECTRE_NOTIFY: {
    std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
    bool error = wp != 0;
    const std::string& message = *text;
    Notice notice = MakeNotice(message, error);
    // El resultado de un «Guardando clip»: cuál era (largo o corto, cuánto),
    // por el nombre del archivo que trae el mensaje.
    std::pair<int, bool> clip{config_.clipSeconds, false};
    for (const char* prefix : {"Clip guardado: ", "No se pudo guardar el clip: "}) {
      if (!StartsWith(message, prefix)) continue;
      auto it = pendingClips_.find(message.substr(strlen(prefix)));
      if (it != pendingClips_.end()) {
        clip = it->second;
        pendingClips_.erase(it);
      }
    }
    if (!error && (StartsWith(message, "Clip guardado") || StartsWith(message, "Captura guardada") ||
                   StartsWith(message, "Recorte guardado") || StartsWith(message, "Unión guardada") ||
                   StartsWith(message, "Cámara lenta guardada") || StartsWith(message, "Fotograma guardado") ||
                   StartsWith(message, "Dividido en dos"))) {
      if (config_.soundOnSave && (StartsWith(message, "Clip") || StartsWith(message, "Captura")))
        PlayCue(Cue::Saved, config_.saveSound);
      if (StartsWith(message, "Clip guardado: ")) {
        const std::string name = message.substr(strlen("Clip guardado: "));
        if (clip.second) notice.title = "Clip corto guardado";
        notice.detail = name + " (" + ui::FormatClipLength(clip.first) + ")";
        // Ajustes > Avisos > Copiar el clip al guardarlo: listo para Ctrl+V en Discord.
        if (config_.copyClipOnSave) {
          auto it = std::find_if(recentNames_.rbegin(), recentNames_.rend(),
                                 [&](const std::string& p) { return FileNameOf(p) == name; });
          if (it != recentNames_.rend() && CopyFileToClipboard(hwnd_, *it))
            notice.detail += ". Copiado: pégalo con Ctrl+V";
        }
      }
      std::string note = LowDiskNote();
      if (!note.empty()) {
        notice.detail += note;
        notice.warning = true; // se ve aunque los avisos estén apagados
      }
    }
    Notify(std::move(notice));
    if (menu_.Visible()) RefreshPanelClips();
    if (appWindow_.Visible()) RescanLibrary();
    EnforceStorageLimit(); // algo nuevo en la carpeta
    return 0;
  }
  case WM_SPECTRE_EXPORT:
    if (wp == kExportDone) {
      FinishExport();
      StartQueuedExport(); // el siguiente de «Para Discord» de varios
    } else if (exporter_.Busy()) {
      appModel_.exportPercent = int(wp);
      appWindow_.Invalidate();
    }
    return 0;
  case WM_SPECTRE_EDIT:
    if (wp == kEditDone) {
      FinishEdit();
    } else if (editWorker_.Busy()) {
      appModel_.editPercent = int(wp);
      appWindow_.Invalidate();
    }
    return 0;
  case WM_SPECTRE_LIBRARY: {
    std::unique_ptr<LimitResult> result(reinterpret_cast<LimitResult*>(lp));
    if (result) ReportStorageLimit(*result);
    if (!result || result->deleted > 0) {
      if (menu_.Visible()) RefreshPanelClips();
      if (appWindow_.Visible()) RescanLibrary();
    }
    return 0;
  }
  case WM_SPECTRE_MEDIA:
    if (appWindow_.Visible()) {
      appUi_.TakeMediaResults(appWindow_, appModel_, appActions_);
      library_->Flush(); // las duraciones medidas, de una vez
    }
    return 0;
  case WM_SPECTRE_UPDATE_READY: {
    std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lp));
    installerPath_ = *path;
    // Nunca cerrar Spectre a mitad de una grabación, una edición o una
    // exportación: espera a que acaben.
    if (pipeline_.Recording()) Toast("La actualización se instalará al terminar la grabación");
    else if (editWorker_.Busy()) Toast("La actualización se instalará al terminar de guardar la edición");
    else if (ExportBusy()) Toast("La actualización se instalará al terminar la exportación");
    else InstallPendingUpdate();
    return 0;
  }
  case WM_SPECTRE_CLOSE_MENU:
    if (UINT(wp) == menuGeneration_) CloseMenu();
    return 0;
  case WM_DISPLAYCHANGE:
    // El juego cambió la resolución (o la restauró) con el panel abierto: el
    // monitor tiene otro tamaño y el panel se recoloca sin tocar el foco. Tras
    // el frame, por si el mensaje llega en mitad de uno.
    if (menu_.Visible()) Defer([this] { if (menu_.Visible()) menu_.Move(MenuRect()); });
    return 0;
  case WM_SPECTRE_SHOW_MENU:
    OpenMenu();
    return 0;
  case WM_SPECTRE_SHOW_APP:
    // Spectre ya estaba abierto y se volvió a abrir (menú Inicio, el exe).
    Defer([this] {
      CloseMenu(false);
      OpenApp();
    });
    return 0;
  case WM_SPECTRE_TRAY:
    if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
      ShowTrayMenu();
    } else if (LOWORD(lp) == WM_LBUTTONUP) {
      // Clic en el icono: la ventana de Spectre. El clic ya cerró el panel si
      // estaba abierto (PollClickOutside).
      Defer([this] {
        CloseMenu(false);
        OpenApp();
      });
    }
    return 0;
  case WM_COMMAND: {
    KeyboardHook::Pause pause(keyboard_);
    switch (LOWORD(wp)) {
    case kCmdOpenApp: Defer([this] { OpenApp(); }); break;
    case kCmdOpenPanel: OpenMenu(); break;
    case kCmdSaveClip: SaveClip(); break;
    case kCmdShortClip: SaveClip(true); break;
    case kCmdRecord: ToggleRecording(); break;
    case kCmdScreenshot: CaptureScreenshot(); break;
    case kCmdReplay: ToggleReplay(); break;
    case kCmdFolder: OpenPath(ResolveOutputDir(config_)); break;
    case kCmdQuit: Quit(); break;
    default: break;
    }
    return 0;
  }
  case WM_QUERYENDSESSION:
    return TRUE;
  case WM_ENDSESSION:
    // Apagado, cierre de sesión o el instalador actualizando Spectre: se cierra
    // bien el archivo de una grabación en curso y se sale.
    if (wp) {
      installerPath_.clear(); // nunca instalar mientras Windows cierra la sesión
      exporter_.Stop();
      editWorker_.Stop();
      pipeline_.Stop();
      WaitForPendingWrites(3000);
      Quit();
    }
    return 0;
  case WM_CLOSE:
    // La ventana principal es invisible: Alt+F4 sobre ella (queda en primer
    // plano tras usar el menú de la bandeja) no debe cerrarla. Para salir está
    // "Salir de Spectre".
    return 0;
  case WM_DESTROY:
    hwnd_ = nullptr;
    // Sin ventana no hay atajos ni icono en la bandeja: nunca quedarse como
    // proceso zombi.
    if (!quitting_) Quit();
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace spectre
