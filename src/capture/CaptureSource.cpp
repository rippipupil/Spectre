#include "capture/CaptureSource.h"

#include "common/Util.h"

#include <algorithm>
#include <cstdio>

namespace spectre {

int MonitorRefreshHz(HMONITOR monitor) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!monitor || !GetMonitorInfoW(monitor, &mi)) return 0;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
  // 0 y 1 significan «el del hardware», sin decir cuál.
  return dm.dmDisplayFrequency > 1 ? int(dm.dmDisplayFrequency) : 0;
}

std::vector<MonitorInfo> EnumerateMonitors() {
  std::vector<MonitorInfo> list;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR mon, HDC, LPRECT, LPARAM param) -> BOOL {
        auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(param);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
          MonitorInfo info;
          info.handle = mon;
          info.rect = mi.rcMonitor;
          info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
          info.refreshHz = MonitorRefreshHz(mon);
          char buf[160];
          snprintf(buf, sizeof(buf), "Pantalla %zu (%ldx%ld)%s", out->size() + 1,
                   long(mi.rcMonitor.right - mi.rcMonitor.left), long(mi.rcMonitor.bottom - mi.rcMonitor.top),
                   info.primary ? " - principal" : "");
          info.name = buf;
          out->push_back(info);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&list));
  return list;
}

void CaptureSource::Publish(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT w, UINT h) {
  D3D11_TEXTURE2D_DESC sd;
  src->GetDesc(&sd);
  w = std::min(w, sd.Width) & ~1u;
  h = std::min(h, sd.Height) & ~1u;
  if (w == 0 || h == 0) return;

  bool recreate = !buffer_;
  if (buffer_) {
    D3D11_TEXTURE2D_DESC bd;
    buffer_->GetDesc(&bd);
    recreate = bd.Width != w || bd.Height != h || bd.Format != sd.Format;
  }
  if (recreate) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = sd.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex))) return;
    buffer_ = tex;
  }
  D3D11_BOX box{0, 0, 0, w, h, 1};
  ctx->CopySubresourceRegion(buffer_.Get(), 0, 0, 0, 0, src, 0, &box);

  std::lock_guard lock(mutex_);
  latest_.texture = buffer_;
  latest_.width = w;
  latest_.height = h;
  latest_.sequence++;
  latest_.qpc = QpcNow();
}

} // namespace spectre
