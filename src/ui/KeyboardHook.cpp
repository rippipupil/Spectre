#include "ui/KeyboardHook.h"

#include "common/Util.h"

#include <cstring>

namespace spectre {

KeyboardHook* KeyboardHook::active_ = nullptr;

namespace {

bool IsModifier(UINT vk) {
  switch (vk) {
  case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
  case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
  case VK_MENU: case VK_LMENU: case VK_RMENU:
  case VK_LWIN: case VK_RWIN:
    return true;
  default:
    return false;
  }
}

bool IsToggle(UINT vk) { return vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL; }

// Teclas que nunca son del panel: Windows o el juego deben seguir viéndolas.
bool AlwaysPasses(UINT vk) {
  switch (vk) {
  case VK_SNAPSHOT: case VK_SLEEP: case VK_PAUSE:
  case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
  case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK: case VK_MEDIA_STOP: case VK_MEDIA_PLAY_PAUSE:
  case VK_BROWSER_BACK: case VK_BROWSER_FORWARD: case VK_BROWSER_REFRESH: case VK_BROWSER_STOP:
  case VK_BROWSER_SEARCH: case VK_BROWSER_FAVORITES: case VK_BROWSER_HOME:
  case VK_LAUNCH_MAIL: case VK_LAUNCH_MEDIA_SELECT: case VK_LAUNCH_APP1: case VK_LAUNCH_APP2:
    return true;
  default:
    return false;
  }
}

// Alt+Tab, Alt+Esc, Ctrl+Esc, Ctrl+Shift+Esc, Ctrl+Alt+Supr: cambios de ventana
// y del sistema, que siguen funcionando con el panel abierto.
bool IsSystemCombo(UINT mods, UINT vk) {
  if ((mods == MOD_ALT || mods == (MOD_ALT | MOD_SHIFT)) && (vk == VK_TAB || vk == VK_ESCAPE)) return true;
  if ((mods & MOD_CONTROL) && vk == VK_ESCAPE) return true;
  if ((mods & MOD_CONTROL) && (mods & MOD_ALT) && vk == VK_DELETE) return true;
  return false;
}

UINT GenericVk(UINT vk) {
  switch (vk) {
  case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
  case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
  case VK_LMENU: case VK_RMENU: return VK_MENU;
  default: return vk;
  }
}

} // namespace

KeyboardHook::~KeyboardHook() { Uninstall(); }

bool KeyboardHook::Install(HWND target, HWND game, std::vector<Hotkey> passThrough) {
  target_ = target;
  game_ = game;
  passThrough_ = std::move(passThrough);
  for (auto& hk : passThrough_) hk.modifiers &= MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
  if (wanted_) return hook_ != nullptr || paused_ > 0;
  wanted_ = true;
  // Estado inicial: lo que esté pulsado ahora mismo (el Alt del atajo que abrió
  // el panel, por ejemplo) y las teclas de bloqueo. El resto, soltado.
  std::memset(state_, 0, sizeof(state_));
  std::memset(swallowed_, 0, sizeof(swallowed_));
  for (UINT vk : {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
    if (GetAsyncKeyState(int(vk)) & 0x8000) SetState(vk, true);
  for (UINT vk : {VK_CAPITAL, VK_NUMLOCK, VK_SCROLL})
    if (GetKeyState(int(vk)) & 1) state_[vk] |= 1;
  if (paused_ == 0) Hook();
  return hook_ != nullptr || paused_ > 0;
}

void KeyboardHook::Uninstall() {
  wanted_ = false;
  if (hook_) {
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
  }
  if (active_ == this) active_ = nullptr;
  std::memset(swallowed_, 0, sizeof(swallowed_));
}

void KeyboardHook::Reinstall() {
  if (!wanted_ || paused_ > 0) return;
  if (hook_) {
    UnhookWindowsHookEx(hook_); // falla si Windows ya lo había quitado: da igual
    hook_ = nullptr;
    active_ = nullptr;
  }
  // Las teclas soltadas mientras el gancho no estaba fueron al juego: que el
  // panel no las crea pulsadas.
  ReleaseSwallowed();
  Hook();
}

void KeyboardHook::Hook() {
  if (hook_) return;
  active_ = this;
  hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyboardHook::Proc, GetModuleHandleW(nullptr), 0);
  if (!hook_) {
    Log("Panel: no se pudo instalar el gancho de teclado (error %lu); el teclado no llegará al panel",
        GetLastError());
    active_ = nullptr;
  }
}

void KeyboardHook::PauseHook() {
  if (++paused_ == 1 && hook_) {
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    active_ = nullptr;
  }
}

void KeyboardHook::ResumeHook() {
  if (--paused_ > 0 || !wanted_) return;
  // Las teclas soltadas durante la pausa no llegaron: que el panel no las crea
  // pulsadas (Intro repetido, por ejemplo).
  ReleaseSwallowed();
  Hook();
}

void KeyboardHook::ReleaseSwallowed() {
  for (UINT vk = 1; vk < 256; ++vk) {
    if (!swallowed_[vk] || (GetAsyncKeyState(int(vk)) & 0x8000)) continue;
    swallowed_[vk] = false;
    SetState(vk, false);
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (target_ && IsWindow(target_)) Forward(false, vk, scan, false, false);
  }
}

void KeyboardHook::SetState(UINT vk, bool down) {
  auto set = [this](UINT k, bool d) {
    if (d) state_[k] |= 0x80;
    else state_[k] &= 0x7F;
  };
  set(vk, down);
  // Las parejas izquierda/derecha y su tecla genérica van juntas, como en el
  // estado que Windows da a una ventana con foco.
  if (vk == VK_LSHIFT || vk == VK_RSHIFT) set(VK_SHIFT, Down(VK_LSHIFT) || Down(VK_RSHIFT));
  else if (vk == VK_LCONTROL || vk == VK_RCONTROL) set(VK_CONTROL, Down(VK_LCONTROL) || Down(VK_RCONTROL));
  else if (vk == VK_LMENU || vk == VK_RMENU) set(VK_MENU, Down(VK_LMENU) || Down(VK_RMENU));
}

UINT KeyboardHook::Modifiers() const {
  UINT mods = 0;
  if (Down(VK_LMENU) || Down(VK_RMENU)) mods |= MOD_ALT;
  if (Down(VK_LCONTROL) || Down(VK_RCONTROL)) mods |= MOD_CONTROL;
  if (Down(VK_LSHIFT) || Down(VK_RSHIFT)) mods |= MOD_SHIFT;
  if (Down(VK_LWIN) || Down(VK_RWIN)) mods |= MOD_WIN;
  return mods;
}

LRESULT CALLBACK KeyboardHook::Proc(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && active_ && lp) {
    const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    if (active_->Handle(UINT(wp), key)) return 1;
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

bool KeyboardHook::Handle(UINT msg, const KBDLLHOOKSTRUCT& key) {
  const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
  const UINT vk = key.vkCode;
  if (vk == 0 || vk >= 256 || !target_ || !IsWindow(target_)) return false;

  // Con otra aplicación delante (Alt+Tab desde el juego) el teclado es suyo.
  HWND foreground = GetForegroundWindow();
  const bool ours = foreground == target_ || (game_ ? foreground == game_ : true);
  if (!ours) {
    if (swallowed_[vk] && !down) swallowed_[vk] = false;
    return false;
  }

  const bool wasDown = Down(vk);
  SetState(vk, down);
  if (IsToggle(vk) && down && !wasDown) state_[vk] ^= 1;
  const bool extended = (key.flags & LLKHF_EXTENDED) != 0;
  const bool repeat = down && wasDown;

  if (IsModifier(vk) || IsToggle(vk)) {
    // El panel debe saber que Shift o Ctrl están pulsados, pero el juego también.
    Forward(down, vk, key.scanCode, extended, repeat);
    return false;
  }

  const UINT mods = Modifiers();
  bool pass = (mods & MOD_WIN) != 0 || AlwaysPasses(vk) || IsSystemCombo(mods, vk);
  for (const auto& hk : passThrough_)
    if (hk.vk == vk && hk.modifiers == mods) pass = true;
  if (pass) return false;

  // Pulsada antes de abrir el panel: el juego espera que se suelte.
  if (!down && !swallowed_[vk]) return false;
  swallowed_[vk] = down;

  if (mods == MOD_ALT && vk == VK_F4) {
    if (down && !repeat && onCloseRequested) onCloseRequested();
    return true;
  }
  Forward(down, vk, key.scanCode, extended, repeat);
  return true;
}

void KeyboardHook::Forward(bool down, UINT vk, UINT scanCode, bool extended, bool repeat) {
  // GetKeyState en el panel (modificadores de ImGui, TranslateMessage) lee el
  // estado del hilo: se pone el que ve el gancho, que es el real.
  SetKeyboardState(state_);
  LPARAM lp = 1 | (LPARAM(scanCode & 0xFF) << 16);
  if (extended) lp |= LPARAM(1) << 24;
  if (!down) lp |= (LPARAM(1) << 30) | (LPARAM(1) << 31);
  else if (repeat) lp |= LPARAM(1) << 30;
  SendMessageW(target_, down ? WM_KEYDOWN : WM_KEYUP, GenericVk(vk), lp);
  if (!down || IsModifier(vk) || IsToggle(vk)) return;

  // El carácter, con la distribución del teclado de la ventana activa (la del
  // juego): tildes y AltGr salen como en cualquier otra aplicación. Las teclas
  // muertas se guardan en el estado de este hilo, no en el del juego, que no
  // ve la tecla.
  DWORD thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  HKL layout = GetKeyboardLayout(thread);
  wchar_t text[8];
  int n = ToUnicodeEx(vk, scanCode, state_, text, 8, 0, layout);
  for (int i = 0; i < n; ++i) SendMessageW(target_, WM_CHAR, WPARAM(text[i]), lp);
}

} // namespace spectre
