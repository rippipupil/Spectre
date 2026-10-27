#include "ui/Menu.h"

#include "common/Util.h"
#include "ui/Backdrop.h"
#include "ui/OverlayWindow.h"
#include "ui/PanelMedia.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace spectre {

using namespace ui;

namespace {

Fonts FontsOf(const OverlayWindow& w) { return Fonts{w.Scale(), w.BoldFont(), w.BigFont(), &w.FontSizes()}; }

// Lo que dice la biblioteca de un archivo: el juego (o el nombre si se
// renombró) y el tipo con su duración.
std::string ClipTitle(const MediaItem& item) {
  if (!item.spectre) return item.stem;
  return item.game.empty() ? std::string("Escritorio") : item.game;
}

std::string ClipDetail(const MediaItem& item) {
  std::string d = item.audioOnly ? "Audio" : MediaKindName(item.kind);
  if (item.duration > 0) d += " · " + FormatDuration(item.duration);
  return d;
}

// La imagen entera dentro del rectángulo, sin deformarla y centrada.
void DrawFitted(ImDrawList* dl, const UiTexture& tex, ImVec2 a, ImVec2 b, float alpha = 1.0f) {
  if (tex.width <= 0 || tex.height <= 0) return;
  const float bw = b.x - a.x, bh = b.y - a.y;
  const float k = std::min(bw / float(tex.width), bh / float(tex.height));
  const ImVec2 size(float(tex.width) * k, float(tex.height) * k);
  const ImVec2 q(std::floor(a.x + (bw - size.x) * 0.5f), std::floor(a.y + (bh - size.y) * 0.5f));
  AddTextureImage(dl, tex, q, q + size, ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE, alpha));
}

// Fila de un clip reciente: miniatura, juego, tipo y duración, y cuándo.
bool ClipRow(OverlayWindow& w, PanelMedia& media, int index, const MediaItem& clip) {
  const Palette& p = Colors();
  ImGui::PushID(index);
  const float h = RowHeight(44);
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  bool pressed = ImGui::InvisibleButton("clip", size);
  Motion m = Animate();
  bool hov = ImGui::IsItemHovered();
  ImGuiID id = ImGui::GetItemID();
  ImGui::PopID();
  if (hov) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip("Clic para verlo y compartirlo aquí, clic derecho para mostrarlo en la carpeta");
  }
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 rowA(a.x - Px(8), a.y), rowB(b.x + Px(8), b.y);
  TrackHover(id, rowA, rowB, hov);
  PaintButton(dl, rowA, rowB, Role::Quiet, m);
  // Miniatura 16:9 a la izquierda.
  const float th = std::round(h - Px(8)), tw = std::round(th * 16.0f / 9.0f);
  const ImVec2 ta(a.x, std::round(a.y + (h - th) * 0.5f)), tb(ta.x + tw, ta.y + th);
  dl->AddRectFilled(ta, tb, C(p.surface));
  if (UiTexture* tex = media.Thumbnail(w, clip, int(tw), int(th))) DrawFitted(dl, *tex, ta, tb);
  else DrawIcon(dl, clip.audioOnly ? Icon::Volume : Icon::Play, (ta + tb) * 0.5f, Px(5), C(p.textFaint));
  dl->AddRect(ta, tb, C(Mix(p.line, p.lineStrong, m.hover)), 0, 0, LineW());
  // Juego y cuándo arriba; tipo y duración debajo.
  const float ss = Px(kSmall), x = tb.x + Px(10), gap = Px(3);
  const float top = (a.y + b.y) * 0.5f - ss - gap * 0.5f;
  const float timeW = TextSize(ImGui::GetFont(), ss, clip.when.c_str()).x;
  std::string title = Ellipsize(Bold(), ss, ClipTitle(clip), b.x - x - timeW - Px(10));
  Text(dl, Bold(), ss, ImVec2(x, top), C(p.text), title.c_str());
  Text(dl, ImGui::GetFont(), ss, ImVec2(b.x - timeW, top), C(p.textFaint), clip.when.c_str());
  std::string detail = Ellipsize(ImGui::GetFont(), ss, ClipDetail(clip), b.x - x);
  Text(dl, ImGui::GetFont(), ss, ImVec2(x, top + ss + gap), C(Mix(p.textMuted, p.text, m.hover * 0.5f)),
       detail.c_str());
  PaintPress(dl, rowA, rowB, Role::Quiet, m);
  return pressed;
}

