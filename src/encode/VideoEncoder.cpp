#include "encode/VideoEncoder.h"

#include "common/Util.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cstring>

namespace spectre {

struct EncoderCandidate {
  std::string id;    // nombre en la configuración y en el log
  std::string codec; // encoder de FFmpeg
  std::string label;
  bool zeroCopy = false; // acepta texturas D3D11 directamente
  bool hardware = true;  // false: codifica en la CPU y le quita núcleos al juego
  bool mfHardware = false; // Media Foundation con el MFT del driver (hw_encoding=1)
};

namespace {

using Candidate = EncoderCandidate;

// Primero todo lo que codifica en la GPU y, al final, la CPU. El MFT del
// driver va justo después de los encoders nativos: funciona aunque FFmpeg no
// pueda abrir NVENC o AMF (driver anterior al que piden sus cabeceras, runtime
// de AMF que falta) y evita caer a la CPU, que es lo que da lag en el juego.
// HEVC y AV1 no tienen opción por CPU: antes que eso, H.264 por GPU.
const std::vector<Candidate>& CandidatesFor(const std::string& codec) {
  static const std::vector<Candidate> h264 = {
      {"h264_nvenc", "h264_nvenc", "NVIDIA NVENC (H.264)", true},
      {"h264_amf", "h264_amf", "AMD AMF (H.264)", true},
      {"h264_qsv", "h264_qsv", "Intel QuickSync (H.264)"},
      {"h264_mf_hw", "h264_mf", "Media Foundation por GPU (H.264)", false, true, true},
      {"libopenh264", "libopenh264", "OpenH264 por CPU", false, false},
      {"h264_mf", "h264_mf", "Media Foundation por CPU (H.264)", false, false},
  };
  static const std::vector<Candidate> hevc = {
      {"hevc_nvenc", "hevc_nvenc", "NVIDIA NVENC (HEVC)", true},
      {"hevc_amf", "hevc_amf", "AMD AMF (HEVC)", true},
      {"hevc_qsv", "hevc_qsv", "Intel QuickSync (HEVC)"},
      {"hevc_mf_hw", "hevc_mf", "Media Foundation por GPU (HEVC)", false, true, true},
  };
  static const std::vector<Candidate> av1 = {
      {"av1_nvenc", "av1_nvenc", "NVIDIA NVENC (AV1)", true},
      {"av1_amf", "av1_amf", "AMD AMF (AV1)", true},
      {"av1_qsv", "av1_qsv", "Intel QuickSync (AV1)"},
  };
  if (codec == "hevc") return hevc;
  if (codec == "av1") return av1;
  return h264;
}

// Un id de la lista o, para configuraciones antiguas, un encoder de FFmpeg
// por su nombre con sus opciones por defecto.
Candidate ResolveCandidate(const std::string& id) {
  for (const char* codec : {"h264", "hevc", "av1"})
    for (const auto& c : CandidatesFor(codec))
      if (id == c.id) return c;
  Candidate c{id, id, id};
  c.hardware = id.find("nvenc") != std::string::npos || id.find("amf") != std::string::npos ||
               id.find("qsv") != std::string::npos;
  return c;
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
  if (!fmts) return true; // el códec no declara formatos
  for (; *fmts != AV_PIX_FMT_NONE; ++fmts)
    if (*fmts == fmt) return true;
  return false;
}

void LockGpu(void*) { GpuMutex().lock(); }
void UnlockGpu(void*) { GpuMutex().unlock(); }

} // namespace

std::vector<EncoderInfo> CandidateEncoders(const std::string& codec) {
  std::vector<EncoderInfo> out;
  for (const auto& c : CandidatesFor(codec))
    if (avcodec_find_encoder_by_name(c.codec.c_str())) out.push_back({c.id, c.label});
  return out;
}

bool VideoEncoder::Open(const VideoEncoderSettings& settings, ID3D11Device* device) {
  Close();
  device_ = device;
  device_->GetImmediateContext(&context_);
  packet_ = av_packet_alloc();

  std::vector<Candidate> order;
  auto add = [&](const Candidate& c) {
    for (const auto& o : order)
      if (o.id == c.id) return;
    order.push_back(c);
  };
  if (settings.preferred != "auto" && !settings.preferred.empty()) add(ResolveCandidate(settings.preferred));
  for (const auto& c : CandidatesFor(settings.codec)) add(c);
  // HEVC/AV1 no disponibles: se vuelve a H.264, que funciona en todas partes.
  if (settings.codec != "h264")
    for (const auto& c : CandidatesFor("h264")) add(c);

  bool triedHardware = false;
  for (const auto& cand : order) {
    const AVCodec* codec = avcodec_find_encoder_by_name(cand.codec.c_str());
    if (!codec) continue;
    if (!cand.hardware && triedHardware)
      Log("AVISO: ningún encoder por GPU se pudo abrir; se prueba %s, que codifica en la CPU y puede dar lag en "
          "el juego (el motivo de cada fallo está en las líneas «ffmpeg:» de arriba)",
          cand.id.c_str());
    triedHardware |= cand.hardware;
    // Primero con las opciones de calidad; si el driver las rechaza, el mismo
    // encoder en modo simple antes que bajar a uno peor.
    bool ok = TryOpen(codec, cand, settings, false);
    if (!ok) {
      Log("%s no aceptó las opciones de calidad; se abre en modo simple", cand.id.c_str());
      ok = TryOpen(codec, cand, settings, true);
    }
    if (ok) {
      name_ = cand.id;
      label_ = cand.label;
      software_ = !cand.hardware;
      Log("Encoder de vídeo: %s %dx%d@%d, %d kbps (tope %d, cq %d) (%s%s)", name_.c_str(), settings.width,
          settings.height, settings.fps, settings.quality.targetKbps, settings.quality.maxKbps,
          settings.quality.cq, ZeroCopy() ? "cero copias" : "memoria de sistema", software_ ? ", CPU" : "");
      return true;
    }
  }
  Log("Ningún encoder de vídeo se pudo abrir");
  Close();
  return false;
}

bool VideoEncoder::TryOpen(const AVCodec* codec, const Candidate& cand, const VideoEncoderSettings& s,
                           bool simple) {
  const bool zeroCopy = cand.zeroCopy && SupportsPixFmt(codec, AV_PIX_FMT_D3D11);
  const QualityPlan& q = s.quality;
  const int level = q.level < 0 ? 2 : q.level; // personalizada usa las opciones de "alta"
  const int64_t target = int64_t(q.targetKbps) * 1000;
  const int64_t cap = int64_t(std::max(q.maxKbps, q.targetKbps)) * 1000;

  ctx_ = avcodec_alloc_context3(codec);
  ctx_->width = s.width;
  ctx_->height = s.height;
  ctx_->time_base = {1, s.fps};
  ctx_->framerate = {s.fps, 1};
  // Keyframe cada 2 s: los clips se cortan sin recodificar y, frente a uno
  // por segundo, sobran bits para el resto de fotogramas.
  ctx_->gop_size = s.fps * 2;
  ctx_->max_b_frames = 0;
  ctx_->bit_rate = target;
  ctx_->rc_max_rate = cap;
  ctx_->rc_buffer_size = int(std::min<int64_t>(cap * 2, INT32_MAX));
  ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  ctx_->color_range = AVCOL_RANGE_MPEG;
  ctx_->color_primaries = AVCOL_PRI_BT709;
  ctx_->color_trc = AVCOL_TRC_BT709;
  ctx_->colorspace = AVCOL_SPC_BT709;

  AVDictionary* opts = nullptr;
  std::string n = codec->name;
  const std::string cq = std::to_string(q.cq);
  if (n.find("nvenc") != std::string::npos) {
    av_dict_set(&opts, "forced-idr", "1", 0);
    // Con delay 0, cada Encode() esperaba a que NVENC terminase ese frame
    // (nvEncLockBitstream bloqueante): con la GPU llena por el juego esa
    // espera se comía la ranura siguiente y el clip salía a saltos. Con 2, el
    // paquete de un frame se recoge al enviar el siguiente, cuando ya está
    // hecho. Llega un frame más tarde al anillo; SaveClip ya espera más.
    av_dict_set(&opts, "delay", "2", 0);
    av_dict_set(&opts, "rc-lookahead", "0", 0);
    av_dict_set(&opts, "tune", "hq", 0);
    if (simple) {
      av_dict_set(&opts, "preset", "p4", 0);
      av_dict_set(&opts, "rc", "cbr", 0);
      ctx_->rc_max_rate = ctx_->bit_rate;
    } else {
      // VBR con calidad objetivo: las escenas fáciles ocupan poco y las de
      // mucho movimiento reciben bits hasta el tope. AQ espacial reparte los
      // bits donde la imagen es más compleja; multipass afina el control de
      // tasa a un cuarto de resolución, casi gratis en NVENC.
      static const char* kPreset[] = {"p4", "p4", "p5", "p6"};
      av_dict_set(&opts, "preset", kPreset[level], 0);
      av_dict_set(&opts, "rc", "vbr", 0);
      av_dict_set(&opts, "spatial-aq", "1", 0);
      av_dict_set(&opts, "aq-strength", "8", 0);
      if (level >= 2) av_dict_set(&opts, "multipass", "qres", 0);
      // Bitrate manual: VBR clásico alrededor del valor elegido. Preajuste:
      // calidad objetivo (cq) y NVENC decide el bitrate hasta el tope.
      if (q.level >= 0) {
        av_dict_set(&opts, "cq", cq.c_str(), 0);
        ctx_->bit_rate = 0;
      }
    }
  } else if (n.find("amf") != std::string::npos) {
    // Por defecto AMF admite 16 frames en vuelo, cada uno con su textura NV12
    // del pool: ~50 MB de VRAM a 1080p y ~200 MB a 4K que el juego no puede
    // usar. Con 4 sobra para que el VCN no espere y, si se atasca, el hilo de
    // vídeo se frena en vez de acumular frames.
    av_dict_set(&opts, "async_depth", "4", 0);
    if (simple) {
      av_dict_set(&opts, "usage", "lowlatency", 0);
      av_dict_set(&opts, "quality", "speed", 0);
      av_dict_set(&opts, "rc", "cbr", 0);
      ctx_->rc_max_rate = ctx_->bit_rate;
    } else {
      static const char* kQuality[] = {"speed", "balanced", "balanced", "quality"};
      av_dict_set(&opts, "usage", "transcoding", 0);
      av_dict_set(&opts, "quality", kQuality[level], 0);
      av_dict_set(&opts, "rc", "vbr_peak", 0);
      av_dict_set(&opts, "enforce_hrd", "0", 0);
      if (level >= 1) av_dict_set(&opts, "vbaq", "1", 0);
    }
  } else if (n.find("qsv") != std::string::npos) {
    av_dict_set(&opts, "look_ahead", "0", 0);
    if (simple) {
      av_dict_set(&opts, "preset", "veryfast", 0);
      ctx_->rc_max_rate = ctx_->bit_rate;
    } else {
      static const char* kPreset[] = {"veryfast", "faster", "medium", "medium"};
      av_dict_set(&opts, "preset", kPreset[level], 0);
    }
  } else if (n.find("_mf") != std::string::npos) {
    // hw_encoding=1: el MFT del fabricante (NVENC, VCN o QuickSync por dentro);
    // sin él, FFmpeg usa el encoder de Microsoft en la CPU. En la GPU, perfil
    // High; en la CPU se queda en Baseline, que cuesta menos.
    if (cand.mfHardware) {
      av_dict_set(&opts, "hw_encoding", "1", 0);
      if (codec->id == AV_CODEC_ID_H264) ctx_->profile = AV_PROFILE_H264_HIGH;
    }
    if (simple) {
      av_dict_set(&opts, "rate_control", "cbr", 0);
      av_dict_set(&opts, "scenario", "live_streaming", 0);
      ctx_->rc_max_rate = ctx_->bit_rate;
    } else {
      av_dict_set(&opts, "rate_control", "pc_vbr", 0);
      av_dict_set(&opts, "scenario", "archive", 0);
    }
  } else if (n.find("openh264") != std::string::npos) {
    // Software: no hay opciones de calidad que no cuesten CPU. Se usa el tope.
    ctx_->bit_rate = cap;
  }

  bool ok = false;
  if (zeroCopy) {
    ok = SetupHwFrames(s);
    if (ok) {
      ctx_->pix_fmt = AV_PIX_FMT_D3D11;
      ctx_->hw_frames_ctx = av_buffer_ref(hwFrames_);
    }
  } else {
    swFormat_ = SupportsPixFmt(codec, AV_PIX_FMT_NV12) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    ctx_->pix_fmt = swFormat_;
    ok = SetupStaging(s);
  }

  if (ok) {
    int err = avcodec_open2(ctx_, codec, &opts);
    if (err < 0) {
      Log("No se pudo abrir %s (%s): %s", codec->name, simple ? "simple" : "calidad", AvError(err).c_str());
      ok = false;
    }
  }
  av_dict_free(&opts);
  if (!ok) {
    avcodec_free_context(&ctx_);
    av_buffer_unref(&hwFrames_);
    av_buffer_unref(&hwDevice_);
    av_buffer_pool_uninit(&framePool_);
    nv12_.Reset();
    for (auto& st : staging_) st.Reset();
  }
  return ok;
}

bool VideoEncoder::SetupHwFrames(const VideoEncoderSettings& s) {
  hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!hwDevice_) return false;
  auto* dc = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(dc->hwctx);
  d3d->device = device_.Get();
  d3d->device->AddRef();
  d3d->lock = LockGpu;
  d3d->unlock = UnlockGpu;
  d3d->lock_ctx = nullptr;
  int err = av_hwdevice_ctx_init(hwDevice_);
  if (err < 0) {
    Log("av_hwdevice_ctx_init: %s", AvError(err).c_str());
    return false;
  }

