#include "audio/AudioCapture.h"

#include "common/Compat.h"
#include "common/Util.h"
#include "encode/Media.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spectre {

using Microsoft::WRL::ComPtr;

namespace {

const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const IID kIID_IAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr size_t kFifoFrames = kAudioRate * 4;
constexpr REFERENCE_TIME kBufferDuration = 2'000'000; // 200 ms
// Un paquete puede llegar hasta 20 ms antes o después de donde el reloj dice
// sin que se toque nada; más allá se rellena con silencio o se descarta.
constexpr int64_t kAlignTolerance = kAudioRate / 50;
constexpr int64_t kAheadLimit = kAudioRate / 2;

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

ComPtr<IMMDeviceEnumerator> Enumerator() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  return enumerator;
}

std::wstring DeviceId(IMMDevice* device) {
  LPWSTR id = nullptr;
  if (!device || FAILED(device->GetId(&id)) || !id) return {};
  std::wstring out(id);
  CoTaskMemFree(id);
  return out;
}

std::string DeviceName(IMMDevice* device) {
  std::string name;
  ComPtr<IPropertyStore> props;
  if (device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(props->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR) name = Narrow(value.pwszVal);
    PropVariantClear(&value);
  }
  return name;
}

std::wstring DefaultDeviceId(bool capture) {
  auto enumerator = Enumerator();
  ComPtr<IMMDevice> device;
  if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &device)))
    return {};
  return DeviceId(device.Get());
}

// Devuelve el dispositivo pedido o, si no existe o no se pidió ninguno, el
// predeterminado. usedDefault dice cuál de los dos fue.
ComPtr<IMMDevice> GetDevice(const std::wstring& id, bool capture, bool* usedDefault) {
  *usedDefault = true;
  auto enumerator = Enumerator();
  if (!enumerator) return nullptr;
  ComPtr<IMMDevice> device;
  if (!id.empty()) {
    DWORD state = DEVICE_STATE_DISABLED;
    if (SUCCEEDED(enumerator->GetDevice(id.c_str(), &device)) && SUCCEEDED(device->GetState(&state)) &&
        state == DEVICE_STATE_ACTIVE) {
      // Un id guardado en los ajustes puede ser de un dispositivo del otro
      // sentido (micrófono como salida): en ese caso se ignora.
      ComPtr<IMMEndpoint> endpoint;
      EDataFlow flow = eRender;
      if (SUCCEEDED(device.As(&endpoint)) && SUCCEEDED(endpoint->GetDataFlow(&flow)) &&
          flow == (capture ? eCapture : eRender)) {
        *usedDefault = false;
        return device;
      }
    }
    Log("Audio: el dispositivo elegido no está disponible, se usa el predeterminado");
    device.Reset();
  }
  enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &device);
  return device;
}

WAVEFORMATEX OurFormat() {
  WAVEFORMATEX fmt{};
  fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  fmt.nChannels = kAudioChannels;
  fmt.nSamplesPerSec = kAudioRate;
  fmt.wBitsPerSample = 32;
  fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
  fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
  return fmt;
}

// Formato de muestra de FFmpeg equivalente a un WAVEFORMATEX, o NONE si no
// se puede convertir (24 bits empaquetados, formatos comprimidos...).
AVSampleFormat SampleFormatOf(const WAVEFORMATEX* f, const char** label) {
  bool isFloat = false, isPcm = false;
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    isFloat = IsEqualGUID(ext->SubFormat, kSubtypeFloat);
    isPcm = IsEqualGUID(ext->SubFormat, kSubtypePcm);
  } else {
    isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    isPcm = f->wFormatTag == WAVE_FORMAT_PCM;
  }
  if (isFloat && f->wBitsPerSample == 32) {
    *label = "float";
    return AV_SAMPLE_FMT_FLT;
  }
  if (isFloat && f->wBitsPerSample == 64) {
    *label = "double";
    return AV_SAMPLE_FMT_DBL;
  }
  if (isPcm && f->wBitsPerSample == 16) {
    *label = "16 bits";
    return AV_SAMPLE_FMT_S16;
  }
  if (isPcm && f->wBitsPerSample == 32) {
    *label = "32 bits";
    return AV_SAMPLE_FMT_S32;
  }
  if (isPcm && f->wBitsPerSample == 8) {
    *label = "8 bits";
    return AV_SAMPLE_FMT_U8;
  }
  *label = "desconocido";
  return AV_SAMPLE_FMT_NONE;
}