// Fila de una acción de compartir: icono, texto y, a la derecha, un dato
// pequeño (lo que hace o cómo va). progress >= 0 dibuja la barra de lo hecho.
bool ShareRow(const char* id, Icon icon, const char* label, const std::string& detail, ImU32 detailColor,
              float progress = -1.0f, const char* tooltip = nullptr) {
  const Palette& p = Colors();
  const float h = RowHeight(36);
  ImVec2 size(ImGui::GetContentRegionAvail().x, h);
  bool pressed = ImGui::InvisibleButton(id, size);
  Motion m = Animate();
  bool hov = ImGui::IsItemHovered();
  if (hov) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (tooltip) ImGui::SetTooltip("%s", tooltip);
  }
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 rowA(a.x - Px(8), a.y), rowB(b.x + Px(8), b.y);
  TrackHover(ImGui::GetItemID(), rowA, rowB, hov);
  PaintButton(dl, rowA, rowB, Role::Quiet, m);
  const float mid = (a.y + b.y) * 0.5f, fs = Px(kBody), ss = Px(kSmall);
  DrawIcon(dl, icon, ImVec2(a.x + Px(8), mid), Px(8), C(Mix(p.textMuted, p.text, m.hover)));
  const float dw = TextSize(ImGui::GetFont(), ss, detail.c_str()).x;
  std::string text = Ellipsize(ImGui::GetFont(), fs, label, b.x - a.x - Px(26) - dw - Px(10));
  Text(dl, ImGui::GetFont(), fs, ImVec2(a.x + Px(26), mid - fs * 0.5f), C(p.text), text.c_str());
  Text(dl, ImGui::GetFont(), ss, ImVec2(b.x - dw, mid - ss * 0.5f), C(detailColor), detail.c_str());
  if (progress >= 0) {
    // 1 px a lo ancho de la fila, con la parte hecha en el acento.
    const float y = b.y - Px(1), done = a.x + (b.x - a.x) * std::clamp(progress, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, b.y), C(p.line));
    dl->AddRectFilled(ImVec2(a.x, y), ImVec2(done, b.y), C(p.accent));
  }
  PaintPress(dl, rowA, rowB, Role::Quiet, m);
  return pressed;
}

// Barra de la vista previa: clic o arrastre para ir a otro momento (mientras
// se arrastra va pausada y salta al fotograma clave más cercano; al soltar,
// al punto exacto).
void PreviewTimeline(MenuModel& m, Player& player, double duration, float width) {
  const Palette& p = Colors();
  const float h = Px(16);
  ImGui::InvisibleButton("seek", ImVec2(width, h));
  const bool hov = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float mx = ImGui::GetMousePos().x;
  auto tOf = [&](float x) {
    return duration > 0 ? std::clamp(double((x - a.x) / (b.x - a.x)) * duration, 0.0, duration) : 0.0;
  };
  if (ImGui::IsItemActivated() && duration > 0) {
    m.scrubbing = true;
    m.scrubWasPlaying = player.Playing();
    player.Pause();
  }
  if (active && m.scrubbing && std::fabs(tOf(mx) - player.Position()) > 0.04) player.Seek(tOf(mx), false);
  if (ImGui::IsItemDeactivated() && m.scrubbing) {
    player.Seek(tOf(mx), true);
    if (m.scrubWasPlaying) player.Play();
    m.scrubbing = false;
  }
  const float mid = std::round((a.y + b.y) * 0.5f);
  const float done = duration > 0 ? a.x + float(std::clamp(player.Position() / duration, 0.0, 1.0)) * (b.x - a.x) : a.x;
  dl->AddRectFilled(ImVec2(a.x, mid - Px(1.5f)), ImVec2(b.x, mid + Px(1.5f)), C(p.surfaceActive));
  dl->AddRectFilled(ImVec2(a.x, mid - Px(1.5f)), ImVec2(done, mid + Px(1.5f)), C(p.accent));
  dl->AddRectFilled(ImVec2(done - Px(1), mid - Px(6)), ImVec2(done + Px(1), mid + Px(6)), C(p.text));
  if (hov && duration > 0) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (!active) ImGui::SetTooltip("%s", FormatDuration(tOf(mx)).c_str());
  }
}

// Vuelve de la vista de un clip a la lista (el vídeo se cierra; lo ya
// dibujado en este frame lo guarda PanelMedia hasta el siguiente).
void CloseClipView(MenuModel& m, PanelMedia& media) {
  m.viewing = false;
  m.scrubbing = false;
  media.ClosePreview();
}

