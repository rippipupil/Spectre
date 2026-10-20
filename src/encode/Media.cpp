#include "encode/Media.h"

namespace spectre {

std::string AvError(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

PacketPtr ClonePacket(const AVPacket* p) { return PacketPtr(av_packet_clone(p)); }

SharedPacket SharePacket(const AVPacket* p) {
  AVPacket* copy = av_packet_clone(p);
  if (!copy) return nullptr;
  return SharedPacket(copy, [](const AVPacket* pkt) {
    AVPacket* owned = const_cast<AVPacket*>(pkt);
    av_packet_free(&owned);
  });
}

StreamDesc MakeStreamDesc(const AVCodecContext* ctx, bool video, std::string title) {
  StreamDesc desc;
  AVCodecParameters* par = avcodec_parameters_alloc();
  avcodec_parameters_from_context(par, ctx);
  desc.params = std::shared_ptr<AVCodecParameters>(par, CodecParDeleter{});
  desc.timeBase = ctx->time_base;
  desc.video = video;
  desc.title = std::move(title);
  return desc;
}

} // namespace spectre
