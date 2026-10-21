#include "ui/SettingsPages.h"

#include "common/Util.h"
#include "encode/Quality.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef SPECTRE_VERSION
#define SPECTRE_VERSION "0.1.0"
#endif

namespace spectre {

using namespace ui;

const char* SettingsPageName(SettingsPage page) {
  switch (page) {
  case SettingsPage::Replay: return "Replay";
  case SettingsPage::Video: return "Vídeo";
  case SettingsPage::Audio: return "Audio";
  case SettingsPage::Camera: return "Cámara";
  case SettingsPage::Hotkeys: return "Atajos";
  case SettingsPage::Storage: return "Almacenamiento";
  case SettingsPage::Notifications: return "Avisos";
  case SettingsPage::General: return "General";
  case SettingsPage::Performance: return "Rendimiento";
  case SettingsPage::Projects: return "Más proyectos";
  default: return "";
  }
}

namespace {

bool DeviceCombo(const char* id, std::string& value, const std::vector<AudioDevice>& devices) {
  std::string current = "Predeterminado del sistema";
  for (const auto& d : devices)
    if (d.id == value) current = d.name;
  bool changed = false;
  if (ImGui::BeginCombo(id, current.c_str())) {
    if (ImGui::Selectable("Predeterminado del sistema", value.empty())) {
      value.clear();
      changed = true;
    }
    for (const auto& d : devices) {
      if (ImGui::Selectable(d.name.c_str(), d.id == value)) {
        value = d.id;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Resolución de salida de la configuración en edición con esa calidad (para
// las estimaciones). Fuente graba a la nativa: la de la captura si ya está en
// Fuente o, si no, la del monitor elegido (la que suele tener un juego a
// pantalla completa).
void EstimatedSize(const SettingsModel& m, const std::string& quality, int* width, int* height) {
  const auto& c = m.config;
  const bool running = m.stats.running && m.stats.height > 0;
  if (IsSourceQuality(quality)) {
    if (running && IsSourceQuality(m.base.quality)) {
      *width = m.stats.width;
      *height = m.stats.height;
      return;
    }
    if (!m.monitors.empty()) {
      const RECT& r = m.monitors[size_t(std::clamp<int>(c.monitorIndex, 0, int(m.monitors.size()) - 1))].rect;
      *width = std::max<int>(r.right - r.left, 2);
      *height = std::max<int>(r.bottom - r.top, 2);
      return;
    }
  }
  *height = c.outputHeight > 0 ? c.outputHeight : (running ? m.stats.height : 1080);
  *width = running ? *height * m.stats.width / m.stats.height : *height * 16 / 9;
}

// Hz del monitor elegido (0 si no se sabe).
int SelectedRefreshHz(const SettingsModel& m) {
  if (m.monitors.empty()) return 0;
  return m.monitors[size_t(std::clamp<int>(m.config.monitorIndex, 0, int(m.monitors.size()) - 1))].refreshHz;
}

// Fps de la configuración en edición con esa calidad. Fuente usa los Hz del
// monitor (los de la captura si ya está en Fuente), como Pipeline::Start.
int EstimatedFps(const SettingsModel& m, const std::string& quality) {
  const auto& c = m.config;
  if (!IsSourceQuality(quality)) return c.fps;
  if (m.stats.running && m.stats.fps > 0 && IsSourceQuality(m.base.quality)) return m.stats.fps;
  if (m.monitors.empty()) return c.fps;
  int width = 0, height = 0;
  EstimatedSize(m, quality, &width, &height);
  return SourceFps(SelectedRefreshHz(m), width, height, c.fps);
}

// Bitrate máximo que usará la configuración en edición (para las estimaciones).
int EstimatedKbps(const SettingsModel& m, const std::string& quality) {
  const auto& c = m.config;
  int width = 0, height = 0;
  EstimatedSize(m, quality, &width, &height);
  return PlanQuality(quality, c.bitrateKbps, width, height, EstimatedFps(m, quality), c.codec).maxKbps;
}
int EstimatedKbps(const SettingsModel& m) { return EstimatedKbps(m, m.config.quality); }

// Config::maxBufferMB == 1024 es «automático»: el pipeline calcula el tope con
// la calidad, la duración y la RAM (Pipeline::BufferLimit).
constexpr int kAutoBufferMB = 1024;

// Lo que puede llegar a ocupar el replay: todo el rato al bitrate máximo del
// preset, más los segundos de margen del anillo, como Pipeline::BufferLimit.
double ReplayPeakBytes(const SettingsModel& m, int kbps) {
  return double(kbps) * 125.0 * (double(m.config.clipSeconds) + ReplayBuffer::kExtraSeconds) * 1.1;
}
double ReplayPeakBytes(const SettingsModel& m) { return ReplayPeakBytes(m, EstimatedKbps(m)); }

// El tope automático: lo que pide la duración, sin pasar de la cuarta parte de
// la RAM. Con la captura en marcha y los mismos ajustes, el que usa de verdad.
double AutoBufferBytes(const SettingsModel& m) {
  const Config &c = m.config, &b = m.base;
  bool same = m.stats.running && m.stats.bufferLimitBytes > 0 && b.maxBufferMB == kAutoBufferMB && b.replayEnabled &&
              c.replayEnabled && c.clipSeconds == b.clipSeconds && c.quality == b.quality &&
              c.bitrateKbps == b.bitrateKbps && c.outputHeight == b.outputHeight && c.fps == b.fps &&
              c.codec == b.codec;
  if (same) return double(m.stats.bufferLimitBytes);
  static const double quarter = [] {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    return GlobalMemoryStatusEx(&mem) ? double(mem.ullTotalPhys) / 4.0 : double(1u << 30);
  }();
  return std::max(std::min(ReplayPeakBytes(m), quarter), double(64u << 20));
}

// Medidor de nivel: barra en la columna de controles, con el dispositivo y
// formato reales debajo. Si la barra no se mueve con el juego sonando, se
// está escuchando el dispositivo equivocado.
void LevelMeter(const char* label, float level, bool active, const std::string& description) {
  const Palette& p = Colors();
  FieldLabel(label);
  float w = ControlWidth();
  float h = Px(6);
  ImVec2 q = ImGui::GetCursorScreenPos();
  float rowH = ImGui::GetFrameHeight();
  ImVec2 a(q.x, q.y + (rowH - h) * 0.5f), b(q.x + w, a.y + h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(p.surface));
  if (active) {
    // Escala en dB: -60 dB a la izquierda, 0 dB a la derecha.
    float db = level > 1e-5f ? 20.0f * std::log10(level) : -100.0f;
    float t = std::clamp(1.0f + db / 60.0f, 0.0f, 1.0f);
    ImU32 color = db > -3.0f ? C(p.danger) : db > -12.0f ? C(p.warning) : C(p.accent);
    if (t > 0) dl->AddRectFilled(a, ImVec2(a.x + w * t, b.y), color);
  }
  ImGui::Dummy(ImVec2(w, rowH));
  if (!active) {
    Help("La captura está parada o esta fuente no se pudo abrir: no hay nivel que mostrar. Mira spectre.log.");
  } else {
    std::string text = "Capturando: " + description;
    Help(text.c_str());
  }
}

void PageReplay(SettingsModel& m) {
  auto& c = m.config;
  Heading("REPLAY");
  m.dirty |= SwitchField("Replay activado", &c.replayEnabled,
                         "Guarda continuamente los últimos segundos para poder crear un clip en cualquier momento.");
  m.dirty |= SwitchField("Solo con un juego abierto", &c.onlyInGames,
                         "El replay se pausa si no hay ninguna app a pantalla completa.");
  FieldLabel("Duración del clip");
  m.dirty |= ChoiceCombo<int>("##len", c.clipSeconds,
                              {{15, "15 segundos"}, {30, "30 segundos"}, {60, "1 minuto"},
                               {120, "2 minutos"}, {300, "5 minutos"}});
  FieldLabel("Límite de memoria", "Automático reserva lo que pide la duración del clip con esta calidad, sin pasar "
                                  "de la cuarta parte de la RAM.");
  const double peak = ReplayPeakBytes(m);
  const double autoBytes = AutoBufferBytes(m);
  // Los topes fijos se saltan 1024 MB, que es «automático».
  std::string autoName = "Automático (hasta " + FormatBytes(uint64_t(autoBytes)) + ")";
  std::string ownName = std::to_string(c.maxBufferMB) + " MB";
  std::vector<std::pair<int, const char*>> limits = {{kAutoBufferMB, autoName.c_str()},
                                                     {256, "256 MB"},
                                                     {512, "512 MB"},
                                                     {768, "768 MB"},
                                                     {1536, "1.5 GB"},
                                                     {2048, "2 GB"},
                                                     {3072, "3 GB"},
                                                     {4096, "4 GB"}};
  if (std::none_of(limits.begin(), limits.end(), [&](const auto& o) { return o.first == c.maxBufferMB; }))
    limits.push_back({c.maxBufferMB, ownName.c_str()}); // uno del deslizador de antes
  m.dirty |= ChoiceCombo<int>("##mem", c.maxBufferMB, limits);
  const bool automatic = c.maxBufferMB == kAutoBufferMB;
  const double limit = automatic ? autoBytes : double(c.maxBufferMB) * 1024.0 * 1024.0;
  char hint[400];
  if (peak > limit * 1.02) {
    if (automatic)
      snprintf(hint, sizeof(hint),
               "Con esta calidad y duración el replay puede necesitar hasta %s de RAM, más que la cuarta parte de "
               "la RAM del PC (%s), que es el tope automático. En escenas de mucho movimiento el clip saldría más "
               "corto: baja la calidad o la duración.",
               FormatBytes(uint64_t(peak)).c_str(), FormatBytes(uint64_t(limit)).c_str());
    else
      snprintf(hint, sizeof(hint),
               "Con esta calidad y duración el replay puede necesitar hasta %s de RAM, más que el límite de %s. En "
               "escenas de mucho movimiento el clip saldría más corto: sube el límite, ponlo en automático o baja "
               "la calidad.",
               FormatBytes(uint64_t(peak)).c_str(), FormatBytes(uint64_t(limit)).c_str());
    Warning(hint);
  } else {
    snprintf(hint, sizeof(hint),
             "El replay guarda vídeo ya comprimido en la RAM. Con esta calidad ocupa como mucho unos %s.",
             FormatBytes(uint64_t(peak)).c_str());
    Help(hint);
  }

  Heading("MARCAS");
  std::string marker = c.hotkeys.marker.empty() ? std::string("el atajo de Marcar (Ajustes > Atajos)")
                                                : c.hotkeys.marker;
  std::string text = "Pulsa " + marker +
                     " durante la partida para marcar un momento. La marca sale en la línea de tiempo del clip o "
                     "de la grabación, en la biblioteca, para ir directo a esa jugada.";
  Help(text.c_str());

  Heading("ARCHIVOS");
  m.dirty |= SwitchField("Una carpeta por juego", &c.folderPerGame,
                         "Guarda los clips, grabaciones y capturas de cada juego en su propia subcarpeta. La "
                         "biblioteca los muestra todos igual.");
}

// Aviso al elegir Fuente: qué consume con los ajustes de ahora. Cancelar
// devuelve la calidad que había; la elección se aplica con Aplicar, como todo.
void SourceDialog(SettingsModel& m) {
  auto& c = m.config;
  const Palette& p = Colors();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(24), Px(20)));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, V(p.bg));
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, V(IM_COL32(0, 0, 0, 140)));
  ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(Px(500), 0));
  if (ImGui::BeginPopupModal("Calidad Fuente", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings)) {
    Text(ImGui::GetWindowDrawList(), Bold(), Px(kBody) + Px(2), ImGui::GetCursorScreenPos(), C(p.text),
         "Fuente consume mucho");
    Gap(30);
    const std::string before = m.qualityBeforeSource.empty() ? std::string("alta") : m.qualityBeforeSource;
    int width = 0, height = 0;
    EstimatedSize(m, "fuente", &width, &height);
    const int kbps = EstimatedKbps(m, "fuente");
    const int beforeKbps = EstimatedKbps(m, before);
    const double peak = ReplayPeakBytes(m, kbps);
    const double limit =
        c.maxBufferMB == kAutoBufferMB ? AutoBufferBytes(m) : double(c.maxBufferMB) * 1024.0 * 1024.0;
    char text[700];
    snprintf(text, sizeof(text),
             "Graba siempre a la resolución nativa y a los Hz del monitor (%dx%d a %d fps), con hasta %d Mbps y "
             "las opciones de más calidad que tu gráfica hace en tiempo real. Es el modo que más gasta de todos:",
             width, height, EstimatedFps(m, "fuente"), kbps / 1000);
    ImGui::PushTextWrapPos(0);
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
    Gap(8);
    snprintf(text, sizeof(text), "RAM: el replay de %s puede ocupar hasta %s (ahora, %s).",
             FormatClipLength(c.clipSeconds).c_str(), FormatBytes(uint64_t(peak)).c_str(),
             FormatBytes(uint64_t(ReplayPeakBytes(m, beforeKbps))).c_str());
    Help(text);
    Help("GPU: el encoder trabaja más, sobre todo en 1440p o 4K. En un PC justo puede bajar los fps del juego.");
    snprintf(text, sizeof(text), "Disco: los clips y las grabaciones ocupan hasta %.1f veces más que ahora.",
             double(kbps) / double(std::max(beforeKbps, 1)));
    Help(text);
    if (peak > limit * 1.02) {
      Gap(4);
      snprintf(text, sizeof(text),
               "Con clips de %s no cabe en el límite de memoria (%s): en escenas de mucho movimiento el clip "
               "saldría más corto. Baja la duración o sube el límite en Ajustes > Replay.",
               FormatClipLength(c.clipSeconds).c_str(), FormatBytes(uint64_t(limit)).c_str());
      Warning(text);
    }
    Gap(4);
    Help("Si notas tirones en el juego, vuelve a Máxima o Alta.");
    Gap(14);
    float bw = Px(150);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw * 2 - Px(8));
    bool cancel = GhostButton("cancel", "Cancelar", bw) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::SameLine(0, Px(8));
    // Intro no cuenta en el frame en que se abre: el combo pudo elegirse con él.
    bool ok = SolidButton("ok", "Usar Fuente", bw) ||
              (!ImGui::IsWindowAppearing() && ImGui::IsKeyPressed(ImGuiKey_Enter, false));
    if (cancel) {
      c.quality = before;
      m.qualityBeforeSource.clear();
      ImGui::CloseCurrentPopup();
    } else if (ok) {
      m.qualityBeforeSource.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}

void PageVideo(SettingsModel& m) {
  auto& c = m.config;
  Heading("QUÉ CAPTURAR");
  FieldLabel("Fuente");
  m.dirty |= ChoiceCombo<std::string>("##src", c.captureMode,
                                      {{"monitor", "Pantalla completa"}, {"game", "Ventana del juego"}});
  if (c.captureMode == "monitor") {
    std::vector<std::pair<int, const char*>> mons;
    for (size_t i = 0; i < m.monitors.size(); ++i) mons.emplace_back(int(i), m.monitors[i].name.c_str());
    FieldLabel("Monitor");
    m.dirty |= ChoiceCombo<int>("##mon", c.monitorIndex, mons);
  } else {
    Help("Captura la ventana que esté a pantalla completa en primer plano. Si no hay ninguna, se captura el monitor.");
  }
  m.dirty |= SwitchField("Mostrar el cursor", &c.captureCursor);

  Heading("CALIDAD");
  FieldLabel("Resolución");
  if (IsSourceQuality(c.quality)) {
    // Fuente fija la nativa; la resolución elegida vuelve al cambiar de calidad.
    int native = 0;
    ImGui::BeginDisabled();
    ChoiceCombo<int>("##res", native, {{0, "Nativa (la fija Fuente)"}});
    ImGui::EndDisabled();
  } else {
    m.dirty |= ChoiceCombo<int>("##res", c.outputHeight,
                                {{0, "Nativa"}, {2160, "2160p (4K)"}, {1440, "1440p"}, {1080, "1080p"},
                                 {720, "720p"}, {480, "480p"}});
  }
  FieldLabel("Fotogramas por segundo");
  if (IsSourceQuality(c.quality)) {
    // Fuente usa los Hz del monitor; los fps elegidos vuelven al cambiar de calidad.
    int fps = 0;
    const int hz = NormalizedRefreshHz(SelectedRefreshHz(m));
    std::string name = std::to_string(EstimatedFps(m, c.quality)) + " fps" +
                       (hz > 0 ? " (monitor a " + std::to_string(hz) + " Hz)" : std::string(" (los fija Fuente)"));
    ImGui::BeginDisabled();
    ChoiceCombo<int>("##fps", fps, {{0, name.c_str()}});
    ImGui::EndDisabled();
  } else {
    m.dirty |= ChoiceCombo<int>("##fps", c.fps, {{30, "30 fps"}, {60, "60 fps"}, {120, "120 fps"}});
  }
  FieldLabel("Calidad");
  const std::string previous = c.quality;
  if (ChoiceCombo<std::string>("##quality", c.quality, QualityOptions())) {
    m.dirty = true;
    // Fuente gasta mucho más que el resto: al elegirla sale el aviso.
    if (IsSourceQuality(c.quality) && !IsSourceQuality(previous)) {
      m.qualityBeforeSource = previous;
      ImGui::OpenPopup("Calidad Fuente");
    }
  }
  SourceDialog(m);
  if (IsSourceQuality(c.quality)) {
    char hint[300];
    snprintf(hint, sizeof(hint),
             "Resolución nativa, los Hz del monitor (como mucho 144 fps; si no caben, 120 o 60, o una fracción "
             "exacta de los Hz para que vaya fluido) y hasta %d Mbps, con lo máximo que tu gráfica codifica en tiempo real. Es el modo que "
             "más RAM, GPU y disco gasta.",
             EstimatedKbps(m) / 1000);
    Warning(hint);
  } else if (c.quality == "personalizada") {
    FieldLabel("Bitrate");
    int mbps = c.bitrateKbps / 1000;
    if (ImGui::SliderInt("##br", &mbps, 2, 100, "%d Mbps")) {
      c.bitrateKbps = mbps * 1000;
      m.dirty = true;
    }
    Help("Bitrate fijo. Para 1080p a 60 fps, 30 Mbps se ve muy bien; más bitrate usa más RAM y disco.");
  } else {
    char hint[240];
    snprintf(hint, sizeof(hint),
             "El encoder apunta a una calidad constante y usa hasta %d Mbps en las escenas de más movimiento "
             "(para la resolución y los fps elegidos). Alta es el equilibrio recomendado; Máxima es lo que usa "
             "ShadowPlay en Alta.",
             EstimatedKbps(m) / 1000);
    Help(hint);
  }

  Heading("CODIFICACIÓN");
  FieldLabel("Códec");
  if (ChoiceCombo<std::string>("##codec", c.codec,
                               {{"h264", "H.264, se reproduce en todas partes"},
                                {"hevc", "HEVC, ocupa menos"},
                                {"av1", "AV1, para RTX 40, RX 7000 o Arc"}})) {
    c.encoder = "auto";
    m.encoders = CandidateEncoders(c.codec);
    m.dirty = true;
  }
  std::vector<std::pair<std::string, const char*>> encs = {{"auto", "Automático"}};
  for (const auto& e : m.encoders) encs.emplace_back(e.name, e.label.c_str());
  FieldLabel("Encoder");
  m.dirty |= ChoiceCombo<std::string>("##enc", c.encoder, encs);
  FieldLabel("Método de captura");
  m.dirty |= ChoiceCombo<std::string>("##api", c.captureApi,
                                      {{"auto", "Automático"},
                                       {"wgc", "Windows Graphics Capture"},
                                       {"dda", "Desktop Duplication"}});
  m.dirty |= SwitchField("Prioridad alta en la GPU", &c.gpuPriority,
                         "Evita saltos en el vídeo cuando el juego usa la GPU al 100 %.");
}

void PageAudio(SettingsModel& m) {
  auto& c = m.config;
  Heading("SONIDO DEL PC");
  m.dirty |= SwitchField("Grabar el sonido del PC", &c.captureSystemAudio);
  ImGui::BeginDisabled(!c.captureSystemAudio);
  FieldLabel("Dispositivo de salida");
  m.dirty |= DeviceCombo("##out", c.outputDeviceId, m.outputs);
  LevelMeter("Nivel", m.systemLevel, m.stats.running && !m.stats.systemAudio.empty(), m.stats.systemAudio);
  m.dirty |= SwitchField("Solo el audio del juego", &c.gameAudioOnly,
                         "Deja fuera Discord, la música y el resto de apps. Necesita Windows 10 2004 o posterior y un "
                         "juego a pantalla completa.");
  FieldLabel("Volumen");
  m.dirty |= ImGui::SliderFloat("##sysvol", &c.systemVolume, 0.0f, 2.0f, "x%.2f");
  ImGui::EndDisabled();

  Heading("MICRÓFONO");
  m.dirty |= SwitchField("Grabar el micrófono", &c.captureMic);
  ImGui::BeginDisabled(!c.captureMic);
  FieldLabel("Dispositivo de entrada");
  m.dirty |= DeviceCombo("##in", c.micDeviceId, m.inputs);
  LevelMeter("Nivel", m.micLevel, m.stats.running && !m.stats.micAudio.empty(), m.stats.micAudio);
  FieldLabel("Volumen");
  m.dirty |= ImGui::SliderFloat("##micvol", &c.micVolume, 0.0f, 2.0f, "x%.2f");
  ImGui::EndDisabled();

  Heading("PISTAS");
  m.dirty |= SwitchField("Pistas separadas", &c.separateTracks,
                         "Además de la mezcla, guarda el PC y el micrófono en pistas aparte para editarlas por "
                         "separado.");
}

void PageCamera(SettingsModel& m) {
  auto& c = m.config;
  Heading("CÁMARA");
  m.dirty |= SwitchField("Incluir la webcam", &c.cameraEnabled);
  ImGui::BeginDisabled(!c.cameraEnabled);
  std::vector<std::pair<std::string, const char*>> cams = {{"", "La primera que encuentre"}};
  for (const auto& cam : m.cameras) cams.emplace_back(cam.id, cam.name.c_str());
  FieldLabel("Dispositivo");
  m.dirty |= ChoiceCombo<std::string>("##cam", c.cameraId, cams);
  FieldLabel("Posición");
  m.dirty |= ChoiceCombo<std::string>("##corner", c.cameraCorner,
                                      {{"top-left", "Arriba a la izquierda"},
                                       {"top-right", "Arriba a la derecha"},
                                       {"bottom-left", "Abajo a la izquierda"},
                                       {"bottom-right", "Abajo a la derecha"}});
  FieldLabel("Tamaño");
  m.dirty |= ImGui::SliderInt("##camsize", &c.cameraSizePercent, 10, 50, "%d %% del ancho");
  ImGui::EndDisabled();
  Help("La cámara se compone en la GPU. Viene apagada porque es lo que más CPU añade.");
}

// ------------------------------------------------------------------ atajos

struct HotkeyField {
  const char* label;
  std::string HotkeyConfig::*value;
};
const HotkeyField kHotkeyFields[] = {
    {"Abrir el panel", &HotkeyConfig::overlay},
    {"Guardar clip", &HotkeyConfig::saveClip},
    {"Grabar o detener", &HotkeyConfig::toggleRecording},
    {"Activar o pausar el replay", &HotkeyConfig::toggleReplay},
    {"Captura de pantalla", &HotkeyConfig::screenshot},
    {"Marcar un momento", &HotkeyConfig::marker},
};

struct KeyText {
  ImGuiKey key;
  const char* text; // como lo entiende ParseHotkey
};

// Tecla pulsada en este frame (sin contar modificadores), con el nombre que
// usa config.json. nullptr si no hay ninguna.
const char* PressedKey() {
  static const KeyText named[] = {
      {ImGuiKey_Space, "Space"},       {ImGuiKey_Tab, "Tab"},           {ImGuiKey_Insert, "Insert"},
      {ImGuiKey_Delete, "Delete"},     {ImGuiKey_Home, "Home"},         {ImGuiKey_End, "End"},
      {ImGuiKey_PageUp, "PageUp"},     {ImGuiKey_PageDown, "PageDown"}, {ImGuiKey_Pause, "Pause"},
      {ImGuiKey_ScrollLock, "ScrollLock"}, {ImGuiKey_PrintScreen, "PrintScreen"},
      {ImGuiKey_UpArrow, "Up"},        {ImGuiKey_DownArrow, "Down"},    {ImGuiKey_LeftArrow, "Left"},
      {ImGuiKey_RightArrow, "Right"},  {ImGuiKey_GraveAccent, "`"},     {ImGuiKey_Minus, "-"},
      {ImGuiKey_Equal, "="},           {ImGuiKey_Comma, ","},           {ImGuiKey_Period, "."},
  };
  static char buf[8];
  for (int k = ImGuiKey_F1; k <= ImGuiKey_F24; ++k)
    if (ImGui::IsKeyPressed(ImGuiKey(k), false)) {
      snprintf(buf, sizeof(buf), "F%d", k - ImGuiKey_F1 + 1);
      return buf;
    }
  for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k)
    if (ImGui::IsKeyPressed(ImGuiKey(k), false)) {
      snprintf(buf, sizeof(buf), "%c", 'A' + (k - ImGuiKey_A));
      return buf;
    }
  for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k)
    if (ImGui::IsKeyPressed(ImGuiKey(k), false)) {
      snprintf(buf, sizeof(buf), "%c", '0' + (k - ImGuiKey_0));
      return buf;
    }
  for (int k = ImGuiKey_Keypad0; k <= ImGuiKey_Keypad9; ++k)
    if (ImGui::IsKeyPressed(ImGuiKey(k), false)) {
      snprintf(buf, sizeof(buf), "Num%d", k - ImGuiKey_Keypad0);
      return buf;
    }
  for (const auto& n : named)
    if (ImGui::IsKeyPressed(n.key, false)) return n.text;
  return nullptr;
}

void EndCapture(SettingsModel& m, const SettingsActions& a) {
  if (m.capturing < 0) return;
  m.capturing = -1;
  if (a.suspendHotkeys) a.suspendHotkeys(false);
}

// Escucha la combinación mientras se graba un atajo.
void PollCapture(SettingsModel& m, const SettingsActions& a) {
  if (m.capturing < 0) return;
  // Otra ventana cogió el foco: las teclas ya no llegan aquí.
  if (ImGui::GetIO().AppFocusLost) {
    EndCapture(m, a);
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    m.captureHint.clear();
    EndCapture(m, a);
    return;
  }
  const char* key = PressedKey();
  if (!key) return;
  std::string& value = m.config.hotkeys.*kHotkeyFields[m.capturing].value;
  if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !io.KeySuper &&
      (strcmp(key, "Delete") == 0 || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
    value.clear();
    m.dirty = true;
    m.captureHint.clear();
    EndCapture(m, a);
    return;
  }
  std::string text;
  if (io.KeyCtrl) text += "Ctrl+";
  if (io.KeyAlt) text += "Alt+";
  if (io.KeyShift) text += "Shift+";
  if (io.KeySuper) text += "Win+";
  text += key;
  UINT mods = 0, vk = 0;
  if (!ParseHotkey(text, &mods, &vk)) {
    m.captureHint = text + " no vale: añade Ctrl o Alt (solas solo valen F13 a F24, Pausa y Bloq Despl).";
    return;
  }
  text = NormalizeHotkey(text);
  for (int i = 0; i < int(std::size(kHotkeyFields)); ++i) {
    if (i == m.capturing) continue;
    if (NormalizeHotkey(m.config.hotkeys.*kHotkeyFields[i].value) == text) {
      m.captureHint = text + " ya lo usa «" + std::string(kHotkeyFields[i].label) + "».";
      return;
    }
  }
  value = text;
  m.dirty = true;
  m.captureHint.clear();
  EndCapture(m, a);
}

void PageHotkeys(SettingsModel& m, const SettingsActions& a) {
  const Palette& p = Colors();
  Heading("ATAJOS DE TECLADO");
  Help("Haz clic en un atajo y pulsa la nueva combinación, por ejemplo Alt+Z o Ctrl+Shift+F10. Esc cancela y "
       "Supr lo deja sin atajo. Funcionan en cualquier juego, también con el panel abierto.");
  Gap(6);
  PollCapture(m, a);
  for (int i = 0; i < int(std::size(kHotkeyFields)); ++i) {
    ImGui::PushID(i);
    std::string& value = m.config.hotkeys.*kHotkeyFields[i].value;
    FieldLabel(kHotkeyFields[i].label);
    const bool capturing = m.capturing == i;
    const float w = Px(200), h = ImGui::GetFrameHeight();
    bool clicked = ImGui::InvisibleButton("key", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImVec2 q0 = ImGui::GetItemRectMin(), q1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(q0, q1, C(capturing ? p.surfaceActive : hov ? p.surfaceHover : p.surface));
    if (capturing) dl->AddRect(q0, q1, C(p.accent), 0, 0, Px(1));
    const char* shown = capturing ? "Pulsa la combinación..." : value.empty() ? "Sin atajo" : value.c_str();
    float fs = Px(kSmall);
    ImU32 color = capturing ? p.text : value.empty() ? p.textFaint : p.text;
    Text(dl, value.empty() || capturing ? ImGui::GetFont() : Bold(), fs,
         ImVec2(q0.x + Px(10), (q0.y + q1.y - fs) * 0.5f), C(color), shown);
    UINT mods = 0, vk = 0;
    bool invalid = !value.empty() && !ParseHotkey(value, &mods, &vk);
    if (clicked) {
      if (capturing) {
        EndCapture(m, a);
      } else {
        EndCapture(m, a);
        m.capturing = i;
        m.captureHint.clear();
        if (a.suspendHotkeys) a.suspendHotkeys(true);
      }
    }
    if (!value.empty() && !capturing) {
      ImGui::SameLine(0, Px(6));
      if (IconButton("clear", Icon::Close, h, "Quitar el atajo")) {
        EndCapture(m, a);
        value.clear();
        m.dirty = true;
      }
    }
    if (invalid) {
      ImGui::SameLine(0, Px(8));
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(V(p.danger), "No es una combinación válida");
    }
    ImGui::PopID();
  }
  if (!m.captureHint.empty()) {
    Gap(4);
    Warning(m.captureHint.c_str());
  }
  if (!m.hotkeyError.empty()) {
    Gap(4);
    ErrorText(m.hotkeyError.c_str());
  }
}

// ------------------------------------------------------------ almacenamiento

void PageStorage(SettingsModel& m, const SettingsActions& a) {
  const Palette& p = Colors();
  auto& c = m.config;
  Heading("CARPETA DE DESTINO");
  FieldLabel("Carpeta");
  if (ImGui::InputTextWithHint("##dir", m.outputDir.c_str(), &c.outputDir)) m.dirty = true;
  ImGui::Dummy(ImVec2(Px(kLabelColumn) - ImGui::GetStyle().ItemSpacing.x, 0));
  ImGui::SameLine();
  if (GhostButton("choose", "Elegir carpeta...", Px(140), Tone::Normal, 28) && a.chooseFolder) a.chooseFolder();
  ImGui::SameLine(0, Px(6));
  if (GhostButton("open", "Abrir", Px(80), Tone::Normal, 28)) a.openPath(m.outputDir);
  Help("Déjalo vacío para usar Vídeos\\Spectre. Los clips que ya tienes se quedan donde están.");

  Heading("ESPACIO");
  const StorageInfo& s = m.storage;
  if (!s.valid) {
    Help("No se pudo leer el disco de la carpeta.");
  } else {
    char buf[200];
    float w = std::min(ImGui::GetContentRegionAvail().x, Px(560));
    double used = double(s.usedBytes), total = double(std::max<uint64_t>(s.totalBytes, 1));
    double other = std::max(0.0, total - double(s.freeBytes) - used);
    ImVec2 q = ImGui::GetCursorScreenPos();
    float h = Px(8);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + h), C(p.surface));
    float xOther = q.x + w * float(other / total);
    dl->AddRectFilled(q, ImVec2(xOther, q.y + h), C(p.lineStrong));
    dl->AddRectFilled(ImVec2(xOther, q.y), ImVec2(xOther + std::max(Px(2), w * float(used / total)), q.y + h),
                      C(p.accent));
    ImGui::Dummy(ImVec2(w, h));
    Gap(6);
    snprintf(buf, sizeof(buf), "%s de Spectre (%zu vídeos, %zu capturas)", FormatBytes(s.usedBytes).c_str(),
             s.videos, s.screenshots);
    ValueRow("Biblioteca", buf);
    if (s.others > 0) {
      snprintf(buf, sizeof(buf), "%s en %zu %s renombrados o de otros programas", FormatBytes(s.otherBytes).c_str(),
               s.others, s.others == 1 ? "archivo" : "archivos");
      ValueRow("Otros", buf);
    }
    ValueRow("Libre en el disco", FormatBytes(s.freeBytes) + " de " + FormatBytes(s.totalBytes));
    if (s.freeBytes < (uint64_t(2) << 30)) {
      Gap(4);
      Warning("Queda poco espacio: con el disco lleno los clips no se pueden guardar.");
    }
    Gap(4);
    if (GhostButton("refresh", "Actualizar", Px(110), Tone::Normal, 26) && a.refreshStorage) a.refreshStorage();
  }

