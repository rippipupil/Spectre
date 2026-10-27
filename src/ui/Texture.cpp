#include "ui/Texture.h"

namespace spectre {

std::shared_ptr<UiTexture> MakeImageTexture(ID3D11Device* device, const uint8_t* bgra, int width, int height) {
  if (!device || !bgra || width <= 0 || height <= 0) return nullptr;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = UINT(width);
  desc.Height = UINT(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{bgra, UINT(width) * 4, 0};
  auto tex = std::make_shared<UiTexture>();
  if (FAILED(device->CreateTexture2D(&desc, &data, &tex->texture)) ||
      FAILED(device->CreateShaderResourceView(tex->texture.Get(), nullptr, &tex->srv)))
    return nullptr;
  tex->width = width;
  tex->height = height;
  return tex;
}

std::shared_ptr<UiTexture> MakeRenderTexture(ID3D11Device* device, int width, int height) {
  if (!device || width <= 0 || height <= 0) return nullptr;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = UINT(width);
  desc.Height = UINT(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  auto tex = std::make_shared<UiTexture>();
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex->texture)) ||
      FAILED(device->CreateShaderResourceView(tex->texture.Get(), nullptr, &tex->srv)))
    return nullptr;
  tex->width = width;
  tex->height = height;
  return tex;
}

} // namespace spectre
