#include "app/Transcode.h"

#include "common/Util.h"

#ifdef _WIN32
#include "core/ThreadPolicy.h"

#include <mfapi.h>
#include <objbase.h>
#endif

#include <algorithm>
#include <climits>
#include <cmath>
#include <thread>

namespace spectre {

const char* const kH264Encoders[6] = {"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf:hw", "h264_mf", "libopenh264"};

#ifdef _WIN32
bool MfHold::Take() {
  if (!MediaFoundationAvailable() || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
  if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return true;
  CoUninitialize();
  return false;
}

MfHold::~MfHold() {
  if (!held) return;
  MFShutdown();
  CoUninitialize();
}
#else
bool MfHold::Take() { return false; }
MfHold::~MfHold() = default;
#endif

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

bool OpenStreamDecoder(const AVStream* st, int threads, AVCodecContext** out) {
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec) return false;
  *out = avcodec_alloc_context3(codec);
  if (!*out || avcodec_parameters_to_context(*out, st->codecpar) < 0) return false;
  (*out)->pkt_timebase = st->time_base;
  (*out)->thread_count = threads;
  return avcodec_open2(*out, codec, nullptr) >= 0;
}

namespace {

// Opciones de calidad para un tamaño fijo: VBR alrededor del bitrate pedido,
// con picos de hasta 1,5 veces. Sin juego ni grabación no hay prisa y se usan
// los modos de más calidad de cada encoder. Con ellos (background), los
// rápidos que usa el replay en directo: el encoder de la GPU es el mismo y no
// hay que quitarle hueco.
void SetQualityOptions(const std::string& name, bool background, AVDictionary** opts) {
  if (name == "h264_nvenc") {
    av_dict_set(opts, "preset", background ? "p4" : "p6", 0);
    av_dict_set(opts, "tune", "hq", 0);
    av_dict_set(opts, "rc", "vbr", 0);
    if (!background) av_dict_set(opts, "multipass", "fullres", 0);
    av_dict_set(opts, "spatial-aq", "1", 0);
    av_dict_set(opts, "profile", "high", 0);
  } else if (name == "h264_amf") {
    av_dict_set(opts, "usage", "transcoding", 0);
    av_dict_set(opts, "quality", background ? "speed" : "quality", 0);
    av_dict_set(opts, "rc", "vbr_peak", 0);
    av_dict_set(opts, "profile", "high", 0);
    if (background) av_dict_set(opts, "async_depth", "4", 0);
  } else if (name == "h264_qsv") {
    av_dict_set(opts, "preset", background ? "veryfast" : "slow", 0);
    av_dict_set(opts, "profile", "high", 0);
  } else if (name == "h264_mf") {
    av_dict_set(opts, "rate_control", "pc_vbr", 0);
    av_dict_set(opts, "scenario", "archive", 0);
  } else if (name == "libopenh264") {
    av_dict_set(opts, "rc_mode", "bitrate", 0);
  }
}

} // namespace

bool OpenH264Encoder(const std::string& id, const VideoTarget& target, const AVCodecContext* dec, bool globalHeader,
                     bool simple, AVCodecContext** out, const char* tag) {
  std::string name = id;
  bool hw = false;
  if (size_t colon = name.find(':'); colon != std::string::npos) {
    hw = name.compare(colon + 1, std::string::npos, "hw") == 0;
    name.erase(colon);
  }
  const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) return false;
  AVCodecContext* c = avcodec_alloc_context3(codec);
  *out = c;
  if (!c) return false;
  c->width = target.width;
  c->height = target.height;
  c->time_base = av_inv_q(target.rate);
  c->framerate = target.rate;
  c->sample_aspect_ratio = {1, 1};
  c->pix_fmt = SupportsPixFmt(codec, AV_PIX_FMT_NV12) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  c->gop_size = std::max(1, int(std::lround(av_q2d(target.rate) * 2)));
  c->max_b_frames = 0;
  c->bit_rate = target.bps;
  c->rc_max_rate = target.bps * 3 / 2;
  c->rc_buffer_size = int(std::min<int64_t>(target.bps * 2, INT32_MAX));
  c->color_range = AVCOL_RANGE_MPEG; // swscale entrega rango limitado
  c->color_primaries = dec && dec->color_primaries != AVCOL_PRI_UNSPECIFIED ? dec->color_primaries : AVCOL_PRI_BT709;
  c->color_trc = dec && dec->color_trc != AVCOL_TRC_UNSPECIFIED ? dec->color_trc : AVCOL_TRC_BT709;
  c->colorspace = dec && dec->colorspace != AVCOL_SPC_UNSPECIFIED ? dec->colorspace : AVCOL_SPC_BT709;
  if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  AVDictionary* opts = nullptr;
  if (!simple) SetQualityOptions(name, target.background, &opts);
  if (hw) av_dict_set(&opts, "hw_encoding", "1", 0);
  int err = avcodec_open2(c, codec, &opts);
  av_dict_free(&opts);
  if (err < 0) {
    Log("%s: no se pudo abrir %s (%s): %s", tag, id.c_str(), simple ? "simple" : "calidad", AvError(err).c_str());
    avcodec_free_context(out);
    return false;
  }
  return true;
}

