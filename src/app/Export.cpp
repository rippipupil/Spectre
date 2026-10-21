#include "app/Export.h"

#include "common/Util.h"
#include "encode/Media.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include "app/CrashHandler.h"
#include "core/ThreadPolicy.h"
#include <mfapi.h>
#include <objbase.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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

// Por GPU primero (NVIDIA, AMD, Intel y Media Foundation con el encoder de la
// tarjeta); por CPU solo si ninguno abre.
const char* const kEncoders[] = {"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf:hw", "h264_mf", "libopenh264"};

// Por debajo de esto el vídeo sería una mancha: mejor pedir que se recorte.
constexpr double kMinVideoBps = 200000;

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
  ~Codec() { avcodec_free_context(&p); }
};
struct Frame {
  AVFrame* p = av_frame_alloc();
  ~Frame() { av_frame_free(&p); }
};
struct Packet {
  AVPacket* p = av_packet_alloc();
  ~Packet() { av_packet_free(&p); }
};
struct Scaler {
  SwsContext* p = nullptr;
  ~Scaler() { sws_freeContext(p); }
};
struct Resampler {
  SwrContext* p = nullptr;
  ~Resampler() { swr_free(&p); }
};
struct Fifo {
  AVAudioFifo* p = nullptr;
  ~Fifo() {
    if (p) av_audio_fifo_free(p);
  }
};

// h264_mf, como en VideoEncoder::TryOpen: si no encuentra el MFT, la limpieza
// de FFmpeg 8.1 hace un MFShutdown() y un CoUninitialize() de más, que se
// llevarían el MFStartup de main (webcam y listas de audio) y el COM de este
// hilo. Antes de cada intento se toma uno de cada. Si el intento falla, o los
// gastó FFmpeg o se quedan sin soltar, que no molesta; si abre, se sueltan
// después de cerrar el encoder (por eso se declara antes que él).
struct MfHold {
  bool held = false;
#ifdef _WIN32
  static bool Take() {
    if (!MediaFoundationAvailable() || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return true;
    CoUninitialize();
    return false;
  }
  ~MfHold() {
    if (!held) return;
    MFShutdown();
    CoUninitialize();
  }
#else
  static bool Take() { return false; }
#endif
};

bool Fail(std::string* error, std::string text) {
  if (error) *error = std::move(text);
  return false;
}

std::string LimitText(uint64_t bytes) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.0f MB", double(bytes) / 1e6);
  return buf;
}

// La primera pista de audio: en los clips de Spectre es la mezcla de todo
// (las demás, si las hay, son el sistema y el micrófono por separado).
int FirstAudio(const AVFormatContext* fmt) {
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    const AVCodecParameters* par = fmt->streams[i]->codecpar;
    if (par->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(par->codec_id)) return int(i);
  }
  return -1;
}

double DurationOf(const AVFormatContext* fmt, int video) {
  if (fmt->duration > 0) return double(fmt->duration) / AV_TIME_BASE;
  const AVStream* st = fmt->streams[video];
  if (st->duration > 0) return double(st->duration) * av_q2d(st->time_base);
  return 0;
}

double TimeOf(const AVFrame* frame, const AVStream* st) {
  int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
  return ts == AV_NOPTS_VALUE ? NAN : double(ts) * av_q2d(st->time_base);
}

bool SupportsPixFmt(const AVCodec* codec, AVPixelFormat fmt) {
  const AVPixelFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  int n = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   reinterpret_cast<const void**>(&fmts), &n) < 0)
    return false;
#else
  fmts = codec->pix_fmts;
#endif
  if (!fmts) return true;
  for (; *fmts != AV_PIX_FMT_NONE; ++fmts)
    if (*fmts == fmt) return true;
  return false;
}

struct Probe {
  double duration = 0;
  int width = 0, height = 0;
  AVRational rate{60, 1}; // fps del original
  double fps = 60;
  int64_t videoBps = 0; // 0 = no se sabe
  bool h264 = false;
  bool audio = false;
  bool aac = false;
};

