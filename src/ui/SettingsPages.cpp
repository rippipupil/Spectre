#include "ui/SettingsPages.h"

#include "app/Sound.h"
#include "common/Util.h"
#include "encode/Quality.h"
#include "ui/Backdrop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

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
  case SettingsPage::Voice: return "Voz";
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

// «30 segundos», «1 minuto», «2 minutos 30 segundos».
std::string LongClipLength(int seconds) {
  char buf[48];
  const int min = seconds / 60, sec = seconds % 60;
  if (min == 0) snprintf(buf, sizeof(buf), "%d segundos", sec);
  else if (sec == 0) snprintf(buf, sizeof(buf), "%d %s", min, min == 1 ? "minuto" : "minutos");
  else snprintf(buf, sizeof(buf), "%d %s %d segundos", min, min == 1 ? "minuto" : "minutos", sec);
  return buf;
}

// La duración más larga que cabe entera en el límite de memoria de la
// configuración en edición, como PipelineStats::replayFitSeconds.
double AutoFitSeconds(const SettingsModel& m) {
  const double perSecond = double(EstimatedKbps(m)) * 125.0 * 1.1;
  if (perSecond <= 0) return 0;
  static const double quarter = [] {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    return GlobalMemoryStatusEx(&mem) ? double(mem.ullTotalPhys) / 4.0 : double(1u << 30);
  }();
  const double cap = m.config.maxBufferMB == kAutoBufferMB ? quarter : double(m.config.maxBufferMB) * 1024.0 * 1024.0;
  return std::max(0.0, cap / perSecond - ReplayBuffer::kExtraSeconds);
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
  {
    // Las de la rejilla del panel; la que trajera config.json de antes, también.
    std::vector<std::string> names;
    std::vector<int> values(std::begin(kClipLengths), std::end(kClipLengths));
    if (std::find(values.begin(), values.end(), c.clipSeconds) == values.end()) values.push_back(c.clipSeconds);
    std::sort(values.begin(), values.end());
    const double fit = AutoFitSeconds(m);
    for (int sec : values)
      names.push_back(LongClipLength(sec) + (fit > 0 && double(sec) > fit + 0.5 ? "  (no cabe en la RAM)" : ""));
    std::vector<std::pair<int, const char*>> options;
    for (size_t i = 0; i < values.size(); ++i) options.emplace_back(values[i], names[i].c_str());
    m.dirty |= ChoiceCombo<int>("##len", c.clipSeconds, options);
  }
  FieldLabel("Clip corto", "Lo que guarda el atajo de clip corto: solo el final de lo que pasó, para no tener que "
                           "recortar.");
  {
    std::vector<std::pair<int, const char*>> options = {
        {5, "5 segundos"},   {10, "10 segundos"}, {15, "15 segundos"}, {20, "20 segundos"},
        {30, "30 segundos"}, {45, "45 segundos"}, {60, "1 minuto"},    {120, "2 minutos"}};
    std::string own = LongClipLength(c.shortClipSeconds);
    if (std::none_of(options.begin(), options.end(), [&](const auto& o) { return o.first == c.shortClipSeconds; }))
      options.emplace_back(c.shortClipSeconds, own.c_str());
    m.dirty |= ChoiceCombo<int>("##short", c.shortClipSeconds, options);
    std::string key = c.hotkeys.shortClip.empty() ? std::string("Sin atajo (ponle uno en Ajustes > Atajos)")
                                                  : "Con " + c.hotkeys.shortClip + ".";
    if (c.shortClipSeconds >= c.clipSeconds)
      key += " Como el replay guarda " + FormatClipLength(c.clipSeconds) + ", el clip corto será igual de largo.";
    Help(key.c_str());
  }
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

  Heading("GRABACIÓN");
  FieldLabel("Detener la grabación tras", "Para no llenar el disco si se te olvida pararla. Se guarda igual que al "
                                          "detenerla con el atajo.");
  {
    std::vector<std::pair<int, const char*>> options = {{0, "Sin límite"}, {5, "5 minutos"},  {10, "10 minutos"},
                                                        {15, "15 minutos"}, {30, "30 minutos"}, {60, "1 hora"},
                                                        {120, "2 horas"},   {180, "3 horas"}};
    std::string own = std::to_string(c.recordLimitMinutes) + " minutos";
    if (std::none_of(options.begin(), options.end(), [&](const auto& o) { return o.first == c.recordLimitMinutes; }))
      options.emplace_back(c.recordLimitMinutes, own.c_str());
    m.dirty |= ChoiceCombo<int>("##reclimit", c.recordLimitMinutes, options);
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
  m.dirty |= SwitchField("Copiar el clip al guardarlo", &c.copyClipOnSave,
                         "Cada clip nuevo va al portapapeles: en Discord o WhatsApp basta con Ctrl+V. Sustituye lo "
                         "que hubiera copiado.");
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
  m.dirty |= SwitchField("Poder editar la cámara después", &c.cameraEditable);
  ImGui::EndDisabled();
  Help("La cámara se compone en la GPU. Viene apagada porque es lo que más CPU añade.");
  Help("«Poder editar la cámara después»: cada clip guarda aparte lo que hay debajo de la cámara y en el editor se "
       "puede mover, cambiar de tamaño o quitar. El replay ocupa algo más de memoria (un 10 % con la cámara al 25 %) "
       "y hace falta el encoder de la gráfica.");
}

// ------------------------------------------------------------------ atajos

struct HotkeyField {
  const char* label;
  std::string HotkeyConfig::*value;
};
const HotkeyField kHotkeyFields[] = {
    {"Abrir el panel", &HotkeyConfig::overlay},
    {"Guardar clip", &HotkeyConfig::saveClip},
    {"Guardar clip corto", &HotkeyConfig::shortClip},
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

// ---------------------------------------------------------------------- voz

std::string Ago(ULONGLONG at) {
  const ULONGLONG seconds = (GetTickCount64() - at) / 1000;
  if (seconds < 2) return "ahora mismo";
  if (seconds < 60) return "hace " + std::to_string(seconds) + " s";
  if (seconds < 3600) return "hace " + std::to_string(seconds / 60) + " min";
  return "hace más de una hora";
}

int Percent(float confidence) { return int(std::lround(std::clamp(confidence, 0.0f, 1.0f) * 100.0f)); }

int MinimumPercent(const std::string& sensitivity) {
  return sensitivity == "estricta" ? 80 : sensitivity == "permisiva" ? 40 : 60;
}

// Si escucha, con qué, y lo último que oyó: sirve para probar las frases.
void VoiceState(const SettingsModel& m) {
  const Palette& p = Colors();
  const VoiceConfig& active = m.base.voice; // lo aplicado; m.config es la copia que se edita
  if (!active.enabled) {
    if (m.config.voice.enabled) Help("Pulsa Aplicar para empezar a escuchar.");
    return;
  }
  const VoiceStatus& v = m.voice;
  FieldLabel("Estado");
  if (m.voiceWaiting) {
    ImGui::TextColored(V(p.textMuted), "En pausa hasta que abras un juego");
  } else if (v.state == VoiceStatus::State::Listening) {
    ImGui::TextColored(V(p.accent), "Escuchando");
  } else if (v.state == VoiceStatus::State::Error) {
    ImGui::TextColored(V(p.danger), "No escucha");
  } else {
    ImGui::TextColored(V(p.textMuted), "Arrancando...");
  }
  if (!m.voiceWaiting && v.state == VoiceStatus::State::Listening && !v.recognizer.empty())
    ValueRow("Reconocedor", v.recognizer);
  if (!m.voiceWaiting && v.state == VoiceStatus::State::Error && !v.error.empty()) ErrorText(v.error.c_str());
  if (!m.voiceWaiting && !v.note.empty()) Warning(v.note.c_str());
  if (!m.voiceWaiting && v.heardAt) {
    ValueRow("Lo último que oyó", v.heard.empty() ? std::string("algo que no es ninguna frase") : "«" + v.heard + "»");
    std::string detail;
    if (v.accepted)
      detail = "Orden aceptada, confianza " + std::to_string(Percent(v.confidence)) + " %, " + Ago(v.heardAt) + ".";
    else if (!v.heard.empty())
      detail = "No se aceptó: confianza " + std::to_string(Percent(v.confidence)) + " % y hace falta " +
               std::to_string(MinimumPercent(active.sensitivity)) + " %, " + Ago(v.heardAt) +
               ". Habla más claro o baja la exigencia.";
    else
      detail = "No era ninguna orden, " + Ago(v.heardAt) + ".";
    Help(detail.c_str());
  }
  if (m.config.voice.enabled && !SameVoiceConfig(m.config.voice, active))
    Help("Los cambios de esta página se usan al pulsar Aplicar.");
}

void PageVoice(SettingsModel& m) {
  auto& v = m.config.voice;
  Heading("COMANDOS DE VOZ");
  m.dirty |= SwitchField("Activar los comandos de voz", &v.enabled,
                         "Di la palabra de activación y una orden, por ejemplo «Spectre clip», y Spectre hace lo "
                         "mismo que con su atajo, sin soltar el mando ni el teclado.");
  Help("Usa el reconocimiento de voz de Windows, sin conexión: lo que dices no sale del PC y no hay nada que "
       "descargar. Apagados, Spectre no abre el micrófono para esto.");
  VoiceState(m);

  ImGui::BeginDisabled(!v.enabled);
  FieldLabel("Palabra de activación");
  m.dirty |= ImGui::InputTextWithHint("##wake", "Ninguna: basta con la frase", &v.wakeWord);
  Help("Se dice antes de cada orden. Así una conversación con tus amigos no guarda clips sin querer.");
  if (v.wakeWord.find_first_not_of(" \t") == std::string::npos) {
    // Sin palabra, una frase de una sola palabra («para», «marca») salta al hablar.
    std::string loose;
    int count = 0;
    for (int i = 0; i < kVoiceCommandCount; ++i)
      for (const std::string& phrase : SplitPhrases(VoicePhrases(v, VoiceCommand(i))))
        if (phrase.find(' ') == std::string::npos && count < 3) {
          loose += std::string(count++ ? ", " : "") + "«" + phrase + "»";
        }
    if (count) {
      std::string text = "Sin palabra de activación, " + loose +
                         " saltan al decirlas hablando con tus amigos: usa frases de dos o tres palabras.";
      Warning(text.c_str());
    }
  }
  std::vector<std::pair<std::string, const char*>> recognizers = {{"", "Automático (el de español)"}};
  for (const auto& r : m.recognizers) recognizers.emplace_back(r.id, r.name.c_str());
  FieldLabel("Reconocedor");
  m.dirty |= ChoiceCombo<std::string>("##recognizer", v.recognizer, recognizers);
  if (m.devicesLoaded && m.recognizers.empty())
    Warning("Windows no tiene ningún reconocedor de voz instalado: mira la ayuda del final de la página.");
  else if (m.devicesLoaded &&
           std::none_of(m.recognizers.begin(), m.recognizers.end(), [](const VoiceRecognizer& r) { return r.spanish; }))
    Warning("No hay reconocedor de español: escribe las frases en el idioma del que tengas o instala el español de "
            "España (mira la ayuda del final de la página).");
  FieldLabel("Exigencia");
  m.dirty |= ChoiceCombo<std::string>("##sensitivity", v.sensitivity,
                                      {{"estricta", "Estricta: casi nunca se equivoca"},
                                       {"normal", "Normal"},
                                       {"permisiva", "Permisiva: entiende con ruido o a lo lejos"}});
  m.dirty |= SwitchField("Solo con un juego abierto", &v.onlyInGames,
                         "Escucha solo con un juego a pantalla completa o mientras grabas. En el escritorio el "
                         "micrófono no se abre.");
  m.dirty |= SwitchField("Sonido al entender una orden", &v.sound,
                         "Dos notas cortas en cuanto entiende la orden, antes de que se guarde nada. Útil si el juego "
                         "tapa los avisos.");

  Heading("FRASES");
  std::vector<std::string> clip = SplitPhrases(v.saveClip);
  std::string example = "Varias frases por orden, separadas por comas. Funcionan mejor las de dos o tres palabras que "
                        "no dices al jugar.";
  if (!clip.empty()) example += " Por ejemplo, para guardar un clip di «" + SpokenPhrase(v, clip.front()) + "».";
  Help(example.c_str());
  Gap(4);
  for (int i = 0; i < kVoiceCommandCount; ++i) {
    ImGui::PushID(i);
    FieldLabel(VoiceCommandName(VoiceCommand(i)));
    m.dirty |= ImGui::InputTextWithHint("##phrases", "Sin frase", &VoicePhrases(v, VoiceCommand(i)));
    ImGui::PopID();
  }
  std::string repeated = RepeatedPhrase(v);
  if (!repeated.empty()) {
    std::string text = "«" + repeated + "» está en dos órdenes: solo cuenta para la primera.";
    Warning(text.c_str());
  }
  Gap(4);
  if (GhostButton("voice-defaults", "Restaurar las frases", Px(190))) {
    const VoiceConfig defaults;
    v.wakeWord = defaults.wakeWord;
    for (int i = 0; i < kVoiceCommandCount; ++i)
      VoicePhrases(v, VoiceCommand(i)) = VoicePhrases(defaults, VoiceCommand(i));
    m.dirty = true;
  }
  Help("El clip acaba donde empezaste a decir la orden, no después: la frase no sale al final. Las marcas, igual. "
       "Al detener una grabación por voz, la frase sí queda al final de la grabación.");

  Heading("CONSUMO Y MICRÓFONO");
  Help("El reconocedor solo compara lo que oye con estas frases: entre frase y frase apenas usa CPU, y el de "
       "Windows ocupa algo de memoria mientras escucha. Escucha el micrófono de Ajustes > Audio (o el predeterminado "
       "de Windows) aunque no lo grabes en los clips, y lo comparte con Discord y el juego.");
  Help("Con auriculares Bluetooth, abrir el micrófono hace que Windows pase los cascos a manos libres y el sonido "
       "pierde calidad. Si te pasa, usa un micrófono con cable o deja la voz apagada; con «Solo con un juego "
       "abierto», al menos fuera de los juegos los cascos suenan bien.");
  Help("Para tener el reconocedor de español: Configuración > Hora e idioma > Idioma y región > Agregar un idioma > "
       "Español (España), y en sus opciones de idioma instala «Reconocimiento de voz». Windows lo trae para español "
       "de España, inglés, francés, alemán, japonés y chino.");
  ImGui::EndDisabled();
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

  Heading("BORRAR LO ANTIGUO");
  FieldLabel("Borrar lo que tenga más de", "Para que la carpeta no crezca sin fin aunque no pongas un límite de "
                                           "espacio.");
  {
    std::vector<std::pair<int, const char*>> options = {{0, "Nunca"},     {7, "7 días"},    {14, "14 días"},
                                                        {30, "30 días"},  {60, "60 días"},  {90, "90 días"},
                                                        {180, "6 meses"}, {365, "1 año"}};
    std::string own = std::to_string(c.deleteAfterDays) + " días";
    if (std::none_of(options.begin(), options.end(), [&](const auto& o) { return o.first == c.deleteAfterDays; }))
      options.emplace_back(c.deleteAfterDays, own.c_str());
    m.dirty |= ChoiceCombo<int>("##age", c.deleteAfterDays, options);
  }
  if (c.deleteAfterDays > 0) {
    Warning("Al aplicar, al abrir Spectre y cada vez que guardes algo, se borran para siempre los clips, grabaciones "
            "y capturas de Spectre más antiguos que eso, y te avisa. Los favoritos y los archivos renombrados o de "
            "otros programas nunca se borran.");
    // Lo que se iría al pulsar Aplicar, con lo que hay ahora en la carpeta: así
    // elegir «7 días» por error no se lleva meses de clips sin verlo antes.
    if (c.deleteAfterDays != m.base.deleteAfterDays && s.valid && c.outputDir == m.base.outputDir) {
      FILETIME nowFt;
      GetSystemTimeAsFileTime(&nowFt);
      const int64_t now = int64_t((uint64_t(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime);
      const int64_t maxAge = int64_t(c.deleteAfterDays) * 24 * 3600 * 10000000;
      int count = 0;
      uint64_t bytes = 0;
      for (const auto& [modified, size] : s.deletable)
        if (now - modified > maxAge) {
          ++count;
          bytes += size;
        }
      Gap(4);
      if (count > 0) {
        std::string text = "Al aplicar se borrarán para siempre " + std::to_string(count) +
                           (count == 1 ? " archivo (" : " archivos (") + FormatBytes(bytes) +
                           "). Marca con la estrella lo que quieras conservar antes de aplicar.";
        Warning(text.c_str());
      } else {
        Help("Ahora no hay nada tan antiguo: al aplicar no se borra nada.");
      }
    }
  }

  Heading("CAPTURAS DE PANTALLA");
  FieldLabel("Formato");
  m.dirty |= ChoiceCombo<std::string>("##shot", c.screenshotFormat,
                                      {{"png", "PNG, sin pérdida"}, {"jpg", "JPG, ocupa menos"}});
  std::string hint = "Con " + (c.hotkeys.screenshot.empty() ? std::string("el atajo de captura") : c.hotkeys.screenshot) +
                     " o desde el panel. No hace falta que el replay esté encendido.";
  Help(hint.c_str());
}

void PageNotifications(SettingsModel& m, const SettingsActions& a) {
  auto& c = m.config;
  Heading("AVISOS EN PANTALLA");
  m.dirty |= SwitchField("Mostrar avisos", &c.showNotifications,
                         "Como los de Medal: al guardar un clip (primero «Guardando» y luego «Guardado»), al empezar "
                         "y terminar una grabación, con capturas y marcas, al activar o quitar el replay, el "
                         "micrófono o la cámara, y al terminar una edición o una exportación. Cada tema los dibuja y "
                         "los anima a su manera. Los errores y el aviso de poco disco se muestran siempre. Sobre un "
                         "juego a pantalla completa, un aviso puede añadir un frame de retraso mientras se ve: si lo "
                         "notas, apágalos y usa el sonido de abajo.");
  ImGui::BeginDisabled(!c.showNotifications);
  m.dirty |= SwitchField("Al detectar un juego", &c.notifyGame,
                         "«Replay listo en ...» la primera vez que se ve cada juego a pantalla completa, con el "
                         "atajo para guardar. No sale con navegadores ni reproductores de vídeo.");
  m.dirty |= SwitchField("Versiones nuevas", &c.notifyUpdates,
                         "Mira en GitHub si hay una versión nueva un minuto después de abrir Spectre y cada 12 horas, "
                         "y lo avisa cuando no estás jugando. Nada se descarga sin pulsar Actualizar en Ajustes > "
                         "General.");
  ImGui::EndDisabled();
  Gap(4);
  if (GhostButton("notice-test", "Probar aviso", Px(160)) && a.previewNotice) a.previewNotice();
  Help("Enseña «Guardando clip» y «Clip guardado» con el tema elegido, sin guardar nada.");
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
  if (c.soundOnSave) {
    FieldLabel("Sonido");
    std::vector<std::pair<std::string, const char*>> sounds(SaveSounds().begin(), SaveSounds().end());
    ImGui::SetNextItemWidth(ControlWidth() - Px(100));
    if (ChoiceCombo<std::string>("##sound", c.saveSound, sounds)) {
      m.dirty = true;
      if (a.playSound) a.playSound(c.saveSound);
    }
    ImGui::SameLine(0, Px(8));
    if (GhostButton("listen", "Escuchar", Px(92), Tone::Normal, 26) && a.playSound) a.playSound(c.saveSound);
  }
  Help("Si capturas todo el sonido del PC, el pitido puede oírse al principio del siguiente clip. Con «Solo el "
       "audio del juego» no se graba.");
}

// Borde de una tarjeta de tema con la forma de su diseño: esquinas cortadas
// (Psíquico, Táctico), corchetes (Terminal), raíl (Suave), línea arriba
// (Contorno), doble grosor (Alto contraste) o un rectángulo.
void StyleFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, const Style& st, ImU32 edge, ImU32 accent, float thickness) {
  thickness *= st.line;
  switch (st.button) {
  case ButtonShape::Chamfer: {
    const float cut = Px(std::min(st.cut, 8.0f));
    const bool tl = st.cutCorners & 1, tr = st.cutCorners & 2, br = st.cutCorners & 4, bl = st.cutCorners & 8;
    ImVec2 pts[8];
    int n = 0;
    if (tl) { pts[n++] = ImVec2(a.x, a.y + cut); pts[n++] = ImVec2(a.x + cut, a.y); } else pts[n++] = a;
    if (tr) { pts[n++] = ImVec2(b.x - cut, a.y); pts[n++] = ImVec2(b.x, a.y + cut); } else pts[n++] = ImVec2(b.x, a.y);
    if (br) { pts[n++] = ImVec2(b.x, b.y - cut); pts[n++] = ImVec2(b.x - cut, b.y); } else pts[n++] = b;
    if (bl) { pts[n++] = ImVec2(a.x + cut, b.y); pts[n++] = ImVec2(a.x, b.y - cut); } else pts[n++] = ImVec2(a.x, b.y);
    dl->AddPolyline(pts, n, edge, ImDrawFlags_Closed, thickness);
    break;
  }
  case ButtonShape::Bracket: {
    const float arm = Px(8);
    dl->AddRect(a, b, C(edge, 0.35f), 0, 0, Px(1));
    for (int i = 0; i < 4; ++i) {
      ImVec2 c(i & 1 ? b.x : a.x, i & 2 ? b.y : a.y);
      const float sx = i & 1 ? -1.0f : 1.0f, sy = i & 2 ? -1.0f : 1.0f;
      ImVec2 pts[3] = {ImVec2(c.x, c.y + sy * arm), c, ImVec2(c.x + sx * arm, c.y)};
      dl->AddPolyline(pts, 3, edge, 0, thickness);
    }
    break;
  }
  case ButtonShape::Rail:
    dl->AddRect(a, b, edge, 0, 0, thickness);
    dl->AddRectFilled(a, ImVec2(a.x + Px(3), b.y), accent);
    break;
  case ButtonShape::Double:
    dl->AddRect(a, b, edge, 0, 0, thickness);
    dl->AddRect(a + ImVec2(Px(3), Px(3)), b - ImVec2(Px(3), Px(3)), C(accent, 0.45f), 0, 0, Px(1));
    break;
  default:
    dl->AddRect(a, b, edge, 0, 0, thickness);
    if (st.toast == ToastLook::TopLine) dl->AddRectFilled(a, ImVec2(b.x, a.y + Px(2)), accent);
    break;
  }
}

// Rectángulo con las esquinas cortadas de un diseño (miniaturas).
void ChamferFill(ImDrawList* dl, ImVec2 a, ImVec2 b, const Style& st, ImU32 color) {
  const float cut = std::min(Px(st.cut) * 0.5f, std::min(b.x - a.x, b.y - a.y) * 0.4f);
  ImVec2 pts[8];
  int n = 0;
  if (st.cutCorners & 1) { pts[n++] = ImVec2(a.x, a.y + cut); pts[n++] = ImVec2(a.x + cut, a.y); } else pts[n++] = a;
  if (st.cutCorners & 2) { pts[n++] = ImVec2(b.x - cut, a.y); pts[n++] = ImVec2(b.x, a.y + cut); } else pts[n++] = ImVec2(b.x, a.y);
  if (st.cutCorners & 4) { pts[n++] = ImVec2(b.x, b.y - cut); pts[n++] = ImVec2(b.x - cut, b.y); } else pts[n++] = b;
  if (st.cutCorners & 8) { pts[n++] = ImVec2(a.x + cut, b.y); pts[n++] = ImVec2(a.x, b.y - cut); } else pts[n++] = ImVec2(a.x, b.y);
  dl->AddConvexPolyFilled(pts, n, color);
}

// Botón principal de muestra con la forma del diseño.
void PreviewButton(ImDrawList* dl, ImVec2 a, ImVec2 b, const Style& st, const Palette& c) {
  switch (st.button) {
  case ButtonShape::Flat: dl->AddRectFilled(a, b, C(c.accent)); break;
  case ButtonShape::Chamfer: ChamferFill(dl, a, b, st, C(c.accent)); break;
  case ButtonShape::Outline: dl->AddRect(a, b, C(c.accent), 0, 0, Px(st.line)); break;
  case ButtonShape::Double:
    dl->AddRect(a, b, C(c.accent), 0, 0, Px(1));
    dl->AddRect(a - ImVec2(Px(2), Px(2)), b + ImVec2(Px(2), Px(2)), C(c.accent, 0.4f), 0, 0, Px(1));
    break;
  case ButtonShape::Bracket: DrawBrackets(dl, a, b, C(c.accent), Px(4), Px(1)); break;
  case ButtonShape::Rail:
    dl->AddRectFilled(a, b, C(c.surface));
    dl->AddRectFilled(a, ImVec2(a.x + Px(2), b.y), C(c.accent));
    break;
  }
}

// Tarjeta de la biblioteca de muestra con la forma del diseño.
void PreviewCard(ImDrawList* dl, ImVec2 a, ImVec2 b, const Style& st, const Palette& c) {
  const float t = Px(1);
  const float textH = (b.y - a.y) * 0.28f;
  ImVec2 ta = a, tb(b.x, b.y - textH);
  switch (st.card) {
  case CardLook::Tile:
  case CardLook::Polaroid:
    dl->AddRectFilled(a, b, C(c.surface));
    if (st.card == CardLook::Polaroid) dl->AddRect(a, b, C(c.line), 0, 0, t);
    ta += ImVec2(Px(2), Px(2));
    tb.x -= Px(2);
    break;
  case CardLook::Framed:
    dl->AddRect(a, b, C(c.lineStrong), 0, 0, t);
    ta += ImVec2(Px(2), Px(2));
    tb.x -= Px(2);
    break;
  case CardLook::Neon:
    dl->AddRect(a, b, C(c.lineStrong), 0, 0, t);
    dl->AddRect(a - ImVec2(Px(2), Px(2)), b + ImVec2(Px(2), Px(2)), C(c.accent, 0.35f), 0, 0, t);
    ta += ImVec2(Px(2), Px(2));
    tb.x -= Px(2);
    break;
  case CardLook::Ascii:
    for (float x = a.x; x < b.x; x += Px(4)) {
      dl->AddRectFilled(ImVec2(x, a.y), ImVec2(std::min(x + Px(2), b.x), a.y + t), C(c.lineStrong));
      dl->AddRectFilled(ImVec2(x, b.y - t), ImVec2(std::min(x + Px(2), b.x), b.y), C(c.lineStrong));
    }
    ta += ImVec2(Px(2), Px(2));
    tb.x -= Px(2);
    break;
  case CardLook::Heavy:
    dl->AddRectFilled(ImVec2(a.x, tb.y), b, C(c.text));
    break;
  case CardLook::Plain:
  case CardLook::Hud:
    break;
  }
  if (st.card == CardLook::Tile) dl->AddRectFilled(ta, tb, C(c.bgAlt));
  else if (st.button == ButtonShape::Chamfer) ChamferFill(dl, ta, tb, st, C(c.surfaceActive));
  else dl->AddRectFilled(ta, tb, C(c.surfaceActive));
  if (st.card == CardLook::Hud) DrawBrackets(dl, ta - ImVec2(Px(2), Px(2)), tb + ImVec2(Px(2), Px(2)), C(c.lineStrong), Px(4), t);
  // Dos líneas de texto en el pie.
  const ImU32 ink = st.card == CardLook::Heavy ? C(c.bg) : C(c.textMuted);
  const float y = tb.y + Px(3), x = ta.x + (st.card == CardLook::Heavy ? Px(2) : 0.0f);
  const float w = tb.x - x;
  if (st.card == CardLook::Polaroid) {
    dl->AddRectFilled(ImVec2(x + w * 0.2f, y), ImVec2(x + w * 0.8f, y + Px(2)), ink);
  } else {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w * 0.7f, y + Px(2)), ink);
    dl->AddRectFilled(ImVec2(x, y + Px(4)), ImVec2(x + w * 0.45f, y + Px(5)), C(c.textFaint));
  }
}

// Título de muestra: una barra con el estilo de títulos del diseño.
void PreviewTitle(ImDrawList* dl, ImVec2 q, float avail, const Style& st, const Palette& c) {
  const float h = Px(st.titleSize >= 30 ? 6.0f : 5.0f), w = avail * 0.42f;
  const ImU32 ink = C(c.text);
  switch (st.title) {
  case TitleLook::Block:
    dl->AddRectFilled(q, ImVec2(q.x + Px(2), q.y + h), C(c.accent));
    dl->AddRectFilled(ImVec2(q.x + Px(5), q.y), ImVec2(q.x + Px(5) + w, q.y + h), ink);
    break;
  case TitleLook::Echo:
    dl->AddRectFilled(q + ImVec2(Px(2), Px(2)), q + ImVec2(Px(2) + w, Px(2) + h * 0.6f), C(c.accent, 0.5f));
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + h * 0.6f), ink);
    break;
  case TitleLook::Chroma:
    dl->AddRectFilled(q - ImVec2(Px(1), 0), ImVec2(q.x - Px(1) + w, q.y + h), C(c.danger, 0.6f));
    dl->AddRectFilled(q + ImVec2(Px(1), 0), ImVec2(q.x + Px(1) + w, q.y + h), C(c.accent, 0.8f));
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + h), ink);
    break;
  case TitleLook::Index:
    dl->AddRectFilled(q, ImVec2(q.x + Px(5), q.y + h), C(c.accent));
    dl->AddRectFilled(ImVec2(q.x + Px(8), q.y), ImVec2(q.x + Px(8) + w, q.y + h), ink);
    dl->AddRectFilled(ImVec2(q.x, q.y + h + Px(2)), ImVec2(q.x + avail, q.y + h + Px(3)), C(c.line));
    break;
  case TitleLook::Prompt:
    dl->AddRectFilled(q, ImVec2(q.x + w * 0.6f, q.y + h * 0.7f), C(c.accent));
    dl->AddRectFilled(ImVec2(q.x + w * 0.6f + Px(3), q.y), ImVec2(q.x + w * 0.6f + Px(6), q.y + h * 0.7f), C(c.accent));
    break;
  case TitleLook::Rule:
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + h * 0.8f), ink);
    dl->AddRectFilled(ImVec2(q.x, q.y + h + Px(2)), ImVec2(q.x + w, q.y + h + Px(3)), C(c.accent));
    dl->AddRectFilled(ImVec2(q.x + w, q.y + h + Px(2)), ImVec2(q.x + avail, q.y + h + Px(2) + 1), C(c.line));
    break;
  case TitleLook::Soft:
    dl->AddRectFilled(q, ImVec2(q.x + Px(5), q.y + h), C(c.accent));
    dl->AddRectFilled(ImVec2(q.x + Px(6), q.y), ImVec2(q.x + w, q.y + h), ink);
    break;
  case TitleLook::Masthead: {
    dl->AddRectFilled(q, ImVec2(q.x + avail, q.y + 1), ink);
    const float x = q.x + (avail - w) * 0.5f;
    dl->AddRectFilled(ImVec2(x, q.y + Px(3)), ImVec2(x + w, q.y + Px(3) + h), ink);
    dl->AddRectFilled(ImVec2(q.x, q.y + h + Px(5)), ImVec2(q.x + avail, q.y + h + Px(5) + 1), C(c.lineStrong));
    break;
  }
  case TitleLook::Heavy:
    dl->AddRectFilled(q, ImVec2(q.x + w + Px(6), q.y + h + Px(4)), ink);
    dl->AddRectFilled(ImVec2(q.x + Px(3), q.y + Px(2)), ImVec2(q.x + w + Px(3), q.y + h + Px(2)), C(c.bg));
    break;
  }
}

