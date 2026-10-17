#pragma once

namespace spectre {

// Si Spectre muere por una excepción (de Windows o de C++ sin capturar), lo
// deja escrito en spectre.log (código, dirección y módulo) y guarda un volcado
// en %APPDATA%\Spectre\crash\ para poder encontrar la causa. Sin esto, "se
// cierra sola" no se puede diagnosticar.
void InstallCrashHandler();
// std::set_terminate es por hilo en el CRT de MSVC: cada std::thread lo llama
// al empezar para que sus excepciones C++ sin capturar también dejen rastro.
void InstallThreadCrashHandler();

} // namespace spectre
