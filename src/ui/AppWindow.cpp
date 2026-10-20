#include "ui/AppWindow.h"

#include "common/Util.h"
#include "ui/Theme.h"

#include <d3d10.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace spectre {

namespace {

constexpr wchar_t kAppClass[] = L"SpectreApp";
constexpr UINT_PTR kModalTimer = 1;

// Tamaño de la ventana al abrirla por primera vez y mínimo, en px a escala 100 %.
constexpr int kDefaultWidth = 1180, kDefaultHeight = 760;
constexpr int kMinWidth = 960, kMinHeight = 620;

// Atributos de DWM que no están en todas las versiones del SDK.
constexpr DWORD kDwmDarkMode = 20;      // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmBorderColor = 34;   // DWMWA_BORDER_COLOR (Windows 11)
constexpr DWORD kDwmCaptionColor = 35;  // DWMWA_CAPTION_COLOR
constexpr DWORD kDwmTextColor = 36;     // DWMWA_TEXT_COLOR
constexpr DWORD kDwmCloak = 13;         // DWMWA_CLOAK

COLORREF ToColorRef(ImU32 c) {
  return RGB((c >> IM_COL32_R_SHIFT) & 0xFF, (c >> IM_COL32_G_SHIFT) & 0xFF, (c >> IM_COL32_B_SHIFT) & 0xFF);
}

std::string FontPath(const wchar_t* file) {
  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  auto p = std::filesystem::path(windir) / L"Fonts" / file;
  std::error_code ec;
  return std::filesystem::exists(p, ec) ? Narrow(p.wstring()) : std::string();
}

void SetCloak(HWND hwnd, bool cloak) {
  BOOL value = cloak ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, kDwmCloak, &value, sizeof(value));
}

} // namespace

void AppWindow::Init(HINSTANCE instance) {
  instance_ = instance;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.lpszClassName = kAppClass;
  RegisterClassExW(&wc);
}

bool AppWindow::Minimized() const { return hwnd_ && IsIconic(hwnd_); }
bool AppWindow::Focused() const { return hwnd_ && GetForegroundWindow() == hwnd_; }

bool AppWindow::Open(bool activate) {
  if (hwnd_) {
    if (activate) {
      if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
      else ShowWindow(hwnd_, SW_SHOW);
      SetForegroundWindow(hwnd_);
    }
    Invalidate();
    return true;
  }
  // Se abre donde estaba al cerrarla o centrada en el monitor del cursor.
  POINT cursor{};
  GetCursorPos(&cursor);
  HMONITOR monitor = hasLastRect_ ? MonitorFromRect(&lastRect_, MONITOR_DEFAULTTONEAREST)
                                  : MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(monitor, &mi);
  UINT dpiX = 96, dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = GetDpiForSystem();
  float scale = float(dpiX) / 96.0f;
  const RECT& work = mi.rcWork;
  RECT r;
  if (hasLastRect_) {
    r = lastRect_;
  } else {
    int w = std::min(int(float(kDefaultWidth) * scale), int(float(work.right - work.left) * 0.92f));
    int h = std::min(int(float(kDefaultHeight) * scale), int(float(work.bottom - work.top) * 0.92f));
    r.left = work.left + (work.right - work.left - w) / 2;
    r.top = work.top + (work.bottom - work.top - h) / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;
  }

  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kAppClass, L"Spectre", WS_OVERLAPPEDWINDOW, r.left, r.top,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance_, this);
  if (!hwnd_) return false;
  scale_ = float(GetDpiForWindow(hwnd_)) / 96.0f;
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, kDwmDarkMode, &dark, sizeof(dark));
  // Oculta para DWM hasta que el primer frame esté dibujado: sin destello blanco.
  SetCloak(hwnd_, true);
  cloaked_ = true;
  if (!CreateGraphics()) {
    Log("App: no se pudo crear la GPU de la ventana");
    Close();
    return false;
  }
  SetupImGui();
  if (activate) {
    ShowWindow(hwnd_, lastMaximized_ ? SW_SHOWMAXIMIZED : SW_SHOW);
    SetForegroundWindow(hwnd_);
  } else {
    ShowWindow(hwnd_, lastMaximized_ ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
  }
  Invalidate();
  Log("App: ventana abierta (%ux%u, escala %.2f%s)", width_, height_, double(scale_),
      videoSupport_ ? "" : ", sin reproductor por GPU");
  return true;
}

void AppWindow::Close() {
  if (!hwnd_) return;
  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  if (GetWindowPlacement(hwnd_, &wp)) {
    lastRect_ = wp.rcNormalPosition;
    hasLastRect_ = true;
    lastMaximized_ = wp.showCmd == SW_SHOWMAXIMIZED;
  }
  if (imgui_) {
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(imgui_);
    imgui_ = nullptr;
    boldFont_ = bigFont_ = nullptr;
  }
  DestroyGraphics();
  HWND h = hwnd_;
  hwnd_ = nullptr;
  lost_ = false;
  sizing_ = false;
  framesPending_ = 0;
  animateAt_ = nextAnimate_ = ~0ull;
  DestroyWindow(h);
  Log("App: ventana cerrada");
}

