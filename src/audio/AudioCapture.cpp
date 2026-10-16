#include "audio/AudioCapture.h"

#include "common/Compat.h"
#include "common/Util.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

namespace spectre {

using Microsoft::WRL::ComPtr;

namespace {

const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const IID kIID_IAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

constexpr size_t kFifoFrames = kAudioRate * 4;

// Handler mínimo para ActivateAudioInterfaceAsync (debe ser agile).
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
  explicit ActivationHandler(HANDLE done) : done_(done) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
        riid == kIID_IAgileObject) {
      *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
    SetEvent(done_);
    return S_OK;
  }

private:
  LONG refs_ = 1;
  HANDLE done_;
};

ComPtr<IAudioClient> ActivateProcessLoopback(DWORD pid) {
  interop::AudioClientActivationParams params{};
  params.Type = interop::ActivationType::ProcessLoopback;
  params.ProcessLoopback.TargetProcessId = pid;
  params.ProcessLoopback.Mode = interop::ProcessLoopbackMode::IncludeTargetProcessTree;

  PROPVARIANT pv{};
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof(params);
  pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  auto* handler = new ActivationHandler(done);
  ComPtr<IActivateAudioInterfaceAsyncOperation> op;
  HRESULT hr = ActivateAudioInterfaceAsync(interop::kVirtualProcessLoopbackDevice, __uuidof(IAudioClient), &pv,
                                           handler, &op);
  ComPtr<IAudioClient> client;
  if (SUCCEEDED(hr) && WaitForSingleObject(done, 5000) == WAIT_OBJECT_0) {
    HRESULT activateHr = E_FAIL;
    ComPtr<IUnknown> unk;
    if (SUCCEEDED(op->GetActivateResult(&activateHr, &unk)) && SUCCEEDED(activateHr) && unk) unk.As(&client);
    else Log("Audio por proceso: activación falló 0x%08X", unsigned(activateHr));
  } else {
    Log("Audio por proceso: ActivateAudioInterfaceAsync falló 0x%08X", unsigned(hr));
  }
  handler->Release();
  CloseHandle(done);
  return client;
}

ComPtr<IMMDevice> GetDevice(const std::wstring& id, bool capture) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
    return nullptr;
  ComPtr<IMMDevice> device;
  if (!id.empty() && SUCCEEDED(enumerator->GetDevice(id.c_str(), &device))) return device;
  enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &device);
  return device;
}

} // namespace

std::vector<AudioDevice> EnumerateAudioDevices(bool capture) {
  std::vector<AudioDevice> list;
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
    return list;
  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection)))
    return list;
  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(i, &device))) continue;
    LPWSTR id = nullptr;
    device->GetId(&id);
    AudioDevice info;
    info.id = Narrow(id ? id : L"");
    CoTaskMemFree(id);
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
      PROPVARIANT name;
      PropVariantInit(&name);
      if (SUCCEEDED(props->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR) info.name = Narrow(name.pwszVal);
      PropVariantClear(&name);
    }
    if (info.name.empty()) info.name = info.id;
    list.push_back(std::move(info));
  }
  return list;
}

bool AudioCapture::Start(AudioSourceKind kind, const std::string& deviceId, DWORD processId, int64_t startQpc) {
  Stop();
  kind_ = kind;
  deviceId_ = Widen(deviceId);
  processId_ = processId;
  startQpc_ = startQpc;
  fifo_.assign(kFifoFrames * kAudioChannels, 0.0f);
  readPos_ = count_ = 0;
  framesWritten_ = 0;
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  startOk_ = false;
  running_ = true;
  thread_ = std::thread([this] { Run(); });
  WaitForSingleObject(readyEvent_, 6000);
  if (!startOk_) Stop();
  return startOk_;
}

void AudioCapture::Stop() {
  if (thread_.joinable()) {
    SetEvent(stopEvent_);
    thread_.join();
  }
  running_ = false;
  if (stopEvent_) CloseHandle(stopEvent_);
  if (readyEvent_) CloseHandle(readyEvent_);
  stopEvent_ = readyEvent_ = nullptr;
}

size_t AudioCapture::Available() {
  std::lock_guard lock(mutex_);
  return count_;
}

size_t AudioCapture::Read(float* dst, size_t frames) {
  std::lock_guard lock(mutex_);
  size_t n = std::min(frames, count_);
  for (size_t i = 0; i < n; ++i) {
    size_t pos = (readPos_ + i) % kFifoFrames;
    dst[i * 2] = fifo_[pos * 2];
    dst[i * 2 + 1] = fifo_[pos * 2 + 1];
  }
  readPos_ = (readPos_ + n) % kFifoFrames;
  count_ -= n;
  return n;
}