bool ProbeInput(const std::string& path, Probe* probe, std::string* error) {
  FormatIn in;
  int err = avformat_open_input(&in.p, path.c_str(), nullptr, nullptr);
  if (err < 0) return Fail(error, "No se pudo abrir el vídeo: " + AvError(err));
  avformat_find_stream_info(in.p, nullptr);
  int v = av_find_best_stream(in.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (v < 0) return Fail(error, "El archivo no tiene vídeo");
  AVStream* st = in.p->streams[v];
  probe->width = st->codecpar->width;
  probe->height = st->codecpar->height;
  if (probe->width < 16 || probe->height < 16) return Fail(error, "El vídeo no tiene un tamaño válido");
  AVRational rate = av_guess_frame_rate(in.p, st, nullptr);
  if (rate.num > 0 && rate.den > 0) probe->rate = rate;
  probe->fps = av_q2d(probe->rate);
  probe->duration = DurationOf(in.p, v);
  if (probe->duration <= 0) return Fail(error, "No se pudo saber cuánto dura el vídeo");
  probe->h264 = st->codecpar->codec_id == AV_CODEC_ID_H264;
  int64_t audioBps = 0;
  if (int a = FirstAudio(in.p); a >= 0) {
    probe->audio = true;
    probe->aac = in.p->streams[a]->codecpar->codec_id == AV_CODEC_ID_AAC;
    audioBps = in.p->streams[a]->codecpar->bit_rate;
  }
  probe->videoBps = st->codecpar->bit_rate;
  if (probe->videoBps <= 0) {
    int64_t size = avio_size(in.p->pb);
    if (size > 0) probe->videoBps = std::max<int64_t>(0, int64_t(double(size) * 8.0 / probe->duration) - audioBps);
  }
  return true;
}

struct Plan {
  int width = 0, height = 0;
  AVRational rate{60, 1}; // fps de la copia
  int step = 1;           // frames del original por cada frame de la copia
  int64_t videoBps = 0;
  int audioBps = 0;
};

// Bitrate que cabe en budgetBytes y la resolución que le toca: con pocos bits
// se ve mejor un vídeo más pequeño que uno grande lleno de bloques.
bool MakePlan(const Probe& probe, double budgetBytes, Plan* plan) {
  const double duration = std::max(probe.duration, 0.5);
  plan->audioBps = !probe.audio ? 0 : duration <= 90 ? 128000 : duration <= 240 ? 96000 : 64000;
  double bps = budgetBytes * 8.0 / duration - plan->audioBps;
  // Más bitrate que el original no mejora nada.
  if (probe.videoBps > 0) bps = std::min(bps, double(probe.videoBps));
  if (bps < kMinVideoBps) return false;

  // Los fps del original divididos por un entero (144 a 48, 120 a 60, 60 a 30
  // con poco bitrate): cada frame de la copia sale del mismo número de frames
  // del original. Con un paso irregular (144 a 60 coge 2 o 3) el movimiento
  // va a saltitos.
  const double maxFps = bps < 2.0e6 ? 30.0 : 60.0;
  const int step = std::max(1, int(std::ceil(probe.fps / maxFps - 1e-6)));
  const AVRational rate = av_mul_q(probe.rate, AVRational{1, step});
  const double fps = av_q2d(rate);
  // Bitrate mínimo de cada altura a 60 fps; a 30 fps basta con el 60 %.
  static const struct {
    int height;
    double bps60;
  } kLadder[] = {{1080, 5.5e6}, {720, 2.8e6}, {540, 1.6e6}, {480, 0.9e6}};
  const double perFps = fps <= 30.5 ? 0.6 : 1.0;
  int height = 360;
  for (const auto& rung : kLadder) {
    if (bps >= rung.bps60 * perFps) {
      height = rung.height;
      break;
    }
  }
  height = std::min(height, probe.height) & ~1;
  int width = int(std::lround(double(probe.width) * height / probe.height)) & ~1;
  if (width < 16 || height < 16) return false;
  plan->width = width;
  plan->height = height;
  plan->rate = rate;
  plan->step = step;
  plan->videoBps = int64_t(bps);
  return true;
}

bool OpenDecoder(const AVStream* st, int threads, Codec* out) {
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec) return false;
  out->p = avcodec_alloc_context3(codec);
  if (!out->p || avcodec_parameters_to_context(out->p, st->codecpar) < 0) return false;
  out->p->pkt_timebase = st->time_base;
  out->p->thread_count = threads;
  return avcodec_open2(out->p, codec, nullptr) >= 0;
}

