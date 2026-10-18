#include "audio/AudioCapture.h"

#include "app/CrashHandler.h"
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
// sin saltos: dentro de ese margen el resampler estira o encoge el audio un
// 0,2 % como mucho (inaudible) hasta volver a cuadrar. Más allá se rellena con
// silencio (por detrás) o se saltan las muestras que sobran (por delante),
// así el desfase con el vídeo se queda en ±20 ms y normalmente en ±2 ms.
constexpr int64_t kAlignTolerance = kAudioRate / 50;
constexpr int64_t kCompensateDeadband = kAudioRate / 500; // 2 ms
constexpr int kCompensateSlowdown = 500;                  // 1 muestra de ajuste cada 500

// Handler mínimo para ActivateAudioInterfaceAsync (debe ser agile).
// Es dueño de su evento: si la activación tarda más que la espera, la
// llamada asíncrona conserva una referencia y el evento sigue vivo hasta que
// llegue ActivateCompleted (antes se cerraba y SetEvent caía en un handle
// cerrado o reutilizado).
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
  ActivationHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~ActivationHandler() {
    if (done_) CloseHandle(done_);
  }
  HANDLE Event() const { return done_; }
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

  auto* handler = new ActivationHandler();
  ComPtr<IActivateAudioInterfaceAsyncOperation> op;
  HRESULT hr = handler->Event() ? ActivateAudioInterfaceAsync(interop::kVirtualProcessLoopbackDevice,
                                                              __uuidof(IAudioClient), &pv, handler, &op)
                                : E_OUTOFMEMORY;
  ComPtr<IAudioClient> client;
  if (FAILED(hr)) {
    Log("Audio por proceso: ActivateAudioInterfaceAsync falló 0x%08X", unsigned(hr));
  } else if (WaitForSingleObject(handler->Event(), 5000) != WAIT_OBJECT_0) {
    Log("Audio por proceso: la activación no respondió en 5 s");
  } else {
    HRESULT activateHr = E_FAIL;
    ComPtr<IUnknown> unk;
    if (SUCCEEDED(op->GetActivateResult(&activateHr, &unk)) && SUCCEEDED(activateHr) && unk) unk.As(&client);
    else Log("Audio por proceso: activación falló 0x%08X", unsigned(activateHr));
  }
  handler->Release();
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

// Avisa cuando Windows cambia el dispositivo predeterminado. Solo levanta
// una bandera: el hilo de captura compara y reabre. Así no hay que enumerar
// dispositivos cada segundo (una llamada COM que a veces tarda).
class DefaultDeviceWatcher final : public IMMNotificationClient {
public:
  explicit DefaultDeviceWatcher(EDataFlow flow) : flow_(flow) {}
  bool TakeChanged() { return changed_.exchange(false); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
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
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == flow_ && role == eConsole) changed_ = true;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
  LONG refs_ = 1;
  EDataFlow flow_;
  std::atomic<bool> changed_{false};
};

// Prepara la conversión a float 48 kHz estéreo desde el formato del cliente.
// SWR_FLAG_RESAMPLE deja el resampler activo aunque la frecuencia coincida,
// para poder usar swr_set_compensation.
SwrContext* MakeResampler(AVSampleFormat fmt, int rate, int channels, uint64_t mask, int* errOut) {
  SwrContext* swr = nullptr;
  AVChannelLayout in{}, out{};
  if (mask && av_popcount64(mask) == channels) av_channel_layout_from_mask(&in, mask);
  else av_channel_layout_default(&in, channels);
  av_channel_layout_default(&out, kAudioChannels);
  int err = swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, kAudioRate, &in, fmt, rate, 0, nullptr);
  if (err >= 0) err = av_opt_set_int(swr, "flags", SWR_FLAG_RESAMPLE, 0);
  if (err >= 0) err = swr_init(swr);
  av_channel_layout_uninit(&in);
  av_channel_layout_uninit(&out);
  if (err < 0) swr_free(&swr);
  *errOut = err;
  return swr;
}

} // namespace