uint64_t ChannelMaskOf(const WAVEFORMATEX* f) {
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->dwChannelMask;
  return 0;
}

const char* KindName(AudioSourceKind kind) {
  switch (kind) {
  case AudioSourceKind::SystemLoopback: return "sonido del PC";
  case AudioSourceKind::ProcessLoopback: return "audio del juego";
  default: return "micrófono";
  }
}

} // namespace

// Todo lo que vive mientras el cliente WASAPI está abierto.
struct AudioCapture::Client {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  HANDLE packetEvent = nullptr; // solo en audio por proceso (modo evento)
  SwrContext* swr = nullptr;    // nullptr = ya llega en float 48 kHz estéreo
  int channels = kAudioChannels;
  int rate = kAudioRate;
  AVSampleFormat sampleFormat = AV_SAMPLE_FMT_FLT;
  std::vector<float> conv;

  ~Client() {
    if (client) client->Stop();
    capture.Reset();
    client.Reset();
    if (packetEvent) CloseHandle(packetEvent);
    swr_free(&swr);
  }
};

std::vector<AudioDevice> EnumerateAudioDevices(bool capture) {
  std::vector<AudioDevice> list;
  auto enumerator = Enumerator();
  if (!enumerator) return list;
  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection)))
    return list;
  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(i, &device))) continue;
    AudioDevice info;
    info.id = Narrow(DeviceId(device.Get()));
    info.name = DeviceName(device.Get());
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
  peak_ = 0.0f;
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  startOk_ = false;
  running_ = true;
  thread_ = std::thread([this] {
    try {
      Run();
    } catch (const std::exception& e) {
      Log("Audio: excepción en el hilo de captura: %s", e.what());
      startOk_ = false;
      SetEvent(readyEvent_);
    }
  });
  // Si el dispositivo tarda demasiado (Bluetooth, activación del audio de un
  // proceso) se da por fallido aunque el hilo termine abriéndolo después.
  bool ok = WaitForSingleObject(readyEvent_, 6000) == WAIT_OBJECT_0 && startOk_;
  if (!ok) Stop();
  return ok;
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

std::string AudioCapture::Description() {
  std::lock_guard lock(mutex_);
  return description_;
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
  float peak = 0.0f;
  {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < frames; ++i) {
      if (count_ == kFifoFrames) { // lleno: se descarta lo más antiguo
        readPos_ = (readPos_ + 1) % kFifoFrames;
        count_--;
      }
      size_t pos = (readPos_ + count_) % kFifoFrames;
      float l = data ? data[i * 2] : 0.0f;
      float r = data ? data[i * 2 + 1] : 0.0f;
      fifo_[pos * 2] = l;
      fifo_[pos * 2 + 1] = r;
      peak = std::max({peak, std::fabs(l), std::fabs(r)});
      count_++;
    }
    framesWritten_ += frames;
  }
  float previous = peak_.load();
  while (peak > previous && !peak_.compare_exchange_weak(previous, peak)) {
  }
}

void AudioCapture::WriteSilence(size_t frames) { Write(nullptr, frames); }

// Coloca la escritura en targetFrames (posición en la línea de tiempo): si
// vamos por detrás se rellena con silencio. Ir por delante se tolera hasta
// kAheadLimit; más allá, quien llama descarta el paquete.
void AudioCapture::AlignTo(int64_t targetFrames) {
  int64_t behind = targetFrames - int64_t(framesWritten_);
  if (behind > kAlignTolerance) WriteSilence(size_t(std::min<int64_t>(behind, kAudioRate * 2)));
}

void AudioCapture::CloseClient() {
  delete client_;
  client_ = nullptr;
}