// Opciones de calidad para un tamaño fijo: VBR alrededor del bitrate del plan,
// con picos de hasta 1,5 veces. No hay prisa (no es en directo), así que se
// usan los modos de más calidad de cada encoder.
void SetQualityOptions(const std::string& name, AVDictionary** opts) {
  if (name == "h264_nvenc") {
    av_dict_set(opts, "preset", "p6", 0);
    av_dict_set(opts, "tune", "hq", 0);
    av_dict_set(opts, "rc", "vbr", 0);
    av_dict_set(opts, "multipass", "fullres", 0);
    av_dict_set(opts, "spatial-aq", "1", 0);
    av_dict_set(opts, "profile", "high", 0);
  } else if (name == "h264_amf") {
    av_dict_set(opts, "usage", "transcoding", 0);
    av_dict_set(opts, "quality", "quality", 0);
    av_dict_set(opts, "rc", "vbr_peak", 0);
    av_dict_set(opts, "profile", "high", 0);
  } else if (name == "h264_qsv") {
    av_dict_set(opts, "preset", "slow", 0);
    av_dict_set(opts, "profile", "high", 0);
  } else if (name == "h264_mf") {
    av_dict_set(opts, "rate_control", "pc_vbr", 0);
    av_dict_set(opts, "scenario", "archive", 0);
  } else if (name == "libopenh264") {
    av_dict_set(opts, "rc_mode", "bitrate", 0);
  }
}

// id: nombre de FFmpeg, con ":hw" para pedir el encoder de la GPU a Media
// Foundation. simple: sin las opciones de calidad, por si el driver no las admite.
bool OpenVideoEncoder(const std::string& id, const Plan& plan, const AVCodecContext* dec, bool globalHeader,
                      bool simple, Codec* out) {
  std::string name = id;
  bool hw = false;
  if (size_t colon = name.find(':'); colon != std::string::npos) {
    hw = name.compare(colon + 1, std::string::npos, "hw") == 0;
    name.erase(colon);
  }
  const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) return false;
  AVCodecContext* c = avcodec_alloc_context3(codec);
  out->p = c;
  if (!c) return false;
  c->width = plan.width;
  c->height = plan.height;
  c->time_base = av_inv_q(plan.rate);
  c->framerate = plan.rate;
  c->sample_aspect_ratio = {1, 1};
  c->pix_fmt = SupportsPixFmt(codec, AV_PIX_FMT_NV12) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  c->gop_size = std::max(1, int(std::lround(av_q2d(plan.rate) * 2)));
  c->max_b_frames = 0;
  c->bit_rate = plan.videoBps;
  c->rc_max_rate = plan.videoBps * 3 / 2;
  c->rc_buffer_size = int(std::min<int64_t>(plan.videoBps * 2, INT32_MAX));
  c->color_range = AVCOL_RANGE_MPEG; // swscale entrega rango limitado
  c->color_primaries = dec->color_primaries != AVCOL_PRI_UNSPECIFIED ? dec->color_primaries : AVCOL_PRI_BT709;
  c->color_trc = dec->color_trc != AVCOL_TRC_UNSPECIFIED ? dec->color_trc : AVCOL_TRC_BT709;
  c->colorspace = dec->colorspace != AVCOL_SPC_UNSPECIFIED ? dec->colorspace : AVCOL_SPC_BT709;
  if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  AVDictionary* opts = nullptr;
  if (!simple) SetQualityOptions(name, &opts);
  if (hw) av_dict_set(&opts, "hw_encoding", "1", 0);
  int err = avcodec_open2(c, codec, &opts);
  av_dict_free(&opts);
  if (err < 0) {
    Log("Exportar: no se pudo abrir %s (%s): %s", id.c_str(), simple ? "simple" : "calidad", AvError(err).c_str());
    avcodec_free_context(&out->p);
    return false;
  }
  return true;
}

