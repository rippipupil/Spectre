#include "app/VideoEdit.h"

#include "app/Transcode.h"
#include "common/Util.h"
#include "encode/Media.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include "app/CrashHandler.h"
#include "core/ThreadPolicy.h"
#include <objbase.h>
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>

namespace spectre {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
fs::path PathOf(const std::string& utf8) { return fs::path(Widen(utf8)); }
#else
fs::path PathOf(const std::string& utf8) { return fs::path(utf8); }
#endif

constexpr int kRate = 48000;             // audio de salida: 48 kHz estéreo
constexpr int kChunk = 1024;             // muestras por frame de AAC
constexpr double kMinSegment = 0.02;     // tramos más cortos que esto no salen
constexpr int kGifShortSide = 480;
constexpr double kGifFps = 15;
// «Al revés» guarda fotogramas decodificados para sacarlos del último al
// primero: como mucho esto de RAM cada vez (unos 60 a 1080p).
constexpr double kReverseBudgetBytes = 192.0 * 1024 * 1024;

// Números en las descripciones de filtros: siempre con punto decimal, sea cual
// sea el idioma del sistema.
std::string Num(double v) {
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, 4);
  std::string s(buf, r.ptr);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s == "-0" ? "0" : s;
}
std::string Int(int64_t v) { return std::to_string(v); }

int Even(double v) { return std::max(2, int(std::lround(v / 2.0)) * 2); }

struct FormatIn {
  AVFormatContext* p = nullptr;
  ~FormatIn() {
    if (p) avformat_close_input(&p);
  }
};
struct FormatOut {
  AVFormatContext* p = nullptr;
  ~FormatOut() {
    if (!p) return;
    if (p->pb && !(p->oformat->flags & AVFMT_NOFILE)) avio_closep(&p->pb);
    avformat_free_context(p);
  }
};
struct Codec {
  AVCodecContext* p = nullptr;
  Codec() = default;
  Codec(const Codec&) = delete;
  Codec& operator=(const Codec&) = delete;
  ~Codec() { avcodec_free_context(&p); }
};
struct Frame {
  AVFrame* p = av_frame_alloc();
  Frame() = default;
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  ~Frame() { av_frame_free(&p); }
};
struct Packet {
  AVPacket* p = av_packet_alloc();
  ~Packet() { av_packet_free(&p); }
};
struct Fifo {
  AVAudioFifo* p = nullptr;
  Fifo() = default;
  Fifo(const Fifo&) = delete;
  Fifo& operator=(const Fifo&) = delete;
  ~Fifo() {
    if (p) av_audio_fifo_free(p);
  }
};

// Grafo de filtros con sus entradas (buffer/abuffer) y una salida.
struct Graph {
  AVFilterGraph* g = nullptr;
  std::vector<AVFilterContext*> inputs; // en el orden de los nombres que se dieron
  AVFilterContext* sink = nullptr;
  Graph() = default;
  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;
  ~Graph() { Reset(); }
  void Reset() {
    avfilter_graph_free(&g);
    inputs.clear();
    sink = nullptr;
  }
};

struct GraphInput {
  std::string name;   // etiqueta en la descripción: [in], [ov0]...
  std::string filter; // "buffer" o "abuffer"
  std::string args;
};

bool BuildGraph(Graph* graph, const std::string& desc, const std::vector<GraphInput>& ins, bool audio, int threads,
                std::string* error) {
  graph->Reset();
  graph->g = avfilter_graph_alloc();
  if (!graph->g) return false;
  graph->g->nb_threads = threads;
  AVFilterInOut* outputs = nullptr; // salidas de nuestras entradas (lo que el texto lee)
  AVFilterInOut* last = nullptr;
  bool ok = true;
  for (size_t i = 0; ok && i < ins.size(); ++i) {
    AVFilterContext* ctx = nullptr;
    std::string name = "src" + std::to_string(i);
    if (avfilter_graph_create_filter(&ctx, avfilter_get_by_name(ins[i].filter.c_str()), name.c_str(),
                                     ins[i].args.c_str(), nullptr, graph->g) < 0) {
      ok = false;
      break;
    }
    graph->inputs.push_back(ctx);
    AVFilterInOut* io = avfilter_inout_alloc();
    io->name = av_strdup(ins[i].name.c_str());
    io->filter_ctx = ctx;
    io->pad_idx = 0;
    io->next = nullptr;
    if (last) last->next = io;
    else outputs = io;
    last = io;
  }
  if (ok && avfilter_graph_create_filter(&graph->sink, avfilter_get_by_name(audio ? "abuffersink" : "buffersink"),
                                         "sink", nullptr, nullptr, graph->g) < 0)
    ok = false;
  AVFilterInOut* inputs = nullptr;
  if (ok) {
    inputs = avfilter_inout_alloc();
    inputs->name = av_strdup("out");
    inputs->filter_ctx = graph->sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;
  }
  int err = 0;
  if (ok) err = avfilter_graph_parse_ptr(graph->g, desc.c_str(), &inputs, &outputs, nullptr);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (ok && err >= 0) err = avfilter_graph_config(graph->g, nullptr);
  if (!ok || err < 0) {
    Log("Editor: grafo no válido (%s): %s", ok ? AvError(err).c_str() : "entrada", desc.c_str());
    if (error) *error = "No se pudieron preparar los filtros del vídeo";
    graph->Reset();
    return false;
  }
  return true;
}

std::string VideoBufferArgs(int w, int h, int pixFmt, AVRational tb, AVRational sar) {
  if (sar.num <= 0 || sar.den <= 0) sar = {1, 1};
  return "video_size=" + Int(w) + "x" + Int(h) + ":pix_fmt=" + Int(pixFmt) + ":time_base=" + Int(tb.num) + "/" +
         Int(tb.den) + ":pixel_aspect=" + Int(sar.num) + "/" + Int(sar.den);
}

std::string AudioBufferArgs(AVRational tb, int rate, int fmt, const AVChannelLayout& layout) {
  char desc[128] = {};
  AVChannelLayout fallback{};
  const AVChannelLayout* l = &layout;
  if (layout.order == AV_CHANNEL_ORDER_UNSPEC || av_channel_layout_check(&layout) == 0) {
    av_channel_layout_default(&fallback, std::max(1, layout.nb_channels));
    l = &fallback;
  }
  av_channel_layout_describe(l, desc, sizeof(desc));
  av_channel_layout_uninit(&fallback);
  return "time_base=" + Int(tb.num) + "/" + Int(tb.den) + ":sample_rate=" + Int(rate) +
         ":sample_fmt=" + av_get_sample_fmt_name(AVSampleFormat(fmt)) + ":channel_layout=" + desc;
}

double DurationOf(const AVFormatContext* fmt, int video) {
  if (fmt->duration > 0) return double(fmt->duration) / AV_TIME_BASE;
  const AVStream* st = fmt->streams[video];
  if (st->duration > 0) return double(st->duration) * av_q2d(st->time_base);
  return 0;
}

std::string TitleOf(const AVStream* st) {
  if (auto* t = av_dict_get(st->metadata, "title", nullptr, 0)) return t->value;
  if (auto* h = av_dict_get(st->metadata, "handler_name", nullptr, 0)) return h->value;
  return {};
}

bool IsCamTrack(const AVStream* st) {
  int x, y, w, h;
  return st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ParseCamTrackTitle(TitleOf(st), &x, &y, &w, &h);
}

// La pista del vídeo: la que elige FFmpeg (la marcada por defecto), nunca la
// del fondo de la cámara.
int MainVideoStream(AVFormatContext* fmt) {
  int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (v < 0 || !IsCamTrack(fmt->streams[v])) return v;
  for (unsigned i = 0; i < fmt->nb_streams; ++i)
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !IsCamTrack(fmt->streams[i])) return int(i);
  return -1;
}

// --------------------------------------------------------------- color

struct ColorParams {
  double contrast = 1, bright = 0, sat = 1, du = 0, dv = 0;
  bool Any() const {
    return std::fabs(contrast - 1) > 1e-3 || std::fabs(bright) > 1e-3 || std::fabs(sat - 1) > 1e-3 ||
           std::fabs(du) > 1e-3 || std::fabs(dv) > 1e-3;
  }
};

// Brillo, contraste, saturación y los filtros rápidos en una sola tabla por
// componente (lutyuv): el coste es una búsqueda por píxel. Los tonos (sepia,
// cálido, frío) desplazan U y V, como lo haría un tinte.
ColorParams ColorOf(const EditSettings& s) {
  ColorParams c;
  c.contrast = 1.0 + std::clamp(s.contrast, -100, 100) / 200.0;
  c.bright = std::clamp(s.brightness, -100, 100) * 0.5;
  c.sat = 1.0 + std::clamp(s.saturation, -100, 100) / 100.0;
  switch (s.look) {
  case EditLook::Vivid:
    c.sat *= 1.35;
    c.contrast *= 1.08;
    break;
  case EditLook::Mono: c.sat = 0; break;
  case EditLook::Sepia:
    c.sat = 0;
    c.du = -15;
    c.dv = 13;
    break;
  case EditLook::Warm:
    c.sat *= 1.05;
    c.du = -7;
    c.dv = 9;
    break;
  case EditLook::Cold:
    c.du = 9;
    c.dv = -6;
    break;
  case EditLook::Faded:
    c.contrast *= 0.82;
    c.bright += 12;
    c.sat *= 0.7;
    break;
  case EditLook::None: break;
  }
  return c;
}

std::string LutFilter(const ColorParams& c) {
  auto chroma = [&](double shift) {
    return "'clip((val-128)*" + Num(c.sat) + "+128+" + Num(shift) + ",minval,maxval)'";
  };
  return "lutyuv=y='clip((val-128)*" + Num(c.contrast) + "+128+" + Num(c.bright) + ",minval,maxval)':u=" +
         chroma(c.du) + ":v=" + chroma(c.dv);
}

// Cadena de atempo para cualquier factor (cada atempo admite de 0,5 a 100).
std::string TempoFilters(double speed) {
  std::string out;
  double f = speed;
  while (f < 0.5 - 1e-9) {
    out += ",atempo=0.5";
    f /= 0.5;
  }
  if (std::fabs(f - 1.0) > 1e-4) out += ",atempo=" + Num(f);
  return out;
}

double AspectOf(EditAspect a, double source) {
  switch (a) {
  case EditAspect::Wide: return 16.0 / 9.0;
  case EditAspect::Vertical: return 9.0 / 16.0;
  case EditAspect::Square: return 1.0;
  case EditAspect::Portrait: return 4.0 / 5.0;
  case EditAspect::Original: break;
  }
  return source;
}

} // namespace

// --------------------------------------------------------------- análisis

bool ProbeEditSource(const std::string& path, EditSource* s, std::string* error) {
  *s = EditSource{};
  FormatIn in;
  int err = avformat_open_input(&in.p, path.c_str(), nullptr, nullptr);
  if (err < 0) {
    if (error) *error = "No se pudo abrir el vídeo: " + AvError(err);
    return false;
  }
  avformat_find_stream_info(in.p, nullptr);
  int v = MainVideoStream(in.p);
  if (v < 0) {
    if (error) *error = "El archivo no tiene vídeo";
    return false;
  }
  AVStream* st = in.p->streams[v];
  s->width = st->codecpar->width;
  s->height = st->codecpar->height;
  if (s->width < 16 || s->height < 16) {
    if (error) *error = "El vídeo no tiene un tamaño válido";
    return false;
  }
  AVRational rate = av_guess_frame_rate(in.p, st, nullptr);
  if (rate.num <= 0 || rate.den <= 0 || av_q2d(rate) > 1000) rate = {60, 1};
  s->fpsNum = rate.num;
  s->fpsDen = rate.den;
  s->fps = av_q2d(rate);
  s->duration = DurationOf(in.p, v);
  if (s->duration <= 0) {
    if (error) *error = "No se pudo saber cuánto dura el vídeo";
    return false;
  }
  int64_t audioBps = 0;
  for (unsigned i = 0; i < in.p->nb_streams; ++i) {
    const AVStream* a = in.p->streams[i];
    if (a->codecpar->codec_type != AVMEDIA_TYPE_AUDIO || !avcodec_find_decoder(a->codecpar->codec_id)) continue;
    if (s->mixTrack < 0) {
      s->mixTrack = int(i);
      audioBps = a->codecpar->bit_rate;
    }
    // Pistas que escribe AudioPipeline con «pistas separadas».
    const std::string title = TitleOf(a);
    if (title.rfind("Sistema", 0) == 0) s->gameTrack = int(i);
    else if (title.rfind("Micr", 0) == 0) s->micTrack = int(i);
  }
  s->audio = s->mixTrack >= 0;
  // Fondo de la cámara: si la cámara cambió de sitio mientras se grababa hay
  // varias pistas; vale la última (la del final del clip).
  int64_t camBps = 0;
  for (unsigned i = 0; i < in.p->nb_streams; ++i) {
    const AVStream* c = in.p->streams[i];
    int x, y, w, h;
    if (int(i) == v || c->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || !avcodec_find_decoder(c->codecpar->codec_id) ||
        !ParseCamTrackTitle(TitleOf(c), &x, &y, &w, &h))
      continue;
    x &= ~1;
    y &= ~1;
    w &= ~1;
    h &= ~1;
    if (x < 0 || y < 0 || w < 16 || h < 16 || x + w > s->width || y + h > s->height) continue;
    s->camTrack = int(i);
    s->camX = x;
    s->camY = y;
    s->camW = w;
    s->camH = h;
    camBps = std::max<int64_t>(0, c->codecpar->bit_rate);
  }
  s->videoBps = st->codecpar->bit_rate;
  if (s->videoBps <= 0) {
    int64_t size = avio_size(in.p->pb);
    if (size > 0)
      s->videoBps = std::max<int64_t>(0, int64_t(double(size) * 8.0 / s->duration) - audioBps - camBps);
  }
  return true;
}

