#pragma once

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "ui/Fonts.h"

#include <functional>

struct ImGuiContext;

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
  // activate=false: la ventana se muestra sin activarse y los clics tampoco la
  // activan (WS_EX_NOACTIVATE + MA_NOACTIVATE). El juego sigue en primer plano
  // y no se entera de que el panel existe: le llega el ratón pero no el
  // teclado, porque no tiene el foco. activate=true: se activa ahora
  // (SetForegroundWindow) y los clics la activan. Se puede cambiar en cada Show.
  bool Show(const RECT& rect, bool activate);
  // Recoloca la ventana visible sin tocar la activación (el monitor cambió de
  // resolución con el panel abierto).
  void Move(const RECT& rect);
  void Hide();
  bool Visible() const { return hwnd_ != nullptr; }
  // Hay que ocultar y volver a mostrar: la GPU se reinició (driver actualizado,
  // TDR) o cambió el DPI, del que dependen fuentes y estilo.
  bool Lost() const { return lost_; }
  // Ventana (del juego) que debe quedar debajo. Mientras el panel esté visible,
  // en cada frame se comprueba si se ha puesto delante (un juego topmost sube
  // al activarse o al restaurarse) y se reafirma el orden Z sin activar nada.
  // nullptr lo desactiva; Hide() también.
  void StayAbove(HWND window);
  HWND Hwnd() const { return hwnd_; }
  UINT Width() const { return width_; }
  UINT Height() const { return height_; }
  // Zona que recibe clics (coordenadas de la ventana); fuera de ella el ratón
  // pasa a la ventana de debajo. nullptr = toda la ventana.
  void SetHitRegion(const RECT* rect);

  // draw se ejecuta dentro de un frame de ImGui.
  void Render(const std::function<void()>& draw);

  ImFont* BoldFont() const { return fonts_.Bold(); }
  ImFont* BigFont() const { return fonts_.Big(); }
  const ui::FontSet& FontSizes() const { return fonts_; }
  float Scale() const { return scale_; }

  std::function<void()> onCloseRequested; // Alt+F4 sobre la ventana
  std::function<void()> onDeactivated;    // el usuario hizo clic en otra ventana

private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  bool CreateGraphics(UINT width, UINT height);
  void DestroyGraphics();
  void SetupImGui();
  void SetNoActivate(bool noActivate);
  bool Resize(const RECT& rect); // false si la GPU se perdió
  void EnsureAbove();

  HINSTANCE instance_ = nullptr;
  Kind kind_ = Kind::Menu;
  HWND hwnd_ = nullptr;
  UINT width_ = 0, height_ = 0;
  float scale_ = 1.0f;
  RECT hitRegion_{};
  bool hasHitRegion_ = false;
  bool lost_ = false;
  bool noActivate_ = false;
  HWND below_ = nullptr;          // ventana que debe quedar debajo (StayAbove)
  ULONGLONG reassertSecond_ = 0;  // ventana de un segundo para contar reafirmaciones
  int reasserts_ = 0;
  ULONGLONG lastReassert_ = 0;
  bool fighting_ = false;         // el juego vuelve a ponerse encima sin parar

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain1> swapChain_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<IDCompositionDevice> dcomp_;
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> visual_;
  ImGuiContext* imgui_ = nullptr;
  ui::FontSet fonts_;
  bool fontMemoryFreed_ = false; // la copia del atlas en RAM ya se liberó
};

} // namespace spectre
