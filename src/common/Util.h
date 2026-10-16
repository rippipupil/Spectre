#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace spectre {

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);

int64_t QpcNow();
int64_t QpcFrequency();
inline double QpcToSeconds(int64_t ticks) { return double(ticks) / double(QpcFrequency()); }

// %APPDATA%\Spectre (se crea si no existe).
std::filesystem::path AppDataDir();
// Carpeta Vídeos del usuario + \Spectre.
std::filesystem::path DefaultOutputDir();
std::filesystem::path ExecutablePath();

// "2026-10-16_05-30-12"
std::string TimestampForFile();
// Quita caracteres no válidos en nombres de archivo de Windows.
std::string SanitizeFileName(std::string_view name);
std::string FormatBytes(uint64_t bytes);

void Log(const char* fmt, ...);

// Media Foundation (webcam) se enlaza en diferido: no existe en Windows N.
// Comprobar antes de llamar a cualquier función MF*.
bool MediaFoundationAvailable();

// Escrituras de archivos en segundo plano (clips, recortes). Al salir se
// espera a que terminen para no dejar MP4 cortados.
std::atomic<int>& PendingWrites();
struct PendingWrite {
  PendingWrite() { ++PendingWrites(); }
  ~PendingWrite() { --PendingWrites(); }
  PendingWrite(const PendingWrite&) = delete;
  PendingWrite& operator=(const PendingWrite&) = delete;
};
void WaitForPendingWrites(DWORD timeoutMs);

// Un solo mutex protege el contexto inmediato de D3D11, que comparten la
// captura, el encoder, la cámara y el overlay. FFmpeg usa el mismo mutex a
// través de los callbacks lock/unlock de AVD3D11VADeviceContext.
std::recursive_mutex& GpuMutex();
using GpuLock = std::lock_guard<std::recursive_mutex>;

// Mensajes de ventana propios.
constexpr UINT WM_SPECTRE_NOTIFY = WM_APP + 10;   // lParam = new std::string* (toast)
constexpr UINT WM_SPECTRE_SHOW_MENU = WM_APP + 11; // otra instancia pide abrir el menú
constexpr UINT WM_SPECTRE_TRAY = WM_APP + 12;
constexpr UINT WM_SPECTRE_PIPELINE_ERROR = WM_APP + 13; // lParam = new std::string*
constexpr UINT WM_SPECTRE_CLOSE_MENU = WM_APP + 14;     // wParam = generación del menú

} // namespace spectre
