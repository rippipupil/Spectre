#include "core/Mp4Writer.h"

#include "app/CrashHandler.h"
#include "common/Util.h"
#include "core/ThreadPolicy.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>

namespace spectre {

bool Mp4Writer::Open(const std::string& path, const StreamList& streams, bool fragmented, int64_t reserveMoov) {
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
  if (fragmented)
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
  else if (reserveMoov > 0)
    av_dict_set_int(&opts, "moov_size", std::min<int64_t>(reserveMoov, INT_MAX), 0);
  else
    av_dict_set(&opts, "movflags", "+faststart", 0);
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
  firstPts_.assign(streams_.size(), AV_NOPTS_VALUE);
  lastPts_.assign(streams_.size(), AV_NOPTS_VALUE);
  dropped_.assign(streams_.size(), 0);
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
    dropped_[size_t(stream)]++;
    return true; // anterior al inicio o desordenado: se descarta
  }
  lastDts_[size_t(stream)] = tmp_->dts;
  if (firstPts_[size_t(stream)] == AV_NOPTS_VALUE) firstPts_[size_t(stream)] = tmp_->pts;
  lastPts_[size_t(stream)] = std::max(lastPts_[size_t(stream)], tmp_->pts);
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

std::string Mp4Writer::Summary() const {
  std::string out;
  for (size_t i = 0; i < streams_.size() && i < firstPts_.size(); ++i) {
    const auto& d = streams_[i];
    char buf[160];
    if (firstPts_[i] == AV_NOPTS_VALUE) {
      snprintf(buf, sizeof(buf), "%s vacía", d.title.c_str());
    } else {
      double tb = av_q2d(d.timeBase);
      snprintf(buf, sizeof(buf), "%s %+.3f..%.3f s", d.title.c_str(), double(firstPts_[i]) * tb,
               double(lastPts_[i]) * tb);
    }
    if (!out.empty()) out += ", ";
    out += buf;
    if (dropped_[i]) out += " (" + std::to_string(dropped_[i]) + " descartados)";
  }
  return out;
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
               const std::vector<std::pair<SharedPacket, int>>& packets) {
  if (packets.empty()) return false;
  // Por cada paquete el índice guarda como mucho ~45 bytes (tamaño, duración,
  // desfase de composición, posición y entrada stsc de su bloque, marca de
  // keyframe), más las cabeceras de cada pista. Lo que sobra queda como una
  // caja "free" de relleno: unos cientos de KB en un clip de decenas de MB.
  int64_t reserve = 64 * 1024 + int64_t(streams.size()) * 16 * 1024 + int64_t(packets.size()) * 48;

  // Escribir el clip no tiene prisa: con la E/S y la memoria en prioridad
  // baja, el juego lee del disco antes que nosotros y las páginas del clip son
  // las primeras que Windows suelta de la caché. Si aun así tarda más de 5 s
  // (disco muy ocupado), se sigue con prioridad normal para que el clip no se
  // quede a medias al cerrar Spectre.
  BackgroundMode background;
  ULONGLONG started = GetTickCount64();
  for (int attempt = 0; attempt < 2; ++attempt) {
    Mp4Writer writer;
    if (!writer.Open(path, streams, false, attempt == 0 ? reserve : 0)) return false;
    for (const auto& [pkt, stream] : packets) {
      if (background.Active() && GetTickCount64() - started > 5000) {
        background.Leave();
        Log("Clip: la escritura va lenta; sigue con prioridad normal");
      }
      if (!writer.Write(pkt.get(), stream)) return false;
    }
    if (attempt == 0) Log("Clip: pistas %s", writer.Summary().c_str());
    if (writer.Close()) return true;
    if (attempt == 0) Log("Clip: el índice no cupo en los %lld KB reservados; se reescribe con faststart", (long long)(reserve / 1024));
  }
  return false;
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
  staged_.clear();
  pendingBytes_ = 0;
  peakQueueBytes_ = 0;
  overflowed_ = false;
  finished_ = false;
  running_ = true;
  thread_ = std::thread([this] {
    InstallThreadCrashHandler();
    Run();
  });
  Log("Grabación iniciada: %s", path.c_str());
  return true;
}

void Recorder::Push(const AVPacket* packet, int stream) {
  if (!running_ || overflowed_) return;
  PacketPtr copy = ClonePacket(packet);
  if (!copy) return;
  size_t bytes = size_t(copy->size);
  if (pendingBytes_ + bytes > kMaxQueueBytes) {
    // El disco no da abasto: se corta aquí. El hilo de escritura vacía la cola,
    // cierra el archivo (válido hasta este punto) y lo apunta en el log; aquí
    // nada de escribir en disco, que este hilo tiene el anillo bloqueado.
    overflowed_ = true;
    cv_.notify_one();
    if (onOverflow) onOverflow();
    return;
  }
  size_t pending = pendingBytes_ += bytes;
  if (pending > peakQueueBytes_) peakQueueBytes_ = pending;
  // Nunca esperar al hilo de escritura: va con prioridad de fondo y, si el
  // juego ocupa la CPU, puede quedarse sin turno justo con el lock cogido. Los
  // hilos de captura llegan aquí con el anillo bloqueado, así que esperarle
  // pararía la captura. Si está ocupado, el paquete espera en staged_ (solo
  // lo tocan los productores, que el anillo ya pone en fila) y entra en la
  // cola con el siguiente.
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    staged_.emplace_back(std::move(copy), stream);
    return;
  }
  for (auto& item : staged_) queue_.push_back(std::move(item));
  staged_.clear();
  queue_.emplace_back(std::move(copy), stream);
  lock.unlock();
  cv_.notify_one();
}

