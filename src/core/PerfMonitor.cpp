#include "core/PerfMonitor.h"

#include "app/CrashHandler.h"
#include "common/Util.h"
#include "core/ThreadPolicy.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <map>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "pdh.lib")
#endif

namespace spectre {

void VideoCounters::AddEncode(double ms) {
  encodeSumMs += ms;
  encodeMaxMs = std::max(encodeMaxMs, ms);
  size_t bin = std::min(encodeHist.size() - 1, size_t(std::max(ms, 0.0) / kBinMs));
  encodeHist[bin]++;
}

void VideoCounters::Merge(const VideoCounters& o) {
  slots += o.slots;
  encoded += o.encoded;
  skipped += o.skipped;
  repeated += o.repeated;
  captured += o.captured;
  inPhase += o.inPhase;
  encodeSumMs += o.encodeSumMs;
  encodeMaxMs = std::max(encodeMaxMs, o.encodeMaxMs);
  lateMaxMs = std::max(lateMaxMs, o.lateMaxMs);
  for (size_t i = 0; i < encodeHist.size(); ++i) encodeHist[i] += o.encodeHist[i];
}

double VideoCounters::EncodePercentileMs(double p) const {
  uint64_t total = 0;
  for (auto n : encodeHist) total += n;
  if (!total) return 0;
  uint64_t want = uint64_t(double(total) * p + 0.5), seen = 0;
  for (size_t i = 0; i < encodeHist.size(); ++i) {
    seen += encodeHist[i];
    if (seen >= want) return double(i + 1) * kBinMs; // límite superior de la casilla
  }
  return double(encodeHist.size()) * kBinMs;
}

namespace {

std::string Mb(uint64_t bytes) {
  char buf[32];
  if (bytes >= (10ull << 30)) snprintf(buf, sizeof(buf), "%.0f GB", double(bytes) / double(1ull << 30));
  else if (bytes >= (1ull << 30)) snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / double(1ull << 30));
  else snprintf(buf, sizeof(buf), "%.0f MB", double(bytes) / double(1ull << 20));
  return buf;
}

std::string DescribeVideo(const VideoCounters& v, double seconds) {
  char buf[400];
  double slots = double(std::max<uint64_t>(v.slots, 1));
  snprintf(buf, sizeof(buf),
           "captura %.1f fps, %llu frames codificados en %llu ranuras (saltadas %llu, repetidas %llu, en fase %.0f %%), "
           "codificación media %.2f ms (p99 %.1f, máx %.1f), despertar tarde máx %.1f ms",
           double(v.captured) / std::max(seconds, 0.001), (unsigned long long)v.encoded, (unsigned long long)v.slots,
           (unsigned long long)v.skipped, (unsigned long long)v.repeated, 100.0 * double(v.inPhase) / slots,
           v.encodeSumMs / slots, v.EncodePercentileMs(0.99), v.encodeMaxMs, v.lateMaxMs);
  return buf;
}

// Uso de cada motor de la GPU con los contadores de rendimiento de Windows (los
// mismos que el Administrador de tareas). Una lectura por minuto: el valor es
// la media entre lecturas.
class GpuEngines {
public:
  ~GpuEngines() {
    if (query_) PdhCloseQuery(query_);
  }
  void Open(LUID luid) {
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
      query_ = nullptr;
      return;
    }
    if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter_) != ERROR_SUCCESS) {
      // Windows 10 anterior a 1709: no hay contadores de GPU por motor.
      PdhCloseQuery(query_);
      query_ = nullptr;
      return;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"pid_%lu_", GetCurrentProcessId());
    pidPrefix_ = buf;
    swprintf(buf, 64, L"luid_0x%08lX_0x%08lX", static_cast<unsigned long>(luid.HighPart), luid.LowPart);
    luid_ = buf;
    PdhCollectQueryData(query_);
  }

  // "GPU de Spectre: VideoEncode 7.9 %, 3D 1.1 %; motor más ocupado: 3D 96 %".
  std::string Line() {
    if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return "GPU: sin contadores";
    DWORD size = 0, count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (DWORD(status) != DWORD(PDH_MORE_DATA)) return "GPU: sin contadores";
    std::vector<uint8_t> data(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(data.data());
    if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
      return "GPU: sin contadores";

    std::map<std::wstring, double> ours;    // por tipo de motor
    std::map<std::wstring, double> engines; // por motor de nuestra GPU, sumando procesos
    std::map<std::wstring, std::wstring> engineType;
    for (DWORD i = 0; i < count; ++i) {
      if (items[i].FmtValue.CStatus != ERROR_SUCCESS || !items[i].szName) continue;
      std::wstring name = items[i].szName;
      size_t luidPos = name.find(L"luid_");
      size_t typePos = name.find(L"_engtype_");
      if (luidPos == std::wstring::npos || typePos == std::wstring::npos || typePos < luidPos) continue;
      double value = items[i].FmtValue.doubleValue;
      std::wstring type = name.substr(typePos + 9);
      if (name.compare(0, pidPrefix_.size(), pidPrefix_) == 0) ours[type] += value;
      if (name.compare(luidPos, luid_.size(), luid_) != 0) continue;
      std::wstring engine = name.substr(luidPos, typePos - luidPos);
      engines[engine] += value;
      engineType[engine] = type;
    }
    std::string out = "GPU de Spectre:";
    bool any = false;
    for (const auto& [type, value] : ours) {
      if (value < 0.05) continue;
      char buf[96];
      snprintf(buf, sizeof(buf), "%s %s %.1f %%", any ? "," : "", Narrow(type).c_str(), value);
      out += buf;
      any = true;
    }
    if (!any) out += " casi nada";
    auto busiest = std::max_element(engines.begin(), engines.end(),
                                    [](const auto& a, const auto& b) { return a.second < b.second; });
    if (busiest != engines.end()) {
      char buf[96];
      snprintf(buf, sizeof(buf), "; motor más ocupado de la GPU (con el juego): %s %.0f %%",
               Narrow(engineType[busiest->first]).c_str(), std::min(busiest->second, 100.0));
      out += buf;
    }
    return out;
  }

private:
  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER counter_ = nullptr;
  std::wstring pidPrefix_;
  std::wstring luid_;
};

