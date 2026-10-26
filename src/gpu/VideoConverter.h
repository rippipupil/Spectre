#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <map>
#include <utility>

namespace spectre {

using Microsoft::WRL::ComPtr;

// Convierte BGRA -> NV12 en la GPU con ID3D11VideoProcessor (lo implementan
// todos los drivers). Escala con letterbox y puede componer la webcam encima.
// Ningún píxel pasa por la CPU.
class VideoConverter {
public:
  bool Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT outWidth, UINT outHeight, UINT fps);
  void Reset();

  // src: textura BGRA; se usa la región (0,0)-(srcWidth,srcHeight).
  // dst: textura NV12 (BIND_RENDER_TARGET), dstSlice para arrays.
  bool Convert(ID3D11Texture2D* src, UINT srcWidth, UINT srcHeight, ID3D11Texture2D* dst, UINT dstSlice);

  // Dibuja overlay (BGRA) dentro de rect (coordenadas de salida) sobre dst.
  bool Overlay(ID3D11Texture2D* overlay, UINT overlayWidth, UINT overlayHeight, const RECT& rect,
               ID3D11Texture2D* dst, UINT dstSlice);

  // La región srcRect de src (BGRA) a todo dst (NV12 de OutWidth x OutHeight),
  // escalada si hace falta: lo que hay debajo de la webcam (ver CamBackground).
  bool ConvertRegion(ID3D11Texture2D* src, const RECT& srcRect, ID3D11Texture2D* dst, UINT dstSlice);

  UINT OutWidth() const { return outWidth_; }
  UINT OutHeight() const { return outHeight_; }

private:
  struct Processor {
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    UINT inWidth = 0, inHeight = 0;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorOutputView>> outputViews;
    ID3D11Texture2D* lastInput = nullptr;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
  };

  bool EnsureProcessor(Processor& p, UINT inWidth, UINT inHeight);
  ID3D11VideoProcessorOutputView* OutputView(Processor& p, ID3D11Texture2D* dst, UINT slice);
  ID3D11VideoProcessorInputView* InputView(Processor& p, ID3D11Texture2D* src);
  void ConfigureColor(Processor& p);
  bool Blit(Processor& p, ID3D11Texture2D* src, const RECT& srcRect, ID3D11Texture2D* dst, UINT slice,
            const RECT& dstRect, bool restrictTarget);

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11VideoDevice> videoDevice_;
  ComPtr<ID3D11VideoContext> videoContext_;
  UINT outWidth_ = 0, outHeight_ = 0, fps_ = 60;
  Processor main_;
  Processor overlay_;
};

} // namespace spectre