bool OpenAudioEncoder(const Plan& plan, const AVCodecContext* dec, bool globalHeader, Codec* out) {
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec) return false;
  AVCodecContext* c = avcodec_alloc_context3(codec);
  out->p = c;
  if (!c) return false;
  const int channels = std::clamp(dec->ch_layout.nb_channels, 1, 2);
  av_channel_layout_default(&c->ch_layout, channels);
  c->sample_fmt = AV_SAMPLE_FMT_FLTP;
  c->sample_rate = dec->sample_rate == 44100 ? 44100 : 48000;
  c->time_base = {1, c->sample_rate};
  c->bit_rate = channels == 1 ? plan.audioBps / 2 : plan.audioBps;
  if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  int err = avcodec_open2(c, codec, nullptr);
  if (err < 0) {
    Log("Exportar: no se pudo abrir el encoder AAC: %s", AvError(err).c_str());
    avcodec_free_context(&out->p);
    return false;
  }
  return true;
}

enum class Outcome { Done, Cancelled, Failed };

Outcome Transcode(const std::string& input, const std::string& output, const Plan& plan,
                  const ExportOptions& options, const std::atomic<bool>& cancel,
                  const std::function<void(float)>& progress, std::string* encoder, std::string* error) {
  auto fail = [&](std::string text) {
    *error = std::move(text);
    return Outcome::Failed;
  };
  FormatIn in;
  int err = avformat_open_input(&in.p, input.c_str(), nullptr, nullptr);
  if (err < 0) return fail("No se pudo abrir el vídeo: " + AvError(err));
  avformat_find_stream_info(in.p, nullptr);
  const int vIndex = av_find_best_stream(in.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vIndex < 0) return fail("El archivo no tiene vídeo");
  int aIndex = plan.audioBps > 0 ? FirstAudio(in.p) : -1;
  const AVStream* vIn = in.p->streams[vIndex];
  const double start = in.p->start_time != AV_NOPTS_VALUE ? double(in.p->start_time) / AV_TIME_BASE : 0.0;
  const double duration = DurationOf(in.p, vIndex);

  // Con un juego abierto, la decodificación va en este hilo (de fondo); los
  // hilos que crearía FFmpeg tendrían prioridad normal.
  const int threads = options.gentle ? 1 : std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 4);
  Codec vdec, adec;
  if (!OpenDecoder(vIn, threads, &vdec)) return fail("No se pudo decodificar el vídeo");
  if (aIndex >= 0 && !OpenDecoder(in.p->streams[aIndex], 1, &adec)) {
    Log("Exportar: no se pudo decodificar el audio; la copia irá sin sonido");
    aIndex = -1;
  }

  FormatOut out;
  err = avformat_alloc_output_context2(&out.p, nullptr, "mp4", output.c_str());
  if (err < 0 || !out.p) return fail("No se pudo crear la copia: " + AvError(err));
  const bool globalHeader = (out.p->oformat->flags & AVFMT_GLOBALHEADER) != 0;

  MfHold mf;
  Codec venc;
  std::vector<std::string> order = options.encoders;
  if (order.empty()) order.assign(std::begin(kEncoders), std::end(kEncoders));
  for (const auto& id : order) {
    for (bool simple : {false, true}) {
      const bool took = id.find("_mf") != std::string::npos && MfHold::Take();
      if (OpenVideoEncoder(id, plan, vdec.p, globalHeader, simple, &venc)) {
        mf.held = took;
        break;
      }
    }
    if (venc.p) {
      *encoder = id;
      break;
    }
  }
  if (!venc.p) return fail("Ningún encoder H.264 se pudo abrir en este PC");
  Codec aenc;
  if (aIndex >= 0 && !OpenAudioEncoder(plan, adec.p, globalHeader, &aenc)) aIndex = -1;

  AVStream* vOut = avformat_new_stream(out.p, nullptr);
  if (!vOut || avcodec_parameters_from_context(vOut->codecpar, venc.p) < 0) return fail("No se pudo crear la copia");
  vOut->time_base = venc.p->time_base;
  AVStream* aOut = nullptr;
  if (aIndex >= 0) {
    aOut = avformat_new_stream(out.p, nullptr);
    if (!aOut || avcodec_parameters_from_context(aOut->codecpar, aenc.p) < 0) return fail("No se pudo crear la copia");
    aOut->time_base = aenc.p->time_base;
  }
  err = avio_open(&out.p->pb, output.c_str(), AVIO_FLAG_WRITE);
  if (err < 0) return fail("No se pudo crear la copia: " + AvError(err));
  AVDictionary* muxOpts = nullptr;
  // El índice delante: Discord empieza a reproducirlo sin bajarlo entero.
  av_dict_set(&muxOpts, "movflags", "+faststart", 0);
  err = avformat_write_header(out.p, &muxOpts);
  av_dict_free(&muxOpts);
  if (err < 0) return fail("No se pudo escribir la copia: " + AvError(err));

  Packet pkt, encoded;
  Frame decoded, scaled, audioFrame, converted;
  Scaler scaler;
  Resampler resampler;
  Fifo fifo;
  int convertedCapacity = 0;
  int64_t lastIndex = -1;
  int64_t audioPts = AV_NOPTS_VALUE; // en muestras
  // Bajar de fps (144 a 48) se queda con uno de cada tres frames; si no, cada
  // frame cae en su sitio aunque los tiempos del original bailen un poco.
  const bool decimate = plan.step > 1;
  const double fps = av_q2d(plan.rate);

  scaled.p->format = venc.p->pix_fmt;
  scaled.p->width = plan.width;
  scaled.p->height = plan.height;
  if (av_frame_get_buffer(scaled.p, 0) < 0) return fail("Sin memoria para exportar");
  const int audioFrameSize = aOut ? (aenc.p->frame_size > 0 ? aenc.p->frame_size : 1024) : 0;
  if (aOut) {
    audioFrame.p->format = aenc.p->sample_fmt;
    av_channel_layout_copy(&audioFrame.p->ch_layout, &aenc.p->ch_layout);
    audioFrame.p->sample_rate = aenc.p->sample_rate;
    audioFrame.p->nb_samples = audioFrameSize;
    fifo.p = av_audio_fifo_alloc(aenc.p->sample_fmt, aenc.p->ch_layout.nb_channels, audioFrameSize * 4);
    if (av_frame_get_buffer(audioFrame.p, 0) < 0 || !fifo.p) return fail("Sin memoria para exportar");
  }

  auto writePackets = [&](AVCodecContext* enc, AVStream* st) {
    for (;;) {
      int r = avcodec_receive_packet(enc, encoded.p);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) return false;
      av_packet_rescale_ts(encoded.p, enc->time_base, st->time_base);
      encoded.p->stream_index = st->index;
      if (av_interleaved_write_frame(out.p, encoded.p) < 0) return false;
    }
  };
  auto encode = [&](AVCodecContext* enc, AVStream* st, AVFrame* frame) {
    int r = avcodec_send_frame(enc, frame);
    if (r < 0 && !(frame == nullptr && r == AVERROR_EOF)) return false;
    return writePackets(enc, st);
  };

  int lastPercent = -1;
  auto onVideo = [&](AVFrame* f) {
    double t = TimeOf(f, vIn) - start;
    int64_t index = lastIndex + 1;
    if (!std::isnan(t)) {
      if (t < -0.001) return true; // antes del inicio (lista de edición del MP4)
      index = decimate ? int64_t(std::floor(t * fps + 1e-3)) : int64_t(std::llround(t * fps));
      if (index <= lastIndex) return true;
    }
    lastIndex = index;
    scaler.p = sws_getCachedContext(scaler.p, f->width, f->height, AVPixelFormat(f->format), plan.width,
                                    plan.height, venc.p->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler.p || av_frame_make_writable(scaled.p) < 0) return false;
    sws_scale(scaler.p, f->data, f->linesize, 0, f->height, scaled.p->data, scaled.p->linesize);
    scaled.p->pts = index;
    if (!encode(venc.p, vOut, scaled.p)) return false;
    if (progress && duration > 0 && !std::isnan(t)) {
      int percent = int(std::clamp(t / duration, 0.0, 1.0) * 100.0);
      if (percent != lastPercent) {
        lastPercent = percent;
        progress((float(percent) + 0.5f) / 100.0f);
      }
    }
    return true;
  };

  // Audio: se pasa al formato del encoder AAC y se entrega en bloques de su
  // tamaño de frame (la cola guarda lo que sobra).
  auto drainAudio = [&](bool last) {
    while (av_audio_fifo_size(fifo.p) >= audioFrameSize || (last && av_audio_fifo_size(fifo.p) > 0)) {
      const int count = std::min(audioFrameSize, av_audio_fifo_size(fifo.p));
      audioFrame.p->nb_samples = audioFrameSize;
      if (av_frame_make_writable(audioFrame.p) < 0) return false;
      audioFrame.p->nb_samples = count;
      if (av_audio_fifo_read(fifo.p, reinterpret_cast<void**>(audioFrame.p->data), count) < count) return false;
      audioFrame.p->pts = audioPts;
      audioPts += count;
      if (!encode(aenc.p, aOut, audioFrame.p)) return false;
    }
    return true;
  };
  auto convertAudio = [&](const uint8_t** data, int samples) {
    int capacity = swr_get_out_samples(resampler.p, samples);
    if (capacity <= 0) return true;
    if (capacity > convertedCapacity) {
      av_frame_unref(converted.p);
      converted.p->format = aenc.p->sample_fmt;
      av_channel_layout_copy(&converted.p->ch_layout, &aenc.p->ch_layout);
      converted.p->sample_rate = aenc.p->sample_rate;
      converted.p->nb_samples = capacity;
      if (av_frame_get_buffer(converted.p, 0) < 0) return false;
      convertedCapacity = capacity;
    }
    int got = swr_convert(resampler.p, converted.p->data, convertedCapacity, data, samples);
    if (got < 0) return false;
    if (got > 0 && av_audio_fifo_write(fifo.p, reinterpret_cast<void**>(converted.p->data), got) < got) return false;
    return true;
  };
  auto onAudio = [&](AVFrame* f) {
    if (!resampler.p) {
      AVChannelLayout layout{};
      if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || av_channel_layout_check(&f->ch_layout) == 0)
        av_channel_layout_default(&layout, std::max(1, f->ch_layout.nb_channels));
      else
        av_channel_layout_copy(&layout, &f->ch_layout);
      int r = swr_alloc_set_opts2(&resampler.p, &aenc.p->ch_layout, aenc.p->sample_fmt, aenc.p->sample_rate, &layout,
                                  AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
      av_channel_layout_uninit(&layout);
      if (r < 0 || swr_init(resampler.p) < 0) return false;
      // El audio empieza donde empezaba en el original respecto al vídeo.
      double t = TimeOf(f, in.p->streams[aIndex]) - start;
      audioPts = std::isnan(t) ? 0 : std::max<int64_t>(0, std::llround(t * aenc.p->sample_rate));
    }
    return convertAudio(const_cast<const uint8_t**>(f->extended_data), f->nb_samples) && drainAudio(false);
  };

  // Envía un paquete (nullptr: vaciar el decoder) y procesa lo que salga. Un
  // paquete dañado se salta, como haría un reproductor.
  auto decode = [&](AVCodecContext* dec, const AVPacket* p, auto&& onFrame) {
    if (avcodec_send_packet(dec, p) < 0 && p) return true;
    for (;;) {
      int r = avcodec_receive_frame(dec, decoded.p);
      if (r < 0) return true; // EAGAIN, fin o frame dañado
      bool ok = onFrame(decoded.p);
      av_frame_unref(decoded.p);
      if (!ok) return false;
    }
  };

  bool ok = true;
  while (ok && !cancel) {
    if (av_read_frame(in.p, pkt.p) < 0) break;
    if (pkt.p->stream_index == vIndex) ok = decode(vdec.p, pkt.p, onVideo);
    else if (pkt.p->stream_index == aIndex) ok = decode(adec.p, pkt.p, onAudio);
    av_packet_unref(pkt.p);
  }
  if (cancel) return Outcome::Cancelled;
  if (ok) ok = decode(vdec.p, nullptr, onVideo);
  if (ok && aOut) {
    ok = decode(adec.p, nullptr, onAudio);
    if (ok && resampler.p) ok = convertAudio(nullptr, 0) && drainAudio(true);
    if (ok) ok = encode(aenc.p, aOut, nullptr);
  }
  if (ok) ok = encode(venc.p, vOut, nullptr);
  if (cancel) return Outcome::Cancelled;
  if (!ok) return fail("No se pudo escribir la copia (¿falta espacio en el disco?)");
  if (lastIndex < 0) return fail("No se pudo decodificar ningún frame del vídeo");
  err = av_write_trailer(out.p);
  if (err < 0) return fail("No se pudo terminar la copia: " + AvError(err));
  return Outcome::Done;
}

} // namespace

