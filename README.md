# Spectre

Grabador y clipper de pantalla para Windows 10/11, ligero y para cualquier gráfica (NVIDIA, AMD e Intel).

- **Clips (replay)**: guarda los últimos 15 s a 5 min con `Alt+F10`.
- **Grabación**: inicia y detiene con `Alt+F9`.
- **Menú overlay**: `Alt+Z` abre el menú encima del juego (duración del clip, audio, cámara, calidad, atajos, rendimiento).
- **Audio**: sonido del PC, solo el del juego o micrófono, en pistas separadas.
- **Cámara**: webcam opcional compuesta en el vídeo.
- **Recorte** de clips sin recodificar.

Los clips se guardan en `Vídeos\Spectre`. La configuración y el log están en `%APPDATA%\Spectre`.

## Descargar

**Instalador:** [Spectre-Setup.exe](https://github.com/rippipupil/Spectre/releases/latest/download/Spectre-Setup.exe). Lo instala todo (FFmpeg incluido, sin redistribuibles extra) y no pide administrador. También hay un [zip portable](https://github.com/rippipupil/Spectre/releases/latest/download/Spectre-Portable.zip).

Para publicar una versión nueva basta con subir una etiqueta `vX.Y.Z`: CI compila y crea la release. Cada compilación deja además los artefactos `spectre-instalador` y `spectre-portable-x64` en *Actions*.

## Cómo consume tan poco

Captura con Windows.Graphics.Capture, convierte a NV12 en la GPU y codifica con el chip de vídeo de la gráfica (NVENC, AMF o QuickSync). El buffer guarda vídeo ya comprimido y los clips se escriben sin recodificar. El menú solo existe mientras está abierto. Detalles en [docs/investigacion.md](docs/investigacion.md).

## Compilar

Requisitos: Visual Studio 2022 o posterior (C++), CMake 3.24+, Ninja y un build *shared LGPL* de FFmpeg (por ejemplo de [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases)) descomprimido en `ffmpeg/`.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPECTRE_FFMPEG_DIR=ffmpeg
cmake --build build
cmake --install build --prefix dist
```

El instalador se genera con Inno Setup: `iscc /DSourceDir=..\dist installer\spectre.iss`.