// Un clip abierto dentro del panel: la vista previa, sus datos y las formas de
// compartirlo sin salir de la partida.
void DrawClipView(OverlayWindow& w, MenuModel& m, const MenuActions& a, PanelMedia& media) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float avail = ImGui::GetContentRegionAvail().x;
  int index = -1;
  for (size_t i = 0; i < m.clips.size(); ++i)
    if (m.clips[i].path == m.view.path) index = int(i);
  // La biblioteca pudo saber algo más desde que se abrió (la duración, un favorito).
  if (index >= 0) m.view = m.clips[size_t(index)];

  // Volver y pasar al anterior o al siguiente de los recientes.
  {
    const float rowH = Px(28);
    ImVec2 q = ImGui::GetCursorScreenPos();
    if (IconButton("back", Icon::Back, rowH, "Volver a los recientes (Esc)")) {
      CloseClipView(m, media);
      return;
    }
    const float ls = Px(kLabel);
    DrawLabel(dl, ImVec2(q.x + rowH + Px(6), q.y + (rowH - ls) * 0.5f), "RECIENTES", ls, C(p.textMuted));
    if (index >= 0 && m.clips.size() > 1) {
      std::string pos = std::to_string(index + 1) + " de " + std::to_string(m.clips.size());
      const float ss = Px(kSmall), pw = TextSize(ImGui::GetFont(), ss, pos.c_str()).x;
      const float right = q.x + avail - rowH * 2 - Px(4);
      Text(dl, ImGui::GetFont(), ss, ImVec2(right - pw - Px(8), q.y + (rowH - ss) * 0.5f), C(p.textFaint),
           pos.c_str());
      ImGui::SetCursorScreenPos(ImVec2(right, q.y));
      const int prev = index - 1, next = index + 1;
      ImGui::BeginDisabled(prev < 0);
      if (IconButton("prev", Icon::Prev, rowH, "Anterior (más nuevo)") && prev >= 0) m.view = m.clips[size_t(prev)];
      ImGui::EndDisabled();
      ImGui::SameLine(0, Px(4));
      ImGui::BeginDisabled(next >= int(m.clips.size()));
      if (IconButton("next", Icon::Next, rowH, "Siguiente (más antiguo)") && next < int(m.clips.size()))
        m.view = m.clips[size_t(next)];
      ImGui::EndDisabled();
    }
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(6)));
  }
  const MediaItem item = m.view; // copia: los botones de abajo pueden cambiarlo

  // Vista previa 16:9 a todo el ancho; clic para pausar o seguir.
  const float pw = std::floor(avail), ph = std::round(pw * 9.0f / 16.0f);
  ImVec2 va = ImGui::GetCursorScreenPos(), vb(va.x + pw, va.y + ph);
  const bool areaClicked = ImGui::InvisibleButton("preview", ImVec2(pw, ph));
  const bool areaHovered = ImGui::IsItemHovered();
  dl->AddRectFilled(va, vb, C(IM_COL32(0, 0, 0, 255)));
  Player* player = media.Preview(w, item, int(pw), int(ph));
  UiTexture* frame = player && !player->Failed() ? media.PreviewFrame() : nullptr;
  if (frame) {
    AddTextureImage(dl, *frame, va, vb, ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE));
  } else if (UiTexture* thumb = media.Thumbnail(w, item, int(pw), int(ph))) {
    DrawFitted(dl, *thumb, va, vb, player ? 0.6f : 1.0f);
  } else if (item.audioOnly) {
    DrawIcon(dl, Icon::Volume, (va + vb) * 0.5f, Px(18), C(p.textFaint));
  }
  const float ss = Px(kSmall);
  if (!player || player->Failed()) {
    // Sin vista previa: se ve la miniatura y el botón de abajo abre el reproductor.
    std::string why = player ? player->Error() : std::string("Aquí no se puede reproducir: ábrelo en Spectre.");
    const float wrap = pw - Px(20);
    ImFont* font = Sized(ImGui::GetFont(), ss);
    ImVec2 ts = font->CalcTextSizeA(ss, FLT_MAX, wrap, why.c_str());
    ImVec2 box(va.x, vb.y - ts.y - Px(16));
    dl->AddRectFilled(box, vb, C(p.bg, 0.85f));
    dl->AddText(font, ss, ImVec2(va.x + Px(10), box.y + Px(8)), C(p.text), why.c_str(), nullptr, wrap);
  } else {
    if (areaClicked) {
      if (player->Playing()) player->Pause();
      else player->Play();
    }
    if (areaHovered) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImGui::SetTooltip("%s", player->Playing() ? "Pausa (espacio)" : "Reproducir (espacio)");
    }
    if (!player->Playing() && player->Ready() && !m.scrubbing) {
      const ImVec2 c = (va + vb) * 0.5f;
      dl->AddRectFilled(c - ImVec2(Px(20), Px(20)), c + ImVec2(Px(20), Px(20)), C(p.bg, 0.75f));
      DrawIcon(dl, Icon::Play, c + ImVec2(Px(2), 0), Px(9), C(p.text));
    }
  }
  dl->AddRect(va, vb, C(p.line), 0, 0, LineW());
  ImGui::SetCursorScreenPos(ImVec2(va.x, vb.y + Px(4)));

  // Barra, reproducir, tiempo, sonido y favorito.
  const double duration = player && player->Duration() > 0 ? player->Duration() : item.duration;
  if (player && !player->Failed()) PreviewTimeline(m, *player, duration, pw);
  {
    const float rowH = Px(28);
    ImVec2 q = ImGui::GetCursorScreenPos();
    const bool playable = player && !player->Failed();
    ImGui::BeginDisabled(!playable);
    if (IconButton("play", playable && player->Playing() ? Icon::Pause : Icon::Play, rowH,
                   playable && player->Playing() ? "Pausa (espacio)" : "Reproducir (espacio)") && playable) {
      if (player->Playing()) player->Pause();
      else player->Play();
    }
    ImGui::EndDisabled();
    std::string time = (playable ? FormatDuration(player->Position()) + " / " : std::string()) +
                       (duration > 0 ? FormatDuration(duration) : std::string("--:--"));
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + rowH + Px(6), q.y + (rowH - ss) * 0.5f), C(p.textMuted), time.c_str());
    ImGui::SetCursorScreenPos(ImVec2(q.x + avail - rowH * 2 - Px(4), q.y));
    ImGui::BeginDisabled(!playable);
    if (IconButton("mute", media.muted ? Icon::Muted : Icon::Volume, rowH,
                   media.muted ? "Activar el sonido (M). Lo que suena en el PC también entra en el replay."
                               : "Silenciar (M)") &&
        playable) {
      media.muted = !media.muted;
      player->SetMuted(media.muted);
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, Px(4));
    if (IconButton("fav", item.favorite ? Icon::StarFilled : Icon::Star, rowH,
                   item.favorite ? "Quitar de favoritos" : "Favorito: el límite de espacio no lo borra", item.favorite) &&
        a.setFavorite) {
      a.setFavorite(item.path, !item.favorite);
      m.view.favorite = !item.favorite;
    }
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(4)));
  }

  // Qué es.
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    const float fs = Px(kBody);
    std::string title = Ellipsize(Bold(), fs, ClipTitle(item), avail);
    Text(dl, Bold(), fs, q, C(p.text), title.c_str());
    std::string detail = ClipDetail(item) + " · " + FormatBytes(item.size) + " · " + item.when;
    detail = Ellipsize(ImGui::GetFont(), ss, detail, avail);
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x, q.y + fs + Px(4)), C(p.textMuted), detail.c_str());
    ImGui::Dummy(ImVec2(avail, fs + ss + Px(6)));
  }

  Divider(10, 6);
  SectionLabel("COMPARTIR");
  if (ShareRow("copy", Icon::Copy, "Copiar el archivo", "Ctrl+V", p.textFaint, -1.0f,
               "Al portapapeles: pégalo con Ctrl+V en Discord, WhatsApp o una carpeta") &&
      a.copyFile)
    a.copyFile(item.path);
  if (!item.audioOnly && a.exportForDiscord) {
    const bool exporting = m.exportPercent >= 0;
    const bool exportingThis = exporting && m.exportPath == item.path;
    if (exportingThis) {
      std::string pct = std::to_string(m.exportPercent) + " %";
      if (ShareRow("discord", Icon::Share, "Exportando para Discord", pct, p.accent, float(m.exportPercent) / 100.0f,
                   "Al acabar se copia sola para pegarla con Ctrl+V. Clic para cancelar.") &&
          a.cancelExport)
        a.cancelExport();
    } else if (ShareRow("discord", Icon::Share, "Para Discord", exporting ? "otro en marcha" : "menos de 20 MB",
                        p.textFaint, -1.0f,
                        exporting ? "Ya se está exportando otro vídeo: espera a que termine"
                                  : "Una copia de menos de 20 MB que Discord reproduce en el chat, sin Nitro. Se "
                                    "copia sola al acabar. El original no cambia.")) {
      a.exportForDiscord(item.path); // si hay otra en marcha, el aviso lo dice
    }
  }
  if (a.shareLink) {
    const PanelLinkState link = a.linkState ? a.linkState(item.path) : PanelLinkState{};
    std::string detail = "enlace";
    ImU32 color = p.textFaint;
    float progress = -1.0f;
    const char* tip = "Sube el clip a tu nube y copia un enlace para pegarlo donde quieras";
    if (!link.configured) {
      detail = "sin nube";
      tip = "Configura tu nube en Spectre (Ajustes > Nube) para compartir con un enlace";
    } else if (link.busy) {
      detail = "subiendo " + std::to_string(int(std::clamp(link.progress, 0.0f, 1.0f) * 100.0f + 0.5f)) + " %";
      color = p.accent;
      progress = link.progress;
      tip = "Al acabar se copia el enlace solo";
    } else if (link.ready) {
      detail = "subido";
      color = p.accent;
      tip = "Ya está en tu nube: copia el enlace otra vez";
    } else if (!link.error.empty()) {
      detail = "falló";
      color = p.danger;
      tip = link.error.c_str();
    }
    if (ShareRow("link", Icon::External, link.ready ? "Copiar el enlace" : "Copiar enlace", detail, color, progress,
                 tip) &&
        !link.busy)
      a.shareLink(item.path);
  }

  // Teclado de la vista (llega también sin foco, por el gancho).
  if (player && !player->Failed()) {
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
      if (player->Playing()) player->Pause();
      else player->Play();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) player->Seek(player->Position() + 5.0);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) player->Seek(std::max(0.0, player->Position() - 5.0));
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
      media.muted = !media.muted;
      player->SetMuted(media.muted);
    }
  }
}

