#include "ui/Toast.h"

#include "app/Config.h"
#include "ui/OverlayWindow.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>

namespace spectre {

using namespace ui;

namespace {

// Medidas en píxeles lógicos.
constexpr float kPadX = 16.0f, kPadY = 13.0f;
constexpr float kIcon = 32.0f;     // columna del icono
constexpr float kIconGap = 12.0f;  // del icono al texto
constexpr float kLineGap = 3.0f;   // entre título y detalle
constexpr float kHeaderH = 30.0f;  // franja del título en Táctico
constexpr float kTile = 36.0f;     // cuadro del icono en Suave

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

size_t Utf8Length(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

// Las primeras `count` letras (sin partir una tilde por la mitad).
std::string Utf8Prefix(const std::string& s, size_t count) {
  size_t i = 0, n = 0;
  while (i < s.size() && n < count) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    ++n;
  }
  return s.substr(0, i);
}

// Líneas que ocupa un texto cortado por palabras a `perLine` letras (estimación).
int WrapLines(const std::string& text, int perLine) {
  if (text.empty()) return 0;
  int lines = 1, used = 0;
  size_t i = 0;
  while (i < text.size()) {
    size_t space = text.find(' ', i);
    if (space == std::string::npos) space = text.size();
    int word = int(Utf8Length(text.substr(i, space - i)));
    if (used > 0 && used + 1 + word > perLine) {
      ++lines;
      used = 0;
    }
    // Una palabra más larga que la línea (un nombre de archivo) se parte.
    while (word > perLine) {
      word -= perLine;
      ++lines;
    }
    used += (used > 0 ? 1 : 0) + word;
    i = space + 1;
  }
  return lines;
}

Icon IconFor(NoticeKind kind) {
  switch (kind) {
  case NoticeKind::ClipSaving:
  case NoticeKind::Clip:
  case NoticeKind::ReplayOn:
  case NoticeKind::ReplayOff: return Icon::Replay;
  case NoticeKind::Recording: return Icon::Record;
  case NoticeKind::RecordingSaved:
  case NoticeKind::Saved: return Icon::Check;
  case NoticeKind::Screenshot: return Icon::Shot;
  case NoticeKind::Marker: return Icon::Marker;
  case NoticeKind::MicOn:
  case NoticeKind::MicOff: return Icon::Mic;
  case NoticeKind::CameraOn:
  case NoticeKind::CameraOff: return Icon::Camera;
  case NoticeKind::Quality: return Icon::Sliders;
  case NoticeKind::Game: return Icon::Gamepad;
  case NoticeKind::Working: return Icon::Pencil;
  case NoticeKind::Export: return Icon::Share;
  case NoticeKind::Copied: return Icon::Copy;
  case NoticeKind::Storage: return Icon::Trash;
  case NoticeKind::Update: return Icon::Download;
  case NoticeKind::Warning:
  case NoticeKind::Error: return Icon::Alert;
  case NoticeKind::Auto:
  case NoticeKind::Info: break;
  }
  return Icon::Info;
}

bool IsOffKind(NoticeKind kind) {
  return kind == NoticeKind::ReplayOff || kind == NoticeKind::MicOff || kind == NoticeKind::CameraOff;
}

// Dónde va cada cosa, en píxeles lógicos.
struct Layout {
  float textX = 0;   // desde el borde izquierdo
  float wrap = 0;    // ancho del texto
  float top = 0;     // primera línea de texto
  bool header = false;
  bool icon = true;
};

Layout LayoutFor(const Style& st) {
  Layout l;
  l.top = kPadY;
  switch (st.toast) {
  case ToastLook::Terminal:
    l.icon = false;
    l.textX = kPadX;
    break;
  case ToastLook::Card:
    l.textX = kPadX + kTile + kIconGap;
    break;
  case ToastLook::Header:
    l.header = true;
    l.top = kHeaderH + kPadY - 2.0f;
    l.textX = kPadX + kIcon + kIconGap;
    break;
  default:
    l.textX = kPadX + kIcon + kIconGap;
    break;
  }
  l.wrap = kToastWidth - l.textX - kPadX;
  return l;
}

// En Táctico el título va en la franja: en mayúsculas.
std::string TitleText(const Style& st, const Notice& n) {
  return st.toast == ToastLook::Header ? Upper(n.title) : n.title;
}

} // namespace

bool IsSavedKind(NoticeKind kind) {
  return kind == NoticeKind::Clip || kind == NoticeKind::RecordingSaved || kind == NoticeKind::Saved ||
         kind == NoticeKind::Export;
}

Notice MakeNotice(const std::string& text, bool error, NoticeKind kind) {
  Notice n;
  n.error = error;
  // "Título: detalle" o "Título. Detalle" si el título es corto; si no, todo
  // es detalle (o título, si cabe en una línea).
  const size_t colon = text.find(": "), dot = text.find(". ");
  if (colon != std::string::npos && colon <= 40 && (dot == std::string::npos || colon < dot)) {
    n.title = text.substr(0, colon);
    n.detail = text.substr(colon + 2);
  } else if (dot != std::string::npos && dot <= 60) {
    n.title = text.substr(0, dot);
    n.detail = text.substr(dot + 2);
  } else if (Utf8Length(text) <= 44) {
    n.title = text;
  } else {
    n.detail = text;
  }
  // Una frase de detalle empieza en mayúscula; un nombre de archivo, tal cual.
  if (!n.detail.empty() && n.detail.find(' ') != std::string::npos && n.detail[0] >= 'a' && n.detail[0] <= 'z')
    n.detail[0] = char(n.detail[0] - 32);
  if (n.title.empty() && error) n.title = "Algo no ha salido bien";

  if (kind != NoticeKind::Auto) {
    n.kind = kind;
    return n;
  }
  struct Rule {
    const char* prefix;
    NoticeKind kind;
  };
  static const Rule kRules[] = {
      {"Clip guardado", NoticeKind::Clip},
      {"Clip corto guardado", NoticeKind::Clip},
      {"Captura guardada", NoticeKind::Screenshot},
      {"Grabación guardada", NoticeKind::RecordingSaved},
      {"Grabando", NoticeKind::Recording},
      {"Marca añadida", NoticeKind::Marker},
      {"Replay activado", NoticeKind::ReplayOn},
      {"Replay desactivado", NoticeKind::ReplayOff},
      {"Micrófono activado", NoticeKind::MicOn},
      {"Micrófono silenciado", NoticeKind::MicOff},
      {"Micrófono desactivado", NoticeKind::MicOff},
      {"Cámara activada", NoticeKind::CameraOn},
      {"Cámara desactivada", NoticeKind::CameraOff},
      {"Calidad", NoticeKind::Quality},
      {"Recorte guardado", NoticeKind::Saved},
      {"Unión guardada", NoticeKind::Saved},
      {"Cámara lenta guardada", NoticeKind::Saved},
      {"Fotograma guardado", NoticeKind::Saved},
      {"Dividido en dos", NoticeKind::Saved},
      {"Edición guardada", NoticeKind::Saved},
      {"Recortando", NoticeKind::Working},
      {"Uniendo", NoticeKind::Working},
      {"Haciendo", NoticeKind::Working},
      {"Dividiendo", NoticeKind::Working},
      {"Exportando", NoticeKind::Working},
      {"Lista para Discord", NoticeKind::Export},
      {"Ya cabe en Discord", NoticeKind::Export},
      {"Copiado", NoticeKind::Copied},
      {"Spectre se ha actualizado", NoticeKind::Update},
      {"La actualización", NoticeKind::Update},
      {"Versión nueva", NoticeKind::Update},
      {"Límite de espacio", NoticeKind::Storage},
      {"Más de ", NoticeKind::Storage},
      {"Tus favoritos", NoticeKind::Storage},
      {"Lo más reciente", NoticeKind::Storage},
      {"Movido a la papelera", NoticeKind::Storage},
      {"Borrado", NoticeKind::Storage},
  };
  n.kind = error ? NoticeKind::Error : NoticeKind::Info;
  for (const Rule& r : kRules) {
    if (StartsWith(text, r.prefix)) {
      // Un error con el mismo comienzo (p. ej. "Grabación detenida") sigue siendo error.
      if (!error) n.kind = r.kind;
      break;
    }
  }
  return n;
}

float EstimateToastHeight(const Config& config, const Notice& n) {
  const Style& st = StyleFor(FindTheme(config.theme), config.themeStyle);
  const Layout l = LayoutFor(st);
  // Unas 8 px por letra en el título (negrita de 15) y 6,5 en el detalle (13).
  const int titleChars = std::max(10, int(l.wrap / (l.header ? 8.6f : 8.2f)));
  const int detailChars = std::max(10, int(l.wrap / 6.5f));
  float h = l.top;
  if (!l.header && !n.title.empty()) h += float(WrapLines(TitleText(st, n), titleChars)) * kBody;
  if (!n.detail.empty()) h += (l.header || n.title.empty() ? 0.0f : kLineGap) + float(WrapLines(n.detail, detailChars)) * kSmall;
  h += kPadY;
  const float iconH = l.top + (st.toast == ToastLook::Card ? kTile : kIcon) + kPadY;
  return std::max({h, l.icon ? iconH : 0.0f, 58.0f});
}

float DrawToast(OverlayWindow& w, const Config& config, const ToastFrame& f) {
  BeginFrame(Fonts{w.Scale(), w.BoldFont(), w.BigFont(), &w.FontSizes()}, config);
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  const Notice& n = *f.notice;
  const bool motion = ThemeMotion();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, f.fade);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::Begin("toast", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 size = io.DisplaySize;
  const Layout l = LayoutFor(st);
  const ImU32 tone = n.error ? p.danger : n.warning ? p.warning : p.accent;
  const ImU32 onTone = n.error ? p.onDanger : n.warning ? IM_COL32(17, 17, 17, 255) : p.onAccent;
  const float age = std::max(0.0f, f.age), phase = std::max(0.0f, f.phaseAge);
  const bool saving = n.kind == NoticeKind::ClipSaving;
  const bool saved = IsSavedKind(n.kind) && !n.error;

  // ---------------------------------------------------------------- entrada
  // Todo se dibuja desplazado `o`: la ventana recorta lo que se sale, así que
  // parece entrar desde detrás de su borde.
  ImVec2 o(0, 0);
  float reveal = 1.0f; // Terminal: líneas ya escritas
  if (motion) {
    const float t = age / 0.30f;
    switch (st.toast) {
    case ToastLook::Bar: o.x = (1.0f - EaseOut(t)) * size.x * 0.55f * (f.fromRight ? 1.0f : -1.0f); break;
    case ToastLook::TopLine: o.y = (1.0f - EaseOut(t)) * Px(22) * (f.fromBottom ? 1.0f : -1.0f); break;
    case ToastLook::Card: o.y = (1.0f - BackOut(t)) * Px(18) * (f.fromBottom ? 1.0f : -1.0f); break;
    case ToastLook::Header: {
      static const float kJitter[] = {9.0f, -6.0f, 5.0f, -3.0f, 2.0f, -1.0f, 0.0f};
      o.x = std::round(Px(kJitter[std::min(int(age / 0.03f), 6)]));
      break;
    }
    case ToastLook::Terminal: reveal = age; break;
    default: break;
    }
  } else {
    reveal = 1e9f;
  }
  const ImVec2 a = o, b = o + size;

  // ---------------------------------------------------------------- tarjeta
  // Destello de "guardado" o de captura (obturador) sobre el fondo.
  float burst = 0.0f;
  if (motion && saved) burst = std::max(0.0f, 1.0f - phase / 0.35f);
  float shutter = motion && n.kind == NoticeKind::Screenshot ? std::max(0.0f, 1.0f - age / 0.22f) : 0.0f;
  switch (st.toast) {
  case ToastLook::Bar: {
    dl->AddRectFilled(a, b, C(Mix(p.bg, tone, 0.22f * burst)));
    dl->AddRect(a, b, C(p.line), 0, 0, LineW());
    // La barra crece de arriba abajo al entrar.
    const float grow = motion ? EaseOut(age / 0.4f) : 1.0f;
    dl->AddRectFilled(a, ImVec2(a.x + Px(3), a.y + (b.y - a.y) * grow), C(tone));
    break;
  }
  case ToastLook::Ghost: {
    ShapeFill(dl, a, b, C(Mix(p.bg, tone, 0.18f * burst), motion ? std::min(1.0f, age / 0.18f) : 1.0f));
    ShapeStroke(dl, a, b, C(p.line));
    // Dos contornos fantasma convergen hacia el borde.
    if (motion && age < 0.36f) {
      for (int i = 0; i < 2; ++i) {
        float t = (age - 0.06f * float(i)) / 0.30f;
        if (t < 0 || t >= 1) continue;
        float inset = Px(14) * (1.0f - EaseOut(t)) + Px(1);
        ShapeStroke(dl, a + ImVec2(inset, inset), b - ImVec2(inset, inset), C(tone, 0.7f * (1.0f - t)), Px(1));
      }
    }
    break;
  }
  case ToastLook::Header: {
    dl->AddRectFilled(a, b, C(p.bg));
    dl->AddRect(a, b, C(p.line), 0, 0, LineW());
    ImVec2 ha = a, hb(b.x, a.y + Px(kHeaderH));
    ShapeFill(dl, ha, hb, C(Mix(tone, onTone, 0.45f * burst)));
    break;
  }
  case ToastLook::Terminal:
    dl->AddRectFilled(a, b, C(p.bg));
    DrawBrackets(dl, a + ImVec2(Px(3), Px(3)), b - ImVec2(Px(3), Px(3)), C(saved && burst > 0 ? tone : p.lineStrong),
                 Px(12));
    break;
  case ToastLook::TopLine: {
    dl->AddRectFilled(a, b, C(p.bg));
    dl->AddRect(a, b, C(p.lineStrong), 0, 0, LineW());
    // La línea de arriba se abre desde el centro.
    const float grow = motion ? EaseOut(age / 0.4f) : 1.0f, mid = (a.x + b.x) * 0.5f, half = (b.x - a.x) * 0.5f * grow;
    dl->AddRectFilled(ImVec2(mid - half, a.y), ImVec2(mid + half, a.y + Px(2)), C(tone));
    break;
  }
  case ToastLook::Card:
    dl->AddRectFilled(a, b, C(Mix(p.surface, tone, 0.12f * burst)));
    break;
  case ToastLook::Ink:
    dl->AddRectFilled(a, b, C(p.bg));
    dl->AddRect(a, b, C(p.lineStrong), 0, 0, LineW());
    break;
  case ToastLook::Bold:
    dl->AddRectFilled(a, b, C(p.bg));
    dl->AddRect(a, b, C(n.error ? p.danger : p.text), 0, 0, LineW());
    break;
  }
  if (shutter > 0) dl->AddRectFilled(a, b, C(p.text, 0.30f * shutter));

  // ---------------------------------------------------------------- icono
  if (l.icon) {
    const bool tile = st.toast == ToastLook::Card;
    const float box = Px(tile ? kTile : kIcon);
    ImVec2 c(a.x + Px(kPadX) + box * 0.5f, a.y + Px(l.top) + box * 0.5f);
    if (l.header) c.y = a.y + Px(l.top) + box * 0.5f - Px(2);
    float r = box * 0.30f;
    // Suave: el icono va en un cuadro del acento que salta al guardar.
    if (tile) {
      float pop = 1.0f;
      if (motion && saved && phase < 0.4f) pop = 1.0f + 0.22f * std::sin(std::min(1.0f, phase / 0.4f) * kPi);
      const float half = box * 0.5f * pop;
      dl->AddRectFilled(c - ImVec2(half, half), c + ImVec2(half, half), C(tone, 0.18f + 0.12f * burst));
      r *= pop;
    }
    // Sin pasar por C(): GetColorU32 aplica la transparencia del aviso una sola vez.
    const ImU32 iconCol = n.error || n.warning ? tone : n.kind == NoticeKind::Recording ? p.danger : p.text;
    const float t = float(ImGui::GetTime());

    if (saving) {
      // Esperando al archivo: cada diseño a su manera.
      DrawIcon(dl, Icon::Replay, c, r, C(p.textMuted));
      if (motion) {
        RequestFrame();
        switch (st.toast) {
        case ToastLook::Ghost:
          for (int i = 0; i < 2; ++i) {
            float k = std::fmod(t / 0.8f + 0.5f * float(i), 1.0f);
            float d = r * (0.6f + 1.2f * k);
            const ImVec2 pts[] = {ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)};
            dl->AddPolyline(pts, 4, C(tone, 0.8f * (1.0f - k)), ImDrawFlags_Closed, Px(1));
          }
          break;
        case ToastLook::Header: {
          // Mira que se cierra y se abre.
          float k = 0.5f + 0.5f * std::sin(t * 9.0f);
          float half = box * (0.42f + 0.10f * k);
          DrawBrackets(dl, c - ImVec2(half, half), c + ImVec2(half, half), C(tone), Px(5));
          break;
        }
        case ToastLook::TopLine: {
          // Un segmento recorre el cuadrado del icono.
          const float half = box * 0.48f, side = half * 2.0f, len = side * 0.6f;
          float pos = std::fmod(t * side * 2.2f, side * 4.0f);
          for (float s = 0; s < len; s += Px(1)) {
            float q = std::fmod(pos + s, side * 4.0f);
            ImVec2 pt;
            if (q < side) pt = ImVec2(c.x - half + q, c.y - half);
            else if (q < side * 2) pt = ImVec2(c.x + half, c.y - half + (q - side));
            else if (q < side * 3) pt = ImVec2(c.x + half - (q - side * 2), c.y + half);
            else pt = ImVec2(c.x - half, c.y + half - (q - side * 3));
            dl->AddRectFilled(pt - ImVec2(Px(1), Px(1)), pt + ImVec2(Px(1), Px(1)), C(tone));
          }
          break;
        }
        case ToastLook::Card:
          // Tres barras que botan.
          for (int i = 0; i < 3; ++i) {
            float k = 0.5f + 0.5f * std::sin(t * 8.0f + float(i) * 1.2f);
            float bh = r * (0.5f + 1.1f * k), x = c.x + (float(i) - 1.0f) * r * 0.75f;
            dl->AddRectFilled(ImVec2(x - r * 0.18f, c.y + r * 0.9f - bh), ImVec2(x + r * 0.18f, c.y + r * 0.9f), C(tone));
          }
          break;
        default: {
          // Un arco que gira alrededor del icono.
          dl->PathArcTo(c, box * 0.46f, t * 6.0f, t * 6.0f + kPi * 0.9f, 18);
          dl->PathStroke(C(tone), 0, Px(2));
          break;
        }
        }
      }
    } else if (saved) {
      // Guardado: el visto se dibuja solo y, según el diseño, sale un eco.
      const float draw = motion ? EaseOut(phase / 0.32f) : 1.0f;
      if (motion && phase < 0.32f) RequestFrame();
      DrawCheck(dl, c, r * 1.15f, C(n.warning ? p.warning : tone), draw);
      if (motion && phase < 0.5f) {
        float k = phase / 0.5f;
        switch (st.toast) {
        case ToastLook::Ghost: {
          float d = r * (1.0f + 1.6f * EaseOut(k));
          const ImVec2 pts[] = {ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)};
          dl->AddPolyline(pts, 4, C(tone, 0.8f * (1.0f - k)), ImDrawFlags_Closed, Px(1));
          break;
        }
        case ToastLook::TopLine: {
          // Onda cuadrada desde el icono por toda la tarjeta.
          float reach = std::max(b.x - c.x, c.x - a.x) * EaseOut(k);
          dl->PushClipRect(a, b, true);
          dl->AddRectFilled(c - ImVec2(reach, reach), c + ImVec2(reach, reach), C(tone, 0.14f * (1.0f - k)));
          dl->PopClipRect();
          break;
        }
        case ToastLook::Header: {
          float half = box * (0.9f - 0.42f * EaseOut(k));
          DrawBrackets(dl, c - ImVec2(half, half), c + ImVec2(half, half), C(tone, 1.0f - k), Px(5));
          break;
        }
        default:
          break;
        }
      }
    } else {
      float alpha = 1.0f;
      ImVec2 at = c;
      if (motion) {
        if (n.kind == NoticeKind::Recording) {
          // La marca de grabar late.
          alpha = 0.55f + 0.45f * std::cos(t * kPi * 2.0f / 1.6f);
          RequestFrame();
        } else if (n.kind == NoticeKind::Marker) {
          at.y -= Px(8) * (1.0f - EaseOut(age / 0.3f)); // la bandera cae en su sitio
        } else if (n.kind == NoticeKind::Update) {
          at.y += Px(2) * std::sin(t * 6.0f); // la flecha bota
          RequestFrame();
        } else if (n.kind == NoticeKind::Working) {
          dl->PathArcTo(c, box * 0.46f, t * 6.0f, t * 6.0f + kPi * 0.9f, 18);
          dl->PathStroke(C(tone), 0, Px(2));
          RequestFrame();
        }
      }
      DrawIcon(dl, IconFor(n.kind), at, r, ImGui::GetColorU32(iconCol, alpha));
      if (IsOffKind(n.kind)) DrawSlash(dl, at, r, C(p.textMuted), C(tile ? Mix(p.surface, tone, 0.18f) : p.bg));
    }
  }

  // ---------------------------------------------------------------- texto
  const float x = a.x + Px(l.textX);
  float y = a.y + Px(l.top);
  const float wrap = Px(l.wrap);
  ImFont* titleFont = Bold();
  ImFont* detailFont = Sized(ImGui::GetFont(), Px(kSmall));
  const float ts = Px(kBody), ds = Px(kSmall);
  std::string title = TitleText(st, n), detail = n.detail;
  std::string prompt;
  if (st.toast == ToastLook::Terminal) {
    // Se escribe solo: el título a 45 letras por segundo y luego el detalle.
    prompt = "> ";
    if (saving) title += std::string("...").substr(0, size_t(std::fmod(age * 3.0f, 4.0f)));
    if (saved) title += "  [OK]";
    const size_t titleLen = Utf8Length(title);
    const float titleTime = float(titleLen) / 45.0f;
    if (reveal < titleTime) {
      title = Utf8Prefix(title, size_t(reveal * 45.0f));
      detail.clear();
    } else {
      detail = Utf8Prefix(detail, size_t((reveal - titleTime) * 110.0f));
    }
    if (motion && reveal < titleTime + float(Utf8Length(n.detail)) / 110.0f) RequestFrame();
    if (saving && motion) RequestFrame();
  }

  if (l.header) {
    // Táctico: el título en la franja, en mayúsculas espaciadas.
    const float hs = Px(kSmall), tracking = Px(1.2f);
    std::string text = title.empty() ? Upper(n.error ? "Error" : "Spectre") : title;
    text = Ellipsize(Bold(), hs, text, b.x - a.x - Px(kPadX) * 2 - tracking * float(Utf8Length(text)));
    float glitch = motion && saved && phase < 0.12f ? Px(phase < 0.06f ? 3.0f : -2.0f) : 0.0f;
    TrackedText(dl, Bold(), hs, ImVec2(a.x + Px(kPadX) + glitch, a.y + (Px(kHeaderH) - hs) * 0.5f), C(onTone),
                text.c_str(), tracking);
    if (!detail.empty()) {
      ImVec2 dsz = detailFont->CalcTextSizeA(ds, FLT_MAX, wrap, detail.c_str());
      dl->AddText(detailFont, ds, ImVec2(x, y), C(p.text), detail.c_str(), nullptr, wrap);
      y += dsz.y;
    }
  } else {
    if (!title.empty() || !prompt.empty()) {
      float px = x;
      if (!prompt.empty()) {
        Text(dl, titleFont, ts, ImVec2(px, y), C(tone), prompt.c_str());
        px += TextSize(titleFont, ts, prompt.c_str()).x;
      }
      const float tw = wrap - (px - x);
      ImVec2 tsz = titleFont->CalcTextSizeA(ts, FLT_MAX, tw, title.empty() ? " " : title.c_str());
      if (!title.empty()) dl->AddText(Sized(titleFont, ts), ts, ImVec2(px, y), C(p.text), title.c_str(), nullptr, tw);
      // Cursor de Terminal al final de lo escrito.
      if (st.toast == ToastLook::Terminal && detail.empty()) {
        ImFont* font = Sized(titleFont, ts);
        const char* text = title.c_str();
        const char* end = font->CalcWordWrapPositionA(ts / font->FontSize, text, text + title.size(), tw);
        const char* lastLine = text;
        while (end < text + title.size()) {
          lastLine = end;
          while (*lastLine == ' ') ++lastLine;
          end = font->CalcWordWrapPositionA(ts / font->FontSize, lastLine, text + title.size(), tw);
        }
        float cx = px + font->CalcTextSizeA(ts, FLT_MAX, 0.0f, lastLine).x + Px(2);
        float cy = y + tsz.y - ts;
        dl->AddRectFilled(ImVec2(cx, cy + Px(2)), ImVec2(cx + Px(8), cy + ts), C(tone, Blink()));
      }
      y += tsz.y;
    }
    if (!detail.empty()) {
      if (!title.empty()) y += Px(kLineGap);
      ImVec2 dsz = detailFont->CalcTextSizeA(ds, FLT_MAX, wrap, detail.c_str());
      dl->AddText(detailFont, ds, ImVec2(x, y), C(p.textMuted), detail.c_str(), nullptr, wrap);
      y += dsz.y;
    }
  }
  if (st.toast == ToastLook::Ink && saving && motion) {
    // Papel: un subrayado que va y viene mientras se guarda.
    float k = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 5.0f);
    float len = wrap * 0.3f, x0 = x + (wrap - len) * k;
    dl->AddRectFilled(ImVec2(x0, y + Px(4)), ImVec2(x0 + len, y + Px(5)), C(tone));
  }

  // ------------------------------------------------- tiempo que le queda
  const float life = std::clamp(f.life, 0.0f, 1.0f);
  switch (st.toast) {
  case ToastLook::Bar:
  case ToastLook::Ghost:
  case ToastLook::TopLine:
    if (!saving) dl->AddRectFilled(ImVec2(a.x, b.y - Px(2)), ImVec2(a.x + (b.x - a.x) * life, b.y), C(tone, 0.55f));
    if (st.toast == ToastLook::Ghost && !n.error && !saving) DrawTrace(dl, a, b, tone);
    break;
  case ToastLook::Header:
    if (!saving)
      dl->AddRectFilled(ImVec2(a.x, a.y + Px(kHeaderH) - Px(2)), ImVec2(a.x + (b.x - a.x) * life, a.y + Px(kHeaderH)),
                        C(onTone, 0.45f));
    break;
  case ToastLook::Ink: {
    // La línea gruesa de abajo es el tiempo; al guardar, barre de nuevo.
    float k = saving ? 1.0f : life;
    if (motion && saved && phase < 0.35f) k = EaseOut(phase / 0.35f) * life;
    dl->AddRectFilled(ImVec2(a.x, b.y - Px(3)), ImVec2(a.x + (b.x - a.x) * k, b.y), C(tone));
    break;
  }
  default:
    break;
  }

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
  // Alto que pide el contenido, sin el desplazamiento de la entrada.
  const float iconBottom = l.icon ? Px(l.top) + Px(st.toast == ToastLook::Card ? kTile : kIcon) : 0.0f;
  return std::ceil(std::max({y - a.y + Px(kPadY) + (l.header ? Px(2) : 0.0f), iconBottom + Px(kPadY), Px(58)}));
}

} // namespace spectre