  hwFrames_ = av_hwframe_ctx_alloc(hwDevice_);
  if (!hwFrames_) return false;
  auto* fc = reinterpret_cast<AVHWFramesContext*>(hwFrames_->data);
  fc->format = AV_PIX_FMT_D3D11;
  fc->sw_format = AV_PIX_FMT_NV12;
  fc->width = s.width;
  fc->height = s.height;
  fc->initial_pool_size = 0;
  auto* fh = static_cast<AVD3D11VAFramesContext*>(fc->hwctx);
  // RENDER_TARGET: el VideoProcessor escribe directamente en la textura del encoder.
  fh->BindFlags = D3D11_BIND_RENDER_TARGET;
  err = av_hwframe_ctx_init(hwFrames_);
  if (err < 0) {
    Log("av_hwframe_ctx_init: %s", AvError(err).c_str());
    return false;
  }
  return true;
}

bool VideoEncoder::SetupStaging(const VideoEncoderSettings& s) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = UINT(s.width);
  desc.Height = UINT(s.height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, &nv12_))) return false;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto& st : staging_)
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &st))) return false;
  // Los frames para el encoder salen de un pool: av_frame_get_buffer pedía
  // ~3 MB nuevos por frame a 1080p, que Windows sirve con VirtualAlloc y
  // páginas a cero (decenas de miles de fallos de página por segundo).
  int size = av_image_get_buffer_size(swFormat_, s.width, s.height, kFrameAlign);
  if (size <= 0) return false;
  framePool_ = av_buffer_pool_init(size_t(size), nullptr);
  if (!framePool_) return false;
  stagingPending_.fill(false);
  frameCounter_ = 0;
  return true;
}