bool AudioCapture::Open() {
  CloseClient();
  auto c = std::make_unique<Client>();
  WAVEFORMATEX ours = OurFormat();
  const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  std::string deviceLabel;
  std::string formatLabel = "float 48000 Hz 2 canales";
  HRESULT hr = E_FAIL;

  if (kind_ == AudioSourceKind::ProcessLoopback) {
    c->client = ActivateProcessLoopback(processId_);
    if (!c->client) return false;
    c->packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // El dispositivo virtual acepta el formato que se le pida y solo trabaja en modo evento.
    hr = c->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration,
                               0, &ours, nullptr);
    if (SUCCEEDED(hr)) hr = c->client->SetEventHandle(c->packetEvent);
    char pid[32];
    snprintf(pid, sizeof(pid), "proceso %lu", static_cast<unsigned long>(processId_));
    deviceLabel = pid;
    openedId_.clear();
    followDefault_ = false;
  } else {
    const bool isCapture = kind_ == AudioSourceKind::Microphone;
    bool usedDefault = true;
    auto device = GetDevice(deviceId_, isCapture, &usedDefault);
    if (!device) {
      Log("Audio (%s): no hay ningún dispositivo", KindName(kind_));
      return false;
    }
    openedId_ = DeviceId(device.Get());
    followDefault_ = usedDefault;
    deviceLabel = DeviceName(device.Get());
    const DWORD loopback = isCapture ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;

    // 1. El micrófono se pide ya convertido: Windows lo hace bien y barato.
    if (isCapture) {
      if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c->client)))
        hr = c->client->Initialize(AUDCLNT_SHAREMODE_SHARED, convert, kBufferDuration, 0, &ours, nullptr);
      if (FAILED(hr)) Log("Audio (micrófono): conversión automática no disponible (0x%08X)", unsigned(hr));
    }
    // 2. Loopback (o micrófono sin conversión): el formato del mezclador tal
    //    cual, sea el que sea, y lo convertimos nosotros. Es lo que hace OBS y
    //    no depende de que AUTOCONVERTPCM funcione con loopback en ese driver.
    if (FAILED(hr)) {
      c->client.Reset();
      if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c->client))) {
        Log("Audio (%s): Activate falló", KindName(kind_));
        return false;
      }
      WAVEFORMATEX* mix = nullptr;
      if (FAILED(c->client->GetMixFormat(&mix)) || !mix) {
        Log("Audio (%s): GetMixFormat falló", KindName(kind_));
        return false;
      }
      const char* sampleLabel = "";
      AVSampleFormat fmt = SampleFormatOf(mix, &sampleLabel);
      char buf[96];
      snprintf(buf, sizeof(buf), "%s %lu Hz %u canales", sampleLabel, static_cast<unsigned long>(mix->nSamplesPerSec),
               unsigned(mix->nChannels));
      formatLabel = buf;
      if (fmt == AV_SAMPLE_FMT_NONE) {
        // Formato que no sabemos leer: última opción, que convierta Windows.
        hr = c->client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback | convert, kBufferDuration, 0, &ours,
                                   nullptr);
        formatLabel += " (convertido por Windows)";
      } else {
        hr = c->client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback, kBufferDuration, 0, mix, nullptr);
        if (SUCCEEDED(hr)) {
          c->channels = mix->nChannels;
          c->rate = int(mix->nSamplesPerSec);
          c->sampleFormat = fmt;
          if (c->channels != kAudioChannels || c->rate != kAudioRate || fmt != AV_SAMPLE_FMT_FLT) {
            AVChannelLayout in{}, out{};
            uint64_t mask = ChannelMaskOf(mix);
            if (mask && av_popcount64(mask) == c->channels) av_channel_layout_from_mask(&in, mask);
            else av_channel_layout_default(&in, c->channels);
            av_channel_layout_default(&out, kAudioChannels);
            int err = swr_alloc_set_opts2(&c->swr, &out, AV_SAMPLE_FMT_FLT, kAudioRate, &in, fmt, c->rate, 0,
                                          nullptr);
            if (err >= 0) err = swr_init(c->swr);
            av_channel_layout_uninit(&in);
            av_channel_layout_uninit(&out);
            if (err < 0) {
              Log("Audio (%s): no se pudo preparar la conversión: %s", KindName(kind_), AvError(err).c_str());
              CoTaskMemFree(mix);
              return false;
            }
          }
        }
      }
      CoTaskMemFree(mix);
    }
  }

  if (FAILED(hr)) {
    Log("Audio (%s, %s): Initialize falló 0x%08X", KindName(kind_), deviceLabel.c_str(), unsigned(hr));
    return false;
  }
  if (FAILED(hr = c->client->GetService(IID_PPV_ARGS(&c->capture))) || FAILED(hr = c->client->Start())) {
    Log("Audio (%s, %s): no se pudo arrancar el cliente 0x%08X", KindName(kind_), deviceLabel.c_str(), unsigned(hr));
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    description_ = deviceLabel + " (" + formatLabel + ")";
  }
  Log("Audio (%s): %s, %s%s", KindName(kind_), deviceLabel.c_str(), formatLabel.c_str(),
      followDefault_ && kind_ != AudioSourceKind::ProcessLoopback ? ", sigue al predeterminado" : "");
  client_ = c.release();
  return true;
}

