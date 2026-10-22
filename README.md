# Spectre

Spectre graba la pantalla y guarda clips de tus partidas en Windows 10 y 11. Funciona en segundo plano, ocupa poca memoria y usa el codificador de vídeo de tu gráfica, sea NVIDIA, AMD o Intel.

**[Descargar Spectre-Setup.exe](https://github.com/rippipupil/Spectre/releases/latest/download/Spectre-Setup.exe)**

El instalador incluye todo lo necesario y no pide permisos de administrador. Si prefieres no instalar nada, hay un [zip portable](https://github.com/rippipupil/Spectre/releases/latest/download/Spectre-Portable.zip).

## Uso

Al abrir Spectre (desde el menú Inicio, el exe o el icono de la bandeja) se abre su ventana: *Inicio* con lo último que has guardado, la *Biblioteca* con todos tus clips, grabaciones y capturas, un reproductor y todos los ajustes. Cerrar la ventana deja Spectre en la bandeja del sistema, junto al reloj, guardando el replay y con los atajos activos; para salir del todo, usa *Salir de Spectre* en el menú del icono de la bandeja o en *Ajustes > General*.

| Atajo | Qué hace |
| --- | --- |
| `Alt+Z` | Abre el panel sobre el juego |
| `Alt+F10` | Guarda un clip con los últimos segundos |
| `Alt+F9` | Empieza o detiene una grabación |
| `Alt+F1` | Hace una captura de pantalla |
| `Alt+F8` | Marca un momento para encontrarlo luego en el clip |
| `Alt+Shift+F10` | Activa o pausa el replay |

Todos los atajos se cambian en *Ajustes > Atajos*.

El panel de `Alt+Z` es una versión pequeña de la app para usar en mitad de la partida: guardar un clip, cambiar su duración (de 15 segundos a 5 minutos), grabar, hacer una captura, apagar el replay, el micrófono o la cámara, cambiar la calidad y abrir los últimos clips. *Abrir Spectre* lleva a la ventana completa.

En la biblioteca puedes filtrar por tipo y por juego, buscar, marcar favoritos, renombrar, mandar a la papelera y copiar un clip para pegarlo en Discord o WhatsApp. El reproductor muestra las marcas en la línea de tiempo y recorta clips sin recodificarlos. *Exportar para Discord* crea una copia de menos de 20 MB en H.264, que Discord sin Nitro acepta y reproduce dentro del chat, y la deja copiada para pegarla; la hace el encoder de la GPU y el original no cambia. En los ajustes están la calidad de vídeo, los dispositivos de audio, la webcam, una carpeta por juego, un límite de espacio que borra lo más antiguo (nunca los favoritos), los avisos y, en *General*, el tema de la interfaz, que se aplica al momento a la ventana, al panel y a los avisos: *Negro* (negro, gris oscuro y blanco), *Psíquico* (negro violáceo y magenta, con animaciones que se pueden apagar) o cualquiera de los 29 temas de RoCer0 (Medianoche, Daniel, Pablo, Catppuccin, Drácula, Nord, los claros...), con sus colores y planos como el resto de la interfaz.

El panel ocupa todo el alto de la pantalla, pegado a la izquierda como el de NVIDIA, y se mueve arrastrándolo desde la cabecera o desde cualquier hueco: al volver a pulsar `Alt+Z` aparece donde lo dejaste. En *Ajustes > General* se puede volver al panel compacto de antes, que también se arrastra. Se abre encima del juego, también a pantalla completa, sin quitarle el foco: el juego no se minimiza y el panel se queda abierto hasta que lo cierras con `Alt+Z`, `Esc` o un clic fuera. Mientras está abierto, el teclado va al panel y no al juego. Solo hay un caso en el que nada puede dibujarse encima sin meterse dentro del juego (cosa que Spectre no hace, por los anticheats): la pantalla completa *exclusiva* de Direct3D con las optimizaciones de pantalla completa de Windows desactivadas en el exe del juego. Ahí el panel coge el foco para verse: el juego sale de la pantalla completa mientras el panel está abierto (muchos se minimizan, como con `Alt+Tab`) y vuelve a ella al cerrarlo. Para seguir viendo el juego detrás del panel, ponlo en ventana sin bordes o vuelve a activar las optimizaciones.

Los vídeos se guardan en `Vídeos\Spectre`. La configuración y el registro de errores (`spectre.log`) están en `%APPDATA%\Spectre`.

## Por qué consume poco

La imagen se captura con Windows Graphics Capture y se convierte en la propia GPU, que también la comprime con su chip de vídeo (NVENC, AMF o Quick Sync). El replay guarda vídeo ya comprimido en memoria, así que 30 segundos a 1080p ocupan unos 80 MB, y los clips se escriben sin volver a comprimir. El panel solo existe mientras está abierto, y la ventana de la app no gasta nada cerrada ni minimizada. La investigación detrás del diseño está en [docs/investigacion.md](docs/investigacion.md).

## Compilar

Hace falta Visual Studio 2022 o posterior con C++, CMake 3.24 o superior, Ninja y una compilación *shared LGPL* de FFmpeg (por ejemplo de [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases)) descomprimida en `ffmpeg/`.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPECTRE_FFMPEG_DIR=ffmpeg
cmake --build build
cmake --install build --prefix dist
iscc /DSourceDir=..\dist installer\spectre.iss
```

Cada cambio que entra en `main` compila Spectre en GitHub Actions y publica una versión nueva con el instalador en *Releases*.