  Heading("LÍMITE DE ESPACIO");
  FieldLabel("Espacio máximo", "Tope para lo que guarda Spectre en la carpeta de destino.");
  m.dirty |= ChoiceCombo<int>("##limit", c.storageLimitGB,
                              {{0, "Sin límite"}, {10, "10 GB"}, {25, "25 GB"}, {50, "50 GB"}, {100, "100 GB"},
                               {250, "250 GB"}, {500, "500 GB"}});
  if (c.storageLimitGB > 0) {
    Warning("Al pasar del límite, Spectre borra para siempre los clips, grabaciones y capturas más antiguos al "
            "guardar uno nuevo, y te avisa. Solo cuenta y borra los archivos con el nombre que les pone Spectre "
            "(Clip_, Grabacion_, Captura_): los de otros programas y los que renombres no cuentan. Los favoritos "
            "cuentan, pero nunca se borran: marca con la estrella lo que quieras conservar.");
    // Con lo que hay ahora en la carpeta (si es la misma que se está editando).
    uint64_t limit = uint64_t(c.storageLimitGB) << 30;
    if (s.valid && c.outputDir == m.base.outputDir) {
      Gap(4);
      std::string now = FormatBytes(s.usedBytes) + " de " + std::to_string(c.storageLimitGB) + " GB";
      if (s.favoriteBytes > 0) now += ", " + FormatBytes(s.favoriteBytes) + " en favoritos";
      ValueRow("Ahora", now);
      if (s.favoriteBytes > limit) {
        Warning("Los favoritos ya ocupan más que este límite, así que Spectre no borrará nada: súbelo o quita "
                "alguna estrella.");
      } else if (s.usedBytes > limit && c.storageLimitGB != m.base.storageLimitGB) {
        std::string text = "Al aplicar se borrarán para siempre unos " + FormatBytes(s.usedBytes - limit) +
                           " de lo más antiguo.";
        Warning(text.c_str());
      }
    }
  } else {
    Help("Sin límite, Spectre no borra nada por su cuenta.");
  }

