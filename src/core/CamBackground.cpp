#include "core/CamBackground.h"

#include "app/CrashHandler.h"
#include "common/Util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace spectre {

namespace {

// Pistas de fondo por sesión de captura como mucho (una cada vez que se abre
// su encoder: al mover la cámara o al volver tras un rato sin ella); el anillo
// guarda la lista entera. La captura se reinicia al abrir o cerrar el juego.
constexpr int kMaxTracks = 16;
// Sin cámara durante este tiempo, su encoder se suelta (VRAM y una sesión
// del encoder de la gráfica). Si vuelve antes, sigue en la misma pista.
constexpr ULONGLONG kIdleMs = 60000;

bool SameRect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// La versión H.264 del encoder del vídeo (hevc_nvenc -> h264_nvenc): el editor
// lee cualquiera, y H.264 es lo que abre en todas las gráficas.
std::string H264Of(const std::string& encoder) {
  for (const char* prefix : {"hevc_", "av1_"}) {
    const size_t n = strlen(prefix);
    if (encoder.compare(0, n, prefix) == 0) return "h264_" + encoder.substr(n);
  }
  return encoder;
}

// Tamaño de la pista para un recuadro de w x h: el mismo, sin pasar de 1280
// de ancho (de 4K, con la cámara al 50 %, serían 1920) ni bajar de 160 x 96,
// que es menos de lo que admiten algunos encoders. El editor la vuelve a
// llevar al tamaño del recuadro.
void AuxSize(LONG w, LONG h, int* aw, int* ah) {
  const double k = std::min(1.0, 1280.0 / std::max<LONG>(w, 2));
  *aw = std::max(int(double(w) * k), 160) & ~1;
  *ah = std::max(int(double(h) * k), 96) & ~1;
}

// Bitrate de la pista: el del vídeo por los píxeles del recuadro, con un 50 %
// más por los keyframes seguidos. Sin las opciones de Fuente, que es solo el
// fondo de un recuadro.
QualityPlan AuxPlan(const QualityPlan& main, int outW, int outH, int w, int h) {
  QualityPlan p = main;
  const double share = std::min(1.0, double(w) * double(h) / std::max(1.0, double(outW) * double(outH))) * 1.5;
  p.maxKbps = std::clamp(int(main.maxKbps * share), 500, 12000);
  p.targetKbps = std::clamp(int(main.targetKbps * share), 400, p.maxKbps);
  if (p.level == kSourceLevel) p.level = 3;
  return p;
}

// Lo contrario del letterbox de VideoConverter::Convert: el recuadro r de la
// salida en píxeles de la textura capturada (src). Si parte de r cae sobre
// las bandas negras (el juego no tiene la proporción del vídeo), dst es la
// parte del fotograma de auxW x auxH que sí es juego; el resto queda negro,
// como en el vídeo.
bool SourceRect(UINT srcW, UINT srcH, int outW, int outH, const RECT& r, int auxW, int auxH, RECT* src, RECT* dst) {
  if (srcW == 0 || srcH == 0 || r.right <= r.left || r.bottom <= r.top) return false;
  const double scale = std::min(double(outW) / srcW, double(outH) / srcH);
  const LONG w = LONG(srcW * scale) & ~1L;
  const LONG h = LONG(srcH * scale) & ~1L;
  const LONG x = (LONG(outW) - w) / 2 & ~1L;
  const LONG y = (LONG(outH) - h) / 2 & ~1L;
  const double ux0 = double(r.left - x) / scale, ux1 = double(r.right - x) / scale;
  const double uy0 = double(r.top - y) / scale, uy1 = double(r.bottom - y) / scale;
  const double cx0 = std::clamp(ux0, 0.0, double(srcW)), cx1 = std::clamp(ux1, 0.0, double(srcW));
  const double cy0 = std::clamp(uy0, 0.0, double(srcH)), cy1 = std::clamp(uy1, 0.0, double(srcH));
  if (cx1 - cx0 < 2 || cy1 - cy0 < 2) return false;
  *src = {LONG(std::lround(cx0)), LONG(std::lround(cy0)), LONG(std::lround(cx1)), LONG(std::lround(cy1))};
  auto dx = [&](double v) { return LONG(std::lround((v - ux0) / (ux1 - ux0) * auxW)) & ~1L; };
  auto dy = [&](double v) { return LONG(std::lround((v - uy0) / (uy1 - uy0) * auxH)) & ~1L; };
  *dst = {dx(cx0), dy(cy0), dx(cx1), dy(cy1)};
  return dst->right > dst->left && dst->bottom > dst->top;
}

// El título que lee ParseCamTrackTitle (VideoEdit): el recuadro en píxeles
// del vídeo.
std::string TrackTitle(const RECT& r) {
  char buf[96];
  snprintf(buf, sizeof(buf), "Fondo de la cámara %ld,%ld,%ld,%ld", long(r.left), long(r.top), long(r.right - r.left),
           long(r.bottom - r.top));
  return buf;
}

} // namespace