// Botón cuadrado con icono, del alto de los botones grandes (captura junto a grabar).
bool SquareButton(const char* id, Icon icon, float size, const char* tooltip) {
  const Palette& p = Colors();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  Motion m = Animate();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ButtonPaint bp = PaintButton(dl, a, b, Role::Normal, m);
  if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (tooltip) ImGui::SetTooltip("%s", tooltip);
  }
  DrawIcon(dl, icon, (bp.a + bp.b) * 0.5f, (bp.b.x - bp.a.x) * 0.2f, C(Mix(p.textMuted, bp.fg, m.hover)));
  PaintPress(dl, a, b, Role::Normal, m);
  return pressed;
}

void PanelStatus(const MenuModel& m) {
  const Palette& p = Colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q = ImGui::GetCursorScreenPos();
  float ss = Px(kSmall);
  ImU32 mark;
  bool filled = true;
  float alpha = 1.0f;
  std::string text;
  if (m.stats.recording) {
    mark = p.danger;
    text = "Grabando " + FormatDuration(m.stats.recordSeconds);
    // La marca late mientras se graba (en los dos temas: es el estado, no adorno).
    alpha = 0.55f + 0.45f * std::cos(float(GetTickCount64() % 1600) / 1600.0f * kPi * 2);
  } else if (m.replayActive) {
    mark = p.accent;
    text = "Replay activo, guarda los últimos " + FormatClipLength(m.config.clipSeconds);
    alpha = AmbientMotion() == Ambient::Cursor ? Blink() : Breathe();
  } else if (m.config.replayEnabled && m.config.onlyInGames) {
    mark = p.textMuted;
    filled = false;
    text = "Replay esperando a un juego";
  } else {
    mark = p.lineStrong;
    text = "Replay apagado";
  }
  float h = ss + Px(2);
  DrawMark(dl, ImVec2(q.x + Px(4), q.y + h * 0.5f), Px(3.5f), C(mark, alpha), filled);
  Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(16), q.y + (h - ss) * 0.5f),
       C(m.stats.recording ? p.text : p.textMuted), text.c_str());
  if (m.gameDetected && !m.gameName.empty()) {
    std::string game = Ellipsize(ImGui::GetFont(), ss, m.gameName, ImGui::GetContentRegionAvail().x - Px(16));
    Text(dl, ImGui::GetFont(), ss, ImVec2(q.x + Px(16), q.y + h + Px(2)), C(p.textFaint), game.c_str());
    h += ss + Px(4);
  }
  ImGui::Dummy(ImVec2(0, h));
}