  Heading("CAPTURAS DE PANTALLA");
  FieldLabel("Formato");
  m.dirty |= ChoiceCombo<std::string>("##shot", c.screenshotFormat,
                                      {{"png", "PNG, sin pérdida"}, {"jpg", "JPG, ocupa menos"}});
  std::string hint = "Con " + (c.hotkeys.screenshot.empty() ? std::string("el atajo de captura") : c.hotkeys.screenshot) +
                     " o desde el panel. No hace falta que el replay esté encendido.";
  Help(hint.c_str());
}

void PageNotifications(SettingsModel& m) {
  auto& c = m.config;
  Heading("AVISOS EN PANTALLA");
  m.dirty |= SwitchField("Mostrar avisos", &c.showNotifications,
                         "Avisos breves al guardar un clip, una captura o empezar a grabar. Los errores se muestran "
                         "siempre. Sobre un juego a pantalla completa, un aviso puede añadir un frame de retraso "
                         "mientras se ve: si lo notas, apágalos y usa el sonido de abajo.");
  FieldLabel("Posición");
  m.dirty |= ChoiceCombo<std::string>("##pos", c.toastPosition,
                                      {{"top-right", "Arriba a la derecha"},
                                       {"top-left", "Arriba a la izquierda"},
                                       {"bottom-right", "Abajo a la derecha"},
                                       {"bottom-left", "Abajo a la izquierda"}});
  Help("Los avisos nunca salen en los clips ni en las capturas.");

  Heading("SONIDO");
  m.dirty |= SwitchField("Sonido al guardar", &c.soundOnSave,
                         "Un pitido corto al guardar un clip o una captura y al marcar un momento. Útil si el juego "
                         "tapa los avisos.");
  Help("Si capturas todo el sonido del PC, el pitido puede oírse al principio del siguiente clip. Con «Solo el "
       "audio del juego» no se graba.");
}