void VideoEncoder::Close() {
  avcodec_free_context(&ctx_);
  av_buffer_unref(&hwFrames_);
  av_buffer_unref(&hwDevice_);
  av_buffer_pool_uninit(&framePool_); // los frames que aún tenga el encoder se liberan al soltarlos
  av_packet_free(&packet_);
  nv12_.Reset();
  for (auto& st : staging_) st.Reset();
  stagingPending_.fill(false);
  context_.Reset();
  device_.Reset();
  name_.clear();
  label_.clear();
  software_ = false;
}

bool VideoEncoder::Encode(int64_t pts, const RenderFn& render, const PacketSink& sink) {
  if (!ctx_) return false;

  if (hwFrames_) {
    AVFrame* frame = av_frame_alloc();
    if (av_hwframe_get_buffer(hwFrames_, frame, 0) < 0) {
      av_frame_free(&frame);
      return false;
    }
    auto* tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    UINT slice = UINT(reinterpret_cast<intptr_t>(frame->data[1]));
    bool rendered;
    {
      GpuLock lock(GpuMutex());
      rendered = render(tex, slice);
      context_->Flush();
    }
    if (!rendered) {
      av_frame_free(&frame);
      return false;
    }
    frame->pts = pts;
    bool ok = Send(frame, sink);
    av_frame_free(&frame);
    return ok;
  }

  // Memoria de sistema: render -> copia a staging[i]; se lee staging[i-2]
  // para que la GPU tenga tiempo de terminar sin bloquear este hilo.
  int index = int(frameCounter_ % kStaging);
  if (stagingPending_[index]) SendStaging(index, sink);
  {
    GpuLock lock(GpuMutex());
    if (!render(nv12_.Get(), 0)) return false;
    context_->CopyResource(staging_[index].Get(), nv12_.Get());
  }
  stagingPts_[index] = pts;
  stagingPending_[index] = true;
  frameCounter_++;
  int ready = int((frameCounter_ + 1) % kStaging); // el más antiguo pendiente
  if (stagingPending_[ready]) return SendStaging(ready, sink);
  return true;
}

