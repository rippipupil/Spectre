// initguid antes de las cabeceras de Media Foundation: los GUID del motor de
// reproducción se definen aquí (selectany) y no dependen de mfuuid.lib.
#include <initguid.h>

#include <windows.h>
#include <mfapi.h>
// windows.h define GetCurrentTime() como macro y choca con el método del motor.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <mfmediaengine.h>

#include "ui/Player.h"

#include "common/Util.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace spectre {

using Microsoft::WRL::ComPtr;

namespace {

// Recibe los eventos del motor (en hilos de Media Foundation).
class EngineNotify final : public IMFMediaEngineNotify {
public:
  std::atomic<bool> ready{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> ended{false};
  std::atomic<int> errorCode{0};
  std::atomic<long> errorHr{0};

  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
      *out = static_cast<IMFMediaEngineNotify*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: ready = true; break;
    case MF_MEDIA_ENGINE_EVENT_ENDED: ended = true; break;
    case MF_MEDIA_ENGINE_EVENT_PLAYING:
    case MF_MEDIA_ENGINE_EVENT_SEEKING: ended = false; break;
    case MF_MEDIA_ENGINE_EVENT_ERROR:
      errorCode = int(param1);
      errorHr = long(param2);
      failed = true;
      break;
    default: break;
    }
    return S_OK;
  }

private:
  std::atomic<ULONG> refs_{1};
};

} // namespace

struct Player::Impl {
  std::string path;
  ComPtr<IMFDXGIDeviceManager> manager;
  ComPtr<IMFMediaEngine> engine;
  ComPtr<IMFMediaEngineEx> engineEx;
  EngineNotify* notify = nullptr;
  std::shared_ptr<UiTexture> texture;
  int width = 0, height = 0;
  float volume = 1.0f;
  bool muted = false;
  double rate = 1.0;
  uint64_t frames = 0;

  ~Impl() {
    if (engine) engine->Shutdown();
    engineEx.Reset();
    engine.Reset();
    manager.Reset();
    if (notify) notify->Release();
  }
};

Player::Player() = default;
Player::~Player() { Close(); }

bool Player::Available() { return MediaFoundationAvailable(); }

bool Player::Open(AppWindow& window, const std::string& path, bool autoplay) {
  Close();
  if (!Available() || !window.Device() || !window.VideoSupport()) return false;
  auto impl = std::make_unique<Impl>();
  impl->path = path;
  UINT token = 0;
  HRESULT hr = MFCreateDXGIDeviceManager(&token, &impl->manager);
  if (SUCCEEDED(hr)) hr = impl->manager->ResetDevice(window.Device(), token);
  ComPtr<IMFMediaEngineClassFactory> factory;
  if (SUCCEEDED(hr))
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  ComPtr<IMFAttributes> attrs;
  if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attrs, 3);
  impl->notify = new EngineNotify();
  if (SUCCEEDED(hr)) hr = attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, impl->manager.Get());
  if (SUCCEEDED(hr)) hr = attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, impl->notify);
  if (SUCCEEDED(hr)) hr = attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
  if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attrs.Get(), &impl->engine);
  if (FAILED(hr)) {
    Log("Reproductor: no se pudo crear el motor de Media Foundation (0x%08X)", unsigned(hr));
    return false;
  }
  impl->engine.As(&impl->engineEx);
  impl->engine->SetAutoPlay(autoplay ? TRUE : FALSE);
  impl->engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
  BSTR url = SysAllocString(Widen(path).c_str());
  hr = url ? impl->engine->SetSource(url) : E_OUTOFMEMORY;
  SysFreeString(url);
  if (FAILED(hr)) {
    Log("Reproductor: no se pudo abrir %s (0x%08X)", path.c_str(), unsigned(hr));
    return false;
  }
  impl_ = std::move(impl);
  return true;
}

void Player::Close() { impl_.reset(); }

bool Player::IsOpen() const { return impl_ != nullptr; }

const std::string& Player::Path() const {
  static const std::string empty;
  return impl_ ? impl_->path : empty;
}

bool Player::Ready() const { return impl_ && impl_->notify->ready; }
bool Player::Failed() const { return impl_ && impl_->notify->failed; }

std::string Player::Error() const {
  if (!impl_ || !impl_->notify->failed) return {};
  switch (impl_->notify->errorCode.load()) {
  case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED:
    return "Windows no puede reproducir este archivo aquí. Ábrelo con el reproductor de Windows.";
  case MF_MEDIA_ENGINE_ERR_DECODE:
    return "El archivo está dañado o a medio escribir.";
  default: {
    char buf[96];
    snprintf(buf, sizeof(buf), "No se pudo reproducir (error 0x%08X).", unsigned(impl_->notify->errorHr.load()));
    return buf;
  }
  }
}

