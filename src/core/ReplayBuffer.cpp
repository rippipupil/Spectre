#include "core/ReplayBuffer.h"

#include <algorithm>

namespace spectre {

void ReplayBuffer::Configure(const StreamList& streams, double seconds, size_t maxBytes) {
  std::lock_guard lock(mutex_);
  streams_ = streams;
  seconds_ = seconds;
  maxBytes_ = maxBytes;
  entries_.clear();
  bytes_ = 0;
  lastVideoTime_ = 0;
  Publish();
}

int ReplayBuffer::AddStream(const StreamDesc& stream) {
  std::lock_guard lock(mutex_);
  streams_.push_back(stream);
  return int(streams_.size()) - 1;
}

StreamList ReplayBuffer::Streams() {
  std::lock_guard lock(mutex_);
  return streams_;
}

void ReplayBuffer::Clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
  bytes_ = 0;
  live_ = nullptr;
  Publish();
}

void ReplayBuffer::SetLimits(double seconds, size_t maxBytes) {
  std::lock_guard lock(mutex_);
  seconds_ = seconds;
  maxBytes_ = maxBytes;
  Trim();
  Publish();
}

void ReplayBuffer::Publish() {
  publishedBytes_ = bytes_;
  publishedSeconds_ = entries_.empty() ? 0.0 : lastVideoTime_ - entries_.front().time;
  publishedMaxBytes_ = maxBytes_;
}

double ReplayBuffer::TimeOf(const AVPacket* p, int stream) const {
  int64_t ts = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
  if (ts == AV_NOPTS_VALUE || stream < 0 || stream >= int(streams_.size())) return 0;
  return double(ts) * av_q2d(streams_[size_t(stream)].timeBase);
}

void ReplayBuffer::Push(const AVPacket* packet, int stream) {
  std::lock_guard lock(mutex_);
  if (live_) live_(packet, stream);
  Entry e{SharePacket(packet), stream, TimeOf(packet, stream)};
  if (!e.packet) return;
  if (stream == 0) lastVideoTime_ = e.time;
  bytes_ += size_t(e.packet->size);
  entries_.push_back(std::move(e));
  Trim();
  Publish();
}

void ReplayBuffer::Trim() {
  auto overLimit = [&] {
    if (entries_.empty()) return false;
    return bytes_ > maxBytes_ || lastVideoTime_ - entries_.front().time > seconds_ + kExtraSeconds;
  };
  if (!overLimit()) return;
  while (overLimit()) {
    bytes_ -= size_t(entries_.front().packet->size);
    entries_.pop_front();
  }
  // El buffer siempre empieza en un keyframe de vídeo.
  while (!entries_.empty() &&
         !(entries_.front().stream == 0 && (entries_.front().packet->flags & AV_PKT_FLAG_KEY))) {
    bytes_ -= size_t(entries_.front().packet->size);
    entries_.pop_front();
  }
}

std::vector<std::pair<SharedPacket, int>> ReplayBuffer::Snapshot(double seconds, double end, double latest) {
  std::vector<Entry> picked;
  {
    std::lock_guard lock(mutex_);
    if (entries_.empty()) return {};
    end = std::min(end, lastVideoTime_);
    // El anillo guarda la duración más un margen y empieza en un keyframe: un
    // clip que acaba unos segundos antes de pedirlo (orden de voz) puede no
    // caber entero. Entonces acaba algo después, como mucho en `latest`, y
    // dura lo pedido. Con un atajo (latest = end) no cambia nada.
    const double fullEnd = entries_.front().time + seconds;
    if (latest > end && fullEnd > end) end = std::min({fullEnd, latest, lastVideoTime_});
    double start = end - seconds;
    // Último keyframe de vídeo con tiempo <= start (o el primero disponible).
    size_t first = entries_.size();
    for (size_t i = 0; i < entries_.size(); ++i) {
      const auto& e = entries_[i];
      if (e.stream == 0 && (e.packet->flags & AV_PKT_FLAG_KEY)) {
        if (first == entries_.size() || e.time <= start) first = i;
        if (e.time > start) break;
      }
    }
    if (first == entries_.size()) return {};
    picked.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i)
      if (InClip(i, first, end)) picked.push_back(entries_[i]);
  }
  std::stable_sort(picked.begin(), picked.end(), ClipOrder);
  std::vector<std::pair<SharedPacket, int>> out;
  out.reserve(picked.size());
  for (auto& e : picked) out.emplace_back(std::move(e.packet), e.stream);
  return out;
}

bool ReplayBuffer::InClip(size_t i, size_t first, double end) const {
  // Normalmente el audio entra en el anillo después que el vídeo del mismo
  // instante (~165 ms), pero si el encoder de vídeo entrega con más retraso
  // que eso puede haber audio con t >= keyframe guardado antes que él: se
  // mira todo el anillo y luego se ordena por tiempo.
  const auto& e = entries_[i];
  if (e.time > end) return false;
  return e.stream == 0 ? i >= first : e.time >= entries_[first].time;
}

bool ReplayBuffer::ClipOrder(const Entry& x, const Entry& y) {
  // Cada pista ya está en orden; a igual tiempo, el vídeo primero (el MP4
  // empieza en el keyframe).
  if (x.time != y.time) return x.time < y.time;
  return x.stream == 0 && y.stream != 0;
}

std::vector<size_t> ReplayBuffer::ClipEntries(size_t first, double end) const {
  std::vector<size_t> idx;
  for (size_t i = 0; i < entries_.size(); ++i)
    if (InClip(i, first, end)) idx.push_back(i);
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return ClipOrder(entries_[a], entries_[b]); });
  return idx;
}

void ReplayBuffer::AttachLive(PacketSink live) {
  std::lock_guard lock(mutex_);
  size_t first = entries_.size();
  for (size_t i = entries_.size(); i-- > 0;) {
    const auto& e = entries_[i];
    if (e.stream == 0 && (e.packet->flags & AV_PKT_FLAG_KEY)) {
      first = i;
      break;
    }
  }
  if (first < entries_.size())
    for (size_t i : ClipEntries(first, std::numeric_limits<double>::infinity())) live(entries_[i].packet.get(), entries_[i].stream);
  live_ = std::move(live);
}

void ReplayBuffer::DetachLive() {
  std::lock_guard lock(mutex_);
  live_ = nullptr;
}

} // namespace spectre