// ------------------------------------------------------------------ frames

void AppWindow::Input() {
  inputUntil_ = GetTickCount64() + 350;
}

void AppWindow::Invalidate() {
  framesPending_ = std::max(framesPending_, 3);
}

void AppWindow::Animate() { nextAnimate_ = 0; }

void AppWindow::AnimateAt(unsigned long long when) { nextAnimate_ = std::min(nextAnimate_, when); }

bool AppWindow::NeedsFrame(unsigned long long now) const {
  if (!hwnd_ || IsIconic(hwnd_)) return false;
  return framesPending_ > 0 || now < inputUntil_ || now >= animateAt_ || resizePending_;
}

DWORD AppWindow::WaitMs(unsigned long long now) const {
  if (!hwnd_ || IsIconic(hwnd_)) return INFINITE;
  if (NeedsFrame(now)) return 0;
  if (animateAt_ == ~0ull) return INFINITE;
  return DWORD(std::min<unsigned long long>(animateAt_ - now, 60000));
}

// ------------------------------------------------------------------ GPU

bool AppWindow::CreateGraphics() {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  width_ = UINT(std::max<LONG>(1, rc.right - rc.left));
  height_ = UINT(std::max<LONG>(1, rc.bottom - rc.top));
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  // VIDEO_SUPPORT es lo que necesita Media Foundation para el reproductor.
  const UINT flags[] = {D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT};
  HRESULT hr = E_FAIL;
  for (UINT f : flags) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, f, levels, UINT(std::size(levels)),
                           D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (SUCCEEDED(hr)) {
      videoSupport_ = (f & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) != 0;
      break;
    }
  }
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           UINT(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &context_);
    videoSupport_ = false;
  }
  if (FAILED(hr)) return false;
  // El reproductor usa el mismo dispositivo desde sus hilos.
  ComPtr<ID3D10Multithread> mt;
  if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
    return false;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width_;
  desc.Height = height_;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_))) {
    // Windows 10 muy antiguo o adaptador básico: secuencial.
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_)))
      return false;
  }
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  return SUCCEEDED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));
}

void AppWindow::DestroyGraphics() {
  rtv_.Reset();
  swapChain_.Reset();
  if (context_) {
    context_->ClearState();
    context_->Flush();
  }
  context_.Reset();
  device_.Reset();
}

bool AppWindow::ResizeBuffers() {
  resizePending_ = false;
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  UINT w = UINT(std::max<LONG>(1, rc.right - rc.left)), h = UINT(std::max<LONG>(1, rc.bottom - rc.top));
  if (w == width_ && h == height_) return true;
  rtv_.Reset();
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  if (FAILED(swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return false;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back))) ||
      FAILED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_)))
    return false;
  width_ = w;
  height_ = h;
  return true;
}

std::shared_ptr<UiTexture> AppWindow::CreateTexture(const uint8_t* bgra, int width, int height) {
  if (!device_ || !bgra || width <= 0 || height <= 0) return nullptr;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = UINT(width);
  desc.Height = UINT(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{bgra, UINT(width) * 4, 0};
  auto tex = std::make_shared<UiTexture>();
  if (FAILED(device_->CreateTexture2D(&desc, &data, &tex->texture)) ||
      FAILED(device_->CreateShaderResourceView(tex->texture.Get(), nullptr, &tex->srv)))
    return nullptr;
  tex->width = width;
  tex->height = height;
  return tex;
}

std::shared_ptr<UiTexture> AppWindow::CreateRenderTexture(int width, int height) {
  if (!device_ || width <= 0 || height <= 0) return nullptr;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = UINT(width);
  desc.Height = UINT(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  auto tex = std::make_shared<UiTexture>();
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, &tex->texture)) ||
      FAILED(device_->CreateShaderResourceView(tex->texture.Get(), nullptr, &tex->srv)))
    return nullptr;
  tex->width = width;
  tex->height = height;
  return tex;
}

// ------------------------------------------------------------------ ImGui

void AppWindow::LoadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  const float base = 15.0f;
  auto regular = FontPath(L"segoeui.ttf");
  auto bold = FontPath(L"segoeuib.ttf");
  ImFont* main = nullptr;
  if (!regular.empty()) main = io.Fonts->AddFontFromFileTTF(regular.c_str(), base * scale_);
  if (!main) {
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * scale_;
    main = io.Fonts->AddFontDefault(&cfg);
  }
  boldFont_ = !bold.empty() ? io.Fonts->AddFontFromFileTTF(bold.c_str(), base * scale_) : nullptr;
  bigFont_ = !bold.empty() ? io.Fonts->AddFontFromFileTTF(bold.c_str(), base * 1.5f * scale_) : nullptr;
  if (!boldFont_) boldFont_ = main;
  if (!bigFont_) bigFont_ = main;
}