// Tarjeta del selector de temas: se pinta con los colores del tema que
// representa, con su nombre y tres muestras (fondo, superficie y acento).
bool ThemeCard(const Theme& t, bool selected) {
  const Palette& p = Colors();
  ImGui::PushID(t.id);
  bool pressed = ImGui::InvisibleButton("theme", ImVec2(Px(170), Px(52)));
  bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, C(t.colors.bg));
  dl->AddRect(a, b, C(selected ? p.accent : hov ? p.lineStrong : p.line), 0, 0, Px(1));
  float mid = (a.y + b.y) * 0.5f, fs = Px(kSmall);
  Text(dl, selected ? Bold() : ImGui::GetFont(), fs, ImVec2(a.x + Px(12), mid - fs * 0.5f),
       C(selected ? t.colors.text : t.colors.textMuted), t.name);
  const float sw = Px(12), gap = Px(3);
  float x = b.x - Px(12) - sw * 3 - gap * 2;
  ImVec2 s0(x, mid - sw * 0.5f);
  dl->AddRectFilled(s0, s0 + ImVec2(sw, sw), C(t.colors.bg));
  dl->AddRect(s0, s0 + ImVec2(sw, sw), C(t.colors.lineStrong), 0, 0, Px(1));
  ImVec2 s1 = s0 + ImVec2(sw + gap, 0);
  dl->AddRectFilled(s1, s1 + ImVec2(sw, sw), C(t.colors.surface));
  ImVec2 s2 = s1 + ImVec2(sw + gap, 0);
  dl->AddRectFilled(s2, s2 + ImVec2(sw, sw), C(t.colors.accent));
  return pressed;
}