// Miniatura de la app con un tema: sus colores, su fondo ilustrado y, en
// pequeño, la estructura de su diseño (barra lateral, raíl o pestañas; título,
// botón principal y dos tarjetas).
void ThemePreview(ImDrawList* dl, ImVec2 a, ImVec2 b, const Theme& t, const Style& st) {
  const Palette& c = t.colors;
  dl->PushClipRect(a, b, true);
  dl->AddRectFilled(a, b, C(c.bg));
  DrawMotif(dl, t.motif, c, t.light, a, b, Px(0.5f), 12.5, 1.3f);
  const float w = b.x - a.x, h = b.y - a.y;
  ImVec2 ca = a, cb = b; // zona de la página
  switch (st.nav) {
  case NavLayout::Sidebar: {
    const float m = st.floatingNav ? Px(3) : 0.0f;
    const ImVec2 na(a.x + m, a.y + m), nb(a.x + m + w * 0.25f, b.y - m);
    dl->AddRectFilled(na, nb, C(c.bgAlt));
    if (!st.floatingNav) dl->AddRectFilled(ImVec2(nb.x - 1, na.y), nb, C(c.line));
    for (int i = 0; i < 5; ++i) {
      const float y = na.y + Px(10) + float(i) * Px(7);
      if (y + Px(4) > nb.y) break;
      if (i == 0) dl->AddRectFilled(ImVec2(na.x, y - Px(2)), ImVec2(nb.x - 1, y + Px(4)), C(c.surface));
      if (i == 0) dl->AddRectFilled(ImVec2(na.x, y - Px(2)), ImVec2(na.x + Px(2), y + Px(4)), C(c.accent));
      dl->AddRectFilled(ImVec2(na.x + Px(5), y), ImVec2(na.x + Px(5) + (nb.x - na.x) * (i == 0 ? 0.5f : 0.4f), y + Px(2)),
                        C(i == 0 ? c.text : c.textFaint));
    }
    ca.x = nb.x;
    break;
  }
  case NavLayout::Rail: {
    const ImVec2 nb(a.x + std::max(Px(14), w * 0.11f), b.y);
    dl->AddRectFilled(a, nb, C(c.bgAlt));
    dl->AddRectFilled(ImVec2(nb.x - 1, a.y), nb, C(c.line));
    const float cx = (a.x + nb.x) * 0.5f, s = Px(2.5f);
    dl->AddRectFilled(ImVec2(cx - s - 1, a.y + Px(5)), ImVec2(cx + s + 1, a.y + Px(5) + s * 2 + 2), C(c.accent));
    for (int i = 0; i < 3; ++i) {
      const float y = a.y + Px(18) + float(i) * Px(10);
      dl->AddRect(ImVec2(cx - s, y), ImVec2(cx + s, y + s * 2), C(i == 0 ? c.accent : c.textFaint), 0, 0, 1);
    }
    ca.x = nb.x;
    break;
  }
  case NavLayout::Topbar: {
    const ImVec2 nb(b.x, a.y + std::max(Px(12), h * 0.16f));
    dl->AddRectFilled(a, nb, C(c.bgAlt));
    dl->AddRectFilled(ImVec2(a.x, nb.y - 1), nb, C(c.line));
    const float mid = (a.y + nb.y) * 0.5f;
    dl->AddRectFilled(ImVec2(a.x + Px(5), mid - Px(2)), ImVec2(a.x + Px(9), mid + Px(2)), C(c.accent));
    for (int i = 0; i < 3; ++i) {
      const float x = a.x + Px(16) + float(i) * Px(16);
      dl->AddRectFilled(ImVec2(x, mid - 1), ImVec2(x + Px(11), mid + 1), C(i == 0 ? c.text : c.textFaint));
      if (i == 0) dl->AddRectFilled(ImVec2(x, nb.y - Px(2)), ImVec2(x + Px(11), nb.y), C(c.accent));
    }
    ca.y = nb.y;
    break;
  }
  }
  // Página: título, botón y dos tarjetas, con el margen del diseño.
  const float pad = Px(st.pad * 0.22f);
  ca += ImVec2(pad, pad * 0.8f);
  cb -= ImVec2(pad, pad * 0.6f);
  const float avail = cb.x - ca.x;
  PreviewTitle(dl, ca, avail, st, c);
  const float by = ca.y + Px(14);
  PreviewButton(dl, ImVec2(ca.x, by), ImVec2(ca.x + avail * 0.32f, by + Px(8)), st, c);
  const float gap = Px(st.gap * 0.3f), cardW = (avail - gap) * 0.5f, cy = by + Px(14);
  const float cardH = std::min(cb.y - cy, cardW * 0.82f);
  if (cardH > Px(10))
    for (int i = 0; i < 2; ++i) {
      const float x = ca.x + float(i) * (cardW + gap);
      PreviewCard(dl, ImVec2(x, cy), ImVec2(x + cardW, cy + cardH), st, c);
    }
  dl->PopClipRect();
}

