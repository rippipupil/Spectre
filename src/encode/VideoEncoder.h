#pragma once

#include "encode/Media.h"
#include "encode/Quality.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace spectre {

using Microsoft::WRL::ComPtr;

struct VideoEncoderSettings {
  int width = 1920;
  int height = 1080;
  int fps = 60;
  QualityPlan quality;        // bitrate, tope y calidad objetivo (ver Quality.h)
  std::string codec = "h264"; // h264 | hevc | av1
  std::string preferred = "auto";
};

struct EncoderInfo {
  std::string name;  // id para la configuración (el nombre de FFmpeg, o h264_mf_hw para MF por GPU)
  std::string label; // texto para la UI
};
struct EncoderCandidate;
// Encoders que FFmpeg tiene compilados para el códec (no garantiza que la GPU los soporte).
std::vector<EncoderInfo> CandidateEncoders(const std::string& codec);

// Codificador de vídeo. Con NVENC y AMF el frame NV12 se escribe directamente
// en la textura que consume el encoder (cero copias). Con QuickSync, Media
// Foundation y OpenH264 se lee de vuelta con texturas staging en anillo para
// no bloquear la GPU.
class VideoEncoder {
public:
  using RenderFn = std::function<bool(ID3D11Texture2D* dst, UINT slice)>;

  ~VideoEncoder() { Close(); }
  bool Open(const VideoEncoderSettings& settings, ID3D11Device* device);
  void Close();

  // Llama a render (con GpuMutex tomado) y envía el frame al encoder.
  bool Encode(int64_t pts, const RenderFn& render, const PacketSink& sink);
  void Flush(const PacketSink& sink);

  const AVCodecContext* Context() const { return ctx_; }
  const std::string& Name() const { return name_; }
  const std::string& Label() const { return label_; }
  bool ZeroCopy() const { return hwFrames_ != nullptr; }
  // Codifica en la CPU (no se pudo abrir ninguno por GPU, o se eligió a mano).
  bool Software() const { return software_; }

private:
  // simple: sin las opciones de calidad (VBR, AQ, multipass...), por si el
  // driver no las admite y preferimos este encoder a uno peor.
  bool TryOpen(const AVCodec* codec, const EncoderCandidate& cand, const VideoEncoderSettings& s, bool simple);
  bool SetupHwFrames(const VideoEncoderSettings& s);
  bool SetupStaging(const VideoEncoderSettings& s);
  bool SendStaging(int index, const PacketSink& sink);
  bool Send(AVFrame* frame, const PacketSink& sink);
  void Drain(const PacketSink& sink);

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  AVCodecContext* ctx_ = nullptr;
  AVBufferRef* hwDevice_ = nullptr;
  AVBufferRef* hwFrames_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::string name_;
  std::string label_;
  bool software_ = false;
  // Hay abierto un encoder de Media Foundation y un MFStartup nuestro que se
  // suelta al cerrarlo (ver TryOpen).
  bool mfStarted_ = false;
  AVPixelFormat swFormat_ = AV_PIX_FMT_NV12;

  // Camino por memoria de sistema.
  static constexpr int kStaging = 3;
  static constexpr int kFrameAlign = 64;
  AVBufferPool* framePool_ = nullptr;
  ComPtr<ID3D11Texture2D> nv12_;
  std::array<ComPtr<ID3D11Texture2D>, kStaging> staging_;
  std::array<int64_t, kStaging> stagingPts_{};
  std::array<bool, kStaging> stagingPending_{};
  int64_t frameCounter_ = 0;
};

} // namespace spectre
