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
}

void ReplayBuffer::Clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
  bytes_ = 0;
  live_ = nullptr;
}

void ReplayBuffer::SetSeconds(double seconds) {
  std::lock_guard lock(mutex_);
  seconds_ = seconds;
  Trim();
}

double ReplayBuffer::TimeOf(const AVPacket* p, int stream) const {
  int64_t ts = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
  if (ts == AV_NOPTS_VALUE || stream < 0 || stream >= int(streams_.size())) return 0;
  return double(ts) * av_q2d(streams_[size_t(stream)].timeBase);
}

void ReplayBuffer::Push(const AVPacket* packet, int stream) {
  std::lock_guard lock(mutex_);
  if (live_) live_(packet, stream);
  Entry e{ClonePacket(packet), stream, TimeOf(packet, stream)};
  if (!e.packet) return;
  if (stream == 0) lastVideoTime_ = e.time;
  bytes_ += size_t(e.packet->size);
  entries_.push_back(std::move(e));
  Trim();
}

void ReplayBuffer::Trim() {
  // Se guardan 5 s extra: un keyframe (uno cada 2 s) antes del inicio del
  // clip, más lo que entra mientras SaveClip espera al audio (hasta 1,5 s).
  auto overLimit = [&] {
    if (entries_.empty()) return false;
    return bytes_ > maxBytes_ || lastVideoTime_ - entries_.front().time > seconds_ + 5.0;
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

std::vector<std::pair<PacketPtr, int>> ReplayBuffer::Snapshot(double seconds, double end) {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<PacketPtr, int>> out;
  if (entries_.empty()) return out;
  end = std::min(end, lastVideoTime_);
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
  if (first == entries_.size()) return out;
  for (size_t i : ClipEntries(first, end)) out.emplace_back(ClonePacket(entries_[i].packet.get()), entries_[i].stream);
  return out;
}

std::vector<size_t> ReplayBuffer::ClipEntries(size_t first, double end) const {
  // Normalmente el audio entra en el anillo después que el vídeo del mismo
  // instante (~165 ms), pero si el encoder de vídeo entrega con más retraso
  // que eso puede haber audio con t >= keyframe guardado antes que él: se
  // recorre todo el anillo y se ordena por tiempo.
  std::vector<size_t> idx;
  double firstTime = entries_[first].time;
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    if (e.time > end || (e.stream == 0 ? i < first : e.time < firstTime)) continue;
    idx.push_back(i);
  }
  // Cada pista ya está en orden; a igual tiempo, el vídeo primero (el MP4
  // empieza en el keyframe).
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    const auto& x = entries_[a];
    const auto& y = entries_[b];
    if (x.time != y.time) return x.time < y.time;
    return x.stream == 0 && y.stream != 0;
  });
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

size_t ReplayBuffer::Bytes() {
  std::lock_guard lock(mutex_);
  return bytes_;
}

double ReplayBuffer::DurationSeconds() {
  std::lock_guard lock(mutex_);
  if (entries_.empty()) return 0;
  return lastVideoTime_ - entries_.front().time;
}

} // namespace spectre