std::string CamTrackTitle(int x, int y, int w, int h) {
  char buf[96];
  snprintf(buf, sizeof(buf), "Fondo de la cámara %d,%d,%d,%d", x, y, w, h);
  return buf;
}

bool ParseCamTrackTitle(const std::string& title, int* x, int* y, int* w, int* h) {
  static const std::string kPrefix = "Fondo de la cámara ";
  if (title.compare(0, kPrefix.size(), kPrefix) != 0) return false;
  return sscanf(title.c_str() + kPrefix.size(), "%d,%d,%d,%d", x, y, w, h) == 4;
}

// --------------------------------------------------------------- tramos

std::vector<EditSegment> BuildSegments(const EditSettings& s, const EditSource& src) {
  using Kind = EditSegment::Kind;
  const double d = src.duration;
  double a = std::clamp(s.start, 0.0, d);
  double b = s.end > a ? std::min(s.end, d) : d;
  std::vector<EditSegment> parts;
  auto addRange = [&](double from, double to) {
    if (to - from >= kMinSegment) parts.push_back({Kind::Forward, from, to, 1.0, 0});
  };
  if (s.cut && s.cutEnd > s.cutStart && s.cutEnd > a && s.cutStart < b) {
    addRange(a, std::clamp(s.cutStart, a, b));
    addRange(std::clamp(s.cutEnd, a, b), b);
  } else {
    addRange(a, b);
  }
  const double speed = std::clamp(s.speed, 0.25, 3.0);
  // Cámara lenta en un tramo: los trozos que caen dentro van más despacio.
  std::vector<EditSegment> timed;
  for (const auto& p : parts) {
    if (!s.slow || s.slowEnd <= s.slowStart || s.slowEnd <= p.from || s.slowStart >= p.to) {
      timed.push_back({Kind::Forward, p.from, p.to, speed, 0});
      continue;
    }
    const double x0 = std::max(p.from, s.slowStart), x1 = std::min(p.to, s.slowEnd);
    const double slow = speed * std::clamp(s.slowSpeed, 0.25, 1.0);
    if (x0 - p.from >= kMinSegment) timed.push_back({Kind::Forward, p.from, x0, speed, 0});
    if (x1 - x0 >= kMinSegment) timed.push_back({Kind::Forward, x0, x1, slow, 0});
    if (p.to - x1 >= kMinSegment) timed.push_back({Kind::Forward, x1, p.to, speed, 0});
  }
  // Imagen congelada: se parte el trozo en ese momento y se mete la pausa.
  if (s.freeze && s.freezeSeconds > 0) {
    for (size_t i = 0; i < timed.size(); ++i) {
      EditSegment seg = timed[i];
      if (s.freezeAt < seg.from - 1e-6 || s.freezeAt >= seg.to) continue;
      std::vector<EditSegment> repl;
      if (s.freezeAt - seg.from >= kMinSegment) repl.push_back({Kind::Forward, seg.from, s.freezeAt, seg.speed, 0});
      const double at = std::max(seg.from, s.freezeAt);
      repl.push_back({Kind::Freeze, at, at, 1.0, std::clamp(s.freezeSeconds, 0.5, 10.0)});
      if (seg.to - at >= kMinSegment) repl.push_back({Kind::Forward, at, seg.to, seg.speed, 0});
      timed.erase(timed.begin() + std::ptrdiff_t(i));
      timed.insert(timed.begin() + std::ptrdiff_t(i), repl.begin(), repl.end());
      break;
    }
  }
  auto reversed = [](std::vector<EditSegment> list, bool keepFreeze) {
    std::vector<EditSegment> out;
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
      EditSegment seg = *it;
      if (seg.kind == Kind::Freeze && !keepFreeze) continue;
      if (seg.kind == Kind::Forward) seg.kind = Kind::Reverse;
      out.push_back(seg);
    }
    return out;
  };
  std::vector<EditSegment> once;
  if (s.direction == EditDirection::Reverse) {
    once = reversed(timed, true);
  } else if (s.direction == EditDirection::Boomerang) {
    once = timed;
    auto back = reversed(timed, false);
    once.insert(once.end(), back.begin(), back.end());
  } else {
    once = timed;
  }
  std::vector<EditSegment> out;
  const int repeat = std::clamp(s.repeat, 1, 5);
  for (int i = 0; i < repeat; ++i) out.insert(out.end(), once.begin(), once.end());
  return out;
}

double SegmentsLength(const std::vector<EditSegment>& segments) {
  double total = 0;
  for (const auto& s : segments) total += s.Length();
  return total;
}

double ReversedSeconds(const EditSettings& settings, const EditSource& source) {
  if (settings.direction == EditDirection::Forward) return 0;
  EditSettings once = settings;
  once.repeat = 1;
  double total = 0;
  for (const auto& seg : BuildSegments(once, source))
    if (seg.kind == EditSegment::Kind::Reverse) total += seg.to - seg.from;
  return total;
}

double OutputTimeOf(const std::vector<EditSegment>& segments, double t) {
  double at = 0;
  for (const auto& s : segments) {
    switch (s.kind) {
    case EditSegment::Kind::Forward:
      if (t >= s.from && t < s.to) return at + (t - s.from) / s.speed;
      break;
    case EditSegment::Kind::Reverse:
      if (t >= s.from && t <= s.to) return at + (s.to - t) / s.speed;
      break;
    case EditSegment::Kind::Freeze:
      if (std::fabs(t - s.from) < 0.05) return at;
      break;
    }
    at += s.Length();
  }
  return -1;
}

// --------------------------------------------------------------- imagen

EditGeometry ComputeGeometry(const EditSettings& s, const EditSource& src) {
  EditGeometry g;
  const int rot = ((s.rotate % 360) + 360) % 360;
  const bool swap = rot == 90 || rot == 270;
  g.rotatedW = swap ? src.height : src.width;
  g.rotatedH = swap ? src.width : src.height;
  const double rw = g.rotatedW, rh = g.rotatedH;
  const double srcAspect = rw / rh;
  const double aspect = AspectOf(s.aspect, srcAspect);
  const double zoom = std::clamp(double(s.zoom), 1.0, 4.0);
  const bool fitInside = s.aspect != EditAspect::Original && s.fit != EditFit::Crop &&
                         std::fabs(aspect - srcAspect) > 0.01;
  const double panX = std::clamp(double(s.panX), 0.0, 1.0), panY = std::clamp(double(s.panY), 0.0, 1.0);
  auto window = [&](double ww, double wh) {
    g.cropW = std::min(Even(ww), g.rotatedW & ~1);
    g.cropH = std::min(Even(wh), g.rotatedH & ~1);
    g.cropX = int(std::lround(panX * (rw - g.cropW))) & ~1;
    g.cropY = int(std::lround(panY * (rh - g.cropH))) & ~1;
    g.cropX = std::clamp(g.cropX, 0, std::max(0, g.rotatedW - g.cropW));
    g.cropY = std::clamp(g.cropY, 0, std::max(0, g.rotatedH - g.cropH));
  };
  if (!fitInside) {
    // La ventana más grande con ese formato, más pequeña con el zoom.
    double ww = rw, wh = rh;
    if (srcAspect > aspect) ww = rh * aspect;
    else wh = rw / aspect;
    window(ww / zoom, wh / zoom);
    g.canvasW = g.fgW = g.cropW;
    g.canvasH = g.fgH = g.cropH;
  } else {
    // Todo el vídeo dentro de un lienzo con otro formato: el lado corto del
    // lienzo es el del original (de 1920x1080 a 9:16 sale 1080x1920, con el
    // vídeo a 1080x608 en medio).
    window(rw / zoom, rh / zoom);
    const double shortSide = std::min(rw, rh);
    double cw = aspect >= 1 ? shortSide * aspect : shortSide;
    double ch = aspect >= 1 ? shortSide : shortSide / aspect;
    g.canvasW = Even(cw);
    g.canvasH = Even(ch);
    const double fw = std::min(double(g.canvasW), g.canvasH * srcAspect);
    g.fgW = std::min(Even(fw), g.canvasW);
    g.fgH = std::min(Even(fw / srcAspect), g.canvasH);
  }
  // Resolución pedida (lado corto), sin pasar de 4096 en el lado largo.
  double outW = g.canvasW, outH = g.canvasH;
  int shortTarget = s.shortSide;
  if (s.format == EditFormat::Gif)
    shortTarget = std::min(shortTarget > 0 ? shortTarget : kGifShortSide, kGifShortSide);
  // Solo hacia abajo: una resolución elegida antes del 9:16 o del zoom (720
  // con un lado corto que ahora es de 608) no amplía el vídeo.
  if (shortTarget > 0 && shortTarget < std::min(outW, outH)) {
    const double k = double(shortTarget) / std::min(outW, outH);
    outW *= k;
    outH *= k;
  }
  const double longSide = std::max(outW, outH);
  if (longSide > 4096) {
    outW *= 4096 / longSide;
    outH *= 4096 / longSide;
  }
  g.outW = Even(outW);
  g.outH = Even(outH);
  // Fps: los del original divididos por un entero (144 a 48, no a 60, que va
  // a saltitos), como en Exportar para Discord.
  AVRational rate{src.fpsNum > 0 ? src.fpsNum : 60, src.fpsDen > 0 ? src.fpsDen : 1};
  double want = s.format == EditFormat::Gif ? kGifFps : s.fps > 0 ? double(s.fps) : 0.0;
  if (want > 0 && av_q2d(rate) > want * 1.01) {
    const int step = std::max(1, int(std::ceil(av_q2d(rate) / want - 0.01)));
    rate = av_mul_q(rate, AVRational{1, step});
  }
  av_reduce(&rate.num, &rate.den, rate.num, rate.den, 1 << 30);
  g.fpsNum = rate.num;
  g.fpsDen = rate.den;
  g.fps = av_q2d(rate);
  return g;
}

bool EditTouchesPicture(const EditSettings& s) {
  return s.aspect != EditAspect::Original || s.zoom > 1.001f || s.rotate % 360 != 0 || s.flipH || s.flipV ||
         s.brightness != 0 || s.contrast != 0 || s.saturation != 0 || s.look != EditLook::None || s.sharpen ||
         s.vignette || s.fadeIn || s.fadeOut || !s.text.empty() || !s.watermark.empty() || s.blur || s.camEdit;
}

EditCamBox ComputeCamBox(const EditSettings& s, const EditSource& src, const EditGeometry& g) {
  EditCamBox b;
  if (!src.Webcam() || !s.camEdit || s.camHide || g.canvasW < 32 || g.canvasH < 32) return b;
  const double aspect = s.camShape == EditCamShape::Original ? double(src.camW) / double(src.camH) : 1.0;
  double w = std::clamp(double(s.camW), 0.05, 1.0) * g.canvasW;
  double h = w / aspect;
  if (h > g.canvasH) {
    h = g.canvasH;
    w = h * aspect;
  }
  b.w = std::clamp(Even(w), 16, g.canvasW & ~1);
  b.h = std::clamp(Even(h), 16, g.canvasH & ~1);
  b.x = std::clamp(int(std::lround(double(s.camX) * g.canvasW)) & ~1, 0, std::max(0, g.canvasW - b.w));
  b.y = std::clamp(int(std::lround(double(s.camY) * g.canvasH)) & ~1, 0, std::max(0, g.canvasH - b.h));
  return b;
}