std::string ExportPathFor(const std::string& input) {
  const fs::path in = PathOf(input);
  fs::path base = in.parent_path() / in.stem();
  base += "_discord";
  fs::path out = base;
  out += ".mp4";
  std::error_code ec;
  for (int i = 2; fs::exists(out, ec) && i < 1000; ++i) {
    out = base;
    out += "_" + std::to_string(i) + ".mp4";
  }
#ifdef _WIN32
  return Narrow(out.wstring());
#else
  return out.string();
#endif
}

ExportResult ExportVideo(const std::string& input, const std::string& output, const ExportOptions& options,
                         const std::atomic<bool>& cancel, const std::function<void(float)>& progress) {
  ExportResult result;
  result.input = input;
  Probe probe;
  if (!ProbeInput(input, &probe, &result.error)) return result;

  // Margen bajo el límite: Discord puede contar los MB de 1024 x 1024 o de un
  // millón; un 2 % por debajo vale para los dos.
  const double hardMax = double(options.maxBytes) * 0.98;
  const std::string limit = LimitText(options.maxBytes);
  std::error_code ec;
  const uint64_t inputBytes = fs::file_size(PathOf(input), ec);
  std::string ext = PathOf(input).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (probe.h264 && (!probe.audio || probe.aac) && (ext == ".mp4" || ext == ".mov") && !ec && inputBytes > 0 &&
      double(inputBytes) <= hardMax) {
    result.ok = true;
    result.alreadyFits = true;
    result.output = input;
    result.bytes = inputBytes;
    return result;
  }

  const std::string part = output + ".part"; // la biblioteca no lo ve hasta que está completo
  const fs::path partPath = PathOf(part);
  // Primer intento al 92 % del límite: los encoders por hardware se pasan un
  // poco del bitrate pedido y el MP4 añade su índice.
  double budget = double(options.maxBytes) * 0.92;
  for (int attempt = 0; attempt < 3; ++attempt) {
    Plan plan;
    if (!MakePlan(probe, budget, &plan)) {
      result.error = "Este vídeo dura demasiado para caber en " + limit +
                     " con una calidad aceptable. Recórtalo y exporta el recorte.";
      return result;
    }
    Log("Exportar para Discord: %dx%d a %.2f fps, vídeo %lld kbps, audio %d kbps (intento %d)", plan.width,
        plan.height, av_q2d(plan.rate), (long long)(plan.videoBps / 1000), plan.audioBps / 1000, attempt + 1);
    std::string error;
    Outcome outcome = Transcode(input, part, plan, options, cancel, progress, &result.encoder, &error);
    if (outcome != Outcome::Done) {
      fs::remove(partPath, ec);
      if (outcome == Outcome::Cancelled) result.cancelled = true;
      else result.error = error;
      if (!error.empty()) Log("Exportar: %s", error.c_str());
      return result;
    }
    const uint64_t bytes = fs::file_size(partPath, ec);
    if (ec) {
      fs::remove(partPath, ec);
      result.error = "No se pudo leer la copia exportada";
      return result;
    }
    Log("Exportar: %s, %llu bytes", result.encoder.c_str(), (unsigned long long)bytes);
    if (double(bytes) <= hardMax) {
      fs::rename(partPath, PathOf(output), ec);
      if (ec) {
        fs::remove(partPath, ec);
        result.error = "No se pudo guardar la copia exportada";
        return result;
      }
      result.ok = true;
      result.output = output;
      result.bytes = bytes;
      return result;
    }
    // Se pasó: otra vez con el bitrate rebajado en la misma proporción.
    budget *= hardMax * 0.95 / double(bytes);
  }
  fs::remove(partPath, ec);
  result.error = "No se pudo dejar la copia por debajo de " + limit;
  return result;
}