// Tarjeta del selector de temas: una miniatura de la app con ese tema y su
// diseño, y debajo su nombre y el del diseño.
float ThemeCardHeight(float width) { return std::floor(width * 0.56f) + Px(28); }

bool ThemeCard(const Theme& t, bool selected, float width, const std::string& styleOverride) {
  const Palette& p = Colors();
  const Style& st = StyleFor(t, styleOverride);
  ImGui::PushID(t.id);
  const float previewH = std::floor(width * 0.56f);
  bool pressed = ImGui::InvisibleButton("theme", ImVec2(width, ThemeCardHeight(width)));
  bool hov = ImGui::IsItemHovered();
  Motion mo = Animate();
  ImGui::PopID();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 pb(b.x, a.y + previewH);
  ThemePreview(dl, a, pb, t, st);
  StyleFrame(dl, a, pb, st, C(selected ? p.accent : Mix(p.line, p.lineStrong, mo.hover)), C(t.colors.accent),
             selected ? Px(2) : Px(1));
  // Nombre del tema y, a la derecha, el del diseño.
  const float fs = Px(kSmall), y = pb.y + Px(7);
  ImFont* font = selected ? Bold() : ImGui::GetFont();
  const float styleW = TextSize(ImGui::GetFont(), Px(kHint), st.name).x;
  std::string name = Ellipsize(font, fs, t.name, width - styleW - Px(12));
  if (selected) DrawMark(dl, ImVec2(a.x + Px(4), y + fs * 0.5f), Px(3), C(p.accent), true);
  const float nx = a.x + (selected ? Px(14) : 0.0f);
  Text(dl, font, fs, ImVec2(nx, y), C(selected || mo.hover > 0.5f ? p.text : p.textMuted), name.c_str());
  Text(dl, ImGui::GetFont(), Px(kHint), ImVec2(b.x - styleW, y + (fs - Px(kHint)) * 0.5f + Px(1)), C(p.textFaint),
       st.name);
  PaintPress(dl, a, pb, Role::Quiet, mo);
  return pressed;
}

