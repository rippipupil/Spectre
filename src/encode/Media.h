#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace spectre {

std::string AvError(int err);

struct PacketDeleter {
  void operator()(AVPacket* p) const { av_packet_free(&p); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
PacketPtr ClonePacket(const AVPacket* p);
// Paquete de solo lectura que comparten el anillo del replay y quien escribe
// un clip: copiar la referencia cuesta un incremento atómico, no un clon.
using SharedPacket = std::shared_ptr<const AVPacket>;
SharedPacket SharePacket(const AVPacket* p);

struct CodecParDeleter {
  void operator()(AVCodecParameters* p) const { avcodec_parameters_free(&p); }
};

// Descripción de una pista del MP4 (vídeo o audio).
struct StreamDesc {
  std::shared_ptr<AVCodecParameters> params;
  AVRational timeBase{1, 1000};
  bool video = false;
  std::string title;
};
using StreamList = std::vector<StreamDesc>;

StreamDesc MakeStreamDesc(const AVCodecContext* ctx, bool video, std::string title);

// Recibe paquetes codificados: (paquete, índice de pista).
using PacketSink = std::function<void(const AVPacket*, int)>;

} // namespace spectre