void AppearanceSection(SettingsModel& m) {
  auto& c = m.config;
  Heading("APARIENCIA");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Tema");
  ImGui::SameLine(Px(kLabelColumn));
  bool first = true;
  for (const Theme* t : Themes()) {
    if (!first) ImGui::SameLine(0, Px(8));
    first = false;
    if (ThemeCard(*t, c.theme == t->id) && c.theme != t->id) {
      c.theme = t->id;
      m.dirty = true;
    }
  }
  Gap(2);
  // Solo el tema Psíquico tiene animaciones: con Negro el interruptor se ve apagado.
  ImGui::BeginDisabled(!FindTheme(c.theme).psychic);
  m.dirty |= SwitchField("Animaciones del tema", &c.themeMotion,
                         "Movimiento propio del tema Psíquico: rastro al abrir el panel, estela en el botón "
                         "principal y marca de estado que respira. El tema Negro no tiene animaciones.");
  ImGui::EndDisabled();
  Help("Los cambios se ven al momento; pulsa Aplicar para conservarlos.");
}

// Estado de la actualización: marca de color (o ruedita si está trabajando) y texto.
void UpdateStatusLine(ImU32 color, bool busy, const std::string& text, bool bold = false) {
  float rowH = ImGui::GetFrameHeight();
  ImVec2 q = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 c(q.x + Px(6), q.y + rowH * 0.5f);
  if (busy) {
    float t = float(ImGui::GetTime()) * 5.0f;
    dl->PathArcTo(c, Px(5), t, t + kPi * 1.4f, 16);
    dl->PathStroke(C(color), 0, Px(1.5f));
  } else {
    DrawMark(dl, c, Px(3.5f), C(color), true);
  }
  ImFont* font = bold ? Bold() : ImGui::GetFont();
  float fs = ImGui::GetFontSize();
  float wrap = std::min(ImGui::GetContentRegionAvail().x, Px(560)) - Px(20);
  ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, wrap, text.c_str());
  dl->AddText(font, fs, ImVec2(q.x + Px(20), q.y + (rowH - fs) * 0.5f), C(Colors().text), text.c_str(), nullptr,
              wrap);
  ImGui::Dummy(ImVec2(0, std::max(rowH, ts.y + (rowH - fs))));
}

