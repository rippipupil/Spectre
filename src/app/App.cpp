#include "app/App.h"

#include "common/Util.h"
#include "core/Mp4Writer.h"

#include <filesystem>
#include <thread>

namespace spectre {

namespace {
constexpr wchar_t kMainClass[] = L"SpectreMain";

enum HotkeyId { kHotkeyOverlay = 1, kHotkeySaveClip, kHotkeyRecord, kHotkeyReplay };
enum TimerId { kTimerGame = 1, kTimerHealth };
enum TrayCommand {
  kCmdOpen = 100,
  kCmdSaveClip,
  kCmdRecord,
  kCmdReplay,
  kCmdFolder,
  kCmdQuit = 199,
};

constexpr ULONGLONG kGameGraceMs = 15000; // el juego sigue "activo" 15 s después de perder el foco
constexpr ULONGLONG kToastMs = 2600;

bool SameCaptureSettings(const Config& a, const Config& b) {
  return a.captureMode == b.captureMode && a.monitorIndex == b.monitorIndex && a.captureApi == b.captureApi &&
         a.captureCursor == b.captureCursor && a.fps == b.fps && a.outputHeight == b.outputHeight &&
         a.bitrateKbps == b.bitrateKbps && a.codec == b.codec && a.encoder == b.encoder &&
         a.gpuPriority == b.gpuPriority && a.captureSystemAudio == b.captureSystemAudio &&
         a.outputDeviceId == b.outputDeviceId && a.gameAudioOnly == b.gameAudioOnly && a.captureMic == b.captureMic &&
         a.micDeviceId == b.micDeviceId && a.systemVolume == b.systemVolume && a.micVolume == b.micVolume &&
         a.separateTracks == b.separateTracks && a.cameraEnabled == b.cameraEnabled && a.cameraId == b.cameraId &&
         a.cameraCorner == b.cameraCorner && a.cameraSizePercent == b.cameraSizePercent &&
         a.maxBufferMB == b.maxBufferMB;
}
} // namespace

int App::Run(HINSTANCE instance, bool startInTray) {
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
  menu_.onCloseRequested = [this] { CloseMenu(); };

  menuActions_.saveClip = [this] { SaveClip(); };
  menuActions_.toggleRecording = [this] { ToggleRecording(); };
  menuActions_.toggleReplay = [this] { ToggleReplay(); };
  menuActions_.close = [this] { CloseMenu(); };
  menuActions_.quit = [this] { Quit(); };
  menuActions_.apply = [this](const Config& c) { ApplyConfig(c); };
  menuActions_.refreshClips = [this] { menuModel_.clips = ListClips(ResolveOutputDir(config_), 30); };
  menuActions_.probeDuration = [](const std::string& path) {
    double seconds = 0;
    return ProbeDuration(path, &seconds) ? seconds : 0.0;
  };
  menuActions_.trim = [this](const std::string& path, double start, double end) {
    HWND hwnd = hwnd_;
    std::thread([hwnd, path, start, end] {
      auto in = std::filesystem::path(Widen(path));
      auto out = in.parent_path() / (in.stem().wstring() + L"_recorte_" + Widen(TimestampForFile()) + L".mp4");
      std::string error;
      bool ok = TrimFile(path, Narrow(out.wstring()), start, end, &error);
      auto* msg = new std::string(ok ? "Recorte guardado: " + Narrow(out.filename().wstring()) : error);
      PostMessageW(hwnd, WM_SPECTRE_NOTIFY, ok ? 0 : 1, reinterpret_cast<LPARAM>(msg));
    }).detach();
    Toast("Recortando...");
  };

  if (!RegisterHotkeys()) Toast(menuModel_.hotkeyError, true);
  SetTimer(hwnd_, kTimerGame, 2000, nullptr);
  SetTimer(hwnd_, kTimerHealth, 1000, nullptr);
  CheckGame();
  UpdatePipeline();
  if (!startInTray)
    Toast("Spectre está activo. Pulsa " + config_.hotkeys.overlay + " para abrir el menú.");

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
  UnregisterHotkeys();
  RemoveTrayIcon();
  if (hwnd_) DestroyWindow(hwnd_);
  return 0;
}

// ---------------------------------------------------------------- acciones

std::string App::OutputName(const char* prefix) const {
  bool gameActive = game_ && GetTickCount64() - lastGameSeen_ < kGameGraceMs;
  std::string app = gameActive && !game_->exeName.empty() ? game_->exeName : "Escritorio";
  std::string name = SanitizeFileName(std::string(prefix) + "_" + app + "_" + TimestampForFile()) + ".mp4";
  return Narrow((std::filesystem::path(Widen(ResolveOutputDir(config_))) / Widen(name)).wstring());
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
    UpdatePipeline();
    return;
  }
  if (!pipeline_.Running() && !StartPipeline()) return;
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

void App::ToggleMenu() {
  if (menu_.Visible()) CloseMenu();
  else OpenMenu();
}

void App::OpenMenu() {
  if (menu_.Visible()) return;
  previousForeground_ = GetForegroundWindow();
  menuModel_ = MenuModel{};
  menuModel_.config = config_;
  RefreshMenuModel(true);
  HMONITOR mon = MonitorFromWindow(previousForeground_ ? previousForeground_ : hwnd_, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  if (!menu_.Show(mi.rcMonitor, true)) Toast("No se pudo abrir el menú", true);
}

void App::CloseMenu() {
  if (!menu_.Visible()) return;
  menu_.Hide();
  if (previousForeground_ && IsWindow(previousForeground_)) SetForegroundWindow(previousForeground_);
  previousForeground_ = nullptr;
}

void App::ApplyConfig(const Config& next) {
  Config old = config_;
  config_ = next;
  config_.hotkeys.overlay = NormalizeHotkey(config_.hotkeys.overlay);
  config_.hotkeys.saveClip = NormalizeHotkey(config_.hotkeys.saveClip);
  config_.hotkeys.toggleRecording = NormalizeHotkey(config_.hotkeys.toggleRecording);
  config_.hotkeys.toggleReplay = NormalizeHotkey(config_.hotkeys.toggleReplay);
  SaveConfig(config_);
  menuModel_.config = config_;

  if (old.autostart != config_.autostart && !SetAutostart(config_.autostart))
    Toast("No se pudo cambiar el inicio con Windows", true);

  UnregisterHotkeys();
  menuModel_.hotkeyError.clear();
  bool hotkeysOk = RegisterHotkeys();

  if (!SameCaptureSettings(old, config_) && pipeline_.Running()) {
    if (pipeline_.Recording()) {
      Toast("Algunos cambios se aplicarán al terminar la grabación");
    } else {
      UpdatePipeline(true);
    }
  } else if (pipeline_.Running()) {
    pipeline_.SetReplaySeconds(config_.replayEnabled ? config_.clipSeconds : 3.0);
  }
  UpdatePipeline();
  if (hotkeysOk) Toast("Cambios aplicados");
  else Toast(menuModel_.hotkeyError, true);
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
  static ULONGLONG lastFailure = 0;
  if (lastFailure && GetTickCount64() - lastFailure < 10000) return false; // no reintentar en bucle
  HWND hwnd = hwnd_;
  bool ok = pipeline_.Start(BuildSettings(), [hwnd](const std::string& message, bool error) {
    PostMessageW(hwnd, WM_SPECTRE_NOTIFY, error ? 1 : 0, reinterpret_cast<LPARAM>(new std::string(message)));
  });
  if (!ok) {
    lastFailure = GetTickCount64();
    Toast("No se pudo iniciar la captura. Revisa spectre.log en %APPDATA%\\Spectre", true);
  } else {
    lastFailure = 0;
  }
  return ok;
}

void App::UpdatePipeline(bool forceRestart) {
  bool want = WantPipeline();
  if (pipeline_.Running() && (!want || forceRestart) && !pipeline_.Recording()) pipeline_.Stop();
  if (want && !pipeline_.Running()) StartPipeline();
}

void App::CheckGame() {
  if (menu_.Visible()) return; // el menú tapa el juego: mantenemos lo último detectado
  auto detected = DetectFullscreenApp();
  if (detected) {
    bool changed = !game_ || game_->window != detected->window;
    game_ = detected;
    lastGameSeen_ = GetTickCount64();
    // En modo "ventana del juego" la captura sigue al juego activo.
    if (changed && config_.captureMode == "game" && pipeline_.Running() && !pipeline_.Recording() &&
        pipeline_.TargetWindow() != game_->window) {
      Log("Juego detectado: %s", game_->exeName.c_str());
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
  toastUntil_ = GetTickCount64() + kToastMs;
  float scale = float(GetDpiForSystem()) / 96.0f;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int w = int(400 * scale), h = int(84 * scale), margin = int(20 * scale);
  RECT r{work.right - w - margin, work.top + margin, work.right - margin, work.top + margin + h};
  toast_.Show(r, false);
}

void App::RefreshMenuModel(bool devices) {
  menuModel_.stats = pipeline_.Stats();
  menuModel_.usage = SampleProcessUsage();
  menuModel_.replayActive = pipeline_.Running() && config_.replayEnabled;
  menuModel_.gameDetected = game_ && GetTickCount64() - lastGameSeen_ < kGameGraceMs;
  menuModel_.gameName = game_ ? game_->exeName : "";
  if (devices) {
    menuModel_.outputDir = ResolveOutputDir(config_);
    menuModel_.monitors = EnumerateMonitors();
    menuModel_.outputs = EnumerateAudioDevices(false);
    menuModel_.inputs = EnumerateAudioDevices(true);
    menuModel_.cameras = EnumerateCameras();
    menuModel_.encoders = CandidateEncoders(menuModel_.config.codec);
    menuModel_.clips = ListClips(menuModel_.outputDir, 30);
  }
}

void App::RenderOverlays() {
  ULONGLONG now = GetTickCount64();
  if (menu_.Visible()) {
    if (now - lastStats_ > 1000) {
      lastStats_ = now;
      RefreshMenuModel(false);
    }
    menu_.Render([this] { DrawMenu(menu_, menuModel_, menuActions_); });
  }
  if (toast_.Visible()) {
    if (now >= toastUntil_) {
      toast_.Hide();
    } else {
      float remaining = float(toastUntil_ - now);
      float alpha = remaining < 400 ? remaining / 400.0f : 1.0f;
      toast_.Render([&] { DrawToast(toast_, toastText_, toastError_, alpha); });
    }
  }
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
  std::wstring open = L"Abrir menú\t" + Widen(config_.hotkeys.overlay);
  std::wstring clip = L"Guardar clip\t" + Widen(config_.hotkeys.saveClip);
  std::wstring rec = (pipeline_.Recording() ? L"Detener grabación\t" : L"Iniciar grabación\t") +
                     Widen(config_.hotkeys.toggleRecording);
  AppendMenuW(menu, MF_STRING, kCmdOpen, open.c_str());
  AppendMenuW(menu, MF_STRING | (pipeline_.Running() && config_.replayEnabled ? 0 : MF_GRAYED), kCmdSaveClip,
              clip.c_str());
  AppendMenuW(menu, MF_STRING, kCmdRecord, rec.c_str());
  AppendMenuW(menu, MF_STRING | (config_.replayEnabled ? MF_CHECKED : 0), kCmdReplay, L"Replay activado");
  AppendMenuW(menu, MF_STRING, kCmdFolder, L"Abrir carpeta de clips");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdQuit, L"Salir");
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
    if (wp == kTimerGame) {
      CheckGame();
    } else if (wp == kTimerHealth && pipeline_.Running() && pipeline_.CaptureLost()) {
      bool wasRecording = pipeline_.Recording();
      if (wasRecording) pipeline_.StopRecording();
      Log("Captura perdida: se reinicia");
      pipeline_.Stop();
      if (wasRecording) Toast("La ventana capturada se cerró; la grabación se guardó");
      UpdatePipeline();
    }
    return 0;
  case WM_SPECTRE_NOTIFY: {
    std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
    Toast(*text, wp != 0);
    if (menu_.Visible()) menuModel_.clips = ListClips(ResolveOutputDir(config_), 30);
    return 0;
  }
  case WM_SPECTRE_SHOW_MENU:
    OpenMenu();
    return 0;
  case WM_SPECTRE_TRAY:
    if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu();
    else if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) OpenMenu();
    return 0;
  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case kCmdOpen: OpenMenu(); break;
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
    if (wp) pipeline_.Stop(); // cierra bien el archivo de una grabación en curso
    return 0;
  case WM_DESTROY:
    hwnd_ = nullptr;
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace spectre