// Rastro fantasma del tema Psíquico: un segundo contorno aparece 10 px por
// dentro del panel y se funde en 240 ms mientras llega al borde. Va por encima
// del panel y dentro de su rectángulo: el panel es opaco y la ventana recorta
// lo que quede fuera de la zona de clic, así que fuera no se vería. Usa la
// posición y el tamaño del frame anterior.
void DrawOpenGhost(const MenuModel& m, float width) {
  if (!ThemeMotion() || CurrentStyle().toast != ToastLook::Ghost || m.panelHeight <= 0) return;
  float t = float(GetTickCount64() - m.openedAt) / 240.0f;
  if (t >= 1.0f) return;
  t = std::clamp(t, 0.0f, 1.0f);
  float ease = 1.0f - (1.0f - t) * (1.0f - t);
  float inset = Px(10) * (1.0f - ease);
  float top = m.panelTop;
  ImGui::GetForegroundDrawList()->AddRect(ImVec2(inset, top + inset),
                                          ImVec2(width - inset, top + m.panelHeight - inset),
                                          ImGui::GetColorU32(Colors().accent, 0.55f * (1.0f - ease)), 0, 0, Px(1));
}

std::string WithKey(const char* text, const std::string& key) {
  return key.empty() ? std::string(text) : std::string(text) + " (" + key + ")";
}