void UpdatesSection(SettingsModel& m, const SettingsActions& a) {
  using S = UpdateStatus::State;
  const Palette& p = Colors();
  const UpdateStatus& u = m.update;
  Heading("ACTUALIZACIONES");
  ValueRow("Versión instalada", SPECTRE_VERSION);
  Gap(2);

  const float bw = Px(210);
  char buf[256];
  switch (u.state) {
  case S::Idle:
    Help("Spectre solo se conecta a GitHub cuando pulsas el botón: no comprueba nada en segundo plano.");
    Gap(4);
    if (GhostButton("update-check", "Buscar actualizaciones", bw)) a.checkUpdates();
    break;
  case S::Checking:
    UpdateStatusLine(p.textMuted, true, "Buscando actualizaciones...");
    break;
  case S::UpToDate:
    snprintf(buf, sizeof(buf), "Tienes la última versión (%s).", SPECTRE_VERSION);
    UpdateStatusLine(p.accent, false, buf);
    Gap(4);
    if (GhostButton("update-check", "Volver a buscar", bw)) a.checkUpdates();
    break;
  case S::Available:
    snprintf(buf, sizeof(buf), "Versión nueva disponible: %s", u.latest.c_str());
    UpdateStatusLine(p.accent, false, buf, true);
    if (u.portable || u.unverified) {
      if (u.portable)
        Help("Esta es la copia portable de Spectre: descarga el zip nuevo y reemplaza la carpeta. Si usas el "
             "instalador, Spectre se actualiza solo desde aquí.");
      else
        Help("GitHub no ha publicado la huella de este instalador, así que Spectre no puede comprobarlo y no lo "
             "instala solo. Descárgalo desde la página de la versión.");
      Gap(4);
      if (SolidButton("update-page", "Abrir página de descarga", bw)) a.openPath(Updater::ReleasePage());
    } else {
      std::string help = "Se descarga el instalador";
      if (u.sizeBytes > 0) help += " (" + FormatBytes(u.sizeBytes) + ")";
      help += ", Spectre se cierra, se actualiza conservando tus ajustes y se vuelve a abrir solo.";
      if (m.stats.recording) help += " Si sigues grabando, se instalará cuando detengas la grabación.";
      Help(help.c_str());
      Gap(4);
      if (SolidButton("update-install", "Actualizar ahora", bw)) a.installUpdate();
    }
    break;
  case S::Downloading: {
    int pct = int(std::clamp(u.downloaded, 0.0, 1.0) * 100.0 + 0.5);
    snprintf(buf, sizeof(buf), "Descargando la versión %s... %d %%", u.latest.c_str(), pct);
    UpdateStatusLine(p.accent, true, buf);
    Bar(ImVec2(ControlWidth(), Px(4)), float(pct) / 100.0f, p.accent);
    Gap(8);
    if (GhostButton("update-cancel", "Cancelar", Px(120))) a.cancelUpdate();
    break;
  }
  case S::Launching:
    if (m.stats.recording)
      UpdateStatusLine(p.accent, false, "Actualización lista: se instalará cuando detengas la grabación.");
    else
      UpdateStatusLine(p.accent, true, "Abriendo el instalador. Spectre se cerrará y volverá a abrirse al terminar.");
    break;
  case S::Error:
    UpdateStatusLine(p.danger, false, u.message);
    Gap(4);
    if (GhostButton("update-check", "Reintentar", bw)) a.checkUpdates();
    break;
  }
}