uint64_t FileTimeTicks(const FILETIME& ft) { return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

uint64_t ProcessCpuTicks() {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  return FileTimeTicks(kernel) + FileTimeTicks(user);
}

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* value, DWORD fallback) {
  DWORD data = 0, size = sizeof(data);
  if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) return fallback;
  return data;
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[256];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
  return buf;
}

const char* YesNo(DWORD v) { return v == 1 ? "sí" : v == 0 ? "no" : "sin valor"; }

} // namespace

void PerfMonitor::Start(IDXGIAdapter1* adapter, int fps, Source source) {
  Stop();
  adapter_.Reset();
  luid_ = {};
  if (adapter) {
    adapter->QueryInterface(IID_PPV_ARGS(&adapter_));
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) luid_ = desc.AdapterLuid;
  }
  periodMs_ = 1000.0 / std::max(fps, 1);
  source_ = std::move(source);
  stop_ = false;
  thread_ = std::thread([this] {
    InstallThreadCrashHandler();
    Run();
  });
}

void PerfMonitor::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_one();
  thread_.join();
}

void PerfMonitor::Run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  SetThreadEcoQoS(true);
  GpuEngines gpu;
  gpu.Open(luid_);
  const double cores = double(std::max<DWORD>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1));

  VideoCounters minute;
  ULONGLONG minuteStart = GetTickCount64(), lastWarn = 0;
  uint64_t cpuStart = ProcessCpuTicks();
  std::unique_lock lock(mutex_);
  for (;;) {
    cv_.wait_for(lock, std::chrono::seconds(5), [&] { return stop_; });
    if (stop_) break;
    lock.unlock();

    // El pipeline copia sus contadores con un lock que también toma el hilo
    // de vídeo en cada frame: ese momento, con prioridad alta, para que este
    // hilo no pueda quedarse sin turno con el lock cogido.
    Sample sample;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    source_(&sample);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    minute.Merge(sample.video);
    ULONGLONG now = GetTickCount64();
    // En el momento, si el hilo de vídeo llegó tarde en estos 5 s: es lo que
    // se ve como tirones en el clip.
    bool late = sample.video.skipped > 0 || sample.video.encodeMaxMs > periodMs_;
    if (late && now - lastWarn >= 30000) {
      Log("Rendimiento (aviso, últimos 5 s): %s", DescribeVideo(sample.video, 5.0).c_str());
      lastWarn = now;
    }

    if (now - minuteStart >= 60000) {
      double seconds = double(now - minuteStart) / 1000.0;
      uint64_t cpu = ProcessCpuTicks();
      double cpuCores = double(cpu - cpuStart) / 1e7 / seconds; // núcleos ocupados de media
      PROCESS_MEMORY_COUNTERS_EX mem{};
      mem.cb = sizeof(mem);
      GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
      std::string vram = "VRAM sin datos";
      DXGI_QUERY_VIDEO_MEMORY_INFO info{};
      if (adapter_ && SUCCEEDED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        vram = "VRAM " + Mb(info.CurrentUsage) + " (el sistema da hasta " + Mb(info.Budget) + ")";
      char proc[200];
      snprintf(proc, sizeof(proc), "CPU %.1f %% de un núcleo (%.1f %% del total), RAM %s (privada %s), %s",
               cpuCores * 100.0, cpuCores * 100.0 / cores, Mb(mem.WorkingSetSize).c_str(),
               Mb(mem.PrivateUsage).c_str(), vram.c_str());
      Log("Rendimiento (%.0f s): %s", seconds, DescribeVideo(minute, seconds).c_str());
      Log("Rendimiento: Spectre usa %s; %s; %s", proc, gpu.Line().c_str(), sample.extra.c_str());
      minute = {};
      minuteStart = now;
      cpuStart = cpu;
    }
    lock.lock();
  }
}