bool VideoEncoder::SendStaging(int index, const PacketSink& sink) {
  stagingPending_[index] = false;
  AVFrame* frame = av_frame_alloc();
  frame->format = swFormat_;
  frame->width = ctx_->width;
  frame->height = ctx_->height;
  frame->buf[0] = av_buffer_pool_get(framePool_);
  if (!frame->buf[0] || av_image_fill_arrays(frame->data, frame->linesize, frame->buf[0]->data, swFormat_,
                                             ctx_->width, ctx_->height, kFrameAlign) < 0) {
    av_frame_free(&frame);
    return false;
  }
  // Solo Map y Unmap van con GpuMutex: la copia a memoria (1-2 ms a 1080p) ya
  // no frena a la captura ni a la cámara. El puntero sigue siendo válido hasta
  // Unmap y la textura staging solo la usa este hilo.
  D3D11_MAPPED_SUBRESOURCE m;
  {
    GpuLock lock(GpuMutex());
    if (FAILED(context_->Map(staging_[index].Get(), 0, D3D11_MAP_READ, 0, &m))) {
      av_frame_free(&frame);
      return false;
    }
  }
  const auto* y = static_cast<const uint8_t*>(m.pData);
  const auto* uv = y + size_t(m.RowPitch) * ctx_->height;
  av_image_copy_plane(frame->data[0], frame->linesize[0], y, int(m.RowPitch), ctx_->width, ctx_->height);
  if (swFormat_ == AV_PIX_FMT_NV12) {
    av_image_copy_plane(frame->data[1], frame->linesize[1], uv, int(m.RowPitch), ctx_->width, ctx_->height / 2);
  } else {
    // YUV420P (OpenH264): separar U y V.
    for (int row = 0; row < ctx_->height / 2; ++row) {
      const uint8_t* src = uv + size_t(row) * m.RowPitch;
      uint8_t* u = frame->data[1] + size_t(row) * frame->linesize[1];
      uint8_t* v = frame->data[2] + size_t(row) * frame->linesize[2];
      for (int col = 0; col < ctx_->width / 2; ++col) {
        u[col] = src[col * 2];
        v[col] = src[col * 2 + 1];
      }
    }
  }
  {
    GpuLock lock(GpuMutex());
    context_->Unmap(staging_[index].Get(), 0);
  }
  frame->pts = stagingPts_[index];
  bool ok = Send(frame, sink);
  av_frame_free(&frame);
  return ok;
}

bool VideoEncoder::Send(AVFrame* frame, const PacketSink& sink) {
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_BT709;
  frame->colorspace = AVCOL_SPC_BT709;
  int err = avcodec_send_frame(ctx_, frame);
  if (err < 0 && err != AVERROR(EAGAIN)) {
    Log("avcodec_send_frame (vídeo): %s", AvError(err).c_str());
    return false;
  }
  Drain(sink);
  return true;
}

void VideoEncoder::Drain(const PacketSink& sink) {
  while (avcodec_receive_packet(ctx_, packet_) == 0) {
    if (sink) sink(packet_, 0);
    av_packet_unref(packet_);
  }
}

void VideoEncoder::Flush(const PacketSink& sink) {
  if (!ctx_) return;
  if (!hwFrames_) {
    for (int i = 0; i < kStaging; ++i) {
      int index = int((frameCounter_ + i) % kStaging);
      if (stagingPending_[index]) SendStaging(index, sink);
    }
  }
  avcodec_send_frame(ctx_, nullptr);
  Drain(sink);
}

} // namespace spectre
