#include "ui/PanelMedia.h"

#include "common/Util.h"
#include "ui/OverlayWindow.h"

#include <algorithm>
#include <utility>

namespace spectre {

namespace {
constexpr size_t kMaxThumbs = 40; // vídeos con miniatura en RAM entre aperturas del panel
constexpr int kLargeFrom = 160;   // desde este ancho en píxeles se usa la miniatura grande
} // namespace

void PanelMedia::Begin(HWND notifyWindow, UINT notifyMsg) {
  worker_.Start(notifyWindow, notifyMsg);
  running_ = true;
  if (thumbs_.size() <= kMaxThumbs) return;
  // Fuera las que hace más que no se ven.
  std::vector<std::pair<unsigned long long, std::string>> byUse;
  for (const auto& [path, t] : thumbs_) byUse.emplace_back(t.lastUse, path);
  std::sort(byUse.begin(), byUse.end());
  for (size_t i = 0; i + kMaxThumbs < byUse.size(); ++i) thumbs_.erase(byUse[i].second);
}

void PanelMedia::End() {
  Release();
  // Sin esperar: una miniatura de una carpeta de red puede tardar. El hilo
  // acaba solo y lo recoge el siguiente Begin.
  if (running_) worker_.Stop(false);
  running_ = false;
  for (auto& [path, t] : thumbs_) {
    t.row.pending = false;
    // La grande solo sirve con el clip abierto.
    t.view = Slot{};
  }
}

void PanelMedia::Release() {
  ClosePreview();
  drawn_.clear();
  for (auto& [path, t] : thumbs_) {
    t.row.texture.reset();
    t.view.texture.reset();
  }
}

void PanelMedia::NewFrame() { drawn_.clear(); }

void PanelMedia::TakeResults() {
  for (auto& r : worker_.Take()) {
    if (r.job != MediaWorker::Job::Thumbnail) continue;
    auto it = thumbs_.find(r.path);
    if (it == thumbs_.end()) continue;
    // De cada vídeo se pide solo una a la vez (el hilo descarta las repetidas
    // del mismo archivo), así que es la que esté pendiente.
    Slot& slot = it->second.view.pending ? it->second.view : it->second.row;
    if (!slot.pending) continue;
    slot.pending = false;
    slot.failed = !r.ok;
    if (!r.ok) continue;
    slot.bgra = std::move(r.bgra);
    slot.width = r.width;
    slot.height = r.height;
    slot.texture.reset();
  }
}

void PanelMedia::Sync(OverlayWindow& window) {
  if (window.Generation() == generation_) return;
  // Otra GPU (la ventana se recreó): lo de antes no se puede dibujar en ella.
  generation_ = window.Generation();
  Release();
}

UiTexture* PanelMedia::Use(OverlayWindow& window, const std::string& path, Slot& slot, int width, int height,
                           bool mayRequest) {
  if (mayRequest && running_ && !slot.pending && !slot.failed && slot.bgra.empty() && width > 0 && height > 0) {
    slot.pending = true;
    slot.requested = width;
    worker_.Request(MediaWorker::Job::Thumbnail, path, width, height);
  }
  if (!slot.texture && !slot.bgra.empty()) slot.texture = window.CreateTexture(slot.bgra.data(), slot.width, slot.height);
  if (slot.texture) drawn_.push_back(slot.texture);
  return slot.texture.get();
}

UiTexture* PanelMedia::Thumbnail(OverlayWindow& window, const MediaItem& item, int width, int height) {
  Sync(window);
  Thumb& t = thumbs_[item.path];
  if (t.modified != item.modified) {
    // El archivo cambió (otro con el mismo nombre): la de antes no vale.
    t = Thumb{};
    t.modified = item.modified;
  }
  t.lastUse = GetTickCount64();
  if (width < kLargeFrom) return Use(window, item.path, t.row, width, height, !t.view.pending);
  if (UiTexture* big = Use(window, item.path, t.view, width, height, !t.row.pending)) return big;
  // Hasta que llega la grande, la pequeña ampliada (sin pedirla otra vez).
  return Use(window, item.path, t.row, 0, 0, false);
}

Player* PanelMedia::Preview(OverlayWindow& window, const MediaItem& item, int frameWidth, int frameHeight) {
  Sync(window);
  if (previewPath_ != item.path) {
    player_.Close();
    previewPath_ = item.path;
    previewFailed_ = !window.VideoSupport() || !player_.Open(window.Device(), item.path, true);
    if (previewFailed_)
      Log("Panel: sin vista previa de %s (%s)", item.name.c_str(),
          !Player::Available()    ? "este Windows no tiene Media Foundation"
          : window.VideoSupport() ? "Media Foundation no lo pudo abrir"
                                  : "la GPU del panel no admite vídeo");
    else player_.SetMuted(muted);
  }
  if (previewFailed_) return nullptr;
  player_.SetFrameSize(frameWidth, frameHeight);
  return &player_;
}

UiTexture* PanelMedia::PreviewFrame() {
  if (previewFailed_ || !player_.IsOpen()) return nullptr;
  std::shared_ptr<UiTexture> frame = player_.Frame();
  if (frame) drawn_.push_back(frame);
  return frame.get();
}

void PanelMedia::ClosePreview() {
  player_.Close();
  previewPath_.clear();
  previewFailed_ = false;
}

} // namespace spectre