void DefaultCamPlacement(EditSettings* s, const EditSource& src) {
  if (!src.Webcam() || src.width <= 0 || src.height <= 0) return;
  EditSettings probe = *s;
  probe.camEdit = true;
  probe.camHide = false;
  const EditGeometry g = ComputeGeometry(probe, src);
  const int rot = ((s->rotate % 360) + 360) % 360;
  if (rot == 0 && !s->flipH && !s->flipV && g.canvasW == g.cropW && g.canvasH == g.cropH && g.cropW > 0 &&
      g.cropH > 0 && src.camX >= g.cropX && src.camY >= g.cropY && src.camX + src.camW <= g.cropX + g.cropW &&
      src.camY + src.camH <= g.cropY + g.cropH) {
    // Se ve entera: donde estaba, con el mismo tamaño.
    s->camX = float(src.camX - g.cropX) / float(g.cropW);
    s->camY = float(src.camY - g.cropY) / float(g.cropH);
    s->camW = float(src.camW) / float(g.cropW);
    return;
  }
  // Si no, abajo a la derecha (en vertical, más grande: el lienzo es estrecho).
  const bool tall = g.canvasH > g.canvasW;
  s->camW = tall ? 0.42f : 0.25f;
  const double aspect = s->camShape == EditCamShape::Original ? double(src.camW) / double(src.camH) : 1.0;
  const double camH = g.canvasW > 0 ? double(s->camW) * g.canvasW / aspect / std::max(1, g.canvasH) : 0.2;
  const float margin = 0.03f;
  s->camX = std::max(0.0f, 1.0f - s->camW - margin);
  s->camY = float(std::max(0.0, 1.0 - camH - margin * double(g.canvasW) / std::max(1, g.canvasH)));
}

namespace {

// Lo que no quepa en la imagen se recorta (filas enteras: el búfer sigue siendo
// de width x height).
void FitOverlay(EditOverlay& o, int outW, int outH) {
  const int w = std::min(o.width, outW), h = std::min(o.height, outH);
  if (w == o.width && h == o.height) return;
  const int x0 = (o.width - w) / 2;
  std::vector<uint8_t> cropped(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; ++y)
    memcpy(cropped.data() + size_t(y) * size_t(w) * 4, o.rgba.data() + (size_t(y) * size_t(o.width) + size_t(x0)) * 4,
           size_t(w) * 4);
  o.rgba = std::move(cropped);
  o.width = w;
  o.height = h;
}

} // namespace

std::vector<EditOverlay> BuildOverlays(const EditSettings& s, int outW, int outH, const TextRasterizer& raster) {
  std::vector<EditOverlay> out;
  if (!raster) return out;
  const int shortSide = std::min(outW, outH);
  if (!s.text.empty()) {
    static const double kSizes[] = {0.05, 0.07, 0.095};
    const int px = std::max(12, int(shortSide * kSizes[std::clamp(s.textSize, 0, 2)]));
    EditOverlay o;
    if (raster(s.text, px, int(outW * 0.9), s.textBox, &o) && o.width > 0 && o.height > 0 &&
        o.rgba.size() >= size_t(o.width) * size_t(o.height) * 4) {
      FitOverlay(o, outW, outH);
      o.x = (outW - o.width) / 2;
      const int margin = int(outH * 0.07);
      o.y = s.textPlace == EditPlace::Top ? margin
            : s.textPlace == EditPlace::Middle ? (outH - o.height) / 2
                                               : outH - o.height - margin;
      o.y = std::clamp(o.y, 0, outH - o.height);
      o.until = s.textSeconds > 0 ? s.textSeconds : 0;
      out.push_back(std::move(o));
    }
  }
  if (!s.watermark.empty()) {
    const int px = std::max(10, int(shortSide * 0.034));
    EditOverlay o;
    if (raster(s.watermark, px, int(outW * 0.5), false, &o) && o.width > 0 && o.height > 0 &&
        o.rgba.size() >= size_t(o.width) * size_t(o.height) * 4) {
      FitOverlay(o, outW, outH);
      // Un poco transparente: que se lea sin tapar la jugada.
      for (size_t i = 3; i < o.rgba.size(); i += 4) o.rgba[i] = uint8_t(o.rgba[i] * 0.85f);
      const int m = int(shortSide * 0.03);
      const bool right = s.watermarkCorner == EditCorner::TopRight || s.watermarkCorner == EditCorner::BottomRight;
      const bool bottom = s.watermarkCorner == EditCorner::BottomLeft || s.watermarkCorner == EditCorner::BottomRight;
      o.x = right ? outW - o.width - m : m;
      o.y = bottom ? outH - o.height - m : m;
      o.x = std::clamp(o.x, 0, outW - o.width);
      o.y = std::clamp(o.y, 0, outH - o.height);
      out.push_back(std::move(o));
    }
  }
  return out;
}

// --------------------------------------------------------------- fondo de la cámara

namespace {

// Formatos en los que se puede copiar y recortar por planos: 8 bits por
// componente, sin paleta ni GPU (yuv420p de los decoders, bgr24 y bgra de la
// vista previa).
bool PlainFormat(int format) {
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(format));
  return d && d->nb_components >= 3 && d->comp[0].depth == 8 &&
         !(d->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM));
}

// Punteros de cada plano de f en el píxel (x, y) (pares, para el croma).
void PlanesAt(const AVFrame* f, int x, int y, const uint8_t* out[4]) {
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
  int steps[4] = {};
  av_image_fill_max_pixsteps(steps, nullptr, d);
  for (int p = 0; p < 4; ++p) {
    out[p] = nullptr;
    if (!f->data[p]) continue;
    const bool chroma = (p == 1 || p == 2) && !(d->flags & AV_PIX_FMT_FLAG_RGB);
    const int sx = chroma ? d->log2_chroma_w : 0, sy = chroma ? d->log2_chroma_h : 0;
    out[p] = f->data[p] + size_t(y >> sy) * size_t(f->linesize[p]) + size_t(x >> sx) * size_t(steps[p]);
  }
}

// Copia src entero dentro de dst en (x, y). Mismo formato; x e y pares.
void Blit(AVFrame* dst, const AVFrame* src, int x, int y) {
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(dst->format));
  int steps[4] = {};
  av_image_fill_max_pixsteps(steps, nullptr, d);
  const int w = std::min(src->width, dst->width - x), h = std::min(src->height, dst->height - y);
  if (w <= 0 || h <= 0) return;
  const uint8_t* at[4];
  PlanesAt(dst, x, y, at);
  for (int p = 0; p < 4 && dst->data[p] && src->data[p]; ++p) {
    const bool chroma = (p == 1 || p == 2) && !(d->flags & AV_PIX_FMT_FLAG_RGB);
    const int sx = chroma ? d->log2_chroma_w : 0, sy = chroma ? d->log2_chroma_h : 0;
    av_image_copy_plane(const_cast<uint8_t*>(at[p]), dst->linesize[p], src->data[p], src->linesize[p],
                        AV_CEIL_RSHIFT(w, sx) * steps[p], AV_CEIL_RSHIFT(h, sy));
  }
}

// Escala la región (x, y, w, h) de src a out (ya con formato y tamaño).
bool ScaleRegion(SwsContext** sws, const AVFrame* src, int x, int y, int w, int h, AVFrame* out) {
  *sws = sws_getCachedContext(*sws, w, h, AVPixelFormat(src->format), out->width, out->height,
                              AVPixelFormat(out->format), SWS_BICUBIC, nullptr, nullptr, nullptr);
  if (!*sws) return false;
  const uint8_t* planes[4];
  PlanesAt(src, x, y, planes);
  return sws_scale(*sws, planes, src->linesize, 0, h, out->data, out->linesize) > 0;
}

} // namespace

struct CamBackgroundReader::Impl {
  FormatIn fmt;
  Codec dec;
  int stream = -1;
  double start = 0, half = 1.0 / 120;
  Frame cur, next;
  bool haveCur = false, haveNext = false, positioned = false, eof = false;
  double curTime = 0, nextTime = 0;
  Packet pkt;
  SwsContext* sws = nullptr;
  ~Impl() { sws_freeContext(sws); }

  double TimeOf(const AVFrame* f) const {
    const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    return ts == AV_NOPTS_VALUE ? NAN : double(ts) * av_q2d(fmt.p->streams[stream]->time_base) - start;
  }

  void Seek(double t) {
    const AVRational tb = fmt.p->streams[stream]->time_base;
    const int64_t ts = av_rescale_q(std::llround((std::max(0.0, t) + start) * AV_TIME_BASE), AV_TIME_BASE_Q, tb);
    av_seek_frame(fmt.p, stream, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec.p);
    av_frame_unref(cur.p);
    av_frame_unref(next.p);
    haveCur = haveNext = eof = false;
    positioned = true;
  }

  // Decodifica hasta tener el último fotograma que empieza antes de t (cur)
  // y el primero de después (next).
  void Advance(double t) {
    for (int guard = 0; guard < 100000; ++guard) {
      if (haveNext && nextTime > t + half) return;
      if (haveNext) {
        std::swap(cur.p, next.p);
        av_frame_unref(next.p);
        curTime = nextTime;
        haveCur = true;
        haveNext = false;
        continue;
      }
      if (eof) return;
      const int r = avcodec_receive_frame(dec.p, next.p);
      if (r == 0) {
        nextTime = TimeOf(next.p);
        if (std::isnan(nextTime)) nextTime = haveCur ? curTime + 2 * half : t;
        haveNext = true;
        continue;
      }
      if (r != AVERROR(EAGAIN)) {
        eof = true;
        continue;
      }
      for (;;) {
        if (av_read_frame(fmt.p, pkt.p) < 0) {
          avcodec_send_packet(dec.p, nullptr);
          break;
        }
        const bool mine = pkt.p->stream_index == stream;
        if (mine) avcodec_send_packet(dec.p, pkt.p); // uno dañado se salta
        av_packet_unref(pkt.p);
        if (mine) break;
      }
    }
  }

  const AVFrame* At(double t) {
    if (!dec.p) return nullptr;
    if (!positioned || (haveCur && t < curTime - half) || (!haveCur && haveNext && t < nextTime - half && t > 0.5) ||
        (haveCur && t > curTime + 3.0))
      Seek(t);
    Advance(t);
    if (haveCur) return cur.p;
    return haveNext ? next.p : nullptr;
  }
};

CamBackgroundReader::CamBackgroundReader() = default;
CamBackgroundReader::~CamBackgroundReader() = default;

bool CamBackgroundReader::Open(const std::string& path, int track) {
  Close();
  auto impl = std::make_unique<Impl>();
  if (avformat_open_input(&impl->fmt.p, path.c_str(), nullptr, nullptr) < 0) return false;
  avformat_find_stream_info(impl->fmt.p, nullptr);
  if (track < 0 || track >= int(impl->fmt.p->nb_streams)) return false;
  AVStream* st = impl->fmt.p->streams[track];
  if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) return false;
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  impl->dec.p = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!impl->dec.p || avcodec_parameters_to_context(impl->dec.p, st->codecpar) < 0) return false;
  impl->dec.p->thread_count = 1; // pequeña: un hilo basta y no hay retraso de fotogramas
  impl->dec.p->pkt_timebase = st->time_base;
  if (avcodec_open2(impl->dec.p, codec, nullptr) < 0) {
    avcodec_free_context(&impl->dec.p);
    return false;
  }
  // Solo se leen los paquetes de esta pista.
  for (unsigned i = 0; i < impl->fmt.p->nb_streams; ++i)
    impl->fmt.p->streams[i]->discard = int(i) == track ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  impl->stream = track;
  impl->start = impl->fmt.p->start_time != AV_NOPTS_VALUE ? double(impl->fmt.p->start_time) / AV_TIME_BASE : 0.0;
  const AVRational rate = av_guess_frame_rate(impl->fmt.p, st, nullptr);
  if (rate.num > 0 && rate.den > 0) impl->half = 0.5 / std::max(1.0, av_q2d(rate));
  impl_ = std::move(impl);
  path_ = path;
  return true;
}

void CamBackgroundReader::Close() {
  impl_.reset();
  path_.clear();
}

bool CamBackgroundReader::IsOpen() const { return impl_ && impl_->dec.p; }

const AVFrame* CamBackgroundReader::At(double t) { return impl_ ? impl_->At(t) : nullptr; }