// Todo lo que vive mientras el cliente WASAPI está abierto.
struct AudioCapture::Client {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  HANDLE packetEvent = nullptr; // solo en audio por proceso (modo evento)
  // Siempre hay resampler, aunque llegue ya en float 48 kHz estéreo: es lo
  // que corrige poco a poco la deriva del reloj del dispositivo.
  SwrContext* swr = nullptr;
  int channels = kAudioChannels;
  int rate = kAudioRate;
  AVSampleFormat sampleFormat = AV_SAMPLE_FMT_FLT;
  uint64_t channelMask = 0;
  std::vector<float> conv;
  std::vector<uint8_t> silence; // entrada a cero para los paquetes SILENT
  bool firstPacket = true;
  // Línea de tiempo (muestras a 48 kHz) de la última vez que la cola de
  // WASAPI quedó vacía; -1 con el cliente recién abierto.
  int64_t emptyAt = -1;

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

void AudioCapture::Begin(AudioSourceKind kind, const std::string& deviceId, DWORD processId, int64_t startQpc) {
  Stop();
  kind_ = kind;
  deviceId_ = Widen(deviceId);
  processId_ = processId;
  startQpc_ = startQpc;
  fifo_.assign(kFifoFrames * kAudioChannels, 0.0f);
  readPos_ = count_ = 0;
  framesWritten_ = 0;
  missingFrames_ = lateFrames_ = jumpFrames_ = silentTail_ = maxStall_ = 0;
  discontinuities_ = stalls_ = 0;
  peak_ = 0.0f;
  lag_ = 0;
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  startOk_ = false;
  running_ = true;
  thread_ = std::thread([this] {
    InstallThreadCrashHandler();
    try {
      Run();
    } catch (const std::exception& e) {
      Log("Audio: excepción en el hilo de captura: %s", e.what());
      startOk_ = false;
      SetEvent(readyEvent_);
    }
  });
}

bool AudioCapture::WaitStarted(DWORD timeoutMs) {
  if (!thread_.joinable()) return false;
  // Si el dispositivo tarda demasiado (Bluetooth, activación del audio de un
  // proceso) se da por fallido aunque el hilo termine abriéndolo después.
  bool ok = WaitForSingleObject(readyEvent_, timeoutMs) == WAIT_OBJECT_0 && startOk_;
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

size_t AudioCapture::ReadAt(int64_t pos, float* dst, size_t frames) {
  std::fill(dst, dst + frames * kAudioChannels, 0.0f);
  std::lock_guard lock(mutex_);
  // Muestras anteriores a pos: llegaron tarde (el lector ya pasó por ahí) y
  // ya se rellenaron con silencio, así que se descartan. Nunca se desplazan.
  int64_t head = framesWritten_ - int64_t(count_);
  if (head < pos && count_ > 0) {
    int64_t drop = std::min<int64_t>(pos - head, int64_t(count_));
    readPos_ = (readPos_ + size_t(drop)) % kFifoFrames;
    count_ -= size_t(drop);
    head += drop;
    lateFrames_ += drop;
  }
  // Huecos al principio (anterior a lo que guarda el anillo) se quedan en
  // silencio; luego se copia lo que haya de [pos, pos + frames).
  int64_t lead = std::clamp<int64_t>(head - pos, 0, int64_t(frames));
  size_t offset = size_t(lead);
  size_t n = std::min(frames - offset, count_);
  for (size_t i = 0; i < n; ++i) {
    size_t at = (readPos_ + i) % kFifoFrames;
    dst[(offset + i) * 2] = fifo_[at * 2];
    dst[(offset + i) * 2 + 1] = fifo_[at * 2 + 1];
  }
  readPos_ = (readPos_ + n) % kFifoFrames;
  count_ -= n;
  // Lo que falta al final aún no había llegado cuando se leyó.
  int64_t end = pos + int64_t(frames);
  if (end > framesWritten_) missingFrames_ += std::min<int64_t>(end - std::max(framesWritten_, pos), int64_t(frames));
  return n;
}

void AudioCapture::Write(const float* data, size_t frames) {
  float peak = 0.0f;
  {
    std::lock_guard lock(mutex_);
    // Más de un anillo de silencio seguido es lo mismo que un anillo entero:
    // se cuenta en la línea de tiempo pero no se escribe (y no se bloquea).
    size_t toWrite = data ? frames : std::min(frames, kFifoFrames);
    for (size_t i = 0; i < toWrite; ++i) {
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
    framesWritten_ += int64_t(frames);
  }
  float previous = peak_.load();
  while (peak > previous && !peak_.compare_exchange_weak(previous, peak)) {
  }
}

void AudioCapture::WriteSilence(size_t frames) { Write(nullptr, frames); }

// Coloca la escritura en targetFrames (posición en la línea de tiempo): si
// vamos por detrás más de la tolerancia se rellena con silencio. Devuelve
// cuántas muestras del paquete hay que saltar si vamos por delante.
size_t AudioCapture::AlignTo(int64_t targetFrames, size_t packetFrames, int64_t tolerance) {
  int64_t behind = targetFrames - framesWritten_;
  // Vuelve el sonido después de un rato en silencio: no hay nada audible que
  // cortar, así que se coloca exacto en vez de tolerar hasta 20 ms. Si el
  // relleno de silencio se pasó, se retira la parte aún no leída.
  if (silentTail_ > 0) {
    if (behind < 0) {
      std::lock_guard lock(mutex_);
      int64_t rewind = std::min({-behind, silentTail_, int64_t(count_)});
      count_ -= size_t(rewind);
      framesWritten_ -= rewind;
      behind += rewind;
    }
    silentTail_ = 0;
    tolerance = 0;
  }
  if (behind > tolerance) {
    jumpFrames_ += behind;
    WriteSilence(size_t(behind));
    return 0;
  }
  if (-behind > tolerance) {
    int64_t skip = std::min<int64_t>(-behind, int64_t(packetFrames));
    jumpFrames_ += skip;
    return size_t(skip);
  }
  return 0;
}

// Dentro de la tolerancia, el resampler absorbe la diferencia sin saltos:
// genera una muestra de más (o de menos) cada kCompensateSlowdown hasta que
// la escritura vuelve a cuadrar con la marca de tiempo. Se recalcula en cada
// paquete con el error actual, así que converge y no se pasa.
void AudioCapture::Compensate(int64_t targetFrames, int64_t deadband, int64_t tolerance) {
  if (!client_ || !client_->swr) return;
  int64_t error = targetFrames - framesWritten_; // > 0: vamos por detrás, estirar
  if (std::abs(error) <= deadband || std::abs(error) > tolerance) error = 0;
  swr_set_compensation(client_->swr, int(error), int(std::abs(error) * kCompensateSlowdown));
}

// Con qué retraso entrega el dispositivo: lo vieja que es la última muestra
// del paquete cuando llega. Se guarda el máximo y baja despacio (5 muestras
// por paquete, ~10 ms/s), para que el lector se mantenga detrás aunque un
// dispositivo (Bluetooth) entregue siempre tarde.
void AudioCapture::UpdateLag(int64_t packetEndFrames, int64_t nowFrames) {
  int64_t lag = std::clamp<int64_t>(nowFrames - packetEndFrames, 0, kAudioRate);
  int64_t decayed = std::max<int64_t>(lag_.load() - 5, 0);
  lag_ = std::max(lag, decayed);
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
          c->channelMask = ChannelMaskOf(mix);
        }
      }
      CoTaskMemFree(mix);
    }
  }

  if (FAILED(hr)) {
    Log("Audio (%s, %s): Initialize falló 0x%08X", KindName(kind_), deviceLabel.c_str(), unsigned(hr));
    return false;
  }
  int err = 0;
  c->swr = MakeResampler(c->sampleFormat, c->rate, c->channels, c->channelMask, &err);
  if (!c->swr) {
    Log("Audio (%s): no se pudo preparar la conversión: %s", KindName(kind_), AvError(err).c_str());
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
  REFERENCE_TIME period = 0, latency = 0;
  c->client->GetDevicePeriod(&period, nullptr);
  c->client->GetStreamLatency(&latency);
  Log("Audio (%s): %s, %s%s, periodo %.1f ms, latencia %.1f ms", KindName(kind_), deviceLabel.c_str(),
      formatLabel.c_str(), followDefault_ && kind_ != AudioSourceKind::ProcessLoopback ? ", sigue al predeterminado" : "",
      double(period) / 10'000.0, double(latency) / 10'000.0);
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
  // Si no se eligió un dispositivo concreto, se sigue al predeterminado de
  // Windows: el aviso llega por IMMNotificationClient.
  auto enumerator = Enumerator();
  DefaultDeviceWatcher* watcher = nullptr;
  if (enumerator && kind_ != AudioSourceKind::ProcessLoopback) {
    watcher = new DefaultDeviceWatcher(kind_ == AudioSourceKind::Microphone ? eCapture : eRender);
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(watcher))) {
      watcher->Release();
      watcher = nullptr;
    }
  }
  int64_t lastDefaultCheck = QpcNow();
  int64_t lastLoop = QpcNow();
  int64_t lastClockLog = QpcNow();
  int clockLogs = 0;

  while (startOk_) {
    HANDLE packetEvent = client_ ? client_->packetEvent : nullptr;
    HANDLE handles[2] = {stopEvent_, packetEvent};
    DWORD r = packetEvent ? WaitForMultipleObjects(2, handles, FALSE, 10) : WaitForSingleObject(stopEvent_, 10);
    if (r == WAIT_OBJECT_0) break;
    // Vueltas del bucle que tardan mucho (reabrir el dispositivo, el sistema
    // ocupado cargando un juego...): el audio de ese rato se pierde, ya no se
    // desplaza, pero conviene verlo en el log.
    {
      int64_t start = QpcNow();
      int64_t loop = start - lastLoop;
      lastLoop = start;
      if (loop > freq / 20) {
        std::lock_guard lock(mutex_);
        stalls_++;
        maxStall_ = std::max(maxStall_, loop);
      }
    }

    bool gotData = false;
    if (client_) {
      Client& c = *client_;
      UINT32 packetSize = 0;
      HRESULT hr;
      bool firstInBatch = true;
      while (SUCCEEDED(hr = c.capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpcPos = 0;
        if (FAILED(hr = c.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPos))) break;
        const int64_t arrival = QpcNow();
        const int64_t nowFrames = (arrival - startQpc_) * kAudioRate / freq;
        const int64_t packetOut = int64_t(frames) * kAudioRate / c.rate;
        // Muestras de salida que el resampler aún retiene: la primera que
        // saque ahora es así de anterior a la primera del paquete.
        const int64_t swrDelay = swr_get_delay(c.swr, kAudioRate);
        // Posición en la línea de tiempo de la primera muestra que sale.
        // Loopback (dispositivo o proceso): la marca de WASAPI (100 ns, mismo
        // origen que QueryPerformanceCounter), como hace OBS. Micrófono, o
        // marca no fiable: la hora de llegada menos un paquete, o antes si la
        // cola quedó vacía antes (porque muchos drivers de micrófono dan
        // marcas erróneas); los demás paquetes del mismo lote van seguidos.
        const bool deviceTiming =
            kind_ != AudioSourceKind::Microphone && qpcPos != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
        int64_t stampQpc = 0;
        int64_t target;
        if (deviceTiming) {
          stampQpc = int64_t(double(qpcPos) * double(freq) / 1e7);
          target = (stampQpc - startQpc_) * kAudioRate / freq - swrDelay;
        } else if (firstInBatch) {
          // Lo capturado desde que la cola quedó vacía la última vez está todo
          // en este lote, así que su primera muestra no es posterior a ese
          // momento: tras un parón del hilo el atasco va a su sitio y no a
          // "ahora menos un paquete".
          int64_t first = nowFrames - packetOut;
          if (c.emptyAt >= 0) first = std::min(first, c.emptyAt);
          target = first - swrDelay;
        } else {
          target = framesWritten_;
        }
        // La hora de llegada tiembla lo que tarde en despertar el hilo, así
        // que con ella se toleran más diferencias antes de corregir.
        const int64_t tolerance = deviceTiming ? kAlignTolerance : kAlignTolerance * 2;
        const int64_t deadband = deviceTiming ? kCompensateDeadband : kAudioRate / 100;
        if (deviceTiming || firstInBatch) Compensate(target, deadband, tolerance);
        firstInBatch = false;

        if (c.firstPacket) {
          c.firstPacket = false;
          if (deviceTiming)
            Log("Audio (%s): primer paquete de %u muestras, marca del dispositivo, llega %.1f ms después de "
                "capturarse, resampler %.2f ms",
                KindName(kind_), frames, QpcToSeconds(arrival - stampQpc) * 1000.0,
                double(swrDelay) * 1000.0 / kAudioRate);
          else
            Log("Audio (%s): primer paquete de %u muestras, hora de llegada%s, resampler %.2f ms", KindName(kind_),
                frames, kind_ == AudioSourceKind::Microphone ? "" : " (marca no fiable)",
                double(swrDelay) * 1000.0 / kAudioRate);
        } else if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
          std::lock_guard lock(mutex_);
          discontinuities_++;
        }

        // Todo pasa por el resampler (también el silencio), así su retardo y
        // la compensación de deriva son siempre los mismos.
        const uint8_t* input = data;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          int bytes = av_samples_get_buffer_size(nullptr, c.channels, int(frames), c.sampleFormat, 1);
          if (bytes > 0) {
            if (c.silence.size() < size_t(bytes)) c.silence.resize(size_t(bytes));
            uint8_t* planes[1] = {c.silence.data()};
            av_samples_set_silence(planes, 0, int(frames), c.channels, c.sampleFormat);
            input = c.silence.data();
          }
        }
        int cap = std::max(swr_get_out_samples(c.swr, int(frames)), int(packetOut) + 64);
        c.conv.resize(size_t(cap) * kAudioChannels);
        uint8_t* out[1] = {reinterpret_cast<uint8_t*>(c.conv.data())};
        const uint8_t* in[1] = {input};
        int n = swr_convert(c.swr, out, cap, in, int(frames));
        size_t outFrames = size_t(std::max(n, 0));
        c.capture->ReleaseBuffer(frames);

        UpdateLag(target + int64_t(outFrames), nowFrames);
        // Reloj del dispositivo adelantado: se saltan las muestras que sobran
        // (o el paquete entero); atrasado: se rellena con silencio.
        size_t skip = AlignTo(target, outFrames, tolerance);
        if (skip < outFrames) Write(c.conv.data() + skip * kAudioChannels, outFrames - skip);
        gotData = true;
      }
      // El bucle sale con GetNextPacketSize = 0: la cola está vacía ahora.
      if (SUCCEEDED(hr)) c.emptyAt = (QpcNow() - startQpc_) * kAudioRate / freq;
      if (FAILED(hr)) {
        // Dispositivo desconectado, servicio de audio reiniciado al instalar
        // un driver... Cualquier error deja el cliente muerto: se reabre.
        Log("Audio (%s): error 0x%08X, se reabre", KindName(kind_), unsigned(hr));
        CloseClient();
        lastRetry = QpcNow();
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
    int64_t fill = expected - framesWritten_ - kAudioRate / 40; // hasta 25 ms antes de "ahora"
    if (!gotData && now - lastData > freq / 20 && fill > 0) {
      WriteSilence(size_t(fill));
      silentTail_ += fill;
    }

    // El aviso de cambio se confirma comparando ids (también llega cuando
    // cambia el de comunicaciones o vuelve a ser el mismo). Sin aviso
    // registrado se comprueba cada 5 s.
    bool check = watcher ? watcher->TakeChanged() : now - lastDefaultCheck > freq * 5;
    if (followDefault_ && client_ && check) {
      lastDefaultCheck = now;
      std::wstring current = DefaultDeviceId(kind_ == AudioSourceKind::Microphone);
      if (!current.empty() && current != openedId_) {
        Log("Audio (%s): cambió el dispositivo predeterminado, se reabre", KindName(kind_));
        CloseClient();
        lastRetry = 0;
      }
    }
    // Estado del reloj a los 10 s y luego cada 5 min: desfase de la escritura
    // con "ahora", cuánto va por detrás como mucho, lo que el lector no
    // encontró a tiempo o llegó tarde y los saltos de reloj.
    if (now - lastClockLog > (clockLogs == 0 ? freq * 10 : freq * 300)) {
      lastClockLog = now;
      clockLogs++;
      int64_t missing, late, jumps, maxStall;
      int disc, stalls;
      {
        std::lock_guard lock(mutex_);
        missing = missingFrames_;
        late = lateFrames_;
        jumps = jumpFrames_;
        disc = discontinuities_;
        stalls = stalls_;
        maxStall = maxStall_;
      }
      auto ms = [](int64_t f) { return double(f) * 1000.0 / kAudioRate; };
      Log("Audio (%s): reloj %+.0f ms, retraso máx %.0f ms, sin llegar a tiempo %.0f ms, tarde %.0f ms, saltos %.0f "
          "ms, discontinuidades %d, parones %d (máx %.0f ms), %s",
          KindName(kind_), ms(framesWritten_ - expected), ms(lag_.load()), ms(missing), ms(late), ms(jumps), disc,
          stalls, QpcToSeconds(maxStall) * 1000.0, client_ ? "capturando" : "sin dispositivo");
    }
  }

  CloseClient();
  if (watcher) {
    enumerator->UnregisterEndpointNotificationCallback(watcher);
    watcher->Release();
  }
  enumerator.Reset();
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  CoUninitialize();
}

} // namespace spectre
