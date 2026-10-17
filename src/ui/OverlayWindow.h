#pragma once

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <functional>

struct ImGuiContext;
struct ImFont;

namespace spectre {

using Microsoft::WRL::ComPtr;

// Ventana transparente siempre encima, dibujada con Dear ImGui sobre una
// swapchain de DirectComposition (alfa por píxel). No se inyecta en el juego
// y se excluye de la captura. Todos los recursos GPU se crean al mostrarla y
// se liberan al ocultarla: cerrada no consume nada.
class OverlayWindow {
public:
  enum class Kind { Menu, Toast };

  ~OverlayWindow() { Hide(); }
  void Init(HINSTANCE instance, Kind kind);
  bool Show(const RECT& rect, bool activate);
  void Hide();
  bool Visible() const { return hwnd_ != nullptr; }
  HWND Hwnd() const { return hwnd_; }
  UINT Width() const { return width_; }
  UINT Height() const { return height_; }
  // Zona que recibe clics (coordenadas de la ventana); fuera de ella el ratón
  // pasa a la ventana de debajo. nullptr = toda la ventana.
  void SetHitRegion(const RECT* rect);

  // draw se ejecuta dentro de un frame de ImGui.
  void Render(const std::function<void()>& draw);

  ImFont* BoldFont() const { return boldFont_; }
  ImFont* BigFont() const { return bigFont_; }
  float Scale() const { return scale_; }

  std::function<void()> onCloseRequested; // Alt+F4 sobre la ventana
  std::function<void()> onDeactivated;    // el usuario hizo clic en otra ventana

private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  bool CreateGraphics(UINT width, UINT height);
  void DestroyGraphics();
  void SetupImGui();

  HINSTANCE instance_ = nullptr;
  Kind kind_ = Kind::Menu;
  HWND hwnd_ = nullptr;
  UINT width_ = 0, height_ = 0;
  float scale_ = 1.0f;
  RECT hitRegion_{};
  bool hasHitRegion_ = false;

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain1> swapChain_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<IDCompositionDevice> dcomp_;
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> visual_;
  ImGuiContext* imgui_ = nullptr;
  ImFont* boldFont_ = nullptr;
  ImFont* bigFont_ = nullptr;
};

} // namespace spectre