bool CamBackgroundReader::ReadBgra(double t, std::vector<uint8_t>* bgra, int* width, int* height) {
  const AVFrame* f = At(t);
  if (!f || !PlainFormat(f->format) || f->width < 2 || f->height < 2) return false;
  impl_->sws = sws_getCachedContext(impl_->sws, f->width, f->height, AVPixelFormat(f->format), f->width, f->height,
                                    AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
  if (!impl_->sws) return false;
  // Lo graba Spectre: BT.709 de rango limitado, como lo enseña el reproductor.
  const int* coefs = sws_getCoefficients(SWS_CS_ITU709);
  sws_setColorspaceDetails(impl_->sws, coefs, f->color_range == AVCOL_RANGE_JPEG ? 1 : 0, coefs, 1, 0, 1 << 16,
                           1 << 16);
  bgra->resize(size_t(f->width) * size_t(f->height) * 4);
  uint8_t* dst[4] = {bgra->data(), nullptr, nullptr, nullptr};
  int lines[4] = {f->width * 4, 0, 0, 0};
  if (sws_scale(impl_->sws, f->data, f->linesize, 0, f->height, dst, lines) <= 0) return false;
  *width = f->width;
  *height = f->height;
  return true;
}

namespace {

// Separa la cámara de cada fotograma: le pone encima lo que había del juego
// debajo (la pista aparte) y devuelve la cámara recortada, a su tamaño final,
// con su forma y su borde, en YUVA 4:2:0 (sin pasar por RGB: los colores no
// cambian). El grafo la pone en su sitio con overlay.
class CamCompositor {
public:
  CamCompositor() = default;
  CamCompositor(const CamCompositor&) = delete;
  CamCompositor& operator=(const CamCompositor&) = delete;
  ~CamCompositor() {
    sws_freeContext(camSws_);
    sws_freeContext(bgSws_);
  }

  // src con el tamaño de los fotogramas que llegarán (y el recuadro de la
  // cámara en esos píxeles).
  void Setup(const EditSettings& s, const EditSource& src, const EditGeometry& g) {
    s_ = s;
    src_ = src;
    active_ = src.Webcam() && s.camEdit;
    box_ = ComputeCamBox(s, src, g);
    if (box_.w <= 0) return;
    // Forma (alfa) y borde (cuánto de cada píxel es borde), a tamaño final.
    const int w = box_.w, h = box_.h;
    alpha_.assign(size_t(w) * size_t(h), 255);
    edge_.assign(size_t(w) * size_t(h), 0);
    const double side = std::min(w, h);
    const double thick = s.camBorder <= 0 ? 0.0 : s.camBorder == 1 ? std::max(2.0, side * 0.025) : std::max(4.0, side * 0.06);
    const bool round = s.camShape == EditCamShape::Circle;
    const double cx = w * 0.5, cy = h * 0.5, r = side * 0.5;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t i = size_t(y) * size_t(w) + size_t(x);
        if (round) {
          const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
          alpha_[i] = uint8_t(std::lround(std::clamp(r - d + 0.5, 0.0, 1.0) * 255));
          if (thick > 0) edge_[i] = uint8_t(std::lround(std::clamp(d - (r - thick) + 0.5, 0.0, 1.0) * 255));
        } else if (thick > 0) {
          const double d = std::min({x + 0.5, w - x - 0.5, y + 0.5, h - y - 0.5});
          edge_[i] = uint8_t(std::lround(std::clamp(thick - d + 0.5, 0.0, 1.0) * 255));
        }
      }
    // Color del borde en YUV BT.709 de rango limitado, como el vídeo.
    const double R = ((s.camBorderColor >> 16) & 255) / 255.0, G = ((s.camBorderColor >> 8) & 255) / 255.0,
                 B = (s.camBorderColor & 255) / 255.0;
    const double Y = 0.2126 * R + 0.7152 * G + 0.0722 * B;
    borderY_ = uint8_t(std::lround(16 + 219 * Y));
    borderU_ = uint8_t(std::clamp(std::lround(128 + 224 * (B - Y) / 1.8556), 16L, 240L));
    borderV_ = uint8_t(std::clamp(std::lround(128 + 224 * (R - Y) / 1.5748), 16L, 240L));
  }

  bool Active() const { return active_; }
  const EditCamBox& Box() const { return box_; }

  // frame: fotograma del original, ya escribible. background: el de la pista
  // aparte en ese momento (o nullptr). cam: la cámara para el grafo, si va
  // encima (si no, se queda vacío).
  bool Process(AVFrame* frame, const AVFrame* background, AVFrame* cam) {
    if (!active_ || !PlainFormat(frame->format)) return false;
    // El recuadro, en píxeles de este fotograma.
    const double kx = double(frame->width) / std::max(1, src_.width), ky = double(frame->height) / std::max(1, src_.height);
    int rx = int(std::lround(src_.camX * kx)) & ~1, ry = int(std::lround(src_.camY * ky)) & ~1;
    int rw = int(std::lround(src_.camW * kx)) & ~1, rh = int(std::lround(src_.camH * ky)) & ~1;
    rx = std::clamp(rx, 0, std::max(0, frame->width - 2));
    ry = std::clamp(ry, 0, std::max(0, frame->height - 2));
    rw = std::min(rw, (frame->width - rx) & ~1);
    rh = std::min(rh, (frame->height - ry) & ~1);
    if (rw < 8 || rh < 8) return false;

    // 1. La cámara, antes de taparla.
    if (box_.w > 0) {
      int cx = rx, cy = ry, cw = rw, ch = rh;
      if (s_.camShape != EditCamShape::Original) {
        const int sq = std::min(rw, rh) & ~1;
        cx = (rx + (rw - sq) / 2) & ~1;
        cy = (ry + (rh - sq) / 2) & ~1;
        cw = ch = sq;
      }
      cam->format = AV_PIX_FMT_YUVA420P;
      cam->width = box_.w;
      cam->height = box_.h;
      if (av_frame_get_buffer(cam, 0) < 0 || !ScaleRegion(&camSws_, frame, cx, cy, cw, ch, cam)) return false;
      Finish(cam);
    }
    // 2. Debajo, el fondo grabado aparte.
    if (background && PlainFormat(background->format) && background->width >= 2 && background->height >= 2) {
      const AVFrame* patch = background;
      if (background->format != frame->format || background->width != rw || background->height != rh) {
        av_frame_unref(bg_.p);
        bg_.p->format = frame->format;
        bg_.p->width = rw;
        bg_.p->height = rh;
        if (av_frame_get_buffer(bg_.p, 0) < 0 ||
            !ScaleRegion(&bgSws_, background, 0, 0, background->width & ~1, background->height & ~1, bg_.p))
          return true;
        patch = bg_.p;
      }
      Blit(frame, patch, rx, ry);
    }
    return true;
  }

private:
  // Espejo, borde y forma sobre la cámara ya escalada.
  void Finish(AVFrame* cam) {
    const int w = cam->width, h = cam->height;
    if (s_.camMirror) {
      for (int p = 0; p < 4; ++p) {
        const int pw = p == 1 || p == 2 ? (w + 1) / 2 : w, ph = p == 1 || p == 2 ? (h + 1) / 2 : h;
        for (int y = 0; y < ph; ++y) std::reverse(cam->data[p] + size_t(y) * size_t(cam->linesize[p]),
                                                  cam->data[p] + size_t(y) * size_t(cam->linesize[p]) + pw);
      }
    }
    for (int y = 0; y < h; ++y) {
      uint8_t* Y = cam->data[0] + size_t(y) * size_t(cam->linesize[0]);
      uint8_t* A = cam->data[3] + size_t(y) * size_t(cam->linesize[3]);
      const uint8_t* e = edge_.data() + size_t(y) * size_t(w);
      memcpy(A, alpha_.data() + size_t(y) * size_t(w), size_t(w));
      for (int x = 0; x < w; ++x)
        if (e[x]) Y[x] = uint8_t((Y[x] * (255 - e[x]) + borderY_ * e[x] + 127) / 255);
    }
    for (int y = 0; y < (h + 1) / 2; ++y) {
      uint8_t* U = cam->data[1] + size_t(y) * size_t(cam->linesize[1]);
      uint8_t* V = cam->data[2] + size_t(y) * size_t(cam->linesize[2]);
      for (int x = 0; x < (w + 1) / 2; ++x) {
        const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
        const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
        const int e = (edge_[size_t(y0) * w + x0] + edge_[size_t(y0) * w + x1] + edge_[size_t(y1) * w + x0] +
                       edge_[size_t(y1) * w + x1] + 2) / 4;
        if (!e) continue;
        U[x] = uint8_t((U[x] * (255 - e) + borderU_ * e + 127) / 255);
        V[x] = uint8_t((V[x] * (255 - e) + borderV_ * e + 127) / 255);
      }
    }
  }

  EditSettings s_;
  EditSource src_;
  bool active_ = false;
  EditCamBox box_;
  std::vector<uint8_t> alpha_, edge_;
  uint8_t borderY_ = 235, borderU_ = 128, borderV_ = 128;
  SwsContext* camSws_ = nullptr;
  SwsContext* bgSws_ = nullptr;
  Frame bg_;
};

void FreeCamFrame(void*, uint8_t* data) {
  AVFrame* f = reinterpret_cast<AVFrame*>(data);
  av_frame_free(&f);
}

// La cámara viaja con su fotograma (en opaque_ref, que FFmpeg copia con cada
// referencia): el Retimer repite o salta fotogramas y cada uno lleva la suya.
void AttachCam(AVFrame* frame, AVFrame* cam) {
  AVBufferRef* buf = av_buffer_create(reinterpret_cast<uint8_t*>(cam), 1, FreeCamFrame, nullptr, 0);
  if (!buf) {
    av_frame_free(&cam);
    return;
  }
  av_buffer_unref(&frame->opaque_ref);
  frame->opaque_ref = buf;
}

const AVFrame* CamOf(const AVFrame* frame) {
  return frame->opaque_ref ? reinterpret_cast<const AVFrame*>(frame->opaque_ref->data) : nullptr;
}

// Pone la cámara del fotograma f en la entrada [cam] con el mismo pts.
bool FeedCam(AVFilterContext* input, const AVFrame* f, int64_t pts) {
  const AVFrame* cam = CamOf(f);
  if (!input || !cam) return true; // sin cámara: overlay repite la anterior
  Frame c;
  if (av_frame_ref(c.p, cam) < 0) return false;
  c.p->pts = pts;
  c.p->duration = 1;
  return av_buffersrc_add_frame_flags(input, c.p, 0) >= 0;
}

GraphInput CamInput(const EditCamBox& box, const EditGeometry& g) {
  return {"cam", "buffer", VideoBufferArgs(box.w, box.h, AV_PIX_FMT_YUVA420P, {g.fpsDen, g.fpsNum}, {1, 1})};
}

} // namespace