int CamBackground::MaxKbps(const QualityPlan& plan, int outWidth, int outHeight, int sizePercent) {
  // La cámara suele ser 16:9 (ver Pipeline::CameraRect).
  const LONG w = LONG(outWidth * sizePercent / 100);
  int aw = 0, ah = 0;
  AuxSize(w, w * 9 / 16, &aw, &ah);
  return AuxPlan(plan, outWidth, outHeight, aw, ah).maxKbps;
}

void CamBackground::Start(ID3D11Device* device, ID3D11DeviceContext* context, std::shared_ptr<ReplayBuffer> buffer,
                          const std::string& encoder, const QualityPlan& plan, int outWidth, int outHeight, int fps) {
  Stop();
  device_ = device;
  context_ = context;
  buffer_ = std::move(buffer);
  encoder_ = H264Of(encoder);
  plan_ = plan;
  outWidth_ = outWidth;
  outHeight_ = outHeight;
  fps_ = std::max(fps, 1);
  asked_ = false;
  lastSeen_ = 0;
  tracks_ = 0;
  disabled_ = false;
  stop_ = false;
  openFailed_ = false;
  worker_ = std::thread([this] {
    InstallThreadCrashHandler();
    Run();
  });
}

void CamBackground::Stop() {
  if (!worker_.joinable()) return;
  if (active_) Retire(std::move(active_));
  activeStream_ = -1;
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  worker_.join();
  buffer_.reset();
  context_.Reset();
  device_.Reset();
}

void CamBackground::Disable(const char* why) {
  if (disabled_) return;
  disabled_ = true;
  Log("Fondo de la cámara: %s; en esta sesión la cámara no se podrá editar en los clips", why);
}

void CamBackground::Retire(std::unique_ptr<Aux> aux) {
  if (!aux) return;
  {
    std::lock_guard lock(mutex_);
    closing_.push_back(std::move(aux));
  }
  cv_.notify_all();
}

void CamBackground::Frame(int64_t index, const CapturedFrame& frame, const RECT* rect) {
  if (!worker_.joinable()) return;
  const ULONGLONG now = GetTickCount64();
  if (!rect || disabled_) {
    if ((active_ || asked_) && (disabled_ || now - lastSeen_ > kIdleMs)) {
      if (active_) Retire(std::move(active_));
      activeStream_ = -1;
      std::unique_ptr<Aux> ready;
      {
        std::lock_guard lock(mutex_);
        ready = std::move(ready_);
      }
      Retire(std::move(ready));
      asked_ = false;
    }
    return;
  }
  lastSeen_ = now;
  if (active_ && !SameRect(active_->rect, *rect)) {
    Retire(std::move(active_));
    activeStream_ = -1;
  }
  if (!active_) {
    std::unique_ptr<Aux> ready;
    {
      std::lock_guard lock(mutex_);
      ready = std::move(ready_);
    }
    if (ready && SameRect(ready->rect, *rect)) {
      active_ = std::move(ready);
      asked_ = false;
      active_->stream = buffer_->AddStream(MakeStreamDesc(active_->encoder.Context(), true, TrackTitle(*rect)));
      activeStream_ = active_->stream;
      ++tracks_;
      Log("Fondo de la cámara: pista %d para el recuadro %s", active_->stream, TrackTitle(*rect).c_str());
    } else {
      Retire(std::move(ready)); // abierto para un sitio de antes
      if (openFailed_) return Disable("no se pudo abrir un segundo encoder por GPU");
      if (tracks_ >= kMaxTracks) return Disable("la cámara ha cambiado de sitio demasiadas veces");
      if (!asked_ || !SameRect(askedRect_, *rect)) {
        asked_ = true;
        askedRect_ = *rect;
        {
          std::lock_guard lock(mutex_);
          openRect_ = *rect;
        }
        cv_.notify_all();
      }
      return; // estos fotogramas se quedan sin fondo: el editor no los toca
    }
  }

  Aux& a = *active_;
  RECT src, dst;
  if (!SourceRect(frame.width, frame.height, outWidth_, outHeight_, *rect, int(a.converter.OutWidth()),
                  int(a.converter.OutHeight()), &src, &dst))
    return;
  const int stream = a.stream;
  auto sink = [this, stream](const AVPacket* pkt, int) { buffer_->Push(pkt, stream); };
  auto render = [&](ID3D11Texture2D* target, UINT slice) {
    return a.converter.ConvertRegion(frame.texture.Get(), src, target, slice, dst);
  };
  const bool ok = a.encoder.Encode(index, render, sink);
  a.errors = ok ? 0 : a.errors + 1;
  if (a.errors > fps_ * 3) Disable("su encoder lleva 3 s sin aceptar fotogramas");
}