void Player::Play() {
  if (!impl_) return;
  if (impl_->engine->IsEnded()) impl_->engine->SetCurrentTime(0);
  impl_->notify->ended = false;
  impl_->engine->Play();
}

void Player::Pause() {
  if (impl_) impl_->engine->Pause();
}

bool Player::Playing() const { return impl_ && !impl_->engine->IsPaused() && !impl_->engine->IsEnded(); }
bool Player::Ended() const { return impl_ && impl_->engine->IsEnded(); }

void Player::Seek(double seconds, bool exact) {
  if (!impl_) return;
  double d = Duration();
  if (d > 0) seconds = std::clamp(seconds, 0.0, d);
  if (!exact && impl_->engineEx)
    impl_->engineEx->SetCurrentTimeEx(seconds, MF_MEDIA_ENGINE_SEEK_MODE_APPROXIMATE);
  else
    impl_->engine->SetCurrentTime(seconds);
}

double Player::Position() const { return impl_ ? impl_->engine->GetCurrentTime() : 0.0; }

double Player::Duration() const {
  if (!impl_) return 0;
  double d = impl_->engine->GetDuration();
  return std::isfinite(d) && d > 0 ? d : 0.0;
}

void Player::SetVolume(float volume) {
  if (!impl_) return;
  impl_->volume = std::clamp(volume, 0.0f, 1.0f);
  impl_->engine->SetVolume(double(impl_->volume));
}

float Player::Volume() const { return impl_ ? impl_->volume : 1.0f; }

void Player::SetMuted(bool muted) {
  if (!impl_) return;
  impl_->muted = muted;
  impl_->engine->SetMuted(muted ? TRUE : FALSE);
}

bool Player::Muted() const { return impl_ && impl_->muted; }

void Player::SetRate(double rate) {
  if (!impl_) return;
  rate = std::clamp(rate, 0.25, 4.0);
  if (std::fabs(rate - impl_->rate) < 1e-3) return;
  impl_->rate = rate;
  impl_->engine->SetPlaybackRate(rate);
}

std::shared_ptr<UiTexture> Player::Frame(AppWindow& window) {
  if (!impl_ || !impl_->notify->ready) return impl_ ? impl_->texture : nullptr;
  LONGLONG pts = 0;
  if (impl_->engine->OnVideoStreamTick(&pts) != S_OK) return impl_->texture;
  DWORD w = 0, h = 0;
  if (FAILED(impl_->engine->GetNativeVideoSize(&w, &h)) || w == 0 || h == 0) return impl_->texture;
  if (!impl_->texture || impl_->texture->width != int(w) || impl_->texture->height != int(h)) {
    impl_->texture = window.CreateRenderTexture(int(w), int(h));
    impl_->width = int(w);
    impl_->height = int(h);
  }
  if (!impl_->texture) return nullptr;
  MFVideoNormalizedRect src{0.0f, 0.0f, 1.0f, 1.0f};
  RECT dst{0, 0, LONG(w), LONG(h)};
  MFARGB border{0, 0, 0, 255};
  if (SUCCEEDED(impl_->engine->TransferVideoFrame(impl_->texture->texture.Get(), &src, &dst, &border)))
    ++impl_->frames;
  return impl_->texture;
}

bool Player::ReadFrame(std::vector<uint8_t>* bgr, int* width, int* height) const {
  if (!impl_ || !impl_->texture || !impl_->texture->texture) return false;
  ID3D11Texture2D* src = impl_->texture->texture.Get();
  ComPtr<ID3D11Device> device;
  src->GetDevice(&device);
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  D3D11_TEXTURE2D_DESC desc{};
  src->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
  context->CopyResource(staging.Get(), src);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
  const UINT w = desc.Width, h = desc.Height;
  bgr->resize(size_t(w) * h * 3);
  for (UINT y = 0; y < h; ++y) {
    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
    uint8_t* out = bgr->data() + size_t(y) * w * 3;
    for (UINT x = 0; x < w; ++x) {
      out[x * 3 + 0] = row[x * 4 + 0];
      out[x * 3 + 1] = row[x * 4 + 1];
      out[x * 3 + 2] = row[x * 4 + 2];
    }
  }
  context->Unmap(staging.Get(), 0);
  *width = int(w);
  *height = int(h);
  return true;
}

int Player::VideoWidth() const { return impl_ ? impl_->width : 0; }
int Player::VideoHeight() const { return impl_ ? impl_->height : 0; }
uint64_t Player::FrameSerial() const { return impl_ ? impl_->frames : 0; }

} // namespace spectre
