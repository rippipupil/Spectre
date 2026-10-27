#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "ui/Fonts.h"
#include "ui/Texture.h"

#include <cstdint>
#include <functional>
#include <memory>

struct ImGuiContext;

namespace spectre {

struct Palette;

// Ventana normal de Spectre (la que se abre desde el menú Inicio, el exe o la
// bandeja), dibujada con Dear ImGui sobre una swapchain flip de Direct3D 11.
// Ventana, GPU e ImGui se crean al abrirla y se destruyen al cerrarla: cerrada
// no gasta nada. Abierta solo dibuja cuando hace falta (ratón, teclado, algo
// que cambia o una animación que lo pide), y nunca minimizada.
class AppWindow {
public:
  ~AppWindow() { Close(); }
  void Init(HINSTANCE instance);
  // La crea o, si ya existe, la restaura y la trae al frente. activate=false
  // la muestra sin quitarle el foco a nadie (recrearla tras perder la GPU).
  bool Open(bool activate = true);
  void Close();
  bool Visible() const { return hwnd_ != nullptr; }
  bool Minimized() const;
  bool Focused() const;
  HWND Hwnd() const { return hwnd_; }
  // La GPU se reinició: hay que cerrarla y volver a abrirla.
  bool Lost() const { return lost_; }
  // Arrastrando o redimensionando: Windows está dentro de su bucle modal.
  bool Sizing() const { return sizing_; }

  // Frames bajo demanda.
  bool NeedsFrame(unsigned long long now) const;
  // Milisegundos hasta el siguiente frame necesario; INFINITE si no hace falta ninguno.
  DWORD WaitMs(unsigned long long now) const;
  void Invalidate(); // cambió algo que se ve: unos frames
  // Durante el dibujo: hace falta otro frame enseguida (vídeo, barra de
  // progreso) o no antes de `when` (un reloj que cambia cada segundo).
  void Animate();
  void AnimateAt(unsigned long long when);

  // draw se ejecuta dentro de un frame de ImGui.
  void Render(const std::function<void()>& draw);

  ID3D11Device* Device() const { return device_.Get(); }
  bool VideoSupport() const { return videoSupport_; }
  // mipmaps=true para imágenes que se ven más pequeñas que su tamaño (capturas
  // en el visor): sin ellas, reducir más de la mitad deja los bordes dentados.
  std::shared_ptr<UiTexture> CreateTexture(const uint8_t* bgra, int width, int height, bool mipmaps = false);
  // Textura en la que se puede dibujar (fotogramas del reproductor).
  std::shared_ptr<UiTexture> CreateRenderTexture(int width, int height);

  ImFont* BoldFont() const { return fonts_.Bold(); }
  ImFont* BigFont() const { return fonts_.Big(); }
  const ui::FontSet& FontSizes() const { return fonts_; }
  float Scale() const { return scale_; }
  // Barra de título con los colores del tema (Windows 11; en Windows 10 solo el modo oscuro).
  void SetTitleColors(const Palette& palette);
  // Letras del diseño del tema. Antes de Open: el primer frame ya sale con
  // ellas. Abierta: se cambian antes del siguiente frame.
  void SetFonts(Face body, Face display);

  std::function<void()> onCloseRequested; // botón X o Alt+F4
  std::function<void()> onMinimized;      // minimizada: el visor pausa el vídeo
  // Mientras se arrastra el borde Windows no devuelve el control al bucle de
  // mensajes: este callback dibuja un frame desde dentro (temporizador).
  std::function<void()> onModalFrame;

private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  bool CreateGraphics();
  void DestroyGraphics();
  void SetupImGui();
  bool ResizeBuffers();
  void Input(); // hubo entrada: unos frames más para que el hover y las animaciones se asienten

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  float scale_ = 1.0f;
  bool lost_ = false;
  bool cloaked_ = false;  // oculta hasta el primer frame (sin destello blanco)
  bool sizing_ = false;   // arrastrando el borde
  bool rebuildFonts_ = false; // cambió el DPI o el diseño trae otras letras
  Face fontBody_ = Face::Segoe, fontDisplay_ = Face::Segoe; // letras del diseño
  UINT width_ = 0, height_ = 0;
  bool resizePending_ = false;
  bool videoSupport_ = false;
  RECT lastRect_{};       // posición al cerrar, para reabrir en el mismo sitio
  bool hasLastRect_ = false;
  bool lastMaximized_ = false;
  COLORREF title_[3] = {};  // colores de la barra de título ya aplicados
  bool titleSet_ = false;

  int framesPending_ = 0;
  unsigned long long inputUntil_ = 0;
  unsigned long long animateAt_ = ~0ull;  // siguiente frame pedido por la interfaz
  unsigned long long nextAnimate_ = ~0ull; // lo que pide el frame en curso

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain1> swapChain_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<ID3D11SamplerState> mipSampler_; // se crea con la primera textura con mipmaps
  ImGuiContext* imgui_ = nullptr;
  ui::FontSet fonts_;
  bool fontMemoryFreed_ = false; // la copia del atlas en RAM ya se liberó
};

} // namespace spectre
