#include "capture/Camera.h"

#include "app/CrashHandler.h"
#include "common/Util.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace spectre {

namespace {

// Sin un solo frame en este tiempo (abriéndose o a mitad) se avisa una vez.
constexpr ULONGLONG kStallMs = 10000;
// Lo que espera una cámara nueva a que la anterior suelte el dispositivo.
constexpr DWORD kPreviousWaitMs = 5000;

// Hilos de cámara vivos, también los que se soltaron al parar. Nunca se
// destruye: un hilo que el driver suelte tarde, ya saliendo, aún lo usa.
struct CameraThreads {
  std::mutex mutex;
  std::condition_variable done;
  int count = 0;
  // Último aviso de error mostrado (ver Report).
  std::string lastError;
  ULONGLONG lastErrorAt = 0;
};
CameraThreads& Threads() {
  static auto* threads = new CameraThreads;
  return *threads;
}

// Lanza un hilo suelto que cuenta para WaitForThreads.
bool SpawnCameraThread(std::function<void()> body) {
  CameraThreads& t = Threads();
  {
    std::lock_guard lock(t.mutex);
    ++t.count;
  }
  try {
    std::thread([&t, body = std::move(body)]() mutable {
      InstallThreadCrashHandler();
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      body();
      body = nullptr; // suelta la sesión (su fuente y su textura) antes de contarse como terminado
      CoUninitialize();
      std::lock_guard lock(t.mutex);
      --t.count;
      t.done.notify_all();
    }).detach();
    return true;
  } catch (const std::exception& e) {
    Log("Cámara: no se pudo crear el hilo: %s", e.what());
    std::lock_guard lock(t.mutex);
    --t.count;
    return false;
  }
}

// Avisa al usuario. Al reiniciarse la captura (abrir o cerrar un juego) la
// cámara se vuelve a abrir sola: el mismo error no se repite en 10 min. Si
// la activó él (repeat), se le dice siempre.
void Report(const Camera::Notify& notify, const std::string& text, bool repeat) {
  if (!notify) return;
  CameraThreads& t = Threads();
  {
    std::lock_guard lock(t.mutex);
    ULONGLONG now = GetTickCount64();
    if (!repeat && text == t.lastError && now - t.lastErrorAt < 10 * 60 * 1000) {
      Log("Cámara: (el aviso ya se mostró hace poco, no se repite)");
      return;
    }
    t.lastError = text;
    t.lastErrorAt = now;
  }
  notify(text, true);
}

// Lo que se le dice al usuario cuando la cámara no abre o se corta.
std::string CameraErrorText(HRESULT hr) {
  if (hr == E_ACCESSDENIED)
    return "Windows no deja usar la cámara. Activa «Permitir que las aplicaciones de escritorio accedan a la "
           "cámara» en Configuración > Privacidad y seguridad > Cámara";
  if (hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED ||
      hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_BUSY))
    return "la está usando otra aplicación. Ciérrala y vuelve a activar la cámara";
  if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED || hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) ||
      hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_GEN_FAILURE))
    return "se ha desconectado";
  char buf[64];
  snprintf(buf, sizeof(buf), "error 0x%08X (mira spectre.log)", unsigned(hr));
  return buf;
}

// "MJPG", "YUY2", "NV12"... para el log.
std::string SubtypeName(const GUID& subtype) {
  if (subtype == MFVideoFormat_RGB32) return "RGB32";
  char cc[5] = {char(subtype.Data1 & 0xFF), char((subtype.Data1 >> 8) & 0xFF), char((subtype.Data1 >> 16) & 0xFF),
                char((subtype.Data1 >> 24) & 0xFF), 0};
  for (int i = 0; i < 4; ++i)
    if (cc[i] < 0x20 || cc[i] > 0x7E) return "0x" + std::to_string(subtype.Data1);
  return cc;
}

} // namespace

struct Camera::Session {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  Notify notify;
  ULONGLONG startedAt = GetTickCount64();

  // Con GpuMutex tomado (Latest). Solo el hilo de la cámara los cambia.
  ComPtr<ID3D11Texture2D> texture;
  UINT width = 0, height = 0;
  bool hasFrame = false;

  bool repeatErrors = false; // la activó el usuario: sus errores se ven siempre (Report)
  std::atomic<bool> running{true};
  std::atomic<ULONGLONG> lastFrameAt{0}; // 0 = todavía ninguno
  std::atomic<bool> stallReported{false};
  std::atomic<bool> finished{false};

  // La fuente se apaga desde Stop para desbloquear ReadSample.
  std::mutex sourceMutex;
  ComPtr<IMFMediaSource> source;

