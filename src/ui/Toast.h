#pragma once

#include <string>

namespace spectre {

class OverlayWindow;
struct Config;

// Qué cuenta un aviso: decide su icono y su animación. Los avisos son como
// las notificaciones de Medal: guardar un clip, grabar, capturas, marcas,
// juego detectado, versión nueva, errores...
enum class NoticeKind {
  Auto,           // se deduce del texto (MakeNotice)
  Info,
  ClipSaving,     // se pulsó guardar clip y el archivo aún se escribe
  Clip,           // clip guardado
  Recording,      // empezó a grabar
  RecordingSaved,
  Screenshot,
  Marker,
  ReplayOn,
  ReplayOff,
  MicOn,
  MicOff,
  CameraOn,
  CameraOff,
  Quality,
  Game,           // juego detectado: el replay está listo
  Working,        // recortando, uniendo, exportando...
  Saved,          // recorte, unión, edición... guardados
  Export,         // listo para Discord
  Copied,
  Storage,        // limpieza por espacio o antigüedad
  Update,         // versión nueva o Spectre actualizado
  Warning,
  Error,
};

struct Notice {
  NoticeKind kind = NoticeKind::Info;
  std::string title;  // corto, en negrita
  std::string detail; // nombre del archivo, siguiente paso...
  bool error = false;   // en rojo; se ve aunque los avisos estén apagados
  bool warning = false; // en ámbar (queda poco disco); también se ve siempre
};

// Título y detalle a partir de una frase ("Clip guardado: x.mp4",
// "Grabando. Pulsa Alt+F9 para detener") y, con Auto, el tipo por cómo empieza.
Notice MakeNotice(const std::string& text, bool error, NoticeKind kind = NoticeKind::Auto);
// El aviso tiene la animación de "guardado" (clip, grabación, edición...).
bool IsSavedKind(NoticeKind kind);

// Lo que necesita el dibujo de un aviso en cada frame.
struct ToastFrame {
  const Notice* notice = nullptr;
  float age = 0;      // segundos desde que apareció
  float phaseAge = 0; // segundos desde que cambió el contenido (guardando -> guardado)
  float life = 1;     // fracción del tiempo que le queda (barra de tiempo)
  float fade = 1;     // opacidad al desaparecer
  bool fromRight = true;  // esquina en la que sale: entra por ese lado
  bool fromBottom = false;
};

// Ancho del aviso en píxeles lógicos.
constexpr float kToastWidth = 380.0f;
// Alto en píxeles lógicos con el diseño de `config`, estimado por letras:
// sirve para crear la ventana antes de tener fuentes. DrawToast devuelve el
// alto exacto y App corrige la ventana si no coincide.
float EstimateToastHeight(const Config& config, const Notice& notice);
// Dibuja el aviso con el tema y el diseño de `config` y devuelve el alto que
// necesita su contenido (píxeles de la ventana).
float DrawToast(OverlayWindow& window, const Config& config, const ToastFrame& frame);

} // namespace spectre