void AppearanceSection(SettingsModel& m) {
  auto& c = m.config;
  Heading("APARIENCIA");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Tema");
  ImGui::SameLine(Px(kLabelColumn));
  // Rejilla de tarjetas en la columna de controles, por grupos: los de
  // Spectre y los de RoCer0. Al pulsar una, el tema se aplica y se guarda
  // (App lo detecta tras el frame): la app, el panel y los avisos lo usan ya.
  ImGui::BeginGroup();
  const float gap = Px(12);
  const float avail = std::max(ImGui::GetContentRegionAvail().x, Px(170));
  const int cols = std::clamp(int((avail + gap) / (Px(170) + gap)), 1, 4);
  const float cardW = std::floor((avail - gap * float(cols - 1)) / float(cols));
  const char* group = nullptr;
  int col = 0;
  for (const Theme* t : Themes()) {
    if (!group || std::strcmp(group, t->group) != 0) {
      if (group) Gap(6);
      group = t->group;
      col = 0;
      std::string label = group;
      for (auto& ch : label) ch = char(toupper((unsigned char)ch));
      ImVec2 q = ImGui::GetCursorScreenPos();
      float ls = Px(kLabel);
      TrackedText(ImGui::GetWindowDrawList(), Bold(), ls, ImVec2(q.x, q.y + Px(2)), C(Colors().textFaint),
                  label.c_str(), Px(1.5f));
      ImGui::Dummy(ImVec2(0, ls + Px(6)));
    }
    if (col > 0) ImGui::SameLine(0, gap);
    if (ThemeCard(*t, c.theme == t->id, cardW, c.themeStyle) && c.theme != t->id) c.theme = t->id;
    col = (col + 1) % cols;
  }
  ImGui::EndGroup();
  Gap(2);
  // Cada tema trae su diseño (forma de los botones, bordes, rótulos,
  // interruptores, avisos y animaciones); aquí se puede usar otro.
  const Theme& theme = FindTheme(c.theme);
  const std::string own = std::string("El del tema (") + FindStyle(theme.style).name + ")";
  std::vector<std::pair<std::string, const char*>> styles{{"", own.c_str()}};
  for (const Style* st : Styles()) styles.emplace_back(st->id, st->name);
  FieldLabel("Diseño");
  ChoiceCombo<std::string>("##style", c.themeStyle, styles);
  const Style& style = StyleFor(theme, c.themeStyle);
  Help(style.summary);
  Gap(2);
  // Alto contraste no tiene movimiento: el interruptor se ve apagado.
  ImGui::BeginDisabled(!style.motion);
  SwitchField("Animaciones", &c.themeMotion,
              "Al pasar por encima y al pulsar un botón, al abrir el panel, al cambiar de página y en los avisos "
              "(la de «Clip guardado» es distinta en cada diseño). Apagadas, todo aparece sin movimiento.");
  ImGui::EndDisabled();
  FieldLabel("Fondo del tema", "Cada tema trae un dibujo de líneas finas detrás de las páginas, del panel y de los "
                               "avisos: estrellas, lluvia de código, una ciudad, olas... Animado solo se mueve con "
                               "esta ventana delante y con el panel abierto; quieto o sin fondo no gasta nada.");
  ChoiceCombo<std::string>("##backdrop", c.themeBackdrop,
                           {{"animado", "Animado"}, {"quieto", "Quieto"}, {"no", "Sin fondo"}});
  Help("El tema y el diseño se guardan al elegirlos y se usan en esta ventana, en el panel (Alt+Z) y en los avisos.");
}