  // Se activa cuando el hilo ya soltó la cámara.
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // La sesión anterior: hay que esperar a que suelte el dispositivo antes de
  // abrirlo otra vez (apagar y encender seguido desde el panel).
  std::shared_ptr<Session> previous;

  ~Session() {
    if (done) CloseHandle(done);
  }

  void ShutdownSource() {
    std::lock_guard lock(sourceMutex);
    if (source) source->Shutdown();
  }
};

std::vector<CameraDevice> EnumerateCameras() {
  std::vector<CameraDevice> list;
  if (!MediaFoundationAvailable()) return list;
  ULONGLONG t0 = GetTickCount64();
  ComPtr<IMFAttributes> attrs;
  if (FAILED(MFCreateAttributes(&attrs, 1))) return list;
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  HRESULT hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
  if (FAILED(hr)) {
    Log("Cámara: no se pudo buscar cámaras (0x%08X)", unsigned(hr));
    return list;
  }
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
  // Con algunas cámaras virtuales buscar tarda segundos: que quede en el log.
  ULONGLONG ms = GetTickCount64() - t0;
  if (ms >= 300) Log("Cámara: buscar cámaras tardó %llu ms", ms);
  return list;
}

bool Camera::Start(ID3D11Device* device, const std::string& id, UINT wantWidth, Notify notify, bool userAsked) {
  Stop();
  if (!MediaFoundationAvailable()) {
    Log("Cámara: Media Foundation no está disponible (Windows N)");
    Report(notify, "La cámara necesita Media Foundation, que esta edición de Windows no trae", userAsked);
    return false;
  }
  auto s = std::make_shared<Session>();
  s->device = device;
  device->GetImmediateContext(&s->context);
  s->notify = std::move(notify);
  s->repeatErrors = userAsked;
  {
    static std::mutex lastMutex;
    static std::weak_ptr<Session> last;
    std::lock_guard lock(lastMutex);
    s->previous = last.lock();
    last = s;
  }
  Log("Cámara: se abre en segundo plano (%s, se ve a %u de ancho)",
      id.empty() ? "la primera que haya" : "la elegida en Ajustes", wantWidth);
  if (!SpawnCameraThread([s, id, wantWidth] { Run(s, Widen(id), wantWidth); })) {
    Report(s->notify, "No se pudo activar la cámara", userAsked);
    return false;
  }
  GpuLock lock(GpuMutex());
  session_ = std::move(s);
  return true;
}

void Camera::Stop() {
  std::shared_ptr<Session> s;
  {
    GpuLock lock(GpuMutex());
    s = std::move(session_);
  }
  if (!s) return;
  s->running = false;
  if (s->finished) return;
  // ReadSample es bloqueante: si la cámara deja de mandar imágenes (tapa de
  // privacidad, otra app la coge), apagar la fuente lo desbloquea. Se hace en
  // otro hilo porque con algunos drivers apagarla también tarda, y quien llama
  // es la interfaz: nada de esto la puede congelar.
  ULONGLONG t0 = GetTickCount64();
  SpawnCameraThread([s, t0] {
    s->ShutdownSource();
    if (WaitForSingleObject(s->done, 5000) == WAIT_OBJECT_0)
      Log("Cámara: cerrada en %llu ms", GetTickCount64() - t0);
    else
      Log("Cámara: el driver no la ha soltado en 5 s; su hilo sigue esperando sin bloquear nada");
  });
}

bool Camera::Latest(ComPtr<ID3D11Texture2D>* tex, UINT* width, UINT* height) {
  const auto& s = session_;
  if (!s || !s->hasFrame || !s->texture) return false;
  *tex = s->texture;
  *width = s->width;
  *height = s->height;
  return true;
}

std::string Camera::TakeStall() {
  const auto& s = session_;
  if (!s || s->finished || s->stallReported) return {};
  ULONGLONG last = s->lastFrameAt;
  ULONGLONG now = GetTickCount64();
  if (now - (last ? last : s->startedAt) < kStallMs) return {};
  s->stallReported = true;
  if (!last) {
    Log("Cámara: lleva %llu s sin abrir ni dar imagen", (now - s->startedAt) / 1000);
    return "La cámara no responde: lleva " + std::to_string((now - s->startedAt) / 1000) +
           " s sin dar imagen. Puede que otra aplicación la esté usando. El resto se sigue grabando";
  }
  Log("Cámara: lleva %llu s sin dar imagen", (now - last) / 1000);
  return "La cámara dejó de dar imagen. El resto se sigue grabando";
}

void Camera::WaitForThreads(DWORD timeoutMs) {
  CameraThreads& t = Threads();
  std::unique_lock lock(t.mutex);
  if (!t.done.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&t] { return t.count == 0; }))
    Log("Cámara: %d hilos siguen sin terminar al salir", t.count);
}

