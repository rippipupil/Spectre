#include "core/ReplayBuffer.h"

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
  // Se guarda 1 s extra para tener siempre un keyframe antes del inicio del clip.
  auto overLimit = [&] {
    if (entries_.empty()) return false;
    return bytes_ > maxBytes_ || lastVideoTime_ - entries_.front().time > seconds_ + 1.5;
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

std::vector<std::pair<PacketPtr, int>> ReplayBuffer::Snapshot(double seconds) {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<PacketPtr, int>> out;
  if (entries_.empty()) return out;
  double start = lastVideoTime_ - seconds;
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
  double firstTime = entries_[first].time;
  for (size_t i = first; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    if (e.stream != 0 && e.time < firstTime) continue; // audio anterior al primer frame
    out.emplace_back(ClonePacket(e.packet.get()), e.stream);
  }
  return out;
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
  if (first < entries_.size()) {
    double firstTime = entries_[first].time;
    for (size_t i = first; i < entries_.size(); ++i) {
      const auto& e = entries_[i];
      if (e.stream != 0 && e.time < firstTime) continue;
      live(e.packet.get(), e.stream);
    }
  }
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