void Recorder::Run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  SetThreadEcoQoS(true);
  // Con la cola al día, E/S y memoria en prioridad de fondo; si se acumulan
  // más de 32 MB (el juego está leyendo mucho del disco, o el disco es lento),
  // prioridad normal hasta bajar de 4 MB.
  BackgroundMode background;
  for (;;) {
    std::deque<std::pair<PacketPtr, int>> batch;
    {
      // Con plazo: Push no avisa cuando deja el paquete en staged_ ni toma el
      // lock al cortar la grabación, así que un aviso puede perderse.
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(200),
                   [&] { return !queue_.empty() || !running_ || overflowed_; });
      batch.swap(queue_);
      if (batch.empty() && (!running_ || overflowed_)) break;
    }
    size_t backlog = pendingBytes_;
    if (background.Active() && backlog > (32u << 20)) background.Leave();
    else if (!background.Active() && backlog < (4u << 20)) background.Enter();
    size_t written = 0;
    for (auto& [pkt, stream] : batch) {
      written += size_t(pkt->size);
      writer_.Write(pkt.get(), stream);
    }
    pendingBytes_ -= written;
    seconds_ = writer_.WrittenSeconds();
  }
  background.Leave();
  if (overflowed_) {
    // Se cerró por la cola: el archivo se termina aquí mismo, sin esperar a
    // que alguien llame a Stop().
    Log("Grabación: el disco no daba abasto (%zu MB en cola); se detuvo la grabación", peakQueueBytes_.load() >> 20);
    Log("Grabación: pistas %s", writer_.Summary().c_str());
    writer_.Close();
    Log("Grabación cortada: %s (%.1f s)", path_.c_str(), seconds_.load());
  }
  finished_ = true;
}

void Recorder::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lock(mutex_);
    // Lo que quedó esperando en staged_: los productores ya no llaman a Push
    // (se desengancharon del anillo antes de Stop).
    for (auto& item : staged_) queue_.push_back(std::move(item));
    staged_.clear();
    running_ = false;
  }
  cv_.notify_one();
  thread_.join();
  if (!overflowed_) {
    Log("Grabación: pistas %s", writer_.Summary().c_str());
    writer_.Close();
    Log("Grabación terminada: %s (%.1f s, cola máxima %zu MB)", path_.c_str(), seconds_.load(),
        peakQueueBytes_.load() >> 20);
  }
}

} // namespace spectre