// Panel Alt+Z: lateral de todo el alto (como el de NVIDIA) o el compacto de
// antes. Los dos se arrastran y se vuelven a abrir donde se dejaron.
void PanelSection(SettingsModel& m) {
  auto& c = m.config;
  const Config defaults;
  Heading("PANEL SOBRE EL JUEGO");
  FieldLabel("Forma del panel");
  m.dirty |= ChoiceCombo<std::string>("##panel", c.panelLayout,
                                      {{"lateral", "Lateral, de arriba abajo"}, {"compacto", "Compacto (el de antes)"}});
  std::string help = c.hotkeys.overlay.empty() ? std::string("El panel") : "El panel de " + c.hotkeys.overlay;
  help += c.panelLayout == "compacto" ? " es una columna estrecha del alto de su contenido."
                                      : " ocupa todo el alto de la pantalla, como el de NVIDIA.";
  help += " Arrástralo desde la cabecera o desde cualquier hueco para moverlo: se vuelve a abrir donde lo dejes.";
  Help(help.c_str());
  Gap(4);
  const bool moved = c.panelLateralX != defaults.panelLateralX || c.panelCompactX != defaults.panelCompactX ||
                     c.panelCompactY != defaults.panelCompactY;
  ImGui::BeginDisabled(!moved);
  if (GhostButton("panel-reset", "Devolverlo a su sitio", Px(210))) {
    // Lateral a la izquierda; compacto a la derecha y centrado.
    c.panelLateralX = defaults.panelLateralX;
    c.panelCompactX = defaults.panelCompactX;
    c.panelCompactY = defaults.panelCompactY;
    m.dirty = true;
  }
  ImGui::EndDisabled();
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
    Help(m.config.notifyUpdates && m.config.showNotifications
             ? "Spectre mira en GitHub si hay versión nueva al abrirse y cada 12 horas, y te avisa (Ajustes > "
               "Avisos). Nada se descarga sin pulsar Actualizar."
             : "Spectre solo se conecta a GitHub cuando pulsas el botón: no comprueba nada en segundo plano.");
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
             "instala solo. Descárgalo y ábrelo tú.");
      Gap(4);
      if (SolidButton("update-page", u.portable ? "Descargar el zip nuevo" : "Descargar el instalador", bw) &&
          !u.manualUrl.empty())
        a.openPath(u.manualUrl);
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