void Camera::Run(std::shared_ptr<Session> s, std::wstring id, UINT wantWidth) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  // Al salir, pase lo que pase: la fuente apagada, nadie espera ya por este
  // hilo y una cámara nueva puede abrir el dispositivo.
  struct Finish {
    Session& s;
    ~Finish() {
      {
        std::lock_guard lock(s.sourceMutex);
        if (s.source) s.source->Shutdown();
        s.source.Reset();
      }
      {
        // Una cámara que se corta no deja su última imagen congelada encima del vídeo.
        GpuLock lock(GpuMutex());
        s.hasFrame = false;
      }
      s.finished = true;
      SetEvent(s.done);
    }
  } finish{*s};
  // Error para el usuario solo si no fue él quien la apagó mientras abría.
  auto fail = [&s](const std::string& text) {
    if (s->running) Report(s->notify, text, s->repeatErrors);
  };

  if (auto previous = std::exchange(s->previous, nullptr)) {
    if (WaitForSingleObject(previous->done, 0) != WAIT_OBJECT_0) {
      ULONGLONG t0 = GetTickCount64();
      if (WaitForSingleObject(previous->done, kPreviousWaitMs) == WAIT_OBJECT_0)
        Log("Cámara: la anterior tardó %llu ms en soltarse", GetTickCount64() - t0);
      else
        Log("Cámara: la anterior sigue sin soltarse tras %u ms; se abre igual", unsigned(kPreviousWaitMs));
    }
  }
  if (!s->running) return;
  // Declarado después de Finish: el lector se suelta antes de apagar la fuente.
  ComPtr<IMFSourceReader> reader;

  // 1. Qué cámara: la elegida si sigue conectada; si no, la primera.
  ULONGLONG t0 = GetTickCount64();
  auto cams = EnumerateCameras();
  if (cams.empty()) {
    Log("Cámara: no hay ninguna conectada");
    fail("No hay ninguna cámara conectada");
    return;
  }
  const CameraDevice* pick = &cams.front();
  if (!id.empty()) {
    bool found = false;
    for (const auto& cam : cams) {
      std::wstring link = Widen(cam.id);
      if (CompareStringOrdinal(link.c_str(), -1, id.c_str(), -1, TRUE) == CSTR_EQUAL) {
        pick = &cam;
        found = true;
        break;
      }
    }
    if (!found) Log("Cámara: la elegida en Ajustes no está conectada; se usa «%s»", pick->name.c_str());
  }
  Log("Cámara: abriendo «%s» (%zu conectadas)", pick->name.c_str(), cams.size());
  if (!s->running) return;

  // 2. Fuente y lector. MFCreateDeviceSource es lo que puede tardar (o no
  // volver) si el driver está ocupado: ya no es el hilo de la interfaz.
  ComPtr<IMFAttributes> attrs;
  ComPtr<IMFMediaSource> source;
  MFCreateAttributes(&attrs, 2);
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  std::wstring link = Widen(pick->id);
  attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
  HRESULT hr = MFCreateDeviceSource(attrs.Get(), &source);
  ULONGLONG openMs = GetTickCount64() - t0;
  if (FAILED(hr)) {
    Log("Cámara: no se pudo abrir «%s» (0x%08X) tras %llu ms", pick->name.c_str(), unsigned(hr), openMs);
    fail("No se pudo abrir la cámara: " + CameraErrorText(hr));
    return;
  }
  {
    std::lock_guard lock(s->sourceMutex);
    s->source = source;
    if (!s->running) return; // Stop() llegó mientras se abría: Finish la apaga
  }
  ComPtr<IMFAttributes> readerAttrs;
  MFCreateAttributes(&readerAttrs, 1);
  readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader);
  const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  if (SUCCEEDED(hr)) {
    // Formato nativo: el más pequeño que llegue a wantWidth con 24 fps o más
    // (si ninguno llega, el más grande), sin pasar de 1280x720. A igual
    // tamaño, sin comprimir (MJPEG hay que decodificarlo en la CPU). Muchas
    // cámaras USB 2 dan los tamaños grandes sin comprimir a 5-10 fps.
    struct Score {
      bool fits = false, fluid = false, raw = false;
      UINT64 area = 0;
    };
    auto better = [](const Score& a, const Score& b) { // a mejor que b
      if (a.fits != b.fits) return a.fits;
      if (a.fluid != b.fluid) return a.fluid;
      if (a.area != b.area) return a.fits ? a.area < b.area : a.area > b.area;
      return a.raw && !b.raw;
    };
    ComPtr<IMFMediaType> best;
    Score bestScore;
    UINT32 bestW = 0, bestH = 0, bestNum = 0, bestDen = 0;
    GUID bestSubtype{};
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      if (FAILED(reader->GetNativeMediaType(stream, i, &type))) break;
      UINT32 w = 0, h = 0, num = 0, den = 0;
      MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
      if (w == 0 || h == 0 || w > 1280 || h > 720) continue;
      MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
      GUID subtype{};
      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      Score score{w >= wantWidth, den && num >= 24 * den, subtype != MFVideoFormat_MJPG, UINT64(w) * h};
      if (!best || better(score, bestScore)) {
        best = type;
        bestScore = score;
        bestW = w;
        bestH = h;
        bestNum = num;
        bestDen = den;
        bestSubtype = subtype;
      }
    }
    if (best) {
      HRESULT set = reader->SetCurrentMediaType(stream, nullptr, best.Get());
      Log("Cámara: formato %ux%u %s a %.1f fps%s", bestW, bestH, SubtypeName(bestSubtype).c_str(),
          bestDen ? double(bestNum) / bestDen : 0.0, SUCCEEDED(set) ? "" : " (no lo aceptó, se deja el suyo)");
    }
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(stream, nullptr, out.Get());
  }
  UINT32 w = 0, h = 0;
  LONG defaultStride = 0;
  auto readFormat = [&] {
    ComPtr<IMFMediaType> current;
    if (FAILED(reader->GetCurrentMediaType(stream, &current))) return false;
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
    UINT32 stride = 0;
    defaultStride = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? LONG(INT32(stride)) : LONG(w * 4);
    return w > 0 && h > 0;
  };
  if (FAILED(hr) || !readFormat()) {
    Log("Cámara: no se pudo preparar la lectura de «%s» (0x%08X)", pick->name.c_str(), unsigned(hr));
    fail("No se pudo abrir la cámara: " + CameraErrorText(FAILED(hr) ? hr : E_FAIL));
    return;
  }
  Log("Cámara: abierta en %llu ms (%ux%u, se ve a %u de ancho)", GetTickCount64() - t0, w, h, wantWidth);

  std::vector<uint8_t> flipped;
  bool first = true;
  while (s->running) {
    DWORD flags = 0;
    ComPtr<IMFSample> sample;
    hr = reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample);
    if (!s->running) break;
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
      Log("Cámara: lectura terminada (0x%08X, flags 0x%X)", unsigned(hr), unsigned(flags));
      fail(first ? "No se pudo abrir la cámara: " + CameraErrorText(FAILED(hr) ? hr : E_FAIL)
                 : "La cámara se ha cortado: " + CameraErrorText(FAILED(hr) ? hr : E_FAIL));
      break;
    }
    if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !readFormat()) {
      Log("Cámara: cambió de formato y no se pudo leer el nuevo");
      fail("La cámara se ha cortado: cambió de formato");
      break;
    }
    if (!sample) continue;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;

    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* raw = nullptr;
    DWORD len = 0;
    bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
    if (!locked2d) {
      if (FAILED(buffer->Lock(&raw, nullptr, &len))) continue;
      pitch = defaultStride;
      // Un buffer más corto que la imagen (formato cambiado a medias): se salta.
      if (size_t(len) < size_t(pitch < 0 ? -pitch : pitch) * h) {
        buffer->Unlock();
        continue;
      }
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

    // La textura se crea fuera del lock (el dispositivo es multihilo): solo
    // el contexto inmediato necesita GpuMutex, y así el hilo de vídeo no
    // espera a una creación de textura.
    ComPtr<ID3D11Texture2D> fresh;
    if (!s->texture || s->width != w || s->height != h) {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = w;
      desc.Height = h;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      HRESULT created = s->device->CreateTexture2D(&desc, nullptr, &fresh);
      if (FAILED(created)) Log("Cámara: no se pudo crear su textura %ux%u (0x%08X)", w, h, unsigned(created));
    }
    {
      GpuLock lock(GpuMutex());
      if (fresh) {
        s->texture = fresh;
        s->width = w;
        s->height = h;
      }
      if (s->texture && s->width == w && s->height == h) {
        s->context->UpdateSubresource(s->texture.Get(), 0, nullptr, data, rowPitch, 0);
        s->hasFrame = true;
      }
    }

    if (locked2d) buffer2d->Unlock2D();
    else buffer->Unlock();

    s->lastFrameAt = GetTickCount64();
    if (s->stallReported.exchange(false)) Log("Cámara: vuelve a dar imagen");
    if (first) {
      first = false;
      Log("Cámara: primer frame a los %llu ms de pedirla", GetTickCount64() - s->startedAt);
    }
  }
  reader.Reset();
}

} // namespace spectre
