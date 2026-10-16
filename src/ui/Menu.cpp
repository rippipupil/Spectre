#include "ui/Menu.h"

#include "common/Util.h"
#include "ui/OverlayWindow.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>

namespace spectre {

namespace {

const ImVec4 kAccent(0.66f, 0.47f, 1.0f, 1.0f);
const ImVec4 kGreen(0.35f, 0.85f, 0.55f, 1.0f);
const ImVec4 kRed(1.0f, 0.35f, 0.40f, 1.0f);
const ImVec4 kMuted(0.60f, 0.60f, 0.68f, 1.0f);

void Hint(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void SectionTitle(OverlayWindow& w, const char* text) {
  ImGui::Spacing();
  ImGui::PushFont(w.BoldFont());
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
}

bool BigButton(const char* label, const ImVec2& size, bool highlighted) {
  if (highlighted) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.58f, 0.36f, 0.98f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.66f, 0.47f, 1.0f, 1.0f));
  }
  bool pressed = ImGui::Button(label, size);
  if (highlighted) ImGui::PopStyleColor(2);
  return pressed;
}

// Combo de dispositivos con opción "Predeterminado" (id vacío).
bool DeviceCombo(const char* label, std::string& id, const std::vector<AudioDevice>& devices) {
  std::string current = "Predeterminado del sistema";
  for (const auto& d : devices)
    if (d.id == id) current = d.name;
  bool changed = false;
  if (ImGui::BeginCombo(label, current.c_str())) {
    if (ImGui::Selectable("Predeterminado del sistema", id.empty())) {
      id.clear();
      changed = true;
    }
    for (const auto& d : devices) {
      if (ImGui::Selectable(d.name.c_str(), d.id == id)) {
        id = d.id;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

template <typename T>
bool ChoiceCombo(const char* label, T& value, const std::vector<std::pair<T, const char*>>& options) {
  const char* current = options.empty() ? "" : options.front().second;
  for (const auto& [v, name] : options)
    if (v == value) current = name;
  bool changed = false;
  if (ImGui::BeginCombo(label, current)) {
    for (const auto& [v, name] : options) {
      if (ImGui::Selectable(name, v == value)) {
        value = v;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

std::string FormatDuration(double seconds) {
  int s = int(seconds);
  char buf[32];
  if (s >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(buf, sizeof(buf), "%02d:%02d", s / 60, s % 60);
  return buf;
}

void DrawHeader(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  ImGui::PushFont(w.BigFont());
  ImGui::TextColored(kAccent, "SPECTRE");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * w.Scale());

  if (m.stats.recording) {
    ImGui::TextColored(kRed, "  \xE2\x97\x8F Grabando %s", FormatDuration(m.stats.recordSeconds).c_str());
    ImGui::SameLine();
  }
  if (m.replayActive)
    ImGui::TextColored(kGreen, "  \xE2\x97\x8F Replay activo (%d s)", m.config.clipSeconds);
  else if (m.config.replayEnabled && m.config.onlyInGames)
    ImGui::TextColored(kMuted, "  \xE2\x97\x8B Replay en espera de un juego");
  else
    ImGui::TextColored(kMuted, "  \xE2\x97\x8B Replay desactivado");
  if (m.gameDetected) {
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "  \xC2\xB7  %s", m.gameName.c_str());
  }

  ImGui::Spacing();
  const float s = w.Scale();
  const ImVec2 size(220 * s, 48 * s);
  char label[128];
  snprintf(label, sizeof(label), "Guardar clip\n%s", m.config.hotkeys.saveClip.c_str());
  ImGui::BeginDisabled(!m.replayActive);
  if (BigButton(label, size, true)) a.saveClip();
  ImGui::EndDisabled();
  ImGui::SameLine();
  snprintf(label, sizeof(label), "%s\n%s", m.stats.recording ? "Detener grabación" : "Grabar pantalla",
           m.config.hotkeys.toggleRecording.c_str());
  if (BigButton(label, size, m.stats.recording)) a.toggleRecording();
  ImGui::SameLine();
  snprintf(label, sizeof(label), "%s\n%s", m.config.replayEnabled ? "Desactivar replay" : "Activar replay",
           m.config.hotkeys.toggleReplay.c_str());
  if (BigButton(label, size, false)) a.toggleReplay();
  ImGui::Spacing();
}

void TabClips(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  auto& c = m.config;
  SectionTitle(w, "Replay");
  m.dirty |= ImGui::Checkbox("Replay activado", &c.replayEnabled);
  ImGui::SameLine(0, 30 * w.Scale());
  m.dirty |= ImGui::Checkbox("Solo cuando haya un juego a pantalla completa", &c.onlyInGames);
  ImGui::SetNextItemWidth(260 * w.Scale());
  m.dirty |= ChoiceCombo<int>("Duración del clip", c.clipSeconds,
                              {{15, "15 segundos"}, {30, "30 segundos"}, {60, "1 minuto"},
                               {120, "2 minutos"}, {300, "5 minutos"}});
  double estimate = double(c.bitrateKbps) * 1000.0 / 8.0 * c.clipSeconds * 1.1;
  char hint[160];
  snprintf(hint, sizeof(hint), "El buffer guarda vídeo ya comprimido: unos %s de RAM con esta calidad.",
           FormatBytes(uint64_t(estimate)).c_str());
  Hint(hint);

  SectionTitle(w, "Clips recientes");
  ImGui::TextColored(kMuted, "%s", m.outputDir.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Abrir carpeta")) OpenPath(m.outputDir);
  ImGui::SameLine();
  if (ImGui::SmallButton("Actualizar")) a.refreshClips();

  if (ImGui::BeginChild("clips", ImVec2(0, 170 * w.Scale()), ImGuiChildFlags_Borders)) {
    if (m.clips.empty()) Hint("Todavía no hay clips. Pulsa el atajo de guardar clip durante una partida.");
    for (int i = 0; i < int(m.clips.size()); ++i) {
      const auto& clip = m.clips[size_t(i)];
      ImGui::PushID(i);
      bool selected = m.selectedClip == i;
      if (ImGui::Selectable(clip.name.c_str(), selected, ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(ImGui::GetContentRegionAvail().x - 260 * w.Scale(), 0))) {
        m.selectedClip = i;
        m.clipDuration = a.probeDuration ? a.probeDuration(clip.path) : 0;
        m.trimStart = 0;
        m.trimEnd = float(m.clipDuration);
      }
      ImGui::SameLine();
      ImGui::TextColored(kMuted, "%s  %s", clip.when.c_str(), FormatBytes(clip.size).c_str());
      ImGui::SameLine(ImGui::GetWindowWidth() - 120 * w.Scale());
      if (ImGui::SmallButton("Ver")) OpenPath(clip.path);
      ImGui::SameLine();
      if (ImGui::SmallButton("Carpeta")) ShowInFolder(clip.path);
      ImGui::PopID();
    }
  }
  ImGui::EndChild();

  if (m.selectedClip >= 0 && m.selectedClip < int(m.clips.size())) {
    const auto& clip = m.clips[size_t(m.selectedClip)];
    SectionTitle(w, "Recortar clip");
    if (m.clipDuration <= 0) {
      Hint("No se pudo leer la duración del clip.");
    } else {
      ImGui::SetNextItemWidth(420 * w.Scale());
      ImGui::DragFloatRange2("Inicio / fin (s)", &m.trimStart, &m.trimEnd, 0.1f, 0.0f, float(m.clipDuration),
                             "%.1f s", "%.1f s", ImGuiSliderFlags_AlwaysClamp);
      ImGui::SameLine();
      ImGui::BeginDisabled(m.trimEnd - m.trimStart < 0.5f);
      if (ImGui::Button("Guardar recorte")) a.trim(clip.path, m.trimStart, m.trimEnd);
      ImGui::EndDisabled();
      Hint("El recorte no recodifica: es instantáneo y sin pérdida. El inicio se ajusta al keyframe anterior (máx. 1 s).");
    }
  }
}

void TabVideo(OverlayWindow& w, MenuModel& m) {
  auto& c = m.config;
  const float width = 320 * w.Scale();
  SectionTitle(w, "Qué capturar");
  ImGui::SetNextItemWidth(width);
  m.dirty |= ChoiceCombo<std::string>("Fuente", c.captureMode,
                                      {{"monitor", "Pantalla completa"},
                                       {"game", "Ventana del juego (automático)"}});
  if (c.captureMode == "monitor") {
    std::vector<std::pair<int, const char*>> mons;
    for (size_t i = 0; i < m.monitors.size(); ++i) mons.emplace_back(int(i), m.monitors[i].name.c_str());
    ImGui::SetNextItemWidth(width);
    m.dirty |= ChoiceCombo<int>("Monitor", c.monitorIndex, mons);
  } else {
    Hint("Captura solo la ventana que esté a pantalla completa en primer plano. Si no hay ninguna, se captura el monitor.");
  }
  m.dirty |= ImGui::Checkbox("Capturar el cursor", &c.captureCursor);

  SectionTitle(w, "Calidad");
  ImGui::SetNextItemWidth(width);
  m.dirty |= ChoiceCombo<int>("Resolución", c.outputHeight,
                              {{0, "Nativa"}, {2160, "2160p (4K)"}, {1440, "1440p"}, {1080, "1080p"},
                               {720, "720p"}, {480, "480p"}});
  ImGui::SetNextItemWidth(width);
  m.dirty |= ChoiceCombo<int>("FPS", c.fps, {{30, "30 fps"}, {60, "60 fps"}, {120, "120 fps"}});
  ImGui::SetNextItemWidth(width);
  int mbps = c.bitrateKbps / 1000;
  if (ImGui::SliderInt("Bitrate (Mbps)", &mbps, 2, 80)) {
    c.bitrateKbps = mbps * 1000;
    m.dirty = true;
  }
  Hint("Recomendado: 1080p60 a 15-20 Mbps. Más bitrate = más calidad, más RAM en el buffer y archivos más grandes.");

  SectionTitle(w, "Codificación");
  ImGui::SetNextItemWidth(width);
  if (ChoiceCombo<std::string>("Códec", c.codec,
                               {{"h264", "H.264 (compatible con todo)"},
                                {"hevc", "HEVC / H.265 (menos espacio)"},
                                {"av1", "AV1 (RTX 40, RX 7000, Arc)"}})) {
    c.encoder = "auto";
    m.encoders = CandidateEncoders(c.codec);
    m.dirty = true;
  }
  std::vector<std::pair<std::string, const char*>> encs = {{"auto", "Automático (el mejor disponible)"}};
  for (const auto& e : m.encoders) encs.emplace_back(e.name, e.label.c_str());
  ImGui::SetNextItemWidth(width);
  m.dirty |= ChoiceCombo<std::string>("Encoder", c.encoder, encs);
  ImGui::SetNextItemWidth(width);
  m.dirty |= ChoiceCombo<std::string>("API de captura", c.captureApi,
                                      {{"auto", "Automática"},
                                       {"wgc", "Windows.Graphics.Capture"},
                                       {"dda", "Desktop Duplication (compatibilidad)"}});
  m.dirty |= ImGui::Checkbox("Prioridad de GPU alta para el encoder", &c.gpuPriority);
  Hint("Evita saltos en el vídeo cuando el juego usa la GPU al 100 %.");
}

void TabAudio(OverlayWindow& w, MenuModel& m) {
  auto& c = m.config;
  const float width = 360 * w.Scale();
  SectionTitle(w, "Sonido del PC");
  m.dirty |= ImGui::Checkbox("Grabar el sonido del PC", &c.captureSystemAudio);
  ImGui::BeginDisabled(!c.captureSystemAudio);
  ImGui::SetNextItemWidth(width);
  m.dirty |= DeviceCombo("Dispositivo de salida", c.outputDeviceId, m.outputs);
  m.dirty |= ImGui::Checkbox("Solo el audio del juego (sin Discord, música...)", &c.gameAudioOnly);
  Hint("Funciona en Windows 10 2004 o superior cuando hay un juego detectado a pantalla completa.");
  ImGui::SetNextItemWidth(width);
  m.dirty |= ImGui::SliderFloat("Volumen del PC", &c.systemVolume, 0.0f, 2.0f, "%.2fx");
  ImGui::EndDisabled();

  SectionTitle(w, "Micrófono");
  m.dirty |= ImGui::Checkbox("Grabar el micrófono", &c.captureMic);
  ImGui::BeginDisabled(!c.captureMic);
  ImGui::SetNextItemWidth(width);
  m.dirty |= DeviceCombo("Dispositivo de entrada", c.micDeviceId, m.inputs);
  ImGui::SetNextItemWidth(width);
  m.dirty |= ImGui::SliderFloat("Volumen del micrófono", &c.micVolume, 0.0f, 2.0f, "%.2fx");
  ImGui::EndDisabled();

  SectionTitle(w, "Pistas");
  m.dirty |= ImGui::Checkbox("Pistas separadas (mezcla + PC + micrófono)", &c.separateTracks);
  Hint("Los reproductores usan la pista de mezcla; los editores de vídeo pueden ajustar cada pista por separado.");
}

void TabCamera(OverlayWindow& w, MenuModel& m) {
  auto& c = m.config;
  const float width = 320 * w.Scale();
  SectionTitle(w, "Cámara");
  m.dirty |= ImGui::Checkbox("Incluir la webcam en el vídeo", &c.cameraEnabled);
  ImGui::BeginDisabled(!c.cameraEnabled);
  std::vector<std::pair<std::string, const char*>> cams = {{"", "Primera disponible"}};
  for (const auto& cam : m.cameras) cams.emplace_back(cam.id, cam.name.c_str());
  ImGui::SetNextItemWidth(width);
  m.dirty |= ChoiceCombo<std::string>("Dispositivo", c.cameraId, cams);
  ImGui::SetNextItemWidth(width);
  m.dirty |= ChoiceCombo<std::string>("Posición", c.cameraCorner,
                                      {{"top-left", "Arriba a la izquierda"},
                                       {"top-right", "Arriba a la derecha"},
                                       {"bottom-left", "Abajo a la izquierda"},
                                       {"bottom-right", "Abajo a la derecha"}});
  ImGui::SetNextItemWidth(width);
  m.dirty |= ImGui::SliderInt("Tamaño (% del ancho)", &c.cameraSizePercent, 10, 50);
  ImGui::EndDisabled();
  Hint("La cámara se compone en la GPU. Está apagada por defecto porque es lo que más CPU añade.");
}

void TabHotkeys(OverlayWindow& w, MenuModel& m) {
  auto& h = m.config.hotkeys;
  const float width = 240 * w.Scale();
  SectionTitle(w, "Atajos de teclado");
  Hint("Escribe la combinación, por ejemplo Alt+Z, Ctrl+Shift+F10 o Alt+Num5.");
  auto field = [&](const char* label, std::string& value) {
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(label, &value)) m.dirty = true;
    UINT mods = 0, vk = 0;
    if (!value.empty() && !ParseHotkey(value, &mods, &vk)) {
      ImGui::SameLine();
      ImGui::TextColored(kRed, "Formato no válido");
    }
  };
  field("Abrir este menú", h.overlay);
  field("Guardar clip", h.saveClip);
  field("Iniciar / detener grabación", h.toggleRecording);
  field("Activar / desactivar replay", h.toggleReplay);
  if (!m.hotkeyError.empty()) ImGui::TextColored(kRed, "%s", m.hotkeyError.c_str());
}

void TabGeneral(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  auto& c = m.config;
  SectionTitle(w, "Carpeta de clips y grabaciones");
  ImGui::SetNextItemWidth(520 * w.Scale());
  if (ImGui::InputTextWithHint("##dir", m.outputDir.c_str(), &c.outputDir)) m.dirty = true;
  Hint("Déjalo vacío para usar Vídeos\\Spectre.");

  SectionTitle(w, "Inicio");
  m.dirty |= ImGui::Checkbox("Iniciar Spectre con Windows", &c.autostart);
  m.dirty |= ImGui::Checkbox("Mostrar avisos (clip guardado, errores)", &c.showNotifications);

  SectionTitle(w, "Memoria");
  ImGui::SetNextItemWidth(260 * w.Scale());
  m.dirty |= ImGui::SliderInt("Límite del buffer (MB)", &c.maxBufferMB, 64, 4096);

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (ImGui::Button("Salir de Spectre")) a.quit();
}

void TabPerformance(OverlayWindow& w, MenuModel& m) {
  const auto& s = m.stats;
  SectionTitle(w, "Consumo de Spectre");
  ImGui::Text("CPU: %.1f %%", m.usage.cpuPercent);
  ImGui::Text("RAM: %s (privada %s)", FormatBytes(m.usage.workingSet).c_str(),
              FormatBytes(m.usage.privateBytes).c_str());
  if (!s.running) {
    Hint("La captura está parada: Spectre no está usando GPU ni encoder.");
    return;
  }
  ImGui::Text("Buffer: %s, %.0f s", FormatBytes(s.bufferBytes).c_str(), s.bufferSeconds);

  SectionTitle(w, "Captura");
  ImGui::Text("GPU: %s", s.gpu.c_str());
  ImGui::Text("Captura: %s", s.capture.c_str());
  ImGui::Text("Encoder: %s", s.encoder.c_str());
  ImGui::Text("Vídeo: %dx%d a %d fps", s.width, s.height, s.fps);
  ImGui::Text("Tiempo por frame: %.2f ms", s.encodeMs);
  ImGui::Text("Frames: %llu codificados, %llu saltados", (unsigned long long)s.framesEncoded,
              (unsigned long long)s.framesSkipped);
  ImGui::Text("Pistas de audio: %d", s.audioTracks);
}

} // namespace

void DrawMenu(OverlayWindow& w, MenuModel& m, const MenuActions& a) {
  ImGuiIO& io = ImGui::GetIO();
  // Fondo oscurecido sobre el juego, como el overlay de GeForce.
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 130));

  const float s = w.Scale();
  ImVec2 size(std::min(io.DisplaySize.x - 40, 820 * s), std::min(io.DisplaySize.y - 40, 640 * s));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("Spectre", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

  DrawHeader(w, m, a);

  float footer = ImGui::GetFrameHeightWithSpacing() + 14 * s;
  if (ImGui::BeginTabBar("tabs")) {
    auto tab = [&](const char* name, auto&& body) {
      if (ImGui::BeginTabItem(name)) {
        ImGui::BeginChild("content", ImVec2(0, -footer));
        body();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
    };
    tab("Clips", [&] { TabClips(w, m, a); });
    tab("Vídeo", [&] { TabVideo(w, m); });
    tab("Audio", [&] { TabAudio(w, m); });
    tab("Cámara", [&] { TabCamera(w, m); });
    tab("Atajos", [&] { TabHotkeys(w, m); });
    tab("General", [&] { TabGeneral(w, m, a); });
    tab("Rendimiento", [&] { TabPerformance(w, m); });
    ImGui::EndTabBar();
  }

  ImGui::Separator();
  ImGui::BeginDisabled(!m.dirty);
  if (BigButton("Aplicar cambios", ImVec2(180 * s, 0), m.dirty)) {
    a.apply(m.config);
    m.dirty = false;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cerrar", ImVec2(120 * s, 0))) a.close();
  ImGui::SameLine();
  ImGui::TextColored(kMuted, "Esc o %s para cerrar", m.config.hotkeys.overlay.c_str());
  ImGui::End();

  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive()) a.close();
}

void DrawToast(OverlayWindow& w, const std::string& text, bool error, float alpha) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::Begin("toast", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::PushFont(w.BoldFont());
  ImGui::TextColored(error ? kRed : kAccent, "SPECTRE");
  ImGui::PopFont();
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::End();
  ImGui::PopStyleVar();
}

} // namespace spectre