namespace {

// Descripción del grafo de vídeo: [in] (y [ov0], [ov1]... para el texto) ->
// [out]. tail se pone antes de la salida (format=..., palettegen...).
std::string VideoChain(const EditSettings& s, const EditGeometry& g, double length,
                       const std::vector<EditOverlay>& overlays, const std::string& tail, const EditCamBox& cam,
                       int previewW = 0, int previewH = 0) {
  std::string f = "[in]format=yuv420p";
  auto add = [&](const std::string& x) { f += "," + x; };
  const int rot = ((s.rotate % 360) + 360) % 360;
  if (rot == 90) add("transpose=clock");
  else if (rot == 270) add("transpose=cclock");
  else if (rot == 180) add("hflip,vflip");
  if (s.flipH) add("hflip");
  if (s.flipV) add("vflip");
  if (g.cropW != g.rotatedW || g.cropH != g.rotatedH || g.cropX != 0 || g.cropY != 0)
    add("crop=" + Int(g.cropW) + ":" + Int(g.cropH) + ":" + Int(g.cropX) + ":" + Int(g.cropY));
  const bool fit = g.canvasW != g.cropW || g.canvasH != g.cropH;
  if (fit && s.fit == EditFit::Blur) {
    // Fondo: el mismo vídeo llenando el lienzo, a 1/8 de tamaño, difuminado y
    // más oscuro (barato aunque el lienzo sea de 1080x1920).
    const int sw = Even(g.canvasW / 8.0), sh = Even(g.canvasH / 8.0);
    const double k = std::max(double(sw) / g.cropW, double(sh) / g.cropH);
    const int bw = std::max(sw, Even(std::ceil(g.cropW * k))), bh = std::max(sh, Even(std::ceil(g.cropH * k)));
    f += ",split=2[fg][bg];[bg]scale=" + Int(bw) + ":" + Int(bh) + ":flags=area,crop=" + Int(sw) + ":" + Int(sh) +
         ",gblur=sigma=2.5,scale=" + Int(g.canvasW) + ":" + Int(g.canvasH) +
         ":flags=bilinear,lutyuv=y='16+(val-16)*0.55'[bgb];[fg]scale=" + Int(g.fgW) + ":" + Int(g.fgH) +
         ":flags=bicubic[fgs];[bgb][fgs]overlay=" + Int((g.canvasW - g.fgW) / 2) + ":" + Int((g.canvasH - g.fgH) / 2);
  } else if (fit) {
    add("scale=" + Int(g.fgW) + ":" + Int(g.fgH) + ":flags=bicubic,pad=" + Int(g.canvasW) + ":" + Int(g.canvasH) + ":" +
        Int((g.canvasW - g.fgW) / 2) + ":" + Int((g.canvasH - g.fgH) / 2) + ":black");
  }
  // La cámara en su sitio nuevo ([cam], ya con su forma), antes del color: se
  // ve con el mismo estilo que el resto.
  if (cam.w > 0) f += "[cb];[cb][cam]overlay=" + Int(cam.x) + ":" + Int(cam.y);
  if (s.blur) {
    // Zona difuminada o pixelada: se recorta, se reduce y se vuelve a poner encima.
    const int zw = std::clamp(Even(s.blurW * g.canvasW), 16, g.canvasW & ~1);
    const int zh = std::clamp(Even(s.blurH * g.canvasH), 16, g.canvasH & ~1);
    const int zx = std::clamp(int(std::lround(s.blurX * g.canvasW)) & ~1, 0, g.canvasW - zw);
    const int zy = std::clamp(int(std::lround(s.blurY * g.canvasH)) & ~1, 0, g.canvasH - zh);
    std::string shrink;
    if (s.pixelate) {
      const int block = std::max(8, std::min(g.canvasW, g.canvasH) / 54);
      shrink = "scale=" + Int(std::max(1, zw / block)) + ":" + Int(std::max(1, zh / block)) + ":flags=area,scale=" +
               Int(zw) + ":" + Int(zh) + ":flags=neighbor";
    } else {
      shrink = "scale=" + Int(std::max(2, zw / 12)) + ":" + Int(std::max(2, zh / 12)) + ":flags=area,gblur=sigma=1.5,scale=" +
               Int(zw) + ":" + Int(zh) + ":flags=bilinear";
    }
    f += ",split=2[zm][zs];[zs]crop=" + Int(zw) + ":" + Int(zh) + ":" + Int(zx) + ":" + Int(zy) + "," + shrink +
         "[zb];[zm][zb]overlay=" + Int(zx) + ":" + Int(zy);
  }
  const ColorParams color = ColorOf(s);
  if (color.Any()) add(LutFilter(color));
  if (s.sharpen) add("unsharp=5:5:0.8:5:5:0");
  if (s.vignette) add("vignette=angle=PI/5");
  if (g.outW != g.canvasW || g.outH != g.canvasH) add("scale=" + Int(g.outW) + ":" + Int(g.outH) + ":flags=bicubic");
  const double fade = std::min(0.6, length / 4);
  if (s.fadeIn && fade > 0.05) add("fade=t=in:st=0:d=" + Num(fade));
  if (s.fadeOut && fade > 0.05) add("fade=t=out:st=" + Num(std::max(0.0, length - fade)) + ":d=" + Num(fade));
  for (size_t i = 0; i < overlays.size(); ++i) {
    const std::string m = "[m" + Int(int64_t(i)) + "]";
    f += m + ";" + m + "[ov" + Int(int64_t(i)) + "]overlay=" + Int(overlays[i].x) + ":" + Int(overlays[i].y);
    if (overlays[i].until > 0) f += ":enable='lt(t," + Num(overlays[i].until) + ")'";
  }
  if (previewW > 0 && previewH > 0 && (previewW != g.outW || previewH != g.outH))
    add("scale=" + Int(previewW) + ":" + Int(previewH) + ":flags=bilinear");
  if (!tail.empty()) add(tail);
  return f + "[out]";
}

// Pone las imágenes del texto en sus entradas del grafo (una sola vez: el
// overlay repite la última).
bool FeedOverlays(Graph& graph, size_t first, const std::vector<EditOverlay>& overlays) {
  for (size_t i = 0; i < overlays.size(); ++i) {
    const EditOverlay& o = overlays[i];
    Frame f;
    f.p->format = AV_PIX_FMT_RGBA;
    f.p->width = o.width;
    f.p->height = o.height;
    if (av_frame_get_buffer(f.p, 0) < 0) return false;
    for (int y = 0; y < o.height; ++y)
      memcpy(f.p->data[0] + size_t(y) * size_t(f.p->linesize[0]), o.rgba.data() + size_t(y) * size_t(o.width) * 4,
             size_t(o.width) * 4);
    f.p->pts = 0;
    if (av_buffersrc_add_frame_flags(graph.inputs[first + i], f.p, 0) < 0) return false;
    if (av_buffersrc_add_frame_flags(graph.inputs[first + i], nullptr, 0) < 0) return false;
  }
  return true;
}

std::vector<GraphInput> OverlayInputs(const std::vector<EditOverlay>& overlays) {
  std::vector<GraphInput> ins;
  for (size_t i = 0; i < overlays.size(); ++i)
    ins.push_back({"ov" + Int(int64_t(i)), "buffer",
                   VideoBufferArgs(overlays[i].width, overlays[i].height, AV_PIX_FMT_RGBA, {1, 1}, {1, 1})});
  return ins;
}

// --------------------------------------------------------------- fps fijos

// Pasa fotogramas con tiempos cualesquiera (acelerados, frenados, al revés) a
// fps fijos: cada hueco de 1/fps se queda con el último fotograma que ya
// tocaba en ese momento; los que caen en el mismo hueco se saltan y en los
// huecos sin fotograma se repite el anterior. Como el filtro fps de FFmpeg,
// pero sabiendo dónde acaba cada tramo.
class Retimer {
public:
  using Emit = std::function<bool(const AVFrame*, int64_t)>;
  explicit Retimer(double fps) : fps_(fps) {}
  ~Retimer() { av_frame_free(&held_); }
  bool Push(const AVFrame* f, double outTime, const Emit& emit) {
    const int64_t k = Slot(outTime);
    if (has_ && k > next_) {
      for (int64_t n = next_; n < k; ++n)
        if (!emit(held_, n)) return false;
      next_ = k;
    }
    av_frame_unref(held_);
    if (av_frame_ref(held_, f) < 0) return false;
    has_ = true;
    return true;
  }
  bool FlushUntil(double outTime, const Emit& emit) {
    const int64_t k = Slot(outTime);
    if (has_)
      for (int64_t n = next_; n < k; ++n)
        if (!emit(held_, n)) return false;
    next_ = std::max(next_, k);
    return true;
  }
  int64_t Emitted() const { return next_; }

private:
  // Un fotograma justo entre dos huecos va al segundo: a doble velocidad, el
  // hueco n se queda con el fotograma 2n, que cae exacto, y no con el 2n - 1.
  int64_t Slot(double t) const { return int64_t(std::floor(t * fps_ + 0.5 + 1e-4)); }
  double fps_;
  AVFrame* held_ = av_frame_alloc();
  bool has_ = false;
  int64_t next_ = 0;
};

enum class Outcome { Done, Cancelled, Failed };

// Lee los tramos del original y entrega fotogramas a fps fijos y audio a
// 48 kHz en bloques de kChunk muestras (una pista o dos: juego y micro).
class Reader {
public:
  using VideoOut = std::function<bool(const AVFrame*, int64_t)>;
  using AudioOut = std::function<bool(AVFrame* const* tracks, int count)>;

  Reader(const EditSource& src, const EditGeometry& g, const std::atomic<bool>& cancel)
      : src_(src), fps_(g.fps), cancel_(cancel), retimer_(g.fps) {}
  ~Reader() {
    for (auto& t : tracks_) avcodec_free_context(&t.dec);
  }

  bool Open(const std::string& path, bool video, const std::vector<int>& audioStreams, int threads,
            std::string* error) {
    int err = avformat_open_input(&fmt_.p, path.c_str(), nullptr, nullptr);
    if (err < 0) {
      *error = "No se pudo abrir el vídeo: " + AvError(err);
      return false;
    }
    avformat_find_stream_info(fmt_.p, nullptr);
    start_ = fmt_.p->start_time != AV_NOPTS_VALUE ? double(fmt_.p->start_time) / AV_TIME_BASE : 0.0;
    vIndex_ = MainVideoStream(fmt_.p);
    if (video) {
      if (vIndex_ < 0 || !OpenStreamDecoder(fmt_.p->streams[vIndex_], threads, &vdec_.p)) {
        *error = "No se pudo decodificar el vídeo";
        return false;
      }
    }
    for (int s : audioStreams) {
      if (s < 0 || s >= int(fmt_.p->nb_streams)) continue;
      Track t;
      t.stream = s;
      if (!OpenStreamDecoder(fmt_.p->streams[s], 1, &t.dec)) {
        avcodec_free_context(&t.dec);
        *error = "No se pudo decodificar el audio";
        return false;
      }
      t.fifo.p = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, kChunk * 4);
      if (!t.fifo.p) return false;
      tracks_.push_back(std::move(t));
    }
    for (auto& t : tracks_) {
      t.chunk.p->format = AV_SAMPLE_FMT_FLTP;
      av_channel_layout_default(&t.chunk.p->ch_layout, 2);
      t.chunk.p->sample_rate = kRate;
      t.chunk.p->nb_samples = kChunk;
      if (av_frame_get_buffer(t.chunk.p, 0) < 0) return false;
    }
    video_ = video;
    return true;
  }

  Outcome Run(const std::vector<EditSegment>& segments, const VideoOut& onVideo, const AudioOut& onAudio,
              const std::function<void(double)>& onTime, std::string* error) {
    onVideo_ = onVideo;
    onAudio_ = onAudio;
    onTime_ = onTime;
    error_ = error;
    double at = 0;
    for (const auto& seg : segments) {
      if (cancel_) return Outcome::Cancelled;
      Outcome o = Segment(seg, at);
      if (o != Outcome::Done) return o;
      at += seg.Length();
    }
    if (video_ && !retimer_.FlushUntil(at, Emitter())) return Fail("Error al escribir el vídeo");
    if (!FlushAudio(true)) return Fail("Error al escribir el sonido");
    return cancel_ ? Outcome::Cancelled : Outcome::Done;
  }

  int64_t VideoFrames() const { return retimer_.Emitted(); }
  const AVCodecContext* VideoDecoder() const { return vdec_.p; }
  // Cambiar la cámara: cada fotograma sale sin ella (con el fondo de la pista
  // aparte) y con la cámara pegada para el grafo (ver AttachCam).
  void SetCam(CamCompositor* cam, CamBackgroundReader* background) {
    cam_ = cam && cam->Active() ? cam : nullptr;
    camBg_ = background && background->IsOpen() ? background : nullptr;
  }

private:
  struct Track {
    int stream = -1;
    AVCodecContext* dec = nullptr;
    Graph seg;            // el tramo: recorte, 48 kHz estéreo y velocidad
    Fifo fifo;            // muestras listas para la salida
    Frame chunk;          // bloque de kChunk muestras que se entrega
    int64_t written = 0;  // muestras de la salida ya puestas en la cola
    int64_t limit = 0;    // hasta dónde llega el tramo actual
    bool done = false;
    Track() = default;
    Track(Track&& o) noexcept : stream(o.stream), dec(o.dec) {
      o.dec = nullptr;
      std::swap(fifo.p, o.fifo.p);
      std::swap(chunk.p, o.chunk.p);
    }
  };

  Outcome Fail(const char* text) {
    if (error_ && error_->empty()) *error_ = text;
    return Outcome::Failed;
  }

  Retimer::Emit Emitter() {
    return [this](const AVFrame* f, int64_t index) {
      if (!onVideo_(f, index)) return false;
      if (onTime_) onTime_(double(index + 1) / fps_);
      return true;
    };
  }

  double TimeOf(const AVFrame* f, const AVStream* st) const {
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    return ts == AV_NOPTS_VALUE ? NAN : double(ts) * av_q2d(st->time_base) - start_;
  }