void AudioCapture::Run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

  startOk_ = Open();
  SetEvent(readyEvent_);

  const int64_t freq = QpcFrequency();
  int64_t lastData = QpcNow();
  int64_t lastRetry = 0;
  int64_t lastDefaultCheck = QpcNow();
  int64_t lastClockLog = 0;
  std::vector<uint8_t> raw;

  while (startOk_) {
    HANDLE packetEvent = client_ ? client_->packetEvent : nullptr;
    HANDLE handles[2] = {stopEvent_, packetEvent};
    DWORD r = packetEvent ? WaitForMultipleObjects(2, handles, FALSE, 10) : WaitForSingleObject(stopEvent_, 10);
    if (r == WAIT_OBJECT_0) break;

    bool gotData = false;
    if (client_) {
      Client& c = *client_;
      UINT32 packetSize = 0;
      HRESULT hr;
      while (SUCCEEDED(hr = c.capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpcPos = 0;
        if (FAILED(hr = c.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPos))) break;
        // Posición en la línea de tiempo: la marca de WASAPI (100 ns, mismo
        // origen que QueryPerformanceCounter) si es fiable; si no, la hora de llegada.
        int64_t target;
        if (qpcPos != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
          double ticks = double(qpcPos) * double(freq) / 1e7;
          target = int64_t((ticks - double(startQpc_)) * kAudioRate / double(freq));
        } else {
          target = (QpcNow() - startQpc_) * kAudioRate / freq - kAudioRate / 50;
        }
        size_t outFrames = frames;
        if (c.swr) outFrames = size_t(std::max<int64_t>(0, (int64_t(frames) * kAudioRate + c.rate - 1) / c.rate));
        bool ahead = int64_t(framesWritten_) > target + kAheadLimit; // reloj del dispositivo adelantado
        if (!ahead) {
          AlignTo(target);
          if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            WriteSilence(outFrames);
          } else if (!c.swr) {
            Write(reinterpret_cast<const float*>(data), frames);
          } else {
            int cap = swr_get_out_samples(c.swr, int(frames));
            if (cap < int(outFrames)) cap = int(outFrames);
            c.conv.resize(size_t(cap) * kAudioChannels);
            uint8_t* out[1] = {reinterpret_cast<uint8_t*>(c.conv.data())};
            const uint8_t* in[1] = {data};
            int n = swr_convert(c.swr, out, cap, in, int(frames));
            if (n > 0) Write(c.conv.data(), size_t(n));
          }
        }
        c.capture->ReleaseBuffer(frames);
        gotData = true;
      }
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        Log("Audio (%s): dispositivo desconectado, reintentando", KindName(kind_));
        CloseClient();
      }
    } else if (QpcNow() - lastRetry > freq) {
      lastRetry = QpcNow();
      Open();
    }

    int64_t now = QpcNow();
    if (gotData) lastData = now;
    // Nada sonando: WASAPI loopback no entrega paquetes, así que rellenamos
    // con silencio para mantener la línea de tiempo.
    int64_t expected = (now - startQpc_) * kAudioRate / freq;
    int64_t behind = expected - int64_t(framesWritten_);
    if (!gotData && now - lastData > freq / 20 && behind > kAudioRate / 100) WriteSilence(size_t(behind - kAudioRate / 40));

    // Si no se eligió un dispositivo concreto, se sigue al predeterminado de Windows.
    if (followDefault_ && client_ && now - lastDefaultCheck > freq) {
      lastDefaultCheck = now;
      std::wstring current = DefaultDeviceId(kind_ == AudioSourceKind::Microphone);
      if (!current.empty() && current != openedId_) {
        Log("Audio (%s): cambió el dispositivo predeterminado, se reabre", KindName(kind_));
        CloseClient();
        lastRetry = 0;
      }
    }
    if (now - lastClockLog > freq * 60) {
      lastClockLog = now;
      Log("Audio (%s): reloj %+.0f ms, %s", KindName(kind_), double(int64_t(framesWritten_) - expected) * 1000.0 / kAudioRate,
          client_ ? "capturando" : "sin dispositivo");
    }
  }

  CloseClient();
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  CoUninitialize();
}

} // namespace spectre
