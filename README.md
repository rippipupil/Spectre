# Spectre

Spectre graba la pantalla y guarda clips de tus partidas en Windows 10 y 11. Funciona en segundo plano, ocupa poca memoria y usa el codificador de vídeo de tu gráfica, sea NVIDIA, AMD o Intel.

**[Descargar Spectre-Setup.exe](https://github.com/rippipupil/Spectre/releases/latest/download/Spectre-Setup.exe)**

El instalador incluye todo lo necesario y no pide permisos de administrador. Si prefieres no instalar nada, hay un [zip portable](https://github.com/rippipupil/Spectre/releases/latest/download/Spectre-Portable.zip).

## Uso

Spectre se queda en la bandeja del sistema, junto al reloj. Cerrar el panel no lo cierra: para salir, usa *Salir de Spectre* en el menú del icono de la bandeja.

| Atajo | Qué hace |
| --- | --- |
| `Alt+Z` | Abre el panel lateral |
| `Alt+F10` | Guarda un clip con los últimos segundos |
| `Alt+F9` | Empieza o detiene una grabación |
| `Alt+Shift+F10` | Activa o pausa el replay |

Desde el panel puedes guardar un clip, cambiar su duración (de 15 segundos a 5 minutos), grabar, silenciar el micrófono, encender la cámara y abrir los últimos clips. En *Ajustes* están la calidad de vídeo, los dispositivos de audio, la posición de la webcam, los atajos y un recortador de clips que no recodifica.

El panel se abre encima del juego, también a pantalla completa, y se queda encima mientras está abierto. Solo hay un caso en el que nada puede dibujarse encima sin meterse dentro del juego (cosa que Spectre no hace, por los anticheats): la pantalla completa *exclusiva* de Direct3D con las optimizaciones de pantalla completa de Windows desactivadas en el exe del juego. Ahí, pon el juego en ventana sin bordes o vuelve a activar las optimizaciones.

Los vídeos se guardan en `Vídeos\Spectre`. La configuración y el registro de errores (`spectre.log`) están en `%APPDATA%\Spectre`.

## Por qué consume poco

La imagen se captura con Windows Graphics Capture y se convierte en la propia GPU, que también la comprime con su chip de vídeo (NVENC, AMF o Quick Sync). El replay guarda vídeo ya comprimido en memoria, así que 30 segundos a 1080p ocupan unos 80 MB, y los clips se escriben sin volver a comprimir. El panel solo existe mientras está abierto. La investigación detrás del diseño está en [docs/investigacion.md](docs/investigacion.md).

## Compilar

Hace falta Visual Studio 2022 o posterior con C++, CMake 3.24 o superior, Ninja y una compilación *shared LGPL* de FFmpeg (por ejemplo de [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases)) descomprimida en `ffmpeg/`.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPECTRE_FFMPEG_DIR=ffmpeg
cmake --build build
cmake --install build --prefix dist
iscc /DSourceDir=..\dist installer\spectre.iss
```

Cada cambio que entra en `main` compila Spectre en GitHub Actions y publica una versión nueva con el instalador en *Releases*.