void AudioCapture::Write(const float* data, size_t frames) {
  std::lock_guard lock(mutex_);
  for (size_t i = 0; i < frames; ++i) {
    if (count_ == kFifoFrames) { // lleno: se descarta lo más antiguo
      readPos_ = (readPos_ + 1) % kFifoFrames;
      count_--;
    }
    size_t pos = (readPos_ + count_) % kFifoFrames;
    fifo_[pos * 2] = data ? data[i * 2] : 0.0f;
    fifo_[pos * 2 + 1] = data ? data[i * 2 + 1] : 0.0f;
    count_++;
  }
  framesWritten_ += frames;
}

void AudioCapture::WriteSilence(size_t frames) { Write(nullptr, frames); }

void AudioCapture::Run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  HANDLE packetEvent = nullptr;
  UINT channels = kAudioChannels;

  auto open = [&]() -> bool {
    client.Reset();
    capture.Reset();
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = kAudioChannels;
    fmt.nSamplesPerSec = kAudioRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    const REFERENCE_TIME bufferDuration = 2'000'000; // 200 ms
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    HRESULT hr;
    if (kind_ == AudioSourceKind::ProcessLoopback) {
      client = ActivateProcessLoopback(processId_);
      if (!client) return false;
      if (!packetEvent) packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
                              bufferDuration, 0, &fmt, nullptr);
      if (SUCCEEDED(hr)) hr = client->SetEventHandle(packetEvent);
    } else {
      bool isCapture = kind_ == AudioSourceKind::Microphone;
      auto device = GetDevice(deviceId_, isCapture);
      if (!device || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return false;
      DWORD flags = convert | (isCapture ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK);
      hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, &fmt, nullptr);
      if (FAILED(hr)) {
        // Sin conversión automática: aceptamos el formato del mezclador si es float a 48 kHz.
        WAVEFORMATEX* mix = nullptr;
        client.Reset();
        device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (client && SUCCEEDED(client->GetMixFormat(&mix)) && mix->nSamplesPerSec == kAudioRate &&
            mix->wBitsPerSample == 32) {
          hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, isCapture ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  bufferDuration, 0, mix, nullptr);
          channels = mix->nChannels;
        }
        CoTaskMemFree(mix);
      }
    }
    if (FAILED(hr)) {
      Log("Audio: Initialize falló 0x%08X", unsigned(hr));
      client.Reset();
      return false;
    }
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
      client.Reset();
      capture.Reset();
      return false;
    }
    return true;
  };

  startOk_ = open();
  SetEvent(readyEvent_);

  const int64_t freq = QpcFrequency();
  int64_t lastData = QpcNow();
  int64_t lastRetry = 0;
  std::vector<float> conv;

  while (startOk_) {
    HANDLE handles[2] = {stopEvent_, packetEvent};
    DWORD r = packetEvent ? WaitForMultipleObjects(2, handles, FALSE, 10) : WaitForSingleObject(stopEvent_, 10);
    if (r == WAIT_OBJECT_0) break;

    bool gotData = false;
    if (capture) {
      UINT32 packetSize = 0;
      HRESULT hr;
      while (SUCCEEDED(hr = capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        int64_t expected = (QpcNow() - startQpc_) * kAudioRate / freq;
        bool ahead = int64_t(framesWritten_) > expected + kAudioRate / 2; // reloj del dispositivo adelantado
        if (!ahead) {
          if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            WriteSilence(frames);
          } else if (channels == kAudioChannels) {
            Write(reinterpret_cast<const float*>(data), frames);
          } else {
            conv.resize(size_t(frames) * 2);
            const auto* src = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
              conv[i * 2] = src[i * channels];
              conv[i * 2 + 1] = channels > 1 ? src[i * channels + 1] : src[i * channels];
            }
            Write(conv.data(), frames);
          }
        }
        capture->ReleaseBuffer(frames);
        gotData = true;
      }
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        Log("Audio: dispositivo desconectado, reintentando");
        capture.Reset();
        client.Reset();
      }
    } else {
      if (QpcNow() - lastRetry > freq) {
        lastRetry = QpcNow();
        open();
      }
    }

    int64_t now = QpcNow();
    if (gotData) lastData = now;
    // Nada sonando: WASAPI loopback no entrega paquetes, así que rellenamos
    // con silencio para mantener la línea de tiempo.
    int64_t expected = (now - startQpc_) * kAudioRate / freq;
    int64_t behind = expected - int64_t(framesWritten_);
    if (!gotData && now - lastData > freq / 20 && behind > kAudioRate / 100)
      WriteSilence(size_t(behind - kAudioRate / 200));
  }

  if (client) client->Stop();
  capture.Reset();
  client.Reset();
  if (packetEvent) CloseHandle(packetEvent);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  CoUninitialize();
}

} // namespace spectre