  void Seek(double t) {
    const int64_t ts = std::llround((std::max(0.0, t) + start_) * AV_TIME_BASE);
    av_seek_frame(fmt_.p, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (vdec_.p) avcodec_flush_buffers(vdec_.p);
    for (auto& tr : tracks_) avcodec_flush_buffers(tr.dec);
  }

  // Envía un paquete (nullptr: vaciar) y pasa cada fotograma a onFrame. Un
  // paquete dañado se salta, como haría un reproductor.
  template <typename F>
  bool Decode(AVCodecContext* dec, const AVPacket* p, F&& onFrame) {
    if (avcodec_send_packet(dec, p) < 0 && p) return true;
    for (;;) {
      int r = avcodec_receive_frame(dec, frame_.p);
      if (r < 0) return true;
      bool ok = onFrame(frame_.p);
      av_frame_unref(frame_.p);
      if (!ok) return false;
    }
  }

  // Grafo de audio del tramo: [from, to) del original, a 48 kHz estéreo y a
  // la velocidad del tramo (al revés si toca).
  bool OpenSegmentAudio(Track& t, const EditSegment& seg) {
    const AVCodecContext* d = t.dec;
    // atempo solo trabaja con muestras intercaladas: el formato de la cola
    // (planar) va al final.
    std::string desc = "[in]atrim=start=" + Num(seg.from) + ":end=" + Num(seg.to) +
                       ",asetpts=PTS-STARTPTS,aresample=" + Int(kRate) + TempoFilters(seg.speed);
    if (seg.kind == EditSegment::Kind::Reverse) desc += ",areverse";
    desc += ",aformat=sample_fmts=fltp:sample_rates=" + Int(kRate) + ":channel_layouts=stereo[out]";
    const int rate = d->sample_rate > 0 ? d->sample_rate : kRate;
    return BuildGraph(&t.seg, desc, {{"in", "abuffer", AudioBufferArgs({1, rate}, rate, d->sample_fmt, d->ch_layout)}},
                      true, 1, error_);
  }

  bool PushSegmentAudio(Track& t, AVFrame* f) {
    const AVStream* st = fmt_.p->streams[t.stream];
    const double time = TimeOf(f, st);
    const int rate = t.dec->sample_rate > 0 ? t.dec->sample_rate : kRate;
    // Tiempo del original en muestras: atrim recorta exacto aunque el bloque
    // empiece antes del tramo.
    f->pts = std::isnan(time) ? AV_NOPTS_VALUE : std::llround(time * rate);
    f->time_base = AVRational{1, rate};
    if (av_buffersrc_add_frame_flags(t.seg.inputs[0], f, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) return false;
    return DrainSegmentAudio(t);
  }

  bool DrainSegmentAudio(Track& t) {
    for (;;) {
      int r = av_buffersink_get_frame(t.seg.sink, out_.p);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) return false;
      // Lo que pase del final del tramo se tira (el tempo redondea).
      const int64_t room = std::max<int64_t>(0, t.limit - t.written);
      const int n = int(std::min<int64_t>(room, out_.p->nb_samples));
      if (n > 0) {
        if (av_audio_fifo_write(t.fifo.p, reinterpret_cast<void**>(out_.p->extended_data), n) < n) return false;
        t.written += n;
      }
      av_frame_unref(out_.p);
    }
  }

  // Cierra el tramo de audio: vacía el grafo y rellena con silencio hasta el final.
  bool EndSegmentAudio(Track& t) {
    if (t.seg.g) {
      if (av_buffersrc_add_frame_flags(t.seg.inputs[0], nullptr, 0) < 0) return false;
      if (!DrainSegmentAudio(t)) return false;
      t.seg.Reset();
    }
    return Silence(t, t.limit - t.written);
  }

  bool Silence(Track& t, int64_t samples) {
    float zeros[kChunk] = {};
    void* planes[2] = {zeros, zeros};
    while (samples > 0) {
      const int n = int(std::min<int64_t>(samples, kChunk));
      if (av_audio_fifo_write(t.fifo.p, planes, n) < n) return false;
      t.written += n;
      samples -= n;
    }
    return true;
  }

  // Entrega bloques completos de todas las pistas a la vez (last: también el
  // último, más corto).
  bool FlushAudio(bool last) {
    if (tracks_.empty()) return true;
    for (;;) {
      int avail = INT32_MAX;
      for (auto& t : tracks_) avail = std::min(avail, av_audio_fifo_size(t.fifo.p));
      if (avail <= 0 || (avail < kChunk && !last)) return true;
      const int n = std::min(avail, kChunk);
      AVFrame* frames[2] = {};
      int count = 0;
      for (auto& t : tracks_) {
        t.chunk.p->nb_samples = kChunk;
        if (av_frame_make_writable(t.chunk.p) < 0) return false;
        t.chunk.p->nb_samples = n;
        if (av_audio_fifo_read(t.fifo.p, reinterpret_cast<void**>(t.chunk.p->data), n) < n) return false;
        t.chunk.p->pts = delivered_;
        if (count < 2) frames[count++] = t.chunk.p;
      }
      delivered_ += n;
      if (!onAudio_(frames, count)) return false;
      if (!video_ && onTime_) onTime_(double(delivered_) / kRate);
    }
  }

  Outcome Segment(const EditSegment& seg, double outStart) {
    const double outEnd = outStart + seg.Length();
    for (auto& t : tracks_) {
      t.limit = std::llround(outEnd * kRate);
      t.done = false;
    }
    if (seg.kind == EditSegment::Kind::Freeze) {
      if (video_) {
        Outcome o = FreezeFrame(seg.from, outStart);
        if (o != Outcome::Done) return o;
      }
      for (auto& t : tracks_)
        if (!EndSegmentAudio(t)) return Fail("Error al escribir el sonido");
    } else {
      for (auto& t : tracks_)
        if (!OpenSegmentAudio(t, seg)) return Outcome::Failed;
      Outcome o = seg.kind == EditSegment::Kind::Reverse ? Reverse(seg, outStart) : Forward(seg, outStart);
      if (o != Outcome::Done) return o;
      for (auto& t : tracks_)
        if (!EndSegmentAudio(t)) return Fail("Error al escribir el sonido");
    }
    if (!FlushAudio(false)) return Fail("Error al escribir el sonido");
    if (video_ && !retimer_.FlushUntil(outEnd, Emitter())) return Fail("Error al escribir el vídeo");
    return cancel_ ? Outcome::Cancelled : Outcome::Done;
  }

  // Lee desde el fotograma clave anterior a from y entrega lo que cae en
  // [from, to). video/audio: qué pistas hacen falta en esta pasada.
  // onVideoFrame: 1 sigue, 0 error, -1 basta de vídeo.
  Outcome ReadRange(double from, double to, bool video, bool audio,
                    const std::function<int(AVFrame*, double)>& onVideoFrame) {
    Seek(from);
    const double half = 0.5 / src_.fps;
    bool vDone = !video;
    for (auto& t : tracks_) t.done = !audio;
    auto audioDone = [&] {
      for (auto& t : tracks_)
        if (!t.done) return false;
      return true;
    };
    auto onVideo = [&](AVFrame* f) {
      double t = TimeOf(f, fmt_.p->streams[vIndex_]);
      if (std::isnan(t)) t = lastVideo_ + 1.0 / src_.fps;
      lastVideo_ = t;
      if (t < from - half) return true;
      if (t >= to - half) {
        vDone = true;
        return true;
      }
      if (cam_) {
        if (av_frame_make_writable(f) < 0) return false;
        AVFrame* cam = av_frame_alloc();
        if (cam && cam_->Process(f, camBg_ ? camBg_->At(t) : nullptr, cam) && cam->data[0]) AttachCam(f, cam);
        else av_frame_free(&cam);
      }
      const int r = onVideoFrame(f, t);
      if (r < 0) vDone = true;
      return r != 0;
    };
    auto onAudio = [&](Track& tr) {
      return [&](AVFrame* f) {
        const double t = TimeOf(f, fmt_.p->streams[tr.stream]);
        if (!std::isnan(t) && t >= to) {
          tr.done = true;
          return true;
        }
        return PushSegmentAudio(tr, f);
      };
    };
    Packet pkt;
    while (!(vDone && audioDone())) {
      if (cancel_) return Outcome::Cancelled;
      if (av_read_frame(fmt_.p, pkt.p) < 0) {
        // Fin del archivo: lo que quede dentro de los decoders.
        if (!vDone && !Decode(vdec_.p, nullptr, onVideo)) return Fail("Error al escribir el vídeo");
        for (auto& tr : tracks_)
          if (!tr.done && !Decode(tr.dec, nullptr, onAudio(tr))) return Fail("Error al escribir el sonido");
        break;
      }
      bool ok = true;
      if (pkt.p->stream_index == vIndex_ && !vDone) {
        ok = Decode(vdec_.p, pkt.p, onVideo);
      } else {
        for (auto& tr : tracks_)
          if (pkt.p->stream_index == tr.stream && !tr.done) ok = Decode(tr.dec, pkt.p, onAudio(tr));
        if (ok && audio) ok = FlushAudio(false);
      }
      av_packet_unref(pkt.p);
      if (!ok) return Fail("Error al escribir el vídeo");
    }
    return Outcome::Done;
  }

  Outcome Forward(const EditSegment& seg, double outStart) {
    return ReadRange(seg.from, seg.to, video_, !tracks_.empty(), [&](AVFrame* f, double t) {
      return retimer_.Push(f, outStart + (t - seg.from) / seg.speed, Emitter()) ? 1 : 0;
    });
  }

  // Al revés: el audio en una pasada (areverse lo da la vuelta al acabar) y el
  // vídeo en ventanas desde el final, cada una decodificada desde su
  // fotograma clave y sacada del último fotograma al primero.
  Outcome Reverse(const EditSegment& seg, double outStart) {
    if (!tracks_.empty()) {
      Outcome o = ReadRange(seg.from, seg.to, false, true, nullptr);
      if (o != Outcome::Done) return o;
    }
    if (!video_) return Outcome::Done;
    const double frameBytes = double(src_.width) * src_.height * 1.5;
    const int maxFrames = std::clamp(int(kReverseBudgetBytes / std::max(1.0, frameBytes)), 8, 240);
    const double window = maxFrames / src_.fps;
    const double frame = 1.0 / src_.fps;
    double w1 = seg.to;
    std::vector<AVFrame*> held;
    auto release = [&] {
      for (auto* f : held) av_frame_free(&f);
      held.clear();
    };
    while (w1 > seg.from + 1e-6) {
      const double w0 = std::max(seg.from, w1 - window);
      Outcome o = ReadRange(w0, w1, true, false, [&](AVFrame* f, double) {
        AVFrame* c = av_frame_clone(f);
        if (!c) return 0;
        held.push_back(c);
        return 1;
      });
      if (o != Outcome::Done) {
        release();
        return o;
      }
      for (auto it = held.rbegin(); it != held.rend(); ++it) {
        double t = TimeOf(*it, fmt_.p->streams[vIndex_]);
        if (std::isnan(t)) t = w0;
        const double out = outStart + std::max(0.0, seg.to - t - frame) / seg.speed;
        if (!retimer_.Push(*it, out, Emitter())) {
          release();
          return Fail("Error al escribir el vídeo");
        }
      }
      release();
      w1 = w0;
    }
    return Outcome::Done;
  }

  Outcome FreezeFrame(double at, double outStart) {
    AVFrame* still = nullptr;
    Outcome o = ReadRange(at, at + 10.0, true, false, [&](AVFrame* f, double) {
      if (!still) still = av_frame_clone(f);
      return -1; // con el primero basta
    });
    if (o != Outcome::Done) {
      av_frame_free(&still);
      return o;
    }
    if (still) {
      bool ok = retimer_.Push(still, outStart, Emitter());
      av_frame_free(&still);
      if (!ok) return Fail("Error al escribir el vídeo");
    }
    return Outcome::Done;
  }

  const EditSource& src_;
  double fps_;
  const std::atomic<bool>& cancel_;
  FormatIn fmt_;
  Codec vdec_;
  int vIndex_ = -1;
  bool video_ = true;
  double start_ = 0;
  double lastVideo_ = 0;
  std::vector<Track> tracks_;
  Retimer retimer_;
  Frame frame_, out_;
  int64_t delivered_ = 0;
  VideoOut onVideo_;
  AudioOut onAudio_;
  std::function<void(double)> onTime_;
  std::string* error_ = nullptr;
  CamCompositor* cam_ = nullptr;
  CamBackgroundReader* camBg_ = nullptr;
};

// Pistas de audio que se leen: juego y micro si están aparte; si no, la primera.
std::vector<int> AudioStreamsFor(const EditSettings& s, const EditSource& src) {
  if (s.mute || !src.audio) return {};
  if (src.SeparateMic()) return {src.gameTrack, src.micTrack};
  return {src.mixTrack};
}

// Grafo de audio de la salida: volúmenes, mezcla, ruido, igualar y fundidos.
std::string AudioChain(const EditSettings& s, int tracks, double length) {
  const double fade = std::min(0.8, length / 4);
  std::string tail;
  if (s.normalize) tail += ",dynaudnorm=f=250:g=15:p=0.9";
  if (s.audioFadeIn && fade > 0.05) tail += ",afade=t=in:st=0:d=" + Num(fade);
  if (s.audioFadeOut && fade > 0.05) tail += ",afade=t=out:st=" + Num(std::max(0.0, length - fade)) + ":d=" + Num(fade);
  // Subir el volumen puede saturar: el limitador lo evita sin cambiar el resto.
  tail += ",alimiter=limit=0.97:level=0,aformat=sample_fmts=fltp:sample_rates=" + Int(kRate) +
          ":channel_layouts=stereo[out]";
  const float volume = std::clamp(s.volume, 0.0f, 2.0f), mic = std::clamp(s.micVolume, 0.0f, 2.0f);
  if (tracks >= 2) {
    std::string micChain = s.denoise ? "afftdn=nr=12:nf=-40," : "";
    return "[a0]volume=" + Num(volume) + "[g];[a1]" + micChain + "volume=" + Num(mic) +
           "[m];[g][m]amix=inputs=2:duration=longest:normalize=0" + tail;
  }
  std::string chain = "[a0]";
  if (s.denoise) chain += "afftdn=nr=8:nf=-45,";
  return chain + "volume=" + Num(volume) + tail;
}

int64_t VideoBitrate(const EditSettings& s, const EditSource& src, const EditGeometry& g) {
  const double outRate = double(g.outW) * g.outH * g.fps;
  double bps = src.videoBps > 0 ? double(src.videoBps) : double(src.width) * src.height * src.fps * 0.1;
  const double ratio = outRate / std::max(1.0, double(src.width) * src.height * src.fps);
  bps *= std::pow(ratio, 0.75);
  double floor = outRate * 0.05, cap = outRate * 0.35;
  if (s.size == EditSize::Light) {
    bps *= 0.5;
    floor = outRate * 0.03;
  }
  bps = std::clamp(bps, floor, std::max(floor, cap));
  return int64_t(std::clamp(bps, 1.0e6, 150.0e6));
}

struct Encoded {
  AVFormatContext* fmt = nullptr;
  bool Write(AVCodecContext* enc, AVStream* st, AVPacket* pkt) {
    for (;;) {
      int r = avcodec_receive_packet(enc, pkt);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) return false;
      av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
      pkt->stream_index = st->index;
      if (av_interleaved_write_frame(fmt, pkt) < 0) return false;
    }
  }
  bool Send(AVCodecContext* enc, AVStream* st, const AVFrame* frame, AVPacket* pkt) {
    int r = avcodec_send_frame(enc, frame);
    if (r < 0 && !(frame == nullptr && r == AVERROR_EOF)) return false;
    return Write(enc, st, pkt);
  }
};