void AppWindow::SetupImGui() {
  IMGUI_CHECKVERSION();
  imgui_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  LoadFonts();
  ApplyBaseStyle(scale_);
  ImGui_ImplWin32_Init(hwnd_);
  ImGui_ImplDX11_Init(device_.Get(), context_.Get());
}

void AppWindow::Render(const std::function<void()>& draw) {
  if (!hwnd_ || !imgui_ || IsIconic(hwnd_)) return;
  if (resizePending_ && !ResizeBuffers()) {
    if (!lost_) Log("App: no se pudo redimensionar la swapchain; se recrea la ventana");
    lost_ = true;
    return;
  }
  ImGui::SetCurrentContext(imgui_);
  if (rebuildFonts_) {
    rebuildFonts_ = false;
    ImGui_ImplDX11_InvalidateDeviceObjects();
    LoadFonts();
    ImGui::GetStyle() = ImGuiStyle();
    ApplyBaseStyle(scale_);
  }
  if (framesPending_ > 0) --framesPending_;
  nextAnimate_ = ~0ull;
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  draw();
  ImGui::Render();
  animateAt_ = nextAnimate_;
  context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
  const float clear[4] = {0, 0, 0, 1};
  context_->ClearRenderTargetView(rtv_.Get(), clear);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  HRESULT hr = swapChain_->Present(1, 0);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    if (!lost_) Log("App: la GPU se reinició (0x%08X), se recrea la ventana", unsigned(hr));
    lost_ = true;
  }
  if (cloaked_) {
    cloaked_ = false;
    SetCloak(hwnd_, false);
  }
}

void AppWindow::SetTitleColors(const Palette& palette) {
  if (!hwnd_) return;
  COLORREF caption = ToColorRef(palette.bg), text = ToColorRef(palette.text), border = ToColorRef(palette.line);
  DwmSetWindowAttribute(hwnd_, kDwmCaptionColor, &caption, sizeof(caption));
  DwmSetWindowAttribute(hwnd_, kDwmTextColor, &text, sizeof(text));
  DwmSetWindowAttribute(hwnd_, kDwmBorderColor, &border, sizeof(border));
}

// ------------------------------------------------------------------ mensajes

LRESULT CALLBACK AppWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  AppWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    self = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self && self->hwnd_ == hwnd) {
    if (self->imgui_) {
      // Se restaura el contexto anterior: este mensaje puede llegar en mitad
      // del frame de otra ventana (SetWindowPos, ShowWindow... son síncronos).
      ImGuiContext* previous = ImGui::GetCurrentContext();
      ImGui::SetCurrentContext(self->imgui_);
      LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
      ImGui::SetCurrentContext(previous);
      if (handled) return 1;
    }
    return self->Handle(msg, wp, lp);
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT AppWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_MOUSEMOVE:
  case WM_MOUSELEAVE:
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_LBUTTONDBLCLK:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  case WM_XBUTTONDOWN:
  case WM_XBUTTONUP:
  case WM_MOUSEWHEEL:
  case WM_MOUSEHWHEEL:
  case WM_KEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
  case WM_CHAR:
    Input();
    break;
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
  case WM_ACTIVATE:
    Invalidate();
    break;
  case WM_SIZE:
    if (wp != SIZE_MINIMIZED) {
      resizePending_ = true;
      Invalidate();
      if (sizing_ && onModalFrame) onModalFrame();
    } else if (onMinimized) {
      onMinimized();
    }
    return 0;
  case WM_ENTERSIZEMOVE:
    sizing_ = true;
    SetTimer(hwnd_, kModalTimer, 16, nullptr);
    return 0;
  case WM_EXITSIZEMOVE:
    sizing_ = false;
    KillTimer(hwnd_, kModalTimer);
    Invalidate();
    return 0;
  case WM_TIMER:
    if (wp == kModalTimer) {
      if (sizing_ && onModalFrame) onModalFrame();
      return 0;
    }
    break;
  case WM_GETMINMAXINFO: {
    auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
    float s = hwnd_ ? float(GetDpiForWindow(hwnd_)) / 96.0f : 1.0f;
    mmi->ptMinTrackSize.x = LONG(float(kMinWidth) * s);
    mmi->ptMinTrackSize.y = LONG(float(kMinHeight) * s);
    return 0;
  }
  case WM_DPICHANGED: {
    // Otro monitor u otra escala: tamaño sugerido por Windows y fuentes nuevas.
    scale_ = float(HIWORD(wp)) / 96.0f;
    rebuildFonts_ = true;
    const RECT* r = reinterpret_cast<const RECT*>(lp);
    SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Invalidate();
    return 0;
  }
  case WM_SYSCOMMAND:
    // F10 o soltar Alt (al grabar un atajo como Alt+F10) activaría el menú de la ventana, que no tiene.
    if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
    break;
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    EndPaint(hwnd_, &ps);
    Invalidate();
    return 0;
  }
  case WM_CLOSE:
    if (onCloseRequested) onCloseRequested();
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace spectre
