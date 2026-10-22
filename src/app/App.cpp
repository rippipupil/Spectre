#include "app/App.h"
#include "app/CrashHandler.h"
#include "app/Screenshot.h"
#include "app/Sound.h"

#include "common/Util.h"
#include "core/Mp4Writer.h"

#include <shellscalingapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
enum HotkeyId { kHotkeyOverlay = 1, kHotkeySaveClip, kHotkeyRecord, kHotkeyReplay, kHotkeyScreenshot, kHotkeyMarker };
constexpr int kHotkeyCount = 6;
enum TimerId { kTimerGame = 1, kTimerHealth };
enum TrayCommand {
  kCmdOpenApp = 100,
  kCmdOpenPanel,
  kCmdSaveClip,
  kCmdRecord,
  kCmdScreenshot,
  kCmdReplay,
  kCmdFolder,
  kCmdQuit = 199,
};

constexpr ULONGLONG kGameGraceMs = 15000; // el juego sigue "activo" 15 s después de perder el foco
constexpr ULONGLONG kToastMs = 2600;
constexpr size_t kPanelClips = 8; // el compacto enseña 3; el lateral, los que quepan
constexpr double kMaxMarkerAge = 5 * 60 + 10;          // segundos: el clip más largo y un margen
constexpr uint64_t kLowDiskBytes = uint64_t(1) << 30; // menos de 1 GB libre: se avisa al guardar