void PageGeneral(SettingsModel& m, const SettingsActions& a) {
  auto& c = m.config;
  AppearanceSection(m);

  Heading("INICIO Y CIERRE");
  m.dirty |= SwitchField("Iniciar con Windows", &c.autostart,
                         "Spectre arranca en la bandeja, sin abrir la ventana, y el replay queda listo.");
  FieldLabel("Al cerrar la ventana");
  m.dirty |= ChoiceCombo<bool>("##close", c.closeToTray,
                               {{true, "Seguir en la bandeja"}, {false, "Salir de Spectre"}});
  std::string tray = "En la bandeja Spectre sigue guardando el replay y los atajos funcionan. " +
                     (c.hotkeys.overlay.empty() ? std::string("El panel") : c.hotkeys.overlay + " abre el panel") +
                     (c.hotkeys.overlay.empty() ? " se abre desde el menú de la bandeja" : " sobre el juego") +
                     " y el icono de la bandeja vuelve a abrir esta ventana.";
  Help(tray.c_str());

  UpdatesSection(m, a);

  Heading("SPECTRE");
  Help("Salir cierra Spectre del todo: sin replay ni atajos hasta que lo vuelvas a abrir.");
  Gap(4);
  if (GhostButton("quit", "Salir de Spectre", Px(170), Tone::Danger)) a.quit();
}

