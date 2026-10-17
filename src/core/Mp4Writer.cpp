#include "core/Mp4Writer.h"

#include "app/CrashHandler.h"
#include "common/Util.h"

namespace spectre {

bool Mp4Writer::Open(const std::string& path, const StreamList& streams, bool fragmented) {
  Close();
  streams_ = streams;
  int err = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", path.c_str());
  if (err < 0 || !fmt_) {
    Log("MP4: no se pudo crear el contexto: %s", AvError(err).c_str());
    return false;
  }
  bool firstAudio = true;
  for (const auto& s : streams_) {
    AVStream* st = avformat_new_stream(fmt_, nullptr);
    avcodec_parameters_copy(st->codecpar, s.params.get());
    st->codecpar->codec_tag = 0;
    if (s.params->codec_id == AV_CODEC_ID_HEVC) st->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    st->time_base = s.timeBase;
    if (!s.title.empty()) {
      av_dict_set(&st->metadata, "title", s.title.c_str(), 0);
      av_dict_set(&st->metadata, "handler_name", s.title.c_str(), 0);
    }
    if (s.video || firstAudio) st->disposition |= AV_DISPOSITION_DEFAULT;
    if (!s.video) firstAudio = false;
  }
  av_dict_set(&fmt_->metadata, "encoder", "Spectre", 0);

  err = avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE);
  if (err < 0) {
    Log("MP4: no se pudo abrir %s: %s", path.c_str(), AvError(err).c_str());
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    return false;
  }
  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "movflags", fragmented ? "frag_keyframe+empty_moov+default_base_moof" : "+faststart", 0);
  err = avformat_write_header(fmt_, &opts);
  av_dict_free(&opts);
  if (err < 0) {
    Log("MP4: write_header falló: %s", AvError(err).c_str());
    avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    return false;
  }
  headerWritten_ = true;
  offsets_.assign(streams_.size(), 0);
  lastDts_.assign(streams_.size(), AV_NOPTS_VALUE);
  haveOrigin_ = false;
  writtenSeconds_ = 0;
  tmp_ = av_packet_alloc();
  return true;
}

bool Mp4Writer::Write(const AVPacket* packet, int stream) {
  if (!fmt_ || stream < 0 || stream >= int(streams_.size())) return false;
  const auto& desc = streams_[size_t(stream)];
  if (!haveOrigin_) {
    // El primer paquete debe ser un keyframe de vídeo: fija t = 0.
    if (stream != 0 || !(packet->flags & AV_PKT_FLAG_KEY)) return true;
    int64_t origin = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    for (size_t i = 0; i < streams_.size(); ++i)
      offsets_[i] = av_rescale_q(origin, streams_[0].timeBase, streams_[i].timeBase);
    haveOrigin_ = true;
  }

  av_packet_ref(tmp_, packet);
  int64_t off = offsets_[size_t(stream)];
  if (tmp_->pts != AV_NOPTS_VALUE) tmp_->pts -= off;
  if (tmp_->dts != AV_NOPTS_VALUE) tmp_->dts -= off;
  if (tmp_->dts == AV_NOPTS_VALUE) tmp_->dts = tmp_->pts;
  if (tmp_->pts < 0 || tmp_->dts < 0 ||
      (lastDts_[size_t(stream)] != AV_NOPTS_VALUE && tmp_->dts <= lastDts_[size_t(stream)])) {
    av_packet_unref(tmp_);
    return true; // anterior al inicio o desordenado: se descarta
  }
  lastDts_[size_t(stream)] = tmp_->dts;
  if (tmp_->duration == 0) tmp_->duration = desc.video ? 1 : 1024;
  if (stream == 0) writtenSeconds_ = double(tmp_->pts) * av_q2d(desc.timeBase);

  AVStream* st = fmt_->streams[stream];
  av_packet_rescale_ts(tmp_, desc.timeBase, st->time_base);
  tmp_->stream_index = stream;
  int err = av_interleaved_write_frame(fmt_, tmp_);
  av_packet_unref(tmp_);
  if (err < 0) {
    Log("MP4: error escribiendo: %s", AvError(err).c_str());
    return false;
  }
  return true;
}

bool Mp4Writer::Close() {
  bool ok = true;
  if (fmt_) {
    if (headerWritten_) ok = av_write_trailer(fmt_) >= 0;
    avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
  }
  headerWritten_ = false;
  av_packet_free(&tmp_);
  return ok;
}

