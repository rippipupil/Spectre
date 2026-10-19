#pragma once

#include <windows.h>

#include <functional>
#include <vector>

namespace spectre {

// Teclado para el panel cuando se abre sin coger el foco.
//
// Sobre un juego a pantalla completa el panel no se activa (si le quita el
// foco, los juegos hechos con GLFW, como Geometry Dash, se minimizan y DXGI
// exclusivo sale del modo). Sin foco el ratón llega igual a la ventana del
// panel, pero el teclado va al juego. Este gancho WH_KEYBOARD_LL, instalado
// solo mientras el panel está abierto, reenvía cada tecla a la ventana del
// panel como WM_KEYDOWN/WM_KEYUP/WM_CHAR (el backend Win32 de ImGui los trata
// igual que con foco) y se la traga para que el juego no la vea. No corre
// nada en el proceso del juego.
//
// Se dejan pasar: los modificadores y las teclas de bloqueo (si se tragaran,
// el juego se quedaría con Alt pulsado y Bloq Mayús no cambiaría), la tecla
// Windows y sus combinaciones, Alt+Tab, Alt+Esc, Ctrl+Esc y Ctrl+Shift+Esc,
// Impr Pant, las teclas multimedia y los atajos de Spectre, que tienen que
// seguir llegando a RegisterHotKey. Alt+F4 cierra el panel en vez del juego.
// Una tecla pulsada antes de abrir el panel se suelta en el juego.
//
// Si el usuario cambia a otra aplicación con el panel abierto (Alt+Tab), el
// teclado deja de tragarse: solo se intercepta mientras el juego sobre el que
// se abrió el panel, o el propio panel, está en primer plano.
class KeyboardHook {
public:
  struct Hotkey {
    UINT modifiers = 0; // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN (como RegisterHotKey)
    UINT vk = 0;
  };

  KeyboardHook() = default;
  ~KeyboardHook();
  KeyboardHook(const KeyboardHook&) = delete;
  KeyboardHook& operator=(const KeyboardHook&) = delete;

  // target: ventana del panel que recibe las teclas. game: ventana que tenía el
  // foco al abrir (nula = cualquiera). Se puede volver a llamar para cambiar
  // la ventana o los atajos sin reinstalar el gancho. Solo desde el hilo de la UI.
  bool Install(HWND target, HWND game, std::vector<Hotkey> passThrough);
  void Uninstall();
  bool Installed() const { return wanted_; }
  // Windows quita sin avisar un gancho de bajo nivel cuyo callback tarda más
  // de LowLevelHooksTimeout (como mucho 1 s), y el handle no lo refleja: tras
  // un frame largo del hilo de la UI se quita y se vuelve a poner.
  void Reinstall();

  // Windows elimina sin avisar un gancho de bajo nivel que tarda en responder,
  // y mientras tanto el teclado de todo el sistema va con retraso: antes de
  // una operación larga en el hilo de la UI (reiniciar la captura, guardar un
  // clip) se quita el gancho y se vuelve a poner al terminar.
  class Pause {
  public:
    explicit Pause(KeyboardHook& hook) : hook_(hook) { hook_.PauseHook(); }
    ~Pause() { hook_.ResumeHook(); }
    Pause(const Pause&) = delete;
    Pause& operator=(const Pause&) = delete;

  private:
    KeyboardHook& hook_;
  };

  std::function<void()> onCloseRequested; // Alt+F4 con el panel abierto

private:
  static LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp);
  bool Handle(UINT msg, const KBDLLHOOKSTRUCT& key); // true = no pasar al juego
  void Forward(bool down, UINT vk, UINT scanCode, bool extended, bool repeat);
  void ReleaseSwallowed();
  void SetState(UINT vk, bool down);
  bool Down(UINT vk) const { return (state_[vk] & 0x80) != 0; }
  UINT Modifiers() const;
  void Hook();
  void PauseHook();
  void ResumeHook();

  HHOOK hook_ = nullptr;
  HWND target_ = nullptr;
  HWND game_ = nullptr;
  std::vector<Hotkey> passThrough_;
  BYTE state_[256]{};      // estado de teclado que se da al panel (SetKeyboardState)
  bool swallowed_[256]{};  // teclas cuya pulsación se tragó: su liberación también
  bool wanted_ = false;    // instalado lógicamente, aunque esté en pausa
  int paused_ = 0;

  static KeyboardHook* active_;
};

} // namespace spectre
