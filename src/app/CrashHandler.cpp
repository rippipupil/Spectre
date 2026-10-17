#include "app/CrashHandler.h"

#include "common/Util.h"

#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <vector>

namespace spectre {

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

LONG volatile g_handling = 0;

std::string ModuleOf(const void* address, uintptr_t* offset) {
  HMODULE module = nullptr;
  *offset = 0;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      !module)
    return "?";
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
  *offset = uintptr_t(address) - uintptr_t(module);
  return Narrow(std::filesystem::path(std::wstring(path, n)).filename().wstring());
}

// Guarda el volcado y deja solo los 5 más recientes.
void WriteDump(EXCEPTION_POINTERS* pointers) {
  HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
  if (!write) {
    Log("CRASH: no se pudo cargar dbghelp.dll, sin volcado");
    return;
  }
  std::error_code ec;
  auto dir = AppDataDir() / L"crash";
  std::filesystem::create_directories(dir, ec);
  auto path = dir / (L"spectre-" + Widen(TimestampForFile()) + L".dmp");
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    Log("CRASH: no se pudo crear %s", Narrow(path.wstring()).c_str());
    return;
  }
  MINIDUMP_EXCEPTION_INFORMATION info{};
  info.ThreadId = GetCurrentThreadId();
  info.ExceptionPointers = pointers;
  info.ClientPointers = FALSE;
  // Pila de todos los hilos y la memoria a la que apuntan; sin las secciones
  // de datos de cada módulo, que harían el archivo de decenas de MB.
  auto type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                            MiniDumpWithUnloadedModules);
  BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, pointers ? &info : nullptr, nullptr, nullptr);
  CloseHandle(file);
  Log("CRASH: volcado %s en %s", ok ? "guardado" : "FALLÓ", Narrow(path.wstring()).c_str());

  std::vector<std::filesystem::path> dumps;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    if (entry.path().extension() == L".dmp") dumps.push_back(entry.path());
  std::sort(dumps.begin(), dumps.end());
  while (dumps.size() > 5) {
    std::filesystem::remove(dumps.front(), ec);
    dumps.erase(dumps.begin());
  }
}

struct DumpRequest {
  EXCEPTION_POINTERS* pointers;
};

DWORD WINAPI DumpThread(void* param) {
  WriteDump(static_cast<DumpRequest*>(param)->pointers);
  return 0;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
  // Si dos hilos fallan a la vez, el segundo espera: si siguiera, Windows
  // mataría el proceso a medio volcado.
  if (InterlockedCompareExchange(&g_handling, 1, 0) != 0) Sleep(INFINITE);
  const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
  if (record) {
    uintptr_t offset = 0;
    std::string module = ModuleOf(record->ExceptionAddress, &offset);
    Log("CRASH: excepción 0x%08X en %s+0x%llX (hilo %lu)", unsigned(record->ExceptionCode), module.c_str(),
        static_cast<unsigned long long>(offset), static_cast<unsigned long>(GetCurrentThreadId()));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
      Log("CRASH: acceso %s a 0x%llX", record->ExceptionInformation[0] == 0 ? "de lectura" : "de escritura",
          static_cast<unsigned long long>(record->ExceptionInformation[1]));
  } else {
    Log("CRASH: excepción sin información");
  }
  // El volcado se escribe desde otro hilo: al que ha fallado puede no quedarle
  // pila (desbordamiento, 0xC00000FD) ni para llamar a MiniDumpWriteDump.
  DumpRequest request{pointers};
  HANDLE thread = CreateThread(nullptr, 0, DumpThread, &request, 0, nullptr);
  if (thread) {
    WaitForSingleObject(thread, 30000);
    CloseHandle(thread);
  } else {
    WriteDump(pointers);
  }
  return EXCEPTION_EXECUTE_HANDLER; // termina el proceso sin el diálogo de Windows
}

// Pasa por el filtro de arriba para que haya volcado también en estos casos.
[[noreturn]] void RaiseFatal(const char* what) {
  Log("CRASH: %s", what);
  __try {
    RaiseException(0xE0535043 /* 'SPC' */, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  } __except (OnUnhandledException(GetExceptionInformation())) {
  }
  TerminateProcess(GetCurrentProcess(), 3);
  std::abort();
}

void OnTerminate() {
  std::string what = "std::terminate";
  if (auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      what = std::string("excepción C++ sin capturar: ") + e.what();
    } catch (...) {
      what = "excepción C++ sin capturar (tipo desconocido)";
    }
  }
  RaiseFatal(what.c_str());
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
  RaiseFatal("parámetro no válido en una función del CRT");
}

void OnPureCall() { RaiseFatal("llamada a función virtual pura"); }

} // namespace

void InstallThreadCrashHandler() { std::set_terminate(OnTerminate); }

void InstallCrashHandler() {
  SetUnhandledExceptionFilter(OnUnhandledException);
  InstallThreadCrashHandler();
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  // Un abort() que se escape de todo lo anterior sigue llegando al informe de
  // errores de Windows (_CALL_REPORTFAULT): solo se quita el mensaje en consola.
  _set_abort_behavior(0, _WRITE_ABORT_MSG);
  // Los errores de carga de DLL o de hardware no sacan el diálogo de Windows.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

} // namespace spectre