std::unique_ptr<CamBackground::Aux> CamBackground::Open(const RECT& rect) {
  auto aux = std::make_unique<Aux>();
  aux->rect = rect;
  int w = 0, h = 0;
  AuxSize(rect.right - rect.left, rect.bottom - rect.top, &w, &h);
  const int64_t t0 = QpcNow();
  if (!aux->converter.Init(device_.Get(), context_.Get(), UINT(w), UINT(h), UINT(fps_))) return nullptr;
  VideoEncoderSettings es;
  es.width = w;
  es.height = h;
  es.fps = fps_;
  es.quality = AuxPlan(plan_, outWidth_, outHeight_, w, h);
  es.codec = "h264";
  es.preferred = encoder_;
  es.hardwareOnly = true;
  es.gopFrames = std::max(fps_ / 4, 1);
  es.role = "fondo de la cámara";
  if (!aux->encoder.Open(es, device_.Get())) {
    GpuLock lock(GpuMutex());
    aux->converter.Reset();
    return nullptr;
  }
  Log("Fondo de la cámara: encoder abierto en %.0f ms", QpcToSeconds(QpcNow() - t0) * 1000.0);
  return aux;
}

void CamBackground::Close(std::unique_ptr<Aux> aux) {
  if (!aux) return;
  // Lo que el encoder aún tenga dentro (NVENC va dos fotogramas por detrás)
  // también entra en el anillo.
  if (aux->stream >= 0) {
    auto buffer = buffer_;
    const int stream = aux->stream;
    aux->encoder.Flush([buffer, stream](const AVPacket* pkt, int) { buffer->Push(pkt, stream); });
  }
  aux->encoder.Close();
  GpuLock lock(GpuMutex());
  aux->converter.Reset();
}

void CamBackground::Run() {
  // Abrir un encoder no tiene prisa: el juego va primero.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  std::unique_lock lock(mutex_);
  for (;;) {
    cv_.wait(lock, [&] { return stop_ || !closing_.empty() || openRect_.has_value(); });
    if (!closing_.empty()) {
      // Primero cerrar: suelta la sesión del encoder antes de abrir otra.
      auto list = std::move(closing_);
      closing_.clear();
      lock.unlock();
      for (auto& aux : list) Close(std::move(aux));
      lock.lock();
      continue;
    }
    if (stop_) break;
    const RECT rect = *openRect_;
    openRect_.reset();
    lock.unlock();
    auto aux = Open(rect);
    lock.lock();
    if (!aux) openFailed_ = true;
    else if (stop_) closing_.push_back(std::move(aux));
    else {
      if (ready_) closing_.push_back(std::move(ready_));
      ready_ = std::move(aux);
    }
  }
  if (ready_) closing_.push_back(std::move(ready_));
  auto list = std::move(closing_);
  closing_.clear();
  openRect_.reset();
  lock.unlock();
  for (auto& aux : list) Close(std::move(aux));
}

} // namespace spectre
