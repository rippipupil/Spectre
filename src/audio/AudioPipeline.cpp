#include "audio/AudioPipeline.h"

#include "app/CrashHandler.h"
#include "common/Util.h"

#include <avrt.h>

#include <algorithm>
#include <cmath>

namespace spectre {

namespace {
constexpr int kFrameSize = 1024;            // muestras por frame AAC
constexpr int64_t kLatencyFrames = 4800;    // 100 ms de margen mínimo para que lleguen las fuentes
constexpr int64_t kMaxLatencyFrames = kAudioRate; // con más de 1 s de retraso se pierde audio antes que esperar
constexpr int64_t kLatencySlack = kFrameSize + kAudioRate / 50; // un frame AAC + 20 ms

// Retraso con el que se codifica: lo que tarde la fuente más lenta en
// entregar más un margen. No afecta a la sincronía (cada muestra va a su
// posición), solo evita codificar silencio en lugar de audio que aún viene.
int64_t ReadMargin(const AudioCapture* system, const AudioCapture* mic) {
  int64_t lag = std::max(system ? system->LagFrames() : 0, mic ? mic->LagFrames() : 0);
  return std::clamp(lag + kLatencySlack, kLatencyFrames, kMaxLatencyFrames);
}
} // namespace

bool AudioPipeline::Start(const AudioSettings& settings, int64_t startQpc, int firstStream, PacketSink sink) {
  Stop();
  settings_ = settings;
  startQpc_ = startQpc;
  sink_ = std::move(sink);
  produced_ = 0;
  margin_ = kLatencyFrames;
  loggedMargin_ = kLatencyFrames;
  micMuted_ = false;

  // Las dos fuentes se abren a la vez: un auricular Bluetooth lento ya no
  // suma su espera a la del otro dispositivo.
  const bool processOnly = settings.processOnly && settings.processId;
  if (settings.system) {
    system_ = std::make_unique<AudioCapture>();
    if (processOnly) system_->Begin(AudioSourceKind::ProcessLoopback, {}, settings.processId, startQpc);
    else system_->Begin(AudioSourceKind::SystemLoopback, settings.outputDeviceId, 0, startQpc);
  }
  if (settings.mic) {
    mic_ = std::make_unique<AudioCapture>();
    mic_->Begin(AudioSourceKind::Microphone, settings.micDeviceId, 0, startQpc);
  }
  if (system_) {
    bool ok = system_->WaitStarted();
    if (!ok && processOnly) ok = system_->Start(AudioSourceKind::SystemLoopback, settings.outputDeviceId, 0, startQpc);
    if (!ok) {
      Log("Audio del sistema no disponible");
      system_.reset();
    }
  }
  if (mic_ && !mic_->WaitStarted()) {
    Log("Micrófono no disponible");
    mic_.reset();
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
  systemLevel_ = micLevel_ = 0.0f;
  systemPeak_ = micPeak_ = 0.0f;
  lastLevelLog_ = startedAt_ = QpcNow();
  thread_ = std::thread([this] {
    InstallThreadCrashHandler();
    try {
      Run();
    } catch (const std::exception& e) {
      Log("Audio: excepción en el hilo de mezcla: %s", e.what());
    }
  });
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
  track.ctx->bit_rate = 192000;
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
    int64_t margin = ReadMargin(system_.get(), mic_.get());
    margin_ = margin;
    if (margin > loggedMargin_ + kAudioRate / 50) {
      loggedMargin_ = margin;
      Log("Audio: una fuente entrega con retraso, se codifica %.0f ms por detrás", double(margin) * 1000.0 / kAudioRate);
    }
    int64_t target = (QpcNow() - startQpc_) * kAudioRate / freq - margin;
    while (running_ && target - produced_ >= kFrameSize) EncodeChunk();
    // Nivel de cada fuente: el medidor lo lee en cada frame y el log lo
    // registra cada 10 s el primer minuto y luego cada 5 min, para no llenarlo.
    // Un pico a 0 con el juego sonando delata que se escucha el dispositivo
    // equivocado.
    float sys = system_ ? system_->TakePeak() : 0.0f;
    float mic = mic_ ? mic_->TakePeak() : 0.0f;
    systemLevel_ = std::max(sys, systemLevel_.load() * 0.85f);
    micLevel_ = std::max(mic, micLevel_.load() * 0.85f);
    systemPeak_ = std::max(systemPeak_, sys);
    micPeak_ = std::max(micPeak_, mic);
    int64_t now = QpcNow();
    int64_t interval = now - startedAt_ < freq * 60 ? freq * 10 : freq * 300;
    if (now - lastLevelLog_ > interval) {
      lastLevelLog_ = now;
      auto db = [](float peak) { return peak > 1e-5f ? 20.0f * std::log10(peak) : -100.0f; };
      Log("Audio nivel: sistema %.0f dB, micrófono %.0f dB", db(systemPeak_), db(micPeak_));
      systemPeak_ = micPeak_ = 0.0f;
    }
  }
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void AudioPipeline::EncodeChunk() {
  // Cada fuente entrega exactamente las muestras de [produced_, produced_ +
  // kFrameSize) en la línea de tiempo común; lo que no llegó es silencio, así
  // que el pts y el contenido siempre coinciden.
  std::fill(sysBuf_.begin(), sysBuf_.end(), 0.0f);
  std::fill(micBuf_.begin(), micBuf_.end(), 0.0f);
  if (system_) system_->ReadAt(produced_, sysBuf_.data(), kFrameSize);
  if (mic_) mic_->ReadAt(produced_, micBuf_.data(), kFrameSize);

  const float micGain = micMuted_ ? 0.0f : settings_.micVolume;
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
        l += micBuf_[i * 2] * micGain;
        r += micBuf_[i * 2 + 1] * micGain;
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
