#include "gpu/VideoConverter.h"

#include "common/Util.h"

#include <d3d11_1.h>

#include <algorithm>

namespace spectre {

bool VideoConverter::Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT outWidth, UINT outHeight,
                          UINT fps) {
  Reset();
  device_ = device;
  context_ = context;
  outWidth_ = outWidth & ~1u;
  outHeight_ = outHeight & ~1u;
  fps_ = fps;
  if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
    Log("El driver no expone ID3D11VideoDevice: no se puede convertir a NV12 en la GPU");
    return false;
  }
  return true;
}

void VideoConverter::Reset() {
  main_ = {};
  overlay_ = {};
  videoContext_.Reset();
  videoDevice_.Reset();
  context_.Reset();
  device_.Reset();
}

bool VideoConverter::EnsureProcessor(Processor& p, UINT inWidth, UINT inHeight) {
  if (p.processor && p.inWidth == inWidth && p.inHeight == inHeight) return true;
  p = {};
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
  desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  desc.InputWidth = inWidth;
  desc.InputHeight = inHeight;
  desc.OutputWidth = outWidth_;
  desc.OutputHeight = outHeight_;
  desc.InputFrameRate = {fps_, 1};
  desc.OutputFrameRate = {fps_, 1};
  desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
  HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&desc, &p.enumerator);
  if (FAILED(hr)) {
    Log("CreateVideoProcessorEnumerator falló: 0x%08X", unsigned(hr));
    return false;
  }
  hr = videoDevice_->CreateVideoProcessor(p.enumerator.Get(), 0, &p.processor);
  if (FAILED(hr)) {
    Log("CreateVideoProcessor falló: 0x%08X", unsigned(hr));
    p = {};
    return false;
  }
  p.inWidth = inWidth;
  p.inHeight = inHeight;
  ConfigureColor(p);
  return true;
}

void VideoConverter::ConfigureColor(Processor& p) {
  auto* vp = p.processor.Get();
  ComPtr<ID3D11VideoContext1> vc1;
  if (SUCCEEDED(videoContext_.As(&vc1))) {
    vc1->VideoProcessorSetStreamColorSpace1(vp, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    vc1->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  } else {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.RGB_Range = 0; // 0-255
    in.YCbCr_Matrix = 1;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.RGB_Range = 1;
    out.YCbCr_Matrix = 1; // BT.709
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(vp, 0, &in);
    videoContext_->VideoProcessorSetOutputColorSpace(vp, &out);
  }
  videoContext_->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
  D3D11_VIDEO_COLOR black{};
  black.RGBA = {0.0f, 0.0f, 0.0f, 1.0f};
  videoContext_->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &black);
}

ID3D11VideoProcessorOutputView* VideoConverter::OutputView(Processor& p, ID3D11Texture2D* dst, UINT slice) {
  auto key = std::make_pair(dst, slice);
  auto it = p.outputViews.find(key);
  if (it != p.outputViews.end()) return it->second.Get();
  if (p.outputViews.size() > 64) p.outputViews.clear();

  D3D11_TEXTURE2D_DESC td;
  dst->GetDesc(&td);
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vd{};
  if (td.ArraySize > 1) {
    vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.MipSlice = 0;
    vd.Texture2DArray.FirstArraySlice = slice;
    vd.Texture2DArray.ArraySize = 1;
  } else {
    vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  }
  ComPtr<ID3D11VideoProcessorOutputView> view;
  HRESULT hr = videoDevice_->CreateVideoProcessorOutputView(dst, p.enumerator.Get(), &vd, &view);
  if (FAILED(hr)) {
    Log("CreateVideoProcessorOutputView falló: 0x%08X", unsigned(hr));
    return nullptr;
  }
  return (p.outputViews[key] = view).Get();
}