void PerfMonitor::LogEnvironment(HMONITOR captured) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);

  static bool once = false;
  if (!once) {
    once = true;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    if (rtlGetVersion) rtlGetVersion(&os);
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring display = RegString(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion");
    DWORD ubr = RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR", 0);
    Log("Windows %s %s (%lu.%lu.%lu.%lu)", os.dwBuildNumber >= 22000 ? "11" : "10", Narrow(display).c_str(),
        os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, ubr);

    MEMORYSTATUSEX ram{};
    ram.dwLength = sizeof(ram);
    GlobalMemoryStatusEx(&ram);
    std::wstring cpu =
        RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    Log("CPU: %s, %lu hilos; RAM %s", Narrow(cpu).c_str(), GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
        Mb(ram.ullTotalPhys).c_str());

    // «Grabar lo ocurrido» de la Game Bar es otro replay como el nuestro:
    // con los dos a la vez el encoder de la GPU trabaja el doble. El modo de
    // juego, según Medal, puede dejar sin turno a los programas de fondo.
    const wchar_t* dvr = L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR";
    DWORD background = RegDword(HKEY_CURRENT_USER, dvr, L"HistoricalCaptureEnabled", 2);
    Log("Game Bar: capturas %s, grabación en segundo plano %s%s; modo de juego %s",
        YesNo(RegDword(HKEY_CURRENT_USER, dvr, L"AppCaptureEnabled", 2)), YesNo(background),
        background == 1 ? " (AVISO: graba a la vez que Spectre)" : "",
        YesNo(RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1)));
  }

  // Monitores y Hz. Con dos o más monitores y un juego en pantalla completa,
  // Windows 11 24H2 puede bajar la captura a pocos fps
  // (investigacion/lag-causas-mitigacion.md).
  struct Ctx {
    HMONITOR captured;
    std::string text;
    int count = 0;
  } ctx{captured, "", 0};
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR mon, HDC, LPRECT, LPARAM param) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(param);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return TRUE;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        char buf[200];
        snprintf(buf, sizeof(buf), "%s%s %lux%lu %lu Hz%s%s", c->count ? ", " : "", Narrow(mi.szDevice).c_str(),
                 dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                 (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (principal)" : "", mon == c->captured ? " (capturado)" : "");
        c->text += buf;
        c->count++;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  static std::string lastMonitors;
  if (ctx.text != lastMonitors) {
    Log("Monitores (%d): %s", ctx.count, ctx.text.c_str());
    lastMonitors = ctx.text;
  }

  // Otros programas que graban o dibujan sobre el juego: dos replays a la vez
  // duplican el trabajo del encoder, y los overlays inyectados (RTSS,
  // Discord, Steam) son la causa de lag que más citan OBS, Medal y Overwolf.
  static const wchar_t* const kKnown[] = {
      L"obs64.exe",     L"obs32.exe",         L"Medal.exe",         L"MedalEncoder.exe", L"NVIDIA Share.exe",
      L"Overwolf.exe",  L"RTSS.exe",          L"MSIAfterburner.exe", L"SteelSeriesGG.exe", L"bdcam.exe",
      L"fraps.exe",     L"XSplit.Core.exe",   L"Streamlabs OBS.exe", L"AMDRSServ.exe",     L"GameBar.exe",
      L"Discord.exe",   L"steam.exe"};
  std::vector<std::wstring> found;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
      for (const wchar_t* name : kKnown)
        if (_wcsicmp(pe.szExeFile, name) == 0 && std::find(found.begin(), found.end(), name) == found.end())
          found.push_back(name);
    CloseHandle(snap);
  }
  std::string others;
  for (const auto& name : found) others += (others.empty() ? "" : ", ") + Narrow(name);
  static std::string lastOthers = "-";
  if (others != lastOthers) {
    Log("Otros programas de captura u overlay abiertos: %s", others.empty() ? "ninguno" : others.c_str());
    lastOthers = others;
  }
}

} // namespace spectre