// Detalles del diseño sobre el marco del panel (dentro de su rectángulo: la
// ventana recorta lo que quede fuera de la zona de clic).
void PanelDecor(ImDrawList* dl, ImVec2 a, ImVec2 b, bool onRight) {
  const Palette& p = Colors();
  const Style& st = CurrentStyle();
  switch (st.toast) {
  case ToastLook::Header:
    // Táctico: una franja del acento en el lado que da al centro de la pantalla.
    if (onRight) dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + Px(2), b.y), C(p.accent));
    else dl->AddRectFilled(ImVec2(b.x - Px(2), a.y), ImVec2(b.x, b.y), C(p.accent));
    break;
  case ToastLook::Terminal:
    DrawBrackets(dl, a + ImVec2(Px(4), Px(4)), b - ImVec2(Px(4), Px(4)), C(p.lineStrong), Px(16));
    break;
  case ToastLook::TopLine:
    dl->AddRectFilled(a, ImVec2(b.x, a.y + Px(2)), C(p.accent));
    break;
  default:
    break;
  }
}

// Marca de la cabecera con la letra y el estilo de títulos del diseño.
void PanelBrand(ImDrawList* dl, ImVec2 q, float rowH) {
  const Palette& p = Colors();
  const float mid = q.y + rowH * 0.5f;
  switch (CurrentStyle().title) {
  case TitleLook::Masthead: {
    // Papel: el nombre con serifa, como la cabecera de un periódico.
    const float fs = Px(kTitle);
    Text(dl, Display(fs), fs, ImVec2(q.x, mid - fs * 0.55f), C(p.text), "Spectre");
    break;
  }
  case TitleLook::Chroma: {
    // Neón: con las dos copias desplazadas de sus títulos.
    const float fs = Px(kBody + 2), tr = Px(4);
    ImFont* df = Display(fs);
    const ImVec2 pos(q.x, mid - fs * 0.5f);
    TrackedText(dl, df, fs, pos - ImVec2(Px(1.5f), 0), C(p.danger, 0.55f), "SPECTRE", tr);
    TrackedText(dl, df, fs, pos + ImVec2(Px(1.5f), 0), C(p.accent, 0.8f), "SPECTRE", tr);
    TrackedText(dl, df, fs, pos, C(p.text), "SPECTRE", tr);
    break;
  }
  case TitleLook::Echo:
  case TitleLook::Rule:
  case TitleLook::Soft:
  case TitleLook::Block: {
    // La "S" del raíl delante del nombre.
    const float mono = rowH * 0.86f;
    Monogram(dl, ImVec2(q.x + mono * 0.5f, mid), mono);
    Wordmark(dl, ImVec2(q.x + mono + Px(10), mid - Px(kSmall) * 0.5f), "SPECTRE", C(p.text));
    break;
  }
  case TitleLook::Index:
  case TitleLook::Prompt:
  case TitleLook::Heavy:
    Wordmark(dl, ImVec2(q.x, mid - Px(kSmall) * 0.5f), "SPECTRE", C(p.text));
    break;
  }
}

} // namespace