ID3D11VideoProcessorInputView* VideoConverter::InputView(Processor& p, ID3D11Texture2D* src) {
  if (p.lastInput == src && p.inputView) return p.inputView.Get();
  p.inputView.Reset();
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd{};
  vd.FourCC = 0;
  vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  vd.Texture2D.MipSlice = 0;
  HRESULT hr = videoDevice_->CreateVideoProcessorInputView(src, p.enumerator.Get(), &vd, &p.inputView);
  if (FAILED(hr)) {
    Log("CreateVideoProcessorInputView falló: 0x%08X", unsigned(hr));
    p.lastInput = nullptr;
    return nullptr;
  }
  p.lastInput = src;
  return p.inputView.Get();
}

bool VideoConverter::Blit(Processor& p, ID3D11Texture2D* src, const RECT& srcRect, ID3D11Texture2D* dst, UINT slice,
                          const RECT& dstRect, bool restrictTarget) {
  auto* in = InputView(p, src);
  auto* out = OutputView(p, dst, slice);
  if (!in || !out) return false;
  auto* vp = p.processor.Get();
  videoContext_->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &srcRect);
  videoContext_->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dstRect);
  videoContext_->VideoProcessorSetOutputTargetRect(vp, restrictTarget ? TRUE : FALSE, &dstRect);

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = in;
  HRESULT hr = videoContext_->VideoProcessorBlt(vp, out, 0, 1, &stream);
  if (FAILED(hr)) {
    Log("VideoProcessorBlt falló: 0x%08X", unsigned(hr));
    return false;
  }
  return true;
}

bool VideoConverter::Convert(ID3D11Texture2D* src, UINT srcWidth, UINT srcHeight, ID3D11Texture2D* dst,
                             UINT dstSlice) {
  if (!videoDevice_ || srcWidth == 0 || srcHeight == 0) return false;
  D3D11_TEXTURE2D_DESC sd;
  src->GetDesc(&sd);
  if (!EnsureProcessor(main_, sd.Width, sd.Height)) return false;

  // Letterbox: encaja la fuente en la salida manteniendo la proporción.
  double scale = std::min(double(outWidth_) / srcWidth, double(outHeight_) / srcHeight);
  LONG w = LONG(srcWidth * scale) & ~1L;
  LONG h = LONG(srcHeight * scale) & ~1L;
  LONG x = (LONG(outWidth_) - w) / 2 & ~1L;
  LONG y = (LONG(outHeight_) - h) / 2 & ~1L;
  RECT srcRect{0, 0, LONG(srcWidth), LONG(srcHeight)};
  RECT dstRect{x, y, x + w, y + h};
  return Blit(main_, src, srcRect, dst, dstSlice, dstRect, false);
}

bool VideoConverter::Overlay(ID3D11Texture2D* overlay, UINT overlayWidth, UINT overlayHeight, const RECT& rect,
                             ID3D11Texture2D* dst, UINT dstSlice) {
  if (!videoDevice_ || overlayWidth == 0 || overlayHeight == 0) return false;
  D3D11_TEXTURE2D_DESC sd;
  overlay->GetDesc(&sd);
  if (!EnsureProcessor(overlay_, sd.Width, sd.Height)) return false;
  RECT srcRect{0, 0, LONG(overlayWidth), LONG(overlayHeight)};
  return Blit(overlay_, overlay, srcRect, dst, dstSlice, rect, true);
}

bool VideoConverter::ConvertRegion(ID3D11Texture2D* src, const RECT& srcRect, ID3D11Texture2D* dst, UINT dstSlice) {
  if (!videoDevice_ || srcRect.right <= srcRect.left || srcRect.bottom <= srcRect.top) return false;
  D3D11_TEXTURE2D_DESC sd;
  src->GetDesc(&sd);
  if (!EnsureProcessor(main_, sd.Width, sd.Height)) return false;
  RECT dstRect{0, 0, LONG(outWidth_), LONG(outHeight_)};
  return Blit(main_, src, srcRect, dst, dstSlice, dstRect, false);
}

} // namespace spectre
