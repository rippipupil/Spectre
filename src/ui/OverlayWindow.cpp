#include "ui/OverlayWindow.h"

#include "common/Compat.h"
#include "common/Util.h"
#include "ui/Theme.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace spectre {

namespace {
constexpr wchar_t kMenuClass[] = L"SpectreOverlay";
constexpr wchar_t kToastClass[] = L"SpectreToast";

std::string FontPath(const wchar_t* file) {
  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  auto p = std::filesystem::path(windir) / L"Fonts" / file;
  std::error_code ec;
  return std::filesystem::exists(p, ec) ? Narrow(p.wstring()) : std::string();
}
} // namespace

void OverlayWindow::Init(HINSTANCE instance, Kind kind) {
  instance_ = instance;
  kind_ = kind;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kind == Kind::Menu ? kMenuClass : kToastClass;
  RegisterClassExW(&wc);
}

bool OverlayWindow::Show(const RECT& rect, bool activate) {
  UINT w = UINT(rect.right - rect.left), h = UINT(rect.bottom - rect.top);
  if (!hwnd_) {
    DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    // El aviso deja pasar los clics a cualquier ventana de debajo (HTTRANSPARENT
    // solo funciona con ventanas del mismo hilo).
    if (kind_ == Kind::Toast) ex |= WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;
    hwnd_ = CreateWindowExW(ex, kind_ == Kind::Menu ? kMenuClass : kToastClass, L"Spectre", WS_POPUP, rect.left,
                            rect.top, int(w), int(h), nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    if (kind_ == Kind::Toast) SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    // El menú y los avisos nunca aparecen en los clips.
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    scale_ = float(GetDpiForWindow(hwnd_)) / 96.0f;
    if (!CreateGraphics(w, h)) {
      Hide();
      return false;
    }
    SetupImGui();
  } else if (w != width_ || h != height_) {
    SetWindowPos(hwnd_, HWND_TOPMOST, rect.left, rect.top, int(w), int(h), SWP_NOACTIVATE);
    rtv_.Reset();
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)) ||
        FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back))) ||
        FAILED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_))) {
      // GPU reiniciada (driver actualizado, TDR...): se crea todo de nuevo.
      Hide();
      return Show(rect, activate);
    }
    width_ = w;
    height_ = h;
  }
  ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd_, HWND_TOPMOST, rect.left, rect.top, int(w), int(h),
               activate ? 0 : SWP_NOACTIVATE);
  if (activate) SetForegroundWindow(hwnd_);
  return true;
}

void OverlayWindow::Hide() {
  if (!hwnd_) return;
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
  hasHitRegion_ = false;
  lost_ = false;
  DestroyWindow(h);
}

void OverlayWindow::SetHitRegion(const RECT* rect) {
  if (!hwnd_) return;
  if (!rect) {
    if (hasHitRegion_) SetWindowRgn(hwnd_, nullptr, FALSE);
    hasHitRegion_ = false;
    return;
  }
  if (hasHitRegion_ && EqualRect(rect, &hitRegion_)) return;
  // Rectangular a propósito: una región redondeada recortaría el antialiasing
  // de las esquinas; fuera del panel la ventana es transparente igualmente.
  HRGN rgn = CreateRectRgn(rect->left, rect->top, rect->right, rect->bottom);
  if (rgn && SetWindowRgn(hwnd_, rgn, FALSE)) {
    hitRegion_ = *rect;
    hasHitRegion_ = true;
  } else if (rgn) {
    DeleteObject(rgn);
  }
}

bool OverlayWindow::CreateGraphics(UINT width, UINT height) {
  width_ = width;
  height_ = height;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                 UINT(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &context_);
  if (FAILED(hr)) {
    // Sin GPU utilizable (escritorio remoto...): WARP por software.
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           UINT(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &context_);
  }
  if (FAILED(hr)) return false;

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  device_.As(&dxgiDevice);
  dxgiDevice->GetAdapter(&adapter);
  adapter->GetParent(IID_PPV_ARGS(&factory));

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  if (FAILED(factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapChain_))) return false;

  if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
  if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_))) return false;
  if (FAILED(dcomp_->CreateVisual(&visual_))) return false;
  visual_->SetContent(swapChain_.Get());
  target_->SetRoot(visual_.Get());
  dcomp_->Commit();

  ComPtr<ID3D11Texture2D> back;
  swapChain_->GetBuffer(0, IID_PPV_ARGS(&back));
  return SUCCEEDED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));
}

void OverlayWindow::DestroyGraphics() {
  rtv_.Reset();
  visual_.Reset();
  target_.Reset();
  dcomp_.Reset();
  swapChain_.Reset();
  if (context_) {
    context_->ClearState();
    context_->Flush();
  }
  context_.Reset();
  device_.Reset();
}

void OverlayWindow::SetupImGui() {
  IMGUI_CHECKVERSION();
  imgui_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

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
  // Solo los rangos por defecto (Latin-1): los iconos se dibujan con líneas,
  // así que no hace falta fusionar Segoe UI Symbol (eran tres lecturas del
  // archivo y tres copias en memoria por cada apertura del panel).
  boldFont_ = !bold.empty() ? io.Fonts->AddFontFromFileTTF(bold.c_str(), base * scale_) : nullptr;
  bigFont_ = !bold.empty() ? io.Fonts->AddFontFromFileTTF(bold.c_str(), base * 1.5f * scale_) : nullptr;
  if (!boldFont_) boldFont_ = main;
  if (!bigFont_) bigFont_ = main;

  ApplyBaseStyle(scale_);
  ImGui_ImplWin32_Init(hwnd_);
  ImGui_ImplDX11_Init(device_.Get(), context_.Get());
}

void OverlayWindow::Render(const std::function<void()>& draw) {
  if (!hwnd_ || !imgui_) return;
  // draw() no debe ocultar ni redimensionar esta ventana: las acciones del
  // menú se aplazan hasta después del frame (ver App::RenderOverlays).
  ImGui::SetCurrentContext(imgui_);
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  draw();
  ImGui::Render();
  const float clear[4] = {0, 0, 0, 0};
  context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
  context_->ClearRenderTargetView(rtv_.Get(), clear);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  HRESULT hr = swapChain_->Present(1, 0);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    if (!lost_) Log("Overlay: la GPU se reinició (0x%08X), se recrea la ventana", unsigned(hr));
    lost_ = true;
  }
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  OverlayWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    self = static_cast<OverlayWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self && self->imgui_) {
    // Se restaura el contexto anterior: este mensaje puede llegar en mitad del
    // frame de la otra ventana (SetWindowPos, ShowWindow... son síncronos).
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(self->imgui_);
    LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
    ImGui::SetCurrentContext(previous);
    if (handled) return 1;
  }
  switch (msg) {
  case WM_MOUSEACTIVATE:
    if (self && self->kind_ == Kind::Toast) return MA_NOACTIVATE;
    break;
  case WM_NCHITTEST:
    if (self && self->kind_ == Kind::Toast) return HTTRANSPARENT;
    break;
  case WM_CLOSE:
    if (self && self->onCloseRequested) self->onCloseRequested();
    return 0;
  case WM_ACTIVATE:
    if (LOWORD(wp) == WA_INACTIVE && self && self->onDeactivated) self->onDeactivated();
    break;
  case WM_ERASEBKGND:
    return 1;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace spectre
