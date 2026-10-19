#include "app/App.h"
#include "app/CrashHandler.h"

#include "common/Util.h"
#include "core/Mp4Writer.h"

#include <shellscalingapi.h>

#include <algorithm>
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

enum HotkeyId { kHotkeyOverlay = 1, kHotkeySaveClip, kHotkeyRecord, kHotkeyReplay };
enum TimerId { kTimerGame = 1, kTimerHealth };
enum TrayCommand {
  kCmdOpen = 100,
  kCmdSettings,
  kCmdSaveClip,
  kCmdRecord,
  kCmdReplay,
  kCmdFolder,
  kCmdQuit = 199,
};

constexpr ULONGLONG kGameGraceMs = 15000; // el juego sigue "activo" 15 s después de perder el foco
constexpr ULONGLONG kToastMs = 2600;
constexpr size_t kPanelClips = 3;
constexpr size_t kSettingsClips = 30;

bool SameHotkeys(const HotkeyConfig& a, const HotkeyConfig& b) {
  return a.overlay == b.overlay && a.saveClip == b.saveClip && a.toggleRecording == b.toggleRecording &&
         a.toggleReplay == b.toggleReplay;
}

float MonitorScale(HMONITOR monitor) {
  UINT dpiX = 96, dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = GetDpiForSystem();
  return float(dpiX) / 96.0f;
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
  // Alt+F4 o hacer clic fuera cierran el menú, pero siempre a través de la
  // cola de mensajes (nunca dentro del WndProc ni del frame de la ventana).
  menu_.onCloseRequested = [this] { PostMessageW(hwnd_, WM_SPECTRE_CLOSE_MENU, menuGeneration_, 0); };
  menu_.onDeactivated = [this] { PostMessageW(hwnd_, WM_SPECTRE_CLOSE_MENU, menuGeneration_, 0); };

  menuActions_.saveClip = [this] { Defer([this] { SaveClip(); }); };
  menuActions_.toggleRecording = [this] { Defer([this] { ToggleRecording(); }); };
  menuActions_.toggleReplay = [this] { Defer([this] { ToggleReplay(); }); };
  menuActions_.toggleMic = [this] { Defer([this] { ToggleMicrophone(); }); };
  menuActions_.toggleCamera = [this] { Defer([this] { ToggleCamera(); }); };
  menuActions_.setClipSeconds = [this](int seconds) { Defer([this, seconds] { SetClipSeconds(seconds); }); };
  menuActions_.openSettings = [this] { Defer([this] { OpenSettings(); }); };
  menuActions_.backToPanel = [this] { Defer([this] { BackToPanel(); }); };
  menuActions_.close = [this] { Defer([this] { CloseMenu(); }); };
  menuActions_.quit = [this] { Defer([this] { Quit(); }); };
  menuActions_.apply = [this](const Config& c) { Defer([this, c] { ApplyConfig(c); }); };
  menuActions_.refreshClips = [this] {
    menuModel_.clips = ListClips(ResolveOutputDir(config_), kSettingsClips);
    menuModel_.selectedClip = -1;
  };
  menuActions_.openPath = [this](const std::string& path) { Defer([path] { OpenPath(path); }); };
  menuActions_.showInFolder = [this](const std::string& path) { Defer([path] { ShowInFolder(path); }); };
  menuActions_.probeDuration = [](const std::string& path) {
    double seconds = 0;
    return ProbeDuration(path, &seconds) ? seconds : 0.0;
  };
  menuActions_.checkUpdates = [this] { updater_.Check(); };
  menuActions_.installUpdate = [this] { updater_.Download(hwnd_); };
  menuActions_.cancelUpdate = [this] { updater_.Cancel(); };
  menuActions_.trim = [this](const std::string& path, double start, double end) {
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
    Defer([this] { Toast("Recortando el clip..."); });
  };

  if (!RegisterHotkeys()) Toast(menuModel_.hotkeyError, true);
  SetTimer(hwnd_, kTimerGame, 2000, nullptr);
  SetTimer(hwnd_, kTimerHealth, 1000, nullptr);
  CheckGame();
  UpdatePipeline();
  if (updated)
    Toast("Spectre se ha actualizado a la versión " SPECTRE_VERSION ". Tus ajustes se conservan.");
  else if (!startInTray)
    Toast("Spectre está en la bandeja del sistema. Pulsa " + config_.hotkeys.overlay + " para abrir el panel.");

  // Bucle de mensajes: bloqueante cuando no hay nada en pantalla (0 % CPU),
  // ~60 fps solo mientras el menú o un aviso están visibles.
  MSG msg;
  ULONGLONG lastFrame = 0;
  while (!quitting_) {
    bool animating = menu_.Visible() || toast_.Visible();
    if (!animating) {
      if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    } else {
      ULONGLONG now = GetTickCount64();
      DWORD wait = now - lastFrame >= 16 ? 0 : DWORD(16 - (now - lastFrame));
      MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) quitting_ = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (quitting_) break;
    ULONGLONG now = GetTickCount64();
    if ((menu_.Visible() || toast_.Visible()) && now - lastFrame >= 16) {
      lastFrame = now;
      RenderOverlays();
    }
  }

  menu_.Hide();
  toast_.Hide();
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

std::string App::OutputName(const char* prefix) {
  bool gameActive = game_ && GetTickCount64() - lastGameSeen_ < kGameGraceMs;
  std::string app = gameActive && !game_->exeName.empty() ? game_->exeName : "Escritorio";
  std::string base = SanitizeFileName(std::string(prefix) + "_" + app + "_" + TimestampForFile());
  auto dir = std::filesystem::path(Widen(ResolveOutputDir(config_)));
  // Dos clips en el mismo segundo no pueden compartir archivo: el segundo
  // todavía se está escribiendo cuando se pide el tercero, así que también se
  // recuerdan los nombres entregados.
  std::string path;
  for (int i = 1;; ++i) {
    std::string name = base + (i > 1 ? "_" + std::to_string(i) : "") + ".mp4";
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

void App::SaveClip() {
  if (!pipeline_.Running() || !config_.replayEnabled) {
    Toast("El replay no está activo", true);
    return;
  }
  pipeline_.SaveClip(OutputName("Clip"), double(config_.clipSeconds));
}

void App::ToggleRecording() {
  if (pipeline_.Recording()) {
    std::string path = pipeline_.Stats().recordPath;
    pipeline_.StopRecording();
    Toast("Grabación guardada: " + Narrow(std::filesystem::path(Widen(path)).filename().wstring()));
    UpdatePipeline(std::exchange(restartPending_, false));
    InstallPendingUpdate();
    return;
  }
  if (!pipeline_.Running() && !StartPipeline()) {
    Toast("No se pudo iniciar la captura. Prueba otro método en Ajustes > Vídeo o mira spectre.log", true);
    return;
  }
  if (pipeline_.StartRecording(OutputName("Grabacion")))
    Toast("Grabando. Pulsa " + config_.hotkeys.toggleRecording + " para detener.");
  else
    Toast("No se pudo iniciar la grabación", true);
}

void App::ToggleReplay() {
  config_.replayEnabled = !config_.replayEnabled;
  SaveConfig(config_);
  menuModel_.config.replayEnabled = config_.replayEnabled;
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
    menuModel_.config.captureMic = config_.captureMic;
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

void App::ToggleMenu() {
  if (menu_.Visible()) CloseMenu();
  else OpenMenu();
}

RECT App::MenuRect() const {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(menuMonitor_, &mi)) {
    mi.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    mi.rcWork = mi.rcMonitor;
  }
  // Sobre un juego a pantalla completa la barra de tareas está tapada: se usa
  // el monitor entero.
  const RECT& work = menuScreen_.fullscreen ? mi.rcMonitor : mi.rcWork;
  float scale = MonitorScale(menuMonitor_);
  if (menuMode_ == MenuMode::Settings) {
    // Ajustes: la ventana es solo el cuadro, centrado, y DrawSettings lo llena.
    // Alrededor no hay ventana: los clics llegan al juego y los búferes son del
    // tamaño del cuadro, no del monitor.
    LONG margin = LONG(48 * scale);
    LONG w = std::clamp<LONG>(LONG(std::ceil(kSettingsWidth * scale)), 1,
                              std::max<LONG>(1, work.right - work.left - margin));
    LONG h = std::clamp<LONG>(LONG(std::ceil(kSettingsHeight * scale)), 1,
                              std::max<LONG>(1, work.bottom - work.top - margin));
    LONG left = work.left + (work.right - work.left - w) / 2;
    LONG top = work.top + (work.bottom - work.top - h) / 2;
    return RECT{left, top, left + w, top + h};
  }
  // Panel: franja estrecha pegada al borde derecho, como el overlay de NVIDIA.
  // La ventana ocupa todo el alto del área de trabajo; solo el panel recibe
  // clics (SetHitRegion) y el resto de la franja es transparente.
  int width = int(std::ceil(kPanelWidth * scale));
  int margin = int(18 * scale);
  return RECT{work.right - margin - width, work.top + margin, work.right - margin, work.bottom - margin};
}

void App::PlaceMenu(bool activate) {
  menuActivated_ = activate;
  if (!menu_.Show(MenuRect(), activate)) {
    Toast("No se pudo abrir el panel", true);
    return;
  }
  // Un juego topmost (GLFW, Unity...) vuelve a subir al activarse: el panel se
  // mantiene por encima mientras esté abierto.
  menu_.StayAbove(menuScreen_.fullscreen ? menuScreen_.window : nullptr);
  if (menuMode_ == MenuMode::Settings) menu_.SetHitRegion(nullptr); // la ventana es el cuadro entero
}

void App::OpenMenu() {
  if (menu_.Visible()) return;
  ++menuGeneration_;
  previousForeground_ = GetForegroundWindow();
  if (previousForeground_ == hwnd_) previousForeground_ = nullptr; // ventana oculta (tras el menú de la bandeja)
  menuMode_ = MenuMode::Panel;
  menuModel_ = MenuModel{};
  menuModel_.config = config_;
  menuModel_.openedAt = GetTickCount64();
  menuModel_.outputDir = ResolveOutputDir(config_);
  menuModel_.clips = ListClips(menuModel_.outputDir, kPanelClips);
  RefreshMenuModel(false);
  // El panel va al monitor de la ventana activa (el juego), con su DPI; sin
  // ventana, al del cursor.
  menuScreen_ = QueryFullscreen(previousForeground_);
  menuMonitor_ = menuScreen_.monitor;
  if (menuScreen_.fullscreen) Log("Panel sobre %s", menuScreen_.Describe().c_str());
  PlaceMenu(true);
}

void App::OpenSettings() {
  if (!menu_.Visible()) OpenMenu();
  menuMode_ = MenuMode::Settings;
  menuModel_.config = config_;
  menuModel_.dirty = false;
  menuModel_.selectedClip = -1;
  menuModel_.openedAt = GetTickCount64();
  if (!menuModel_.devicesLoaded) {
    RefreshMenuModel(true);
    menuModel_.devicesLoaded = true;
  }
  menuModel_.clips = ListClips(menuModel_.outputDir, kSettingsClips);
  PlaceMenu(true);
}

void App::BackToPanel() {
  if (!menu_.Visible()) return;
  menuMode_ = MenuMode::Panel;
  menuModel_.config = config_; // descarta lo no aplicado
  menuModel_.dirty = false;
  menuModel_.selectedClip = -1;
  menuModel_.panelHeight = 0;
  menuModel_.openedAt = GetTickCount64();
  menuModel_.clips = ListClips(menuModel_.outputDir, kPanelClips);
  PlaceMenu(true);
}

void App::CloseMenu() {
  if (!menu_.Visible()) return;
  ++menuGeneration_;
  menuClosedAt_ = GetTickCount64();
  menu_.Hide();
  // Solo se devuelve el foco si nadie lo ha cogido ya (p. ej. el reproductor
  // que acaba de abrir un clip).
  HWND fg = GetForegroundWindow();
  if (previousForeground_ && previousForeground_ != hwnd_ && IsWindow(previousForeground_) && (!fg || fg == hwnd_))
    SetForegroundWindow(previousForeground_);
  previousForeground_ = nullptr;
}

void App::ApplyConfig(const Config& next, bool quiet) {
  Config old = config_;
  config_ = next;
  config_.hotkeys.overlay = NormalizeHotkey(config_.hotkeys.overlay);
  config_.hotkeys.saveClip = NormalizeHotkey(config_.hotkeys.saveClip);
  config_.hotkeys.toggleRecording = NormalizeHotkey(config_.hotkeys.toggleRecording);
  config_.hotkeys.toggleReplay = NormalizeHotkey(config_.hotkeys.toggleReplay);
  if (config_.outputDir != old.outputDir) menuModel_.outputDir = ResolveOutputDir(config_);
  SaveConfig(config_);
  menuModel_.config = config_;

  if (old.autostart != config_.autostart && !SetAutostart(config_.autostart))
    Toast("No se pudo cambiar el inicio con Windows", true);

  bool hotkeysOk = true;
  if (!SameHotkeys(old.hotkeys, config_.hotkeys)) {
    UnregisterHotkeys();
    menuModel_.hotkeyError.clear();
    hotkeysOk = RegisterHotkeys();
    if (!hotkeysOk) {
      // Se vuelve a los atajos anteriores: mejor eso que quedarse sin Alt+Z.
      UnregisterHotkeys();
      config_.hotkeys = old.hotkeys;
      RegisterHotkeys();
      SaveConfig(config_);
      menuModel_.config.hotkeys = config_.hotkeys;
    }
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
  if (!hotkeysOk) Toast(menuModel_.hotkeyError, true);
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
  return game_ && GetTickCount64() - lastGameSeen_ < kGameGraceMs;
}

PipelineSettings App::BuildSettings() const {
  PipelineSettings s;
  bool gameActive = game_ && GetTickCount64() - lastGameSeen_ < kGameGraceMs && IsWindow(game_->window);
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
  s.maxBufferBytes = size_t(config_.maxBufferMB) << 20;
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
  struct Entry {
    int id;
    const std::string* text;
  };
  const Entry entries[] = {{kHotkeyOverlay, &config_.hotkeys.overlay},
                           {kHotkeySaveClip, &config_.hotkeys.saveClip},
                           {kHotkeyRecord, &config_.hotkeys.toggleRecording},
                           {kHotkeyReplay, &config_.hotkeys.toggleReplay}};
  std::string failed;
  for (const auto& e : entries) {
    if (e.text->empty()) continue;
    UINT mods = 0, vk = 0;
    if (!ParseHotkey(*e.text, &mods, &vk) || !RegisterHotKey(hwnd_, e.id, mods, vk))
      failed += (failed.empty() ? "" : ", ") + *e.text;
  }
  if (!failed.empty()) {
    menuModel_.hotkeyError = "No se pudo registrar " + failed + " (puede que otra app ya lo use, p. ej. GeForce Experience)";
    Log("%s", menuModel_.hotkeyError.c_str());
    return false;
  }
  return true;
}

void App::UnregisterHotkeys() {
  for (int id = kHotkeyOverlay; id <= kHotkeyReplay; ++id) UnregisterHotKey(hwnd_, id);
}

// ---------------------------------------------------------------------- UI

void App::Toast(const std::string& text, bool error) {
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
  const RECT& work = mi.rcWork;
  RECT r{work.right - w - margin, work.top + margin, work.right - margin, work.top + margin + h};
  if (!toast_.Show(r, false)) return;
  // "Clip guardado" tiene que verse también encima de un juego topmost.
  FullscreenInfo under = menu_.Visible() ? menuScreen_ : QueryFullscreen(GetForegroundWindow(), false);
  toast_.StayAbove(under.fullscreen ? under.window : nullptr);
}

void App::RefreshMenuModel(bool devices) {
  menuModel_.stats = pipeline_.Stats();
  menuModel_.usage = SampleProcessUsage();
  menuModel_.replayActive = pipeline_.Running() && config_.replayEnabled;
  menuModel_.micLive = pipeline_.MicCaptured() && config_.captureMic;
  menuModel_.gameDetected = game_ && GetTickCount64() - lastGameSeen_ < kGameGraceMs;
  menuModel_.gameName = game_ ? game_->exeName : "";
  if (devices) {
    menuModel_.outputDir = ResolveOutputDir(config_);
    menuModel_.monitors = EnumerateMonitors();
    menuModel_.outputs = EnumerateAudioDevices(false);
    menuModel_.inputs = EnumerateAudioDevices(true);
    menuModel_.cameras = EnumerateCameras();
    menuModel_.encoders = CandidateEncoders(menuModel_.config.codec);
  }
}

void App::RenderOverlays() {
  ULONGLONG now = GetTickCount64();
  // GPU reiniciada mientras el menú estaba abierto: se recrea en el mismo sitio.
  // Hide() provoca un aviso de desactivación con la generación actual; se sube
  // después para que ese aviso quede obsoleto y no cierre el menú nuevo.
  // Se recrea con la misma activación con la que se abrió y vuelve a quedar
  // sobre el juego (PlaceMenu llama a StayAbove).
  if (menu_.Visible() && menu_.Lost()) {
    menu_.Hide();
    ++menuGeneration_;
    PlaceMenu(menuActivated_);
  }
  if (toast_.Visible() && toast_.Lost()) toast_.Hide();
  if (menu_.Visible()) {
    if (now - lastStats_ > 1000) {
      lastStats_ = now;
      RefreshMenuModel(false);
    }
    pipeline_.AudioLevels(&menuModel_.systemLevel, &menuModel_.micLevel);
    if (menuMode_ == MenuMode::Panel) {
      menu_.Render([this] { DrawPanel(menu_, menuModel_, menuActions_); });
      if (menuModel_.panelHeight > 0) {
        // Mismo cálculo que la posición del panel en DrawPanel (centrado en la franja).
        LONG h = LONG(std::ceil(menuModel_.panelHeight));
        LONG top = std::max<LONG>(0, (LONG(menu_.Height()) - h) / 2);
        RECT hit{0, top, LONG(menu_.Width()), top + h};
        menu_.SetHitRegion(&hit);
      }
    } else {
      menuModel_.update = updater_.Status();
      menu_.Render([this] { DrawSettings(menu_, menuModel_, menuActions_); });
    }
  }
  if (toast_.Visible()) {
    if (now >= toastUntil_) {
      toast_.Hide();
    } else {
      float remaining = float(toastUntil_ - now);
      float alpha = remaining < 400 ? remaining / 400.0f : 1.0f;
      toast_.Render([&] { DrawToast(toast_, config_, toastText_, toastError_, alpha); });
    }
  }
  RunDeferred();
}

void App::RunDeferred() {
  auto actions = std::move(deferred_);
  deferred_.clear();
  for (auto& action : actions) action();
}

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
  HMENU menu = CreatePopupMenu();
  std::wstring open = L"Abrir panel\t" + Widen(config_.hotkeys.overlay);
  std::wstring clip = L"Guardar clip\t" + Widen(config_.hotkeys.saveClip);
  std::wstring rec = (pipeline_.Recording() ? L"Detener grabación\t" : L"Iniciar grabación\t") +
                     Widen(config_.hotkeys.toggleRecording);
  AppendMenuW(menu, MF_STRING, kCmdOpen, open.c_str());
  AppendMenuW(menu, MF_STRING, kCmdSettings, L"Ajustes");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (pipeline_.Running() && config_.replayEnabled ? 0 : MF_GRAYED), kCmdSaveClip,
              clip.c_str());
  AppendMenuW(menu, MF_STRING, kCmdRecord, rec.c_str());
  AppendMenuW(menu, MF_STRING | (config_.replayEnabled ? MF_CHECKED : 0), kCmdReplay, L"Replay activado");
  AppendMenuW(menu, MF_STRING, kCmdFolder, L"Abrir carpeta de clips");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdQuit, L"Salir de Spectre");
  SetMenuDefaultItem(menu, kCmdOpen, FALSE);
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
  case WM_HOTKEY:
    switch (int(wp)) {
    case kHotkeyOverlay: ToggleMenu(); break;
    case kHotkeySaveClip: SaveClip(); break;
    case kHotkeyRecord: ToggleRecording(); break;
    case kHotkeyReplay: ToggleReplay(); break;
    default: break;
    }
    return 0;
  case WM_TIMER:
    // La grabación pudo acabar sola (error de disco, etc.): la actualización pendiente no se queda colgada.
    if (wp == kTimerHealth) InstallPendingUpdate();
    if (wp == kTimerGame) {
      CheckGame();
    } else if (wp == kTimerHealth && pipeline_.Running() && pipeline_.CaptureLost()) {
      bool wasRecording = pipeline_.Recording();
      if (wasRecording) pipeline_.StopRecording();
      Log("Captura perdida: se reinicia");
      pipeline_.Stop();
      restartPending_ = false;
      if (wasRecording) Toast("La ventana capturada se cerró; la grabación se guardó");
      UpdatePipeline();
      InstallPendingUpdate();
    }
    return 0;
  case WM_SPECTRE_NOTIFY: {
    std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
    Toast(*text, wp != 0);
    if (menu_.Visible()) {
      menuModel_.clips = ListClips(menuModel_.outputDir,
                                   menuMode_ == MenuMode::Panel ? kPanelClips : kSettingsClips);
      menuModel_.selectedClip = -1;
    }
    return 0;
  }
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
  case WM_SPECTRE_TRAY:
    if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu();
    else if (LOWORD(lp) == WM_LBUTTONUP) {
      // Al pulsar el icono el panel pierde el foco y se cierra justo antes:
      // ese clic lo cierra, no lo vuelve a abrir.
      if (menu_.Visible()) CloseMenu();
      else if (GetTickCount64() - menuClosedAt_ > 400) OpenMenu();
    }
    return 0;
  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case kCmdOpen: OpenMenu(); break;
    case kCmdSettings: OpenSettings(); break;
    case kCmdSaveClip: SaveClip(); break;
    case kCmdRecord: ToggleRecording(); break;
    case kCmdReplay: ToggleReplay(); break;
    case kCmdFolder: OpenPath(ResolveOutputDir(config_)); break;
    case kCmdQuit: Quit(); break;
    default: break;
    }
    return 0;
  case WM_QUERYENDSESSION:
    return TRUE;
  case WM_ENDSESSION:
    // Apagado, cierre de sesión o el instalador actualizando Spectre: se cierra
    // bien el archivo de una grabación en curso y se sale.
    if (wp) {
      installerPath_.clear(); // nunca instalar mientras Windows cierra la sesión
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