void DrawPanel(OverlayWindow& w, MenuModel& m, const MenuActions& a, PanelMedia& media) {
  media.NewFrame();
  BeginFrame(FontsOf(w), m.config);
  // La sombra de fila no sobrevive a una reapertura: el reloj de ImGui vuelve
  // a cero con cada contexto nuevo y un resto antiguo parpadearía.
  ResetHoverTrail(m.openedAt);
  const Palette& p = Colors();
  const float width = Px(kPanelWidth);
  const bool lateral = IsLateralPanel(m.config);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  // Entra desde su lado de la pantalla, con el movimiento del diseño.
  const bool onRight = (lateral ? m.config.panelLateralX : m.config.panelCompactX) > 0.5f;
  const Entrance enter = EnterMotion(m.openedAt, onRight);
  const float appear = ThemeMotion() ? enter.alpha : OpenProgress(m.openedAt);
  bool startDrag = false;

  DrawOpenGhost(m, width);

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(16), Px(16)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  // Suave va sin borde; Alto contraste, con 2 px.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                      CurrentStyle().divider == DividerStyle::Space ? 0.0f : CurrentStyle().line);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8), Px(4)));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, V(p.bg));
  ImGui::PushStyleColor(ImGuiCol_Border, V(p.line));
  // La ventana es una franja del ancho del panel (App::MenuRect). Lateral: el
  // panel la llena de arriba abajo. Compacto: del alto de su contenido, a la
  // altura guardada (0 arriba, 1 abajo; 0,5 es el centrado de antes). App
  // calcula la zona de clic con panelTop. Si no cabe (monitor pequeño con
  // escala alta), la rueda del ratón lo desplaza.
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
  if (lateral) {
    ImGui::SetNextWindowPos(enter.offset);
    ImGui::SetNextWindowSize(display);
  } else {
    const float fy = m.config.panelCompactY;
    ImGui::SetNextWindowPos(ImVec2(0, display.y * fy) + enter.offset, ImGuiCond_Always, ImVec2(0, fy));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, display.y));
    flags |= ImGuiWindowFlags_AlwaysAutoResize;
  }
  ImGui::Begin("##panel", nullptr, flags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& c = m.config;
  // El fondo ilustrado del tema detrás de todo el panel.
  DrawBackdrop(dl, ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImGui::GetWindowSize(), 0.7f);
  DrawHoverTrail(dl);

  // Cabecera: marca, cerrar y estado. La fila de la marca es el asa para
  // mover el panel (también vale cualquier hueco sin controles).
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float rowH = Px(28);
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("move", ImVec2(avail - rowH - Px(4), rowH));
    if (ImGui::IsItemActivated()) startDrag = true;
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (!m.dragging && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
      ImGui::SetTooltip("Arrastra para mover el panel");
    PanelBrand(dl, q, rowH);
    ImGui::SetCursorScreenPos(ImVec2(q.x + avail - rowH, q.y));
    if (IconButton("close", Icon::Close, rowH, "Cerrar (Esc)")) a.close();
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH + Px(6)));
    PanelStatus(m);
  }
  Divider(12, 12);

  // Lateral: el pie va pegado abajo, como en una barra lateral, y lo de
  // encima (los recientes o el clip abierto) llena el hueco que queda.
  const float spacing = ImGui::GetStyle().ItemSpacing.y;
  const float footerTop = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y -
                          Px(34) - (Px(6) + Px(1) + Px(8) + spacing * 3); // el separador de encima
  if (m.viewing) {
    DrawClipView(w, m, a, media);
  } else {
    if (ActionButton("clip", "Guardar clip", c.hotkeys.saveClip, Tone::Primary, m.replayActive)) a.saveClip();

    Gap(10);
    SectionLabel("DURACIÓN DEL CLIP");
    Gap(2);
    if (int v = ClipLengthPicker("len", c.clipSeconds, m.stats.replayFitSeconds); v > 0 && v != c.clipSeconds)
      a.setClipSeconds(v);

    Gap(6);
    {
      const float bh = ActionHeight(), gap = Px(6);
      float recW = ImGui::GetContentRegionAvail().x - bh - gap;
      if (m.stats.recording) {
        if (ActionButton("rec", "Detener grabación", c.hotkeys.toggleRecording, Tone::Danger, true, false, recW))
          a.toggleRecording();
      } else {
        if (ActionButton("rec", "Grabar pantalla", c.hotkeys.toggleRecording, Tone::Normal, true, true, recW))
          a.toggleRecording();
      }
      ImGui::SameLine(0, gap);
      std::string tip = WithKey("Captura de pantalla", c.hotkeys.screenshot);
      if (SquareButton("shot", Icon::Shot, bh, tip.c_str())) a.screenshot();
    }

    Divider(12, 6);
    SectionLabel("CAPTURA");
    if (ToggleRow("replay", Icon::Replay, "Replay", c.replayEnabled)) a.toggleReplay();
    if (ToggleRow("mic", Icon::Mic, "Micrófono", c.captureMic)) a.toggleMic();
    if (ToggleRow("cam", Icon::Camera, "Cámara", c.cameraEnabled)) a.toggleCamera();

    Divider(6, 8);
    SectionLabel("CALIDAD");
    Gap(2);
    {
      // Cambiarla reinicia la captura (el aviso lo dice). Personalizada se elige en la app.
      static const std::vector<std::pair<int, std::string>> qualities = {
          {1, "Baja"}, {2, "Media"}, {3, "Alta"}, {4, "Máx."}, {5, "Fuente"}};
      static const char* ids[] = {"", "baja", "media", "alta", "maxima", "fuente"};
      constexpr int kSource = 5;
      int current = 0;
      for (int i = 1; i <= kSource; ++i)
        if (c.quality == ids[i]) current = i;
      if (int v = Segmented("quality", qualities, current); v > 0) {
        // Fuente gasta mucho más que el resto: antes de activarla, el aviso de
        // debajo lo explica. Cualquier otro segmento lo descarta.
        m.confirmSource = v == kSource && current != kSource;
        if (!m.confirmSource && v != current) a.setQuality(ids[v]);
      }
      if (m.confirmSource) {
        Gap(8);
        const char* text = "Fuente graba a la resolución nativa y a los Hz del monitor, con el bitrate más alto "
                           "que da tu gráfica. Gasta mucha más RAM, GPU y disco: si el juego va a tirones, vuelve "
                           "a Alta.";
        const float w = ImGui::GetContentRegionAvail().x;
        const float pad = Px(10), bar = Px(3), fs = Px(kSmall);
        const float wrap = w - bar - pad * 2;
        ImVec2 q = ImGui::GetCursorScreenPos();
        ImFont* font = Sized(ImGui::GetFont(), fs);
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, wrap, text);
        ImVec2 end(q.x + w, q.y + ts.y + pad * 2);
        // Una caja del diseño con una barra a la izquierda.
        CardFrame(dl, q, end, p.warning);
        dl->AddText(font, fs, ImVec2(q.x + bar + pad, q.y + pad), C(p.text), text, nullptr, wrap);
        ImGui::Dummy(ImVec2(w, end.y - q.y));
        Gap(6);
        float half = (w - Px(8)) * 0.5f;
        if (GhostButton("source-cancel", "Cancelar", half)) m.confirmSource = false;
        ImGui::SameLine(0, Px(8));
        if (SolidButton("source-ok", "Activar Fuente", half)) {
          m.confirmSource = false;
          a.setQuality(ids[kSource]);
        }
      }
    }

    Divider(12, 6);
    bool viewAll = false;
    SectionLabel("RECIENTES", "Ver todos", &viewAll);
    if (viewAll) a.openLibrary();
    if (m.clips.empty()) {
      ImVec2 q = ImGui::GetCursorScreenPos();
      float ss = Px(kSmall);
      Text(dl, ImGui::GetFont(), ss, ImVec2(q.x, q.y + Px(6)), C(p.textFaint), "Aún no hay clips ni grabaciones.");
      ImGui::Dummy(ImVec2(0, ss + Px(12)));
    }
    // Lateral: los que quepan (al menos tres, como el compacto).
    size_t shown = std::min<size_t>(m.clips.size(), 3);
    if (lateral) {
      float room = footerTop - ImGui::GetCursorScreenPos().y;
      shown = std::min(m.clips.size(), std::max(shown, size_t(std::max(0.0f, room / (RowHeight(44) + spacing)))));
    }
    int open = -1;
    for (int i = 0; i < int(shown); ++i) {
      const auto& clip = m.clips[size_t(i)];
      if (ClipRow(w, media, i, clip)) open = i;
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.showInFolder(clip.path);
    }
    // Se abre dentro del panel: verlo y compartirlo no saca del juego.
    if (open >= 0) {
      m.view = m.clips[size_t(open)];
      m.viewing = true;
      m.scrubbing = false;
    }
  }
  if (lateral && ImGui::GetCursorScreenPos().y < footerTop)
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, footerTop));

  Divider(6, 8);
  float half = (ImGui::GetContentRegionAvail().x - Px(8)) * 0.5f;
  if (m.viewing) {
    // El visor de la biblioteca y la carpeta con este clip elegido.
    if (GhostButton("app", "Abrir en Spectre", half, Tone::Normal, 34)) a.openClip(m.view.path);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("El reproductor de la biblioteca: recortar, editar y más");
    ImGui::SameLine(0, Px(8));
    if (GhostButton("folder", "Carpeta", half, Tone::Normal, 34)) a.showInFolder(m.view.path);
  } else {
    if (GhostButton("app", "Abrir Spectre", half, Tone::Normal, 34)) a.openApp();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Biblioteca, reproductor y todos los ajustes");
    ImGui::SameLine(0, Px(8));
    if (GhostButton("folder", "Carpeta", half, Tone::Normal, 34)) a.openPath(m.outputDir);
  }

  // Un hueco del panel también sirve de asa: el clic no cae en ningún control.
  if (!m.dragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
      !ImGui::IsAnyItemHovered())
    startDrag = true;
  if (startDrag && !m.dragging) {
    m.dragging = true;
    m.dragFromX = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].x;
    m.dragFromY = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].y;
  } else if (m.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    m.dragging = false;
  }
  if (m.dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

  m.panelWidth = ImGui::GetWindowWidth();
  m.panelHeight = ImGui::GetWindowHeight();
  m.panelTop = ImGui::GetWindowPos().y;
  {
    const ImVec2 wa = ImGui::GetWindowPos(), wb = wa + ImGui::GetWindowSize();
    PanelDecor(dl, wa, wb, onRight);
    // Terminal: una línea barre el panel de arriba abajo y lo va descubriendo.
    if (enter.reveal < 1.0f) {
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      const float y = wa.y + (wb.y - wa.y) * enter.reveal;
      fg->AddRectFilled(ImVec2(wa.x, y), wb, ImGui::GetColorU32(p.bg));
      fg->AddRectFilled(ImVec2(wa.x, y - Px(1)), ImVec2(wb.x, y + Px(1)), ImGui::GetColorU32(p.accent));
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(5);

  // Esc vuelve de un clip a la lista; en la lista cierra el panel.
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (m.viewing) CloseClipView(m, media);
    else a.close();
  }
}

} // namespace spectre