struct PassContext {
  const EditJob& job;
  const EditSource& src;
  const std::vector<EditSegment>& segments;
  const EditGeometry& g;
  double length;
  std::vector<EditOverlay> overlays;
  int threads;
  const std::atomic<bool>& cancel;
  std::function<void(double)> progress; // 0..1 de esta pasada
  EditCamBox cam;                       // la cámara en su sitio nuevo (w = 0: no se pone)
};

// Cambiar la cámara en una pasada: el compositor y el lector de la pista aparte.
struct PassCam {
  CamCompositor compositor;
  CamBackgroundReader background;
  void Attach(PassContext& c, Reader& reader) {
    if (!c.src.Webcam() || !c.job.settings.camEdit) return;
    compositor.Setup(c.job.settings, c.src, c.g);
    if (!background.Open(c.job.input, c.src.camTrack))
      Log("Editor: no se pudo leer la pista del fondo de la cámara; se mueve sin quitarla del todo");
    reader.SetCam(&compositor, &background);
  }
};

// Paleta del GIF: una pasada con palettegen; devuelve su único fotograma.
Outcome GifPalette(PassContext& c, AVFrame* palette, std::string* error) {
  Reader reader(c.src, c.g, c.cancel);
  if (!reader.Open(c.job.input, true, {}, c.threads, error)) return Outcome::Failed;
  PassCam cam;
  cam.Attach(c, reader);
  AVFilterContext* camIn = nullptr;
  Graph graph;
  Frame out;
  bool got = false;
  auto drain = [&] {
    for (;;) {
      int r = av_buffersink_get_frame(graph.sink, out.p);
      if (r < 0) return r == AVERROR(EAGAIN) || r == AVERROR_EOF;
      if (!got) {
        av_frame_ref(palette, out.p);
        got = true;
      }
      av_frame_unref(out.p);
    }
  };
  Frame tmp;
  auto onVideo = [&](const AVFrame* f, int64_t index) {
    if (!graph.g) {
      std::vector<GraphInput> ins = {{"in", "buffer",
                                      VideoBufferArgs(f->width, f->height, f->format,
                                                      {c.g.fpsDen, c.g.fpsNum}, f->sample_aspect_ratio)}};
      auto ov = OverlayInputs(c.overlays);
      ins.insert(ins.end(), ov.begin(), ov.end());
      if (c.cam.w > 0) ins.push_back(CamInput(c.cam, c.g));
      if (!BuildGraph(&graph,
                      VideoChain(c.job.settings, c.g, c.length, c.overlays, "palettegen=stats_mode=full", c.cam), ins,
                      false, c.threads, error) ||
          !FeedOverlays(graph, 1, c.overlays))
        return false;
      if (c.cam.w > 0) camIn = graph.inputs.back();
    }
    if (!FeedCam(camIn, f, index)) return false;
    if (av_frame_ref(tmp.p, f) < 0) return false;
    av_buffer_unref(&tmp.p->opaque_ref);
    tmp.p->pts = index;
    int r = av_buffersrc_add_frame_flags(graph.inputs[0], tmp.p, 0);
    av_frame_unref(tmp.p);
    return r >= 0 && drain();
  };
  Outcome o = reader.Run(c.segments, onVideo, [](AVFrame* const*, int) { return true; },
                         [&](double t) { if (c.progress) c.progress(t / c.length); }, error);
  if (o != Outcome::Done) return o;
  if (!graph.g || (camIn && av_buffersrc_add_frame_flags(camIn, nullptr, 0) < 0) ||
      av_buffersrc_add_frame_flags(graph.inputs[0], nullptr, 0) < 0 || !drain() || !got) {
    if (error->empty()) *error = "No se pudo hacer la paleta del GIF";
    return Outcome::Failed;
  }
  return Outcome::Done;
}

// La pasada que escribe el archivo: MP4 (vídeo y audio), GIF o M4A.
Outcome Encode(PassContext& c, const std::string& output, const AVFrame* palette, std::string* encoderUsed,
               std::string* error) {
  const EditSettings& s = c.job.settings;
  const bool gif = s.format == EditFormat::Gif;
  const bool video = s.format != EditFormat::Audio;
  const std::vector<int> audioStreams = gif ? std::vector<int>{} : AudioStreamsFor(s, c.src);
  const bool audio = !audioStreams.empty();
  if (!video && !audio) {
    *error = "Este vídeo no tiene sonido";
    return Outcome::Failed;
  }

  Reader reader(c.src, c.g, c.cancel);
  if (!reader.Open(c.job.input, video, audioStreams, c.threads, error)) return Outcome::Failed;
  PassCam cam;
  if (video) cam.Attach(c, reader);
  AVFilterContext* camIn = nullptr;

  FormatOut out;
  const char* muxer = gif ? "gif" : video ? "mp4" : "ipod";
  int err = avformat_alloc_output_context2(&out.p, nullptr, muxer, output.c_str());
  if (err < 0 || !out.p) {
    *error = "No se pudo crear el archivo: " + AvError(err);
    return Outcome::Failed;
  }
  const bool globalHeader = (out.p->oformat->flags & AVFMT_GLOBALHEADER) != 0;

  MfHold mf;
  Codec venc, aenc;
  AVStream* vOut = nullptr;
  AVStream* aOut = nullptr;
  if (video) {
    if (gif) {
      const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_GIF);
      venc.p = codec ? avcodec_alloc_context3(codec) : nullptr;
      if (venc.p) {
        venc.p->width = c.g.outW;
        venc.p->height = c.g.outH;
        venc.p->time_base = {c.g.fpsDen, c.g.fpsNum};
        venc.p->framerate = {c.g.fpsNum, c.g.fpsDen};
        venc.p->pix_fmt = AV_PIX_FMT_PAL8;
        if (avcodec_open2(venc.p, codec, nullptr) < 0) avcodec_free_context(&venc.p);
      }
      if (!venc.p) {
        *error = "No se pudo crear el GIF";
        return Outcome::Failed;
      }
      *encoderUsed = "gif";
    } else {
      // Con los colores del original, como en Exportar.
      const VideoTarget target{c.g.outW, c.g.outH, {c.g.fpsNum, c.g.fpsDen}, VideoBitrate(s, c.src, c.g),
                               c.job.gentle};
      if (!OpenFirstH264Encoder(c.job.encoders, target, reader.VideoDecoder(), globalHeader, &venc.p, encoderUsed,
                                &mf, "Editor")) {
        *error = "Ningún encoder H.264 se pudo abrir en este PC";
        return Outcome::Failed;
      }
      Log("Editor: %dx%d a %.2f fps, %lld kbps con %s", c.g.outW, c.g.outH, c.g.fps,
          (long long)(target.bps / 1000), encoderUsed->c_str());
    }
    vOut = avformat_new_stream(out.p, nullptr);
    if (!vOut || avcodec_parameters_from_context(vOut->codecpar, venc.p) < 0) {
      *error = "No se pudo crear el archivo";
      return Outcome::Failed;
    }
    vOut->time_base = venc.p->time_base;
  }
  if (audio) {
    if (!OpenAacEncoder(192000, 2, kRate, globalHeader, &aenc.p, "Editor")) {
      *error = "No se pudo codificar el sonido";
      return Outcome::Failed;
    }
    aOut = avformat_new_stream(out.p, nullptr);
    if (!aOut || avcodec_parameters_from_context(aOut->codecpar, aenc.p) < 0) {
      *error = "No se pudo crear el archivo";
      return Outcome::Failed;
    }
    aOut->time_base = aenc.p->time_base;
  }
  err = avio_open(&out.p->pb, output.c_str(), AVIO_FLAG_WRITE);
  if (err < 0) {
    *error = "No se pudo crear el archivo: " + AvError(err);
    return Outcome::Failed;
  }
  AVDictionary* muxOpts = nullptr;
  if (!gif) av_dict_set(&muxOpts, "movflags", "+faststart", 0); // se empieza a ver sin bajarlo entero
  err = avformat_write_header(out.p, &muxOpts);
  av_dict_free(&muxOpts);
  if (err < 0) {
    *error = "No se pudo escribir el archivo: " + AvError(err);
    return Outcome::Failed;
  }

  Encoded enc{out.p};
  Packet pkt;
  Frame filtered, tmp;
  Graph vgraph, agraph;
  const char* pixFmt = venc.p ? av_get_pix_fmt_name(venc.p->pix_fmt) : "yuv420p";
  auto drainVideo = [&] {
    for (;;) {
      int r = av_buffersink_get_frame(vgraph.sink, filtered.p);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) return false;
      filtered.p->pict_type = AV_PICTURE_TYPE_NONE;
      filtered.p->pts = av_rescale_q(filtered.p->pts, av_buffersink_get_time_base(vgraph.sink), venc.p->time_base);
      bool ok = enc.Send(venc.p, vOut, filtered.p, pkt.p);
      av_frame_unref(filtered.p);
      if (!ok) return false;
    }
  };
  auto onVideo = [&](const AVFrame* f, int64_t index) {
    if (!vgraph.g) {
      std::vector<GraphInput> ins = {{"in", "buffer",
                                      VideoBufferArgs(f->width, f->height, f->format,
                                                      {c.g.fpsDen, c.g.fpsNum}, f->sample_aspect_ratio)}};
      auto ov = OverlayInputs(c.overlays);
      ins.insert(ins.end(), ov.begin(), ov.end());
      std::string desc;
      if (gif) {
        ins.push_back({"pal", "buffer", VideoBufferArgs(palette->width, palette->height, palette->format, {1, 1}, {1, 1})});
        desc = VideoChain(s, c.g, c.length, c.overlays, "", c.cam);
        desc.erase(desc.size() - strlen("[out]"));
        desc += "[x];[x][pal]paletteuse=dither=sierra2_4a[out]";
      } else {
        desc = VideoChain(s, c.g, c.length, c.overlays, std::string("format=") + pixFmt, c.cam);
      }
      const size_t palIndex = ins.size() - 1;
      if (c.cam.w > 0) ins.push_back(CamInput(c.cam, c.g));
      if (!BuildGraph(&vgraph, desc, ins, false, c.threads, error) || !FeedOverlays(vgraph, 1, c.overlays))
        return false;
      if (c.cam.w > 0) camIn = vgraph.inputs.back();
      if (gif) {
        Frame pal;
        if (av_frame_ref(pal.p, palette) < 0) return false;
        pal.p->pts = 0;
        AVFilterContext* palIn = vgraph.inputs[palIndex];
        if (av_buffersrc_add_frame_flags(palIn, pal.p, 0) < 0 || av_buffersrc_add_frame_flags(palIn, nullptr, 0) < 0)
          return false;
      }
    }
    if (!FeedCam(camIn, f, index)) return false;
    if (av_frame_ref(tmp.p, f) < 0) return false;
    av_buffer_unref(&tmp.p->opaque_ref);
    tmp.p->pts = index;
    tmp.p->duration = 1;
    int r = av_buffersrc_add_frame_flags(vgraph.inputs[0], tmp.p, 0);
    av_frame_unref(tmp.p);
    return r >= 0 && drainVideo();
  };

  const int tracks = int(audioStreams.size());
  if (audio) {
    std::vector<GraphInput> ins;
    AVChannelLayout stereo{};
    av_channel_layout_default(&stereo, 2);
    for (int i = 0; i < tracks; ++i)
      ins.push_back({"a" + Int(i), "abuffer", AudioBufferArgs({1, kRate}, kRate, AV_SAMPLE_FMT_FLTP, stereo)});
    av_channel_layout_uninit(&stereo);
    if (!BuildGraph(&agraph, AudioChain(s, tracks, c.length), ins, true, 1, error)) return Outcome::Failed;
    av_buffersink_set_frame_size(agraph.sink, aenc.p->frame_size > 0 ? aenc.p->frame_size : kChunk);
  }
  Frame audioOut;
  auto drainAudio = [&] {
    for (;;) {
      int r = av_buffersink_get_frame(agraph.sink, audioOut.p);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) return false;
      bool ok = enc.Send(aenc.p, aOut, audioOut.p, pkt.p);
      av_frame_unref(audioOut.p);
      if (!ok) return false;
    }
  };
  auto onAudio = [&](AVFrame* const* frames, int count) {
    for (int i = 0; i < count && i < int(agraph.inputs.size()); ++i) {
      frames[i]->time_base = AVRational{1, kRate};
      if (av_buffersrc_add_frame_flags(agraph.inputs[size_t(i)], frames[i], AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
        return false;
    }
    return drainAudio();
  };

  // El MP4 usa el encoder de la GPU (el GIF y el M4A, solo la CPU).
  LivePacer pacer(video && !gif ? c.job.live : nullptr);
  Outcome o = reader.Run(c.segments, video ? Reader::VideoOut(onVideo) : Reader::VideoOut(),
                         audio ? Reader::AudioOut(onAudio) : Reader::AudioOut(),
                         [&](double t) {
                           if (c.progress) c.progress(std::clamp(t / c.length, 0.0, 1.0));
                           pacer.Wait(t, c.cancel);
                         },
                         error);
  if (o != Outcome::Done) return o;
  bool ok = true;
  if (video) {
    if (!vgraph.g) {
      *error = "No se pudo decodificar ningún fotograma del vídeo";
      return Outcome::Failed;
    }
    ok = (!camIn || av_buffersrc_add_frame_flags(camIn, nullptr, 0) >= 0) &&
         av_buffersrc_add_frame_flags(vgraph.inputs[0], nullptr, 0) >= 0 && drainVideo() &&
         enc.Send(venc.p, vOut, nullptr, pkt.p);
  }
  if (ok && audio) {
    for (auto* in : agraph.inputs) ok = ok && av_buffersrc_add_frame_flags(in, nullptr, 0) >= 0;
    ok = ok && drainAudio() && enc.Send(aenc.p, aOut, nullptr, pkt.p);
  }
  if (c.cancel) return Outcome::Cancelled;
  if (!ok) {
    *error = "No se pudo escribir el archivo (¿falta espacio en el disco?)";
    return Outcome::Failed;
  }
  err = av_write_trailer(out.p);
  if (err < 0) {
    *error = "No se pudo terminar el archivo: " + AvError(err);
    return Outcome::Failed;
  }
  return Outcome::Done;
}

} // namespace