const std::string& HotkeyText(const HotkeyConfig& h, int index) {
  switch (index) {
  case 0: return h.overlay;
  case 1: return h.saveClip;
  case 2: return h.toggleRecording;
  case 3: return h.toggleReplay;
  case 4: return h.screenshot;
  default: return h.marker;
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

std::string FileNameOf(const std::string& path) { return Narrow(std::filesystem::path(Widen(path)).filename().wstring()); }

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
         a.separateTracks == b.separateTracks && a.cameraEnabled == b.cameraEnabled && a.cameraId == b.cameraId &&
         a.cameraCorner == b.cameraCorner && a.cameraSizePercent == b.cameraSizePercent &&
         a.maxBufferMB == b.maxBufferMB;
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
  appActions_.cancelExport = [this] { exporter_.Cancel(); };
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

  if (!RegisterHotkeys()) Toast(hotkeyError_, true);
  SetTimer(hwnd_, kTimerGame, 2000, nullptr);
  SetTimer(hwnd_, kTimerHealth, 1000, nullptr);
  CheckGame();
  UpdatePipeline();
  EnforceStorageLimit();
  if (updated) Toast("Spectre se ha actualizado a la versión " SPECTRE_VERSION ". Tus ajustes se conservan.");
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

void App::SaveClip() {
  if (!pipeline_.Running() || !config_.replayEnabled) {
    Toast("El replay no está activo", true);
    return;
  }
  std::string path = OutputName("Clip");
  // Las marcas de los últimos segundos van con el clip, que acaba justo ahora.
  int64_t now = QpcNow();
  std::vector<double> tail;
  for (int64_t at : markers_) {
    double ago = QpcToSeconds(now - at);
    if (ago >= 0 && ago <= double(config_.clipSeconds)) tail.push_back(ago);
  }
  library_->SetTailMarkers(path, std::move(tail));
  pipeline_.SaveClip(path, double(config_.clipSeconds));
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
    std::string path = pipeline_.Stats().recordPath;
    int64_t stoppedAt = QpcNow();
    pipeline_.StopRecording();
    if (config_.soundOnSave) PlayCue(Cue::Saved);
    std::string note = LowDiskNote();
    Toast("Grabación guardada: " + FileNameOf(path) + note, !note.empty());
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
    std::string note = LowDiskNote();
    Toast("Grabando. Pulsa " + config_.hotkeys.toggleRecording + " para detener" + (note.empty() ? "." : note),
          !note.empty());
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
  Toast(config_.replayEnabled ? "Replay activado" : "Replay desactivado");
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
  Config next = config_;
  next.cameraEnabled = !next.cameraEnabled;
  bool restarts = pipeline_.Running() && !pipeline_.Recording();
  ApplyConfig(next, true);
  if (!restartPending_)
    Toast(std::string(config_.cameraEnabled ? "Cámara activada" : "Cámara desactivada") +
          (restarts ? ". El replay empieza de cero." : ""));
}

void App::SetClipSeconds(int seconds) {
  Config next = config_;
  next.clipSeconds = seconds;
  ApplyConfig(next, true);
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
  if (config_.soundOnSave) PlayCue(Cue::Marker);
  Toast(pipeline_.Recording() ? "Marca añadida a la grabación y al replay" : "Marca añadida: saldrá en el próximo clip");
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

void App::ExportForDiscord(const std::string& path) {
  if (exporter_.Busy()) {
    Toast("Ya se está exportando otro vídeo: espera a que termine", true);
    return;
  }
  ExportOptions options;
  // Con un juego abierto se exporta sin prisa, para no quitarle CPU.
  options.gentle = GameActive() || pipeline_.Recording();
  HWND hwnd = hwnd_;
  exporter_.Start(
      path, options, [hwnd](int percent) { PostMessageW(hwnd, WM_SPECTRE_EXPORT, WPARAM(percent), 0); },
      [hwnd] { PostMessageW(hwnd, WM_SPECTRE_EXPORT, kExportDone, 0); });
  appModel_.exportPath = path;
  appModel_.exportPercent = 0;
  appWindow_.Invalidate();
  Toast("Exportando para Discord...");
}

void App::FinishExport() {
  ExportResult r;
  if (!exporter_.TakeResult(&r)) return;
  appModel_.exportPath.clear();
  appModel_.exportPercent = -1;
  appWindow_.Invalidate();
  if (r.cancelled) {
    Toast("Exportación cancelada");
    return;
  }
  if (!r.ok) {
    Toast(r.error, true);
    return;
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
  UpdatePipeline();

  const bool folderChanged = old.outputDir != config_.outputDir || old.folderPerGame != config_.folderPerGame;
  if (folderChanged && appWindow_.Visible()) RescanLibrary();
  if (old.storageLimitGB != config_.storageLimitGB || old.outputDir != config_.outputDir) EnforceStorageLimit();

  if (!hotkeysOk) Toast(hotkeyError_, true);
  else if (deferred) Toast("Se aplicará al terminar la grabación");
  else if (!quiet) Toast("Cambios aplicados");
}

void App::InstallPendingUpdate() {
  if (installerPath_.empty() || pipeline_.Recording()) return;
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
      return;
    }
  }
  UpdatePipeline();
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
  if (config_.storageLimitGB <= 0) {
    limitWarned_.clear();
    return;
  }
  // Una pasada cada vez: el hilo escanea la carpeta y borra lo más antiguo.
  static std::atomic<bool> running{false};
  if (running.exchange(true)) return;
  std::string folder = ResolveOutputDir(config_);
  uint64_t limit = uint64_t(config_.storageLimitGB) << 30;
  // La grabación en curso cuenta, pero no se toca.
  std::string recording = pipeline_.Recording() ? pipeline_.Stats().recordPath : std::string();
  std::shared_ptr<Library> library = library_;
  HWND hwnd = hwnd_;
  auto pending = std::make_shared<PendingWrite>(); // al salir se espera a que acabe
  std::thread([folder, limit, recording, library, hwnd, pending] {
    InstallThreadCrashHandler();
    auto result = std::make_unique<LimitResult>(library->EnforceLimit(folder, limit, recording));
    if (result->deleted > 0)
      Log("Límite de espacio: %d archivos borrados, %s liberados", result->deleted,
          FormatBytes(result->freedBytes).c_str());
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
  if (r.blocked) {
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
  if (r.deleted > 0) ToastAfter("Límite de espacio (" + limit + "): " + DeletedText(r));
}

// ---------------------------------------------------------------------- UI

void App::Toast(const std::string& text, bool error) {
  // Crear la ventana del aviso cambia el contexto de ImGui: nunca en mitad
  // del frame de otra ventana.
  if (inFrame_) {
    Defer([this, text, error] { Toast(text, error); });
    return;
  }
  if (!error && !config_.showNotifications) return;
  toastText_ = text;
  toastError_ = error;
  // En el monitor donde se está jugando (o donde está el panel), no siempre
  // en el principal.
  HMONITOR monitor = menu_.Visible() ? menuMonitor_
                                     : MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(monitor, &mi);
  float scale = MonitorScale(monitor);
  // Alto según el texto (unas 42 letras por línea): los errores largos no se cortan.
  size_t chars = 0;
  for (unsigned char ch : text) chars += (ch & 0xC0) != 0x80;
  int lines = std::max<int>(1, int((chars + 41) / 42));
  int w = int(400 * scale), h = int(float(std::max(64, 30 + lines * 22)) * scale), margin = int(20 * scale);
  toastUntil_ = GetTickCount64() + kToastMs + ULONGLONG(std::max(0, lines - 2)) * 1200 + (error ? 1500 : 0);
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
  if (!toast_.Show(r, false)) return;
  // "Clip guardado" tiene que verse también encima de un juego topmost.
  FullscreenInfo under = menu_.Visible() ? menuScreen_ : QueryFullscreen(GetForegroundWindow(), false);
  toast_.StayAbove(under.fullscreen ? under.window : nullptr);
}

void App::ToastAfter(const std::string& text, bool error) {
  if (!toast_.Visible() || GetTickCount64() >= toastUntil_ || toastText_.empty()) {
    Toast(text, error);
    return;
  }
  std::string joined = toastText_;
  if (joined.back() != '.') joined += '.';
  Toast(joined + " " + text, error || toastError_);
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
      float alpha = remaining < 400 ? remaining / 400.0f : 1.0f;
      inFrame_ = true;
      toast_.Render([&] { DrawToast(toast_, config_, toastText_, toastError_, alpha); });
      inFrame_ = false;
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
  // Soltado. También si ImGui no se enteró (otra ventana se quedó con la
  // captura del ratón y no llegó el WM_LBUTTONUP): ningún botón pulsado.
  if (!menuModel_.dragging || !AnyMouseButtonDown()) {
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
  std::wstring rec = withKey(pipeline_.Recording() ? L"Detener grabación" : L"Iniciar grabación",
                             config_.hotkeys.toggleRecording);
  std::wstring shot = withKey(L"Captura de pantalla", config_.hotkeys.screenshot);
  AppendMenuW(menu, MF_STRING, kCmdOpenApp, L"Abrir Spectre");
  AppendMenuW(menu, MF_STRING, kCmdOpenPanel, panel.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (pipeline_.Running() && config_.replayEnabled ? 0 : MF_GRAYED), kCmdSaveClip,
              clip.c_str());
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
    case kHotkeyRecord: ToggleRecording(); break;
    case kHotkeyReplay: ToggleReplay(); break;
    case kHotkeyScreenshot: CaptureScreenshot(); break;
    case kHotkeyMarker: AddMarker(); break;
    default: break;
    }
    return 0;
  }
  case WM_TIMER:
    // La grabación pudo acabar sola (disco lento o con errores): ni los ajustes ni la actualización pendientes se
    // quedan colgados.
    if (wp == kTimerHealth && restartPending_ && !pipeline_.Recording())
      UpdatePipeline(std::exchange(restartPending_, false));
    if (wp == kTimerHealth) InstallPendingUpdate();
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
        Toast("La ventana capturada se cerró; la grabación se guardó");
        OnRecordingStopped(path, stoppedAt);
      }
      UpdatePipeline();
      InstallPendingUpdate();
    }
    return 0;
  case WM_SPECTRE_NOTIFY: {
    std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
    bool error = wp != 0;
    std::string message = *text;
    if (!error && (StartsWith(message, "Clip guardado") || StartsWith(message, "Captura guardada") ||
                   StartsWith(message, "Recorte guardado"))) {
      if (config_.soundOnSave && !StartsWith(message, "Recorte")) PlayCue(Cue::Saved);
      std::string note = LowDiskNote();
      if (!note.empty()) {
        message += note;
        error = true; // se ve aunque los avisos estén apagados
      }
    }
    Toast(message, error);
    if (menu_.Visible()) RefreshPanelClips();
    if (appWindow_.Visible()) RescanLibrary();
    EnforceStorageLimit(); // algo nuevo en la carpeta
    return 0;
  }
  case WM_SPECTRE_EXPORT:
    if (wp == kExportDone) {
      FinishExport();
    } else if (exporter_.Busy()) {
      appModel_.exportPercent = int(wp);
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
    // Nunca cerrar Spectre a mitad de una grabación: espera a que se detenga.
    if (pipeline_.Recording()) Toast("La actualización se instalará al terminar la grabación");
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