bool Exporter::Start(const std::string& input, const ExportOptions& options, std::function<void(int)> onProgress,
                     std::function<void()> onDone) {
  if (busy_) return false;
  if (thread_.joinable()) thread_.join();
  cancel_ = false;
  percent_ = 0;
  {
    std::lock_guard lock(mutex_);
    input_ = input;
    haveResult_ = false;
  }
  busy_ = true;
  thread_ = std::thread([this, input, options, onProgress = std::move(onProgress), onDone = std::move(onDone)] {
#ifdef _WIN32
    InstallThreadCrashHandler();
    // Media Foundation (h264_mf) necesita COM en este hilo.
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Con un juego abierto, a los núcleos de eficiencia (EcoQoS) y en modo de
    // fondo: CPU, disco y memoria ceden ante el juego.
    std::optional<BackgroundMode> background;
    if (options.gentle) {
      SetThreadEcoQoS(true);
      background.emplace();
    } else {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
#endif
    ExportResult result;
    try {
      result = ExportVideo(input, ExportPathFor(input), options, cancel_, [&](float fraction) {
        int percent = std::clamp(int(fraction * 100.0f), 0, 99);
        if (percent_.exchange(percent) != percent && onProgress) onProgress(percent);
      });
    } catch (const std::exception& e) {
      result = ExportResult{};
      result.input = input;
      result.error = std::string("No se pudo exportar: ") + e.what();
      Log("Exportar: excepción: %s", e.what());
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

void Exporter::Stop() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
}

std::string Exporter::Input() const {
  std::lock_guard lock(mutex_);
  return input_;
}

bool Exporter::TakeResult(ExportResult* result) {
  std::lock_guard lock(mutex_);
  if (!haveResult_) return false;
  *result = std::move(result_);
  haveResult_ = false;
  return true;
}

} // namespace spectre
