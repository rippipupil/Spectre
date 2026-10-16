#include "capture/Camera.h"

#include "common/Util.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>

#include <vector>

namespace spectre {

std::vector<CameraDevice> EnumerateCameras() {
  std::vector<CameraDevice> list;
  ComPtr<IMFAttributes> attrs;
  if (FAILED(MFCreateAttributes(&attrs, 1))) return list;
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) return list;
  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* name = nullptr;
    WCHAR* link = nullptr;
    UINT32 len = 0;
    devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
    devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
    if (link) list.push_back({Narrow(link), Narrow(name ? name : L"Cámara")});
    CoTaskMemFree(name);
    CoTaskMemFree(link);
    devices[i]->Release();
  }
  CoTaskMemFree(devices);
  return list;
}

bool Camera::Start(ID3D11Device* device, const std::string& id) {
  Stop();
  device_ = device;
  device_->GetImmediateContext(&context_);
  std::wstring wid = Widen(id);
  if (wid.empty()) {
    auto cams = EnumerateCameras();
    if (cams.empty()) {
      Log("Cámara: no hay ninguna conectada");
      return false;
    }
    wid = Widen(cams.front().id);
  }
  running_ = true;
  thread_ = std::thread([this, wid] { Run(wid); });
  return true;
}

void Camera::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  GpuLock lock(GpuMutex());
  texture_.Reset();
  hasFrame_ = false;
  context_.Reset();
  device_.Reset();
}

bool Camera::Latest(ComPtr<ID3D11Texture2D>* tex, UINT* width, UINT* height) {
  if (!hasFrame_ || !texture_) return false;
  *tex = texture_;
  *width = width_;
  *height = height_;
  return true;
}

void Camera::Run(std::wstring id) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

  ComPtr<IMFAttributes> attrs;
  ComPtr<IMFMediaSource> source;
  ComPtr<IMFSourceReader> reader;
  MFCreateAttributes(&attrs, 2);
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, id.c_str());
  HRESULT hr = MFCreateDeviceSource(attrs.Get(), &source);
  if (SUCCEEDED(hr)) {
    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 1);
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader);
  }
  const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  if (SUCCEEDED(hr)) {
    // Formato nativo más grande que no pase de 1280x720.
    ComPtr<IMFMediaType> best;
    UINT64 bestArea = 0;
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      if (FAILED(reader->GetNativeMediaType(stream, i, &type))) break;
      UINT32 w = 0, h = 0;
      MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
      UINT64 area = UINT64(w) * h;
      if (w <= 1280 && h <= 720 && area > bestArea) {
        bestArea = area;
        best = type;
      }
    }
    if (best) reader->SetCurrentMediaType(stream, nullptr, best.Get());
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(stream, nullptr, out.Get());
  }
  UINT32 w = 0, h = 0;
  LONG defaultStride = 0;
  if (SUCCEEDED(hr)) {
    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(stream, &current);
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
    UINT32 stride = 0;
    defaultStride = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? LONG(INT32(stride)) : LONG(w * 4);
  }
  if (FAILED(hr) || w == 0 || h == 0) {
    Log("Cámara: no se pudo abrir (0x%08X)", unsigned(hr));
    if (source) source->Shutdown();
    CoUninitialize();
    return;
  }
  Log("Cámara: %ux%u", w, h);

  std::vector<uint8_t> flipped;
  while (running_) {
    DWORD flags = 0;
    ComPtr<IMFSample> sample;
    hr = reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
      Log("Cámara: lectura terminada (0x%08X)", unsigned(hr));
      break;
    }
    if (!sample) continue;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;

    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* raw = nullptr;
    bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
    if (!locked2d) {
      DWORD len = 0;
      if (FAILED(buffer->Lock(&raw, nullptr, &len))) continue;
      pitch = defaultStride;
      scan0 = pitch < 0 ? raw + size_t(-pitch) * (h - 1) : raw;
    }

    const BYTE* data = scan0;
    UINT rowPitch = UINT(pitch);
    if (pitch < 0) { // imagen de abajo arriba: se le da la vuelta
      flipped.resize(size_t(w) * 4 * h);
      for (UINT y = 0; y < h; ++y) memcpy(&flipped[size_t(y) * w * 4], scan0 + LONG(y) * pitch, size_t(w) * 4);
      data = flipped.data();
      rowPitch = w * 4;
    }

    {
      GpuLock lock(GpuMutex());
      if (!texture_ || width_ != w || height_ != h) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        texture_.Reset();
        device_->CreateTexture2D(&desc, nullptr, &texture_);
        width_ = w;
        height_ = h;
      }
      if (texture_) {
        context_->UpdateSubresource(texture_.Get(), 0, nullptr, data, rowPitch, 0);
        hasFrame_ = true;
      }
    }

    if (locked2d) buffer2d->Unlock2D();
    else buffer->Unlock();
  }
  reader.Reset();
  if (source) source->Shutdown();
  CoUninitialize();
}

} // namespace spectre