void PagePerformance(SettingsModel& m) {
  const auto& s = m.stats;
  char buf[160];
  Heading("CONSUMO DE SPECTRE");
  snprintf(buf, sizeof(buf), "%.1f %%", m.usage.cpuPercent);
  ValueRow("CPU", buf);
  ValueRow("RAM", FormatBytes(m.usage.workingSet) + " (privada " + FormatBytes(m.usage.privateBytes) + ")");
  Help("Con la ventana abierta Spectre dibuja la interfaz; cerrada o minimizada no gasta nada en ella.");
  if (!s.running) {
    Gap(4);
    Help("La captura está parada, así que Spectre no usa la GPU ni el encoder.");
    return;
  }
  std::string replay = FormatBytes(s.bufferBytes);
  if (s.bufferLimitBytes > 0) replay += " de " + FormatBytes(s.bufferLimitBytes) + " como mucho";
  snprintf(buf, sizeof(buf), "%s, %.0f s", replay.c_str(), s.bufferSeconds);
  ValueRow("Replay en memoria", buf);

  Heading("CAPTURA");
  ValueRow("GPU", s.gpu);
  ValueRow("Método", s.capture);
  ValueRow("Encoder", s.encoder);
  if (s.encoderSoftware) {
    Gap(2);
    Warning("La gráfica no pudo codificar el vídeo: se codifica con la CPU, a 720p30 como mucho. Actualiza su driver.");
  }
  snprintf(buf, sizeof(buf), "%d x %d a %d fps", s.width, s.height, s.fps);
  ValueRow("Vídeo", buf);
  snprintf(buf, sizeof(buf), "hasta %d Mbps", s.maxKbps / 1000);
  ValueRow("Bitrate", buf);
  snprintf(buf, sizeof(buf), "%.2f ms", s.encodeMs);
  ValueRow("Tiempo por fotograma", buf);
  snprintf(buf, sizeof(buf), "%llu codificados, %llu descartados", (unsigned long long)s.framesEncoded,
           (unsigned long long)s.framesSkipped);
  ValueRow("Fotogramas", buf);
  snprintf(buf, sizeof(buf), "%d", s.audioTracks);
  ValueRow("Pistas de audio", buf);
}

// Enlace a una web: el nombre a la izquierda y la dirección en un botón que la
// abre en el navegador.
void LinkRow(const char* name, const char* url, const SettingsActions& a) {
  FieldLabel(name);
  const char* shown = std::strncmp(url, "https://", 8) == 0 ? url + 8 : url;
  ImGui::PushID(url);
  const bool open = GhostButton("link", shown, ControlWidth(), Tone::Normal, 28);
  ImGui::PopID();
  ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
  DrawIcon(ImGui::GetWindowDrawList(), Icon::External, ImVec2(hi.x - Px(16), (lo.y + hi.y) * 0.5f), Px(5),
           C(ImGui::IsItemHovered() ? Colors().text : Colors().textMuted));
  if (open) a.openPath(url);
}

void PageProjects(const SettingsActions& a) {
  Heading("GITHUB");
  LinkRow("marieteffk", "https://github.com/marieteffk", a);
  LinkRow("rippipupil", "https://github.com/rippipupil", a);
  Help("Se abren en el navegador.");
}

} // namespace

void CancelHotkeyCapture(SettingsModel& model, const SettingsActions& actions) {
  model.captureHint.clear();
  EndCapture(model, actions);
}

void DrawSettingsPage(SettingsPage page, SettingsModel& m, const SettingsActions& a) {
  if (page != SettingsPage::Hotkeys) CancelHotkeyCapture(m, a);
  switch (page) {
  case SettingsPage::Replay: PageReplay(m); break;
  case SettingsPage::Video: PageVideo(m); break;
  case SettingsPage::Audio: PageAudio(m); break;
  case SettingsPage::Camera: PageCamera(m); break;
  case SettingsPage::Hotkeys: PageHotkeys(m, a); break;
  case SettingsPage::Storage: PageStorage(m, a); break;
  case SettingsPage::Notifications: PageNotifications(m); break;
  case SettingsPage::General: PageGeneral(m, a); break;
  case SettingsPage::Projects: PageProjects(a); break;
  default: PagePerformance(m); break;
  }
}

} // namespace spectre