// Estado en Discord (Rich Presence): el interruptor y cómo va la conexión.
void DiscordSection(SettingsModel& m) {
  using S = DiscordStatus::State;
  const Palette& p = Colors();
  auto& c = m.config;
  Heading("DISCORD");
  m.dirty |= SwitchField("Mostrar en Discord lo que grabas", &c.discordPresence,
                         "Tu perfil de Discord enseña «Jugando a Spectre» con el juego que clipeas y los clips que "
                         "llevas guardados en la partida. Solo con Discord abierto en este PC.");
  if (!c.discordPresence) return;
  const DiscordStatus& d = m.discord;
  switch (d.state) {
  case S::Off:
    Help("Se conecta al pulsar Aplicar.");
    break;
  case S::NoAppId:
    UpdateStatusLine(p.warning, false, "Esta versión de Spectre no trae su aplicación de Discord: el estado no se puede enseñar.");
    break;
  case S::Waiting:
    UpdateStatusLine(p.textMuted, false, "Discord no está abierto: Spectre se conecta solo cuando lo abras.");
    break;
  case S::Connected:
    UpdateStatusLine(p.accent, false,
                     d.user.empty() ? std::string("Conectado a Discord.") : "Conectado a Discord como " + d.user + ".");
    Help("Se ve mientras juegas con el replay activo o grabas; en el escritorio no se enseña nada.");
    break;
  case S::Rejected:
    UpdateStatusLine(p.danger, false,
                     "Discord no aceptó la conexión" + (d.error.empty() ? std::string(".") : ": " + d.error));
    break;
  }
}

void PageGeneral(SettingsModel& m, const SettingsActions& a) {
  auto& c = m.config;
  AppearanceSection(m);
  PanelSection(m);
  DiscordSection(m);

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
  case SettingsPage::Voice: PageVoice(m); break;
  case SettingsPage::Storage: PageStorage(m, a); break;
  case SettingsPage::Notifications: PageNotifications(m, a); break;
  case SettingsPage::General: PageGeneral(m, a); break;
  case SettingsPage::Projects: PageProjects(a); break;
  default: PagePerformance(m); break;
  }
}

} // namespace spectre
