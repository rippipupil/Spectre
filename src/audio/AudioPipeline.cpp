#include "audio/AudioPipeline.h"

#include "common/Util.h"

#include <avrt.h>

#include <algorithm>

namespace spectre {

namespace {
constexpr int kFrameSize = 1024;            // muestras por frame AAC
constexpr int64_t kLatencyFrames = 4800;    // 100 ms de margen para que lleguen las fuentes
} // namespace

bool AudioPipeline::Start(const AudioSettings& settings, int64_t startQpc, int firstStream, PacketSink sink) {
  Stop();
  settings_ = settings;
  startQpc_ = startQpc;
  sink_ = std::move(sink);
  produced_ = 0;

  if (settings.system) {
    system_ = std::make_unique<AudioCapture>();
    bool ok = false;
    if (settings.processOnly && settings.processId)
      ok = system_->Start(AudioSourceKind::ProcessLoopback, {}, settings.processId, startQpc);
    if (!ok) ok = system_->Start(AudioSourceKind::SystemLoopback, settings.outputDeviceId, 0, startQpc);
    if (!ok) {
      Log("Audio del sistema no disponible");
      system_.reset();
    }
  }
  if (settings.mic) {
    mic_ = std::make_unique<AudioCapture>();
    if (!mic_->Start(AudioSourceKind::Microphone, settings.micDeviceId, 0, startQpc)) {
      Log("Micrófono no disponible");
      mic_.reset();
    }
  }
  if (!system_ && !mic_) return false;

  unsigned all = (system_ ? 1u : 0u) | (mic_ ? 2u : 0u);
  tracks_.push_back({nullptr, all, firstStream});
  if (settings.separateTracks && system_ && mic_) {
    tracks_.push_back({nullptr, 1u, firstStream + 1});
    tracks_.push_back({nullptr, 2u, firstStream + 2});
  }
  for (auto& t : tracks_) {
    const char* title = t.sourceMask == 3 ? "Mezcla" : t.sourceMask == 1 ? "Sistema" : "Micrófono";
    if (!OpenTrack(t, title)) {
      Stop();
      return false;
    }
  }

  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  sysBuf_.assign(kFrameSize * kAudioChannels, 0.0f);
  micBuf_.assign(kFrameSize * kAudioChannels, 0.0f);
  running_ = true;
  thread_ = std::thread([this] { Run(); });
  Log("Audio: %zu pista(s), sistema=%d micrófono=%d", tracks_.size(), system_ ? 1 : 0, mic_ ? 1 : 0);
  return true;
}

bool AudioPipeline::OpenTrack(Track& track, const char* title) {
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec) return false;
  track.ctx = avcodec_alloc_context3(codec);
  track.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
  track.ctx->sample_rate = kAudioRate;
  av_channel_layout_default(&track.ctx->ch_layout, kAudioChannels);
  track.ctx->bit_rate = 160000;
  track.ctx->time_base = {1, kAudioRate};
  track.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  int err = avcodec_open2(track.ctx, codec, nullptr);
  if (err < 0) {
    Log("No se pudo abrir AAC: %s", AvError(err).c_str());
    return false;
  }
  streams_.push_back(MakeStreamDesc(track.ctx, false, title));
  return true;
}

void AudioPipeline::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (system_) system_->Stop();
  if (mic_) mic_->Stop();
  for (auto& t : tracks_) {
    if (t.ctx && packet_) {
      avcodec_send_frame(t.ctx, nullptr);
      while (avcodec_receive_packet(t.ctx, packet_) == 0) {
        if (sink_) sink_(packet_, t.streamIndex);
        av_packet_unref(packet_);
      }
    }
    avcodec_free_context(&t.ctx);
  }
  tracks_.clear();
  streams_.clear();
  system_.reset();
  mic_.reset();
  av_frame_free(&frame_);
  av_packet_free(&packet_);
}

void AudioPipeline::Run() {
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
  const int64_t freq = QpcFrequency();
  while (running_) {
    Sleep(10);
    int64_t target = (QpcNow() - startQpc_) * kAudioRate / freq - kLatencyFrames;
    while (running_ && target - produced_ >= kFrameSize) EncodeChunk();
  }
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void AudioPipeline::EncodeChunk() {
  // Si una fuente va retrasada se rellena con silencio: el reloj manda.
  std::fill(sysBuf_.begin(), sysBuf_.end(), 0.0f);
  std::fill(micBuf_.begin(), micBuf_.end(), 0.0f);
  if (system_) system_->Read(sysBuf_.data(), kFrameSize);
  if (mic_) mic_->Read(micBuf_.data(), kFrameSize);

  for (auto& t : tracks_) {
    frame_->nb_samples = kFrameSize;
    frame_->format = AV_SAMPLE_FMT_FLTP;
    frame_->sample_rate = kAudioRate;
    av_channel_layout_copy(&frame_->ch_layout, &t.ctx->ch_layout);
    if (av_frame_get_buffer(frame_, 0) < 0) return;
    auto* left = reinterpret_cast<float*>(frame_->data[0]);
    auto* right = reinterpret_cast<float*>(frame_->data[1]);
    for (int i = 0; i < kFrameSize; ++i) {
      float l = 0, r = 0;
      if (t.sourceMask & 1) {
        l += sysBuf_[i * 2] * settings_.systemVolume;
        r += sysBuf_[i * 2 + 1] * settings_.systemVolume;
      }
      if (t.sourceMask & 2) {
        l += micBuf_[i * 2] * settings_.micVolume;
        r += micBuf_[i * 2 + 1] * settings_.micVolume;
      }
      left[i] = std::clamp(l, -1.0f, 1.0f);
      right[i] = std::clamp(r, -1.0f, 1.0f);
    }
    frame_->pts = produced_;
    if (avcodec_send_frame(t.ctx, frame_) == 0) {
      while (avcodec_receive_packet(t.ctx, packet_) == 0) {
        if (sink_) sink_(packet_, t.streamIndex);
        av_packet_unref(packet_);
      }
    }
    av_frame_unref(frame_);
  }
  produced_ += kFrameSize;
}

} // namespace spectre
