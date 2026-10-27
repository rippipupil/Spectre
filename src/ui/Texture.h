#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>

#include <cstdint>
#include <memory>

namespace spectre {

using Microsoft::WRL::ComPtr;

// Textura de la interfaz (miniatura, captura, fotograma de vídeo). Se libera
// con el último shared_ptr. Es de la GPU de la ventana que la creó: no se
// puede dibujar en otra.
struct UiTexture {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> srv;
  // Solo en texturas con mipmaps: el muestreador de ImGui lee únicamente el
  // nivel 0, así que se dibujan con este (ver AddTextureImage).
  ComPtr<ID3D11SamplerState> sampler;
  int width = 0, height = 0;
  unsigned long long Id() const { return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(srv.Get())); }
};

// dl->AddImage, pero con el muestreador de la textura si lo tiene.
void AddTextureImage(ImDrawList* dl, const UiTexture& tex, ImVec2 p0, ImVec2 p1, ImVec2 uv0, ImVec2 uv1, ImU32 tint);

// Imagen fija (BGRA de arriba abajo) sin mipmaps: para verla a su tamaño.
std::shared_ptr<UiTexture> MakeImageTexture(ID3D11Device* device, const uint8_t* bgra, int width, int height);
// Textura en la que se puede dibujar (fotogramas del reproductor).
std::shared_ptr<UiTexture> MakeRenderTexture(ID3D11Device* device, int width, int height);

} // namespace spectre
