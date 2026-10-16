#pragma once

// Declaraciones de interop WinRT <-> Win32 y de audio por proceso.
// El SDK de Windows las trae en cabeceras propias, pero las declaramos aquí
// para no depender de ellas (y poder hacer chequeos de sintaxis con MinGW).

#include <windows.h>
#include <unknwn.h>
#include <inspectable.h>
#include <dxgi.h>

namespace spectre::interop {

struct DECLSPEC_NOVTABLE IGraphicsCaptureItemInterop : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void** result) = 0;
};

struct DECLSPEC_NOVTABLE IDirect3DDxgiInterfaceAccess : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};

inline constexpr GUID IID_IGraphicsCaptureItemInterop = {
    0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
inline constexpr GUID IID_IDirect3DDxgiInterfaceAccess = {
    0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};

// Valores de audioclientactivationparams.h (Windows 10 2004+).
enum class ActivationType : int { Default = 0, ProcessLoopback = 1 };
enum class ProcessLoopbackMode : int { IncludeTargetProcessTree = 0, ExcludeTargetProcessTree = 1 };
struct ProcessLoopbackParams {
  DWORD TargetProcessId;
  ProcessLoopbackMode Mode;
};
struct AudioClientActivationParams {
  ActivationType Type;
  ProcessLoopbackParams ProcessLoopback;
};
inline constexpr wchar_t kVirtualProcessLoopbackDevice[] = L"VAD\\Process_Loopback";

} // namespace spectre::interop

extern "C" HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice* dxgiDevice,
                                                                  IInspectable** graphicsDevice);

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
