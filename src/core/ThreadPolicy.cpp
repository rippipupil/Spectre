#include "core/ThreadPolicy.h"

namespace spectre {

void SetThreadEcoQoS(bool eco) {
  // SetThreadInformation(ThreadPowerThrottling) existe desde Windows 10 1709;
  // se busca en tiempo de ejecución porque no todas las cabeceras la declaran.
  struct PowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
  };
  constexpr int kThreadPowerThrottling = 3;       // THREAD_INFORMATION_CLASS
  constexpr ULONG kExecutionSpeed = 0x1;          // THREAD_POWER_THROTTLING_EXECUTION_SPEED
  using SetThreadInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
  static const auto fn = reinterpret_cast<SetThreadInformationFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation")));
  if (!fn) return;
  PowerThrottlingState state{1, kExecutionSpeed, eco ? kExecutionSpeed : 0};
  fn(GetCurrentThread(), kThreadPowerThrottling, &state, sizeof(state));
}

} // namespace spectre