bool WriteClip(const std::string& path, const StreamList& streams,
               const std::vector<std::pair<PacketPtr, int>>& packets) {
  if (packets.empty()) return false;
  Mp4Writer writer;
  if (!writer.Open(path, streams, false)) return false;
  for (const auto& [pkt, stream] : packets)
    if (!writer.Write(pkt.get(), stream)) return false;
  return writer.Close();
}

bool ProbeDuration(const std::string& path, double* seconds) {
  AVFormatContext* in = nullptr;
  if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0) return false;
  bool ok = avformat_find_stream_info(in, nullptr) >= 0 && in->duration > 0;
  if (ok) *seconds = double(in->duration) / AV_TIME_BASE;
  avformat_close_input(&in);
  return ok;
}

bool TrimFile(const std::string& input, const std::string& output, double start, double end, std::string* error) {
  AVFormatContext* in = nullptr;
  int err = avformat_open_input(&in, input.c_str(), nullptr, nullptr);
  if (err < 0) {
    if (error) *error = "No se pudo abrir el clip: " + AvError(err);
    return false;
  }
  avformat_find_stream_info(in, nullptr);
  int video = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) {
    avformat_close_input(&in);
    if (error) *error = "El archivo no tiene vídeo";
    return false;
  }

  // Reordenamos para que el vídeo sea la pista 0, como espera Mp4Writer.
  StreamList streams;
  std::vector<int> map(in->nb_streams, -1);
  auto addStream = [&](unsigned i) {
    AVStream* st = in->streams[i];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return;
    StreamDesc d;
    AVCodecParameters* par = avcodec_parameters_alloc();
    avcodec_parameters_copy(par, st->codecpar);
    d.params = std::shared_ptr<AVCodecParameters>(par, CodecParDeleter{});
    d.timeBase = st->time_base;
    d.video = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    if (auto* t = av_dict_get(st->metadata, "title", nullptr, 0)) d.title = t->value;
    else if (auto* h = av_dict_get(st->metadata, "handler_name", nullptr, 0)) d.title = h->value;
    map[i] = int(streams.size());
    streams.push_back(std::move(d));
  };
  addStream(unsigned(video));
  for (unsigned i = 0; i < in->nb_streams; ++i)
    if (int(i) != video) addStream(i);

  int64_t seekTs = av_rescale_q(int64_t(start * AV_TIME_BASE), AV_TIME_BASE_Q, in->streams[video]->time_base);
  av_seek_frame(in, video, seekTs, AVSEEK_FLAG_BACKWARD);

  Mp4Writer writer;
  if (!writer.Open(output, streams, false)) {
    avformat_close_input(&in);
    if (error) *error = "No se pudo crear el archivo recortado";
    return false;
  }
  AVPacket* pkt = av_packet_alloc();
  bool ok = true;
  while (av_read_frame(in, pkt) >= 0) {
    int out = map[size_t(pkt->stream_index)];
    if (out >= 0) {
      int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
      double t = ts == AV_NOPTS_VALUE ? 0 : double(ts) * av_q2d(in->streams[pkt->stream_index]->time_base);
      if (t > end) {
        if (out == 0) {
          av_packet_unref(pkt);
          break;
        }
      } else if (!writer.Write(pkt, out)) {
        ok = false;
        av_packet_unref(pkt);
        break;
      }
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  ok = writer.Close() && ok;
  avformat_close_input(&in);
  if (!ok && error) *error = "Error al escribir el recorte";
  return ok;
}

bool Recorder::Start(const std::string& path, const StreamList& streams) {
  Stop();
  if (!writer_.Open(path, streams, true)) return false;
  path_ = path;
  seconds_ = 0;
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
  }
  running_ = true;
  thread_ = std::thread([this] {
    InstallThreadCrashHandler();
    Run();
  });
  Log("Grabación iniciada: %s", path.c_str());
  return true;
}

void Recorder::Push(const AVPacket* packet, int stream) {
  if (!running_) return;
  {
    std::lock_guard lock(mutex_);
    queue_.emplace_back(ClonePacket(packet), stream);
  }
  cv_.notify_one();
}

void Recorder::Run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    std::deque<std::pair<PacketPtr, int>> batch;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return !queue_.empty() || !running_; });
      batch.swap(queue_);
      if (batch.empty() && !running_) break;
    }
    for (auto& [pkt, stream] : batch) writer_.Write(pkt.get(), stream);
    seconds_ = writer_.WrittenSeconds();
  }
}

void Recorder::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lock(mutex_);
    running_ = false;
  }
  cv_.notify_one();
  thread_.join();
  writer_.Close();
  Log("Grabación terminada: %s (%.1f s)", path_.c_str(), seconds_.load());
}

} // namespace spectre