bool OpenFirstH264Encoder(const std::vector<std::string>& order, const VideoTarget& target, const AVCodecContext* dec,
                          bool globalHeader, AVCodecContext** out, std::string* used, MfHold* mf, const char* tag) {
  std::vector<std::string> ids = order;
  if (ids.empty()) ids.assign(std::begin(kH264Encoders), std::end(kH264Encoders));
  for (const auto& id : ids) {
    for (bool simple : {false, true}) {
      const bool took = id.find("_mf") != std::string::npos && MfHold::Take();
      if (OpenH264Encoder(id, target, dec, globalHeader, simple, out, tag)) {
        mf->held = took;
        *used = id;
        return true;
      }
    }
  }
  return false;
}

#ifdef _WIN32
struct LivePacer::Calm {
  BackgroundMode background;
};
#else
struct LivePacer::Calm {};
#endif

LivePacer::LivePacer(std::shared_ptr<const std::atomic<bool>> live) : live_(std::move(live)) {}

LivePacer::~LivePacer() = default;

void LivePacer::Wait(double seconds, const std::atomic<bool>& cancel) {
  if (!live_ || !*live_) {
    pacing_ = false;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!pacing_) {
    // Desde aquí: lo que ya se hizo deprisa no cuenta.
    pacing_ = true;
    from_ = seconds;
    start_ = now;
    if (!calm_) {
#ifdef _WIN32
      SetThreadEcoQoS(true);
#endif
      calm_ = std::make_unique<Calm>();
    }
    return;
  }
  // Hasta una décima de adelanto: el encoder recibe tandas cortas y el hilo
  // no se despierta en cada fotograma.
  for (;;) {
    const auto at = std::chrono::steady_clock::now();
    const double ahead = (seconds - from_) - std::chrono::duration<double>(at - start_).count();
    if (ahead < 0) {
      // Va por detrás («Al revés» decodifica ventanas sin entregar nada): ese
      // retraso no se recupera después de golpe.
      from_ = seconds;
      start_ = at;
      return;
    }
    if (ahead <= 0.1 || cancel || !*live_) return;
    std::this_thread::sleep_for(std::chrono::duration<double>(std::min(ahead, 0.1)));
  }
}

bool OpenAacEncoder(int64_t bps, int channels, int sampleRate, bool globalHeader, AVCodecContext** out,
                    const char* tag) {
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec) return false;
  AVCodecContext* c = avcodec_alloc_context3(codec);
  *out = c;
  if (!c) return false;
  channels = std::clamp(channels, 1, 2);
  av_channel_layout_default(&c->ch_layout, channels);
  c->sample_fmt = AV_SAMPLE_FMT_FLTP;
  c->sample_rate = sampleRate == 44100 ? 44100 : 48000;
  c->time_base = {1, c->sample_rate};
  c->bit_rate = channels == 1 ? bps / 2 : bps;
  if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  int err = avcodec_open2(c, codec, nullptr);
  if (err < 0) {
    Log("%s: no se pudo abrir el encoder AAC: %s", tag, AvError(err).c_str());
    avcodec_free_context(out);
    return false;
  }
  return true;
}

bool OpenAacEncoder(int64_t bps, const AVCodecContext* dec, bool globalHeader, AVCodecContext** out, const char* tag) {
  return OpenAacEncoder(bps, dec->ch_layout.nb_channels, dec->sample_rate, globalHeader, out, tag);
}

} // namespace spectre