EditResult RenderEdit(const EditJob& job, const std::atomic<bool>& cancel, const std::function<void(float)>& progress) {
  EditResult r;
  r.input = job.input;
  r.output = job.output;
  r.settings = job.settings;
  EditSource src;
  if (!ProbeEditSource(job.input, &src, &r.error)) return r;
  const EditSettings& s = job.settings;
  const auto segments = BuildSegments(s, src);
  const double length = SegmentsLength(segments);
  if (segments.empty() || length < 0.1) {
    r.error = "El resultado quedaría vacío: revisa el recorte y el trozo quitado";
    return r;
  }
  if (s.format == EditFormat::Gif && length > kMaxGifSeconds + 0.05) {
    r.error = "Un GIF puede durar como mucho 30 s y este saldría de " + Num(std::round(length)) +
              " s: recórtalo o súbele la velocidad";
    return r;
  }
  if (ReversedSeconds(s, src) > kMaxReverseSeconds + 0.05) {
    r.error = "«Al revés» y «Bumerán» van con 1 min del original como mucho: recorta el tramo";
    return r;
  }
  if (s.format == EditFormat::Audio && (s.mute || !src.audio)) {
    r.error = src.audio ? "El sonido está quitado: no queda nada que guardar" : "Este vídeo no tiene sonido";
    return r;
  }
  const EditGeometry g = ComputeGeometry(s, src);
  const int threads = job.gentle ? 1 : std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 4);
  PassContext c{job, src, segments, g, length, {}, threads, cancel, nullptr, ComputeCamBox(s, src, g)};
  if (s.format != EditFormat::Audio) c.overlays = BuildOverlays(s, g.outW, g.outH, job.raster);

  const std::string part = job.output + ".part"; // la biblioteca no lo ve hasta que está completo
  const fs::path partPath = PathOf(part);
  std::error_code ec;
  Outcome o = Outcome::Done;
  Frame palette;
  if (s.format == EditFormat::Gif) {
    c.progress = [&](double f) { if (progress) progress(float(f * 0.4)); };
    o = GifPalette(c, palette.p, &r.error);
    c.progress = [&](double f) { if (progress) progress(float(0.4 + f * 0.6)); };
  } else {
    c.progress = [&](double f) { if (progress) progress(float(f)); };
  }
  if (o == Outcome::Done) o = Encode(c, part, palette.p, &r.encoder, &r.error);
  if (o != Outcome::Done) {
    fs::remove(partPath, ec);
    if (o == Outcome::Cancelled || cancel) {
      r.cancelled = true;
      r.error.clear();
    } else if (r.error.empty()) {
      r.error = "No se pudo guardar la edición";
    }
    if (!r.error.empty()) Log("Editor: %s", r.error.c_str());
    return r;
  }
  r.bytes = fs::file_size(partPath, ec);
  if (!ec) fs::rename(partPath, PathOf(job.output), ec);
  if (ec) {
    fs::remove(partPath, ec);
    r.error = "No se pudo guardar la edición";
    return r;
  }
  r.ok = true;
  r.seconds = length;
  Log("Editor: %s guardado (%.1f s, %llu bytes, %s)", job.output.c_str(), length, (unsigned long long)r.bytes,
      r.encoder.c_str());
  return r;
}

bool RenderEditPreview(const EditSettings& settings, const EditSource& source, const uint8_t* bgr, int width,
                       int height, double sourceSeconds, const TextRasterizer& raster, int maxW, int maxH,
                       std::vector<uint8_t>* bgra, int* outW, int* outH, std::string* error, EditImage camBackground) {
  if (!bgr || width < 16 || height < 16) return false;
  // El fotograma del reproductor manda: si su tamaño no es el que dijo FFmpeg
  // (alto de 1088 en vez de 1080...), la geometría se calcula con el suyo. El
  // recuadro de la cámara solo se escala si de verdad es otro tamaño (no unas
  // filas de relleno).
  EditSource src = source;
  if (src.Webcam() && source.width > 0 && source.height > 0 &&
      (std::abs(width - source.width) > 16 || std::abs(height - source.height) > 16)) {
    const double kx = double(width) / source.width, ky = double(height) / source.height;
    src.camX = int(src.camX * kx) & ~1;
    src.camY = int(src.camY * ky) & ~1;
    src.camW = int(src.camW * kx) & ~1;
    src.camH = int(src.camH * ky) & ~1;
  }
  src.width = width;
  src.height = height;
  EditSettings s = settings;
  if (s.format == EditFormat::Audio) s.format = EditFormat::Video;
  const EditGeometry g = ComputeGeometry(s, src);
  const auto segments = BuildSegments(s, src);
  const double length = std::max(0.1, SegmentsLength(segments));
  double t = OutputTimeOf(segments, sourceSeconds);
  if (t < 0) t = length * 0.5; // un momento que no sale: sin fundidos
  int pw = g.outW, ph = g.outH;
  if (maxW > 0 && maxH > 0 && (pw > maxW || ph > maxH)) {
    const double k = std::min(double(maxW) / pw, double(maxH) / ph);
    pw = std::max(2, int(pw * k));
    ph = std::max(2, int(ph * k));
  }
  // El texto se dibuja a la resolución final, y su borde cuesta (cerca de 1 s
  // en 4K): mientras no cambie, el de la petición anterior. Cada hilo guarda
  // el suyo (la vista previa va siempre en el mismo).
  thread_local std::string overlayKey;
  thread_local std::vector<EditOverlay> overlayCache;
  std::string key = s.text + '\n' + s.watermark + '\n' + Int(int(s.textPlace)) + "," + Int(s.textSize) + "," +
                    Int(s.textBox) + "," + Num(s.textSeconds) + "," + Int(int(s.watermarkCorner)) + "," +
                    Int(g.outW) + "x" + Int(g.outH) + (raster ? "" : ",-");
  if (key != overlayKey) {
    overlayCache = BuildOverlays(s, g.outW, g.outH, raster);
    overlayKey = std::move(key);
  }
  const std::vector<EditOverlay>& overlays = overlayCache;

  Frame in;
  in.p->format = AV_PIX_FMT_BGR24;
  in.p->width = width;
  in.p->height = height;
  if (av_frame_get_buffer(in.p, 0) < 0) return false;
  for (int y = 0; y < height; ++y)
    memcpy(in.p->data[0] + size_t(y) * size_t(in.p->linesize[0]), bgr + size_t(y) * size_t(width) * 3,
           size_t(width) * 3);
  in.p->pts = int64_t(std::floor(t * g.fps + 0.5));

  // La cámara: fuera de su sitio y, si va, en el nuevo.
  CamCompositor compositor;
  Frame cam;
  EditCamBox camBox;
  if (src.Webcam() && s.camEdit) {
    compositor.Setup(s, src, g);
    Frame background;
    const AVFrame* bg = nullptr;
    if (camBackground.data && camBackground.width >= 2 && camBackground.height >= 2) {
      background.p->format = AV_PIX_FMT_BGRA;
      background.p->width = camBackground.width;
      background.p->height = camBackground.height;
      background.p->data[0] = const_cast<uint8_t*>(camBackground.data);
      background.p->linesize[0] = camBackground.width * 4;
      bg = background.p;
    }
    if (compositor.Process(in.p, bg, cam.p) && cam.p->data[0]) camBox = compositor.Box();
    background.p->data[0] = nullptr; // no es suyo
  }

  std::vector<GraphInput> ins = {
      {"in", "buffer", VideoBufferArgs(width, height, AV_PIX_FMT_BGR24, {g.fpsDen, g.fpsNum}, {1, 1})}};
  auto ov = OverlayInputs(overlays);
  ins.insert(ins.end(), ov.begin(), ov.end());
  if (camBox.w > 0) ins.push_back(CamInput(camBox, g));
  Graph graph;
  if (!BuildGraph(&graph, VideoChain(s, g, length, overlays, "format=bgra", camBox, pw, ph), ins, false, 1, error) ||
      !FeedOverlays(graph, 1, overlays))
    return false;
  if (camBox.w > 0) {
    cam.p->pts = in.p->pts;
    AVFilterContext* camIn = graph.inputs.back();
    if (av_buffersrc_add_frame_flags(camIn, cam.p, 0) < 0 || av_buffersrc_add_frame_flags(camIn, nullptr, 0) < 0)
      return false;
  }
  if (av_buffersrc_add_frame_flags(graph.inputs[0], in.p, 0) < 0 ||
      av_buffersrc_add_frame_flags(graph.inputs[0], nullptr, 0) < 0)
    return false;
  Frame out;
  if (av_buffersink_get_frame(graph.sink, out.p) < 0) return false;
  *outW = out.p->width;
  *outH = out.p->height;
  bgra->resize(size_t(out.p->width) * size_t(out.p->height) * 4);
  for (int y = 0; y < out.p->height; ++y)
    memcpy(bgra->data() + size_t(y) * size_t(out.p->width) * 4,
           out.p->data[0] + size_t(y) * size_t(out.p->linesize[0]), size_t(out.p->width) * 4);
  return true;
}

// --------------------------------------------------------------- hilo

bool EditWorker::Start(EditJob job, std::function<void(int)> onProgress, std::function<void()> onDone) {
  if (busy_) return false;
  if (thread_.joinable()) thread_.join();
  cancel_ = false;
  percent_ = 0;
  {
    std::lock_guard lock(mutex_);
    input_ = job.input;
    haveResult_ = false;
  }
  busy_ = true;
  thread_ = std::thread([this, job = std::move(job), onProgress = std::move(onProgress), onDone = std::move(onDone)] {
#ifdef _WIN32
    InstallThreadCrashHandler();
    // Media Foundation (h264_mf) necesita COM en este hilo.
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Con un juego abierto o grabando, a los núcleos de eficiencia (EcoQoS) y
    // en modo de fondo: CPU, disco y memoria ceden ante el juego.
    std::optional<BackgroundMode> background;
    if (job.gentle) {
      SetThreadEcoQoS(true);
      background.emplace();
    } else {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
#endif
    EditResult result;
    try {
      result = RenderEdit(job, cancel_, [&](float fraction) {
        int percent = std::clamp(int(fraction * 100.0f), 0, 99);
        if (percent_.exchange(percent) != percent && onProgress) onProgress(percent);
      });
    } catch (const std::exception& e) {
      result = EditResult{};
      result.input = job.input;
      result.error = std::string("No se pudo guardar la edición: ") + e.what();
      Log("Editor: excepción: %s", e.what());
    }
#ifdef _WIN32
    background.reset();
    if (SUCCEEDED(com)) CoUninitialize();
#endif
    {
      std::lock_guard lock(mutex_);
      result_ = std::move(result);
      haveResult_ = true;
    }
    busy_ = false;
    if (onDone) onDone();
  });
  return true;
}

void EditWorker::Stop() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
}

std::string EditWorker::Input() const {
  std::lock_guard lock(mutex_);
  return input_;
}

bool EditWorker::TakeResult(EditResult* result) {
  std::lock_guard lock(mutex_);
  if (!haveResult_) return false;
  *result = std::move(result_);
  haveResult_ = false;
  return true;
}

} // namespace spectre
