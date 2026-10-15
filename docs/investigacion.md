# Spectre: investigación y arquitectura recomendada

Objetivo: una app de PC tipo ShadowPlay / Medal que guarde clips (replay buffer) y grabe la pantalla con un atajo, con un menú overlay tipo Alt+Z, selección de audio de entrada y salida, cámara opcional, y **el menor consumo posible de RAM, CPU y GPU** en prácticamente cualquier gráfica.

Plataforma elegida: **Windows 10 (2004 o superior) y Windows 11**, ya que la petición era "app de PC" sin más. Es donde están casi todos los jugadores y donde existen las APIs de captura y codificación por hardware que hacen falta.

---

## 1. Resumen en una línea

Núcleo nativo en **C++20** que captura con **Windows.Graphics.Capture** (fallback DXGI Desktop Duplication), convierte el color en la GPU, codifica con el **encoder hardware de la gráfica** (NVENC / AMF / QuickSync, vía FFmpeg) sin copiar frames a la CPU, guarda el replay buffer **ya comprimido** en un anillo en RAM, y al pulsar el atajo hace un **remux sin recodificar** a MP4. La UI es un overlay **Dear ImGui sobre el mismo dispositivo D3D11**, que solo se crea cuando se abre. En reposo la app vive en la bandeja del sistema.

---

## 2. Por qué las apps actuales consumen lo que consumen

| App | Cómo está hecha | Consecuencia |
|---|---|---|
| NVIDIA ShadowPlay / NVIDIA App | Captura y NVENC dentro del driver, todo en la GPU | Impacto muy bajo (típicamente 1-5 % de FPS), pero **solo NVIDIA** |
| AMD ReLive / Adrenalin | Igual, con AMF | Bajo impacto, **solo AMD** |
| Xbox Game Bar | WGC + Media Foundation | Funciona en cualquier GPU, pero con menos control y a veces más impacto |
| Medal.tv | Cliente **Electron** (Chromium + Node) más un proceso de captura | La interfaz sola puede ocupar cientos de MB de RAM aunque no la mires |
| OBS (replay buffer) | Nativo C/C++, hooks de juego, encoders hardware | Muy eficiente, pero pensado para streaming, no para "instalar y olvidar" |
| Steam Game Recording | Nativo, buffer en disco por defecto | Bajo impacto, pero solo juegos de Steam y escribe al disco continuamente |

Conclusiones que guían el diseño:
1. Lo caro no es capturar, es **mover frames entre GPU y CPU** y **codificar por software**. Si todo se queda en la GPU y codifica el chip dedicado (NVENC/AMF/QSV), el impacto es mínimo.
2. La mayor parte de la RAM de Medal es la **interfaz web**, no la captura. Una UI nativa que solo existe mientras está abierta elimina ese coste.
3. ShadowPlay es el listón de rendimiento; nuestra ventaja es funcionar igual en NVIDIA, AMD e Intel.

---

## 3. Captura de vídeo

| Método | Pros | Contras | Uso en Spectre |
|---|---|---|---|
| **Windows.Graphics.Capture (WGC)** | API oficial, captura monitor o ventana, entrega textura D3D11 (cero copias), funciona con cualquier GPU, no toca el proceso del juego (sin problemas con anti-cheat) | Necesita Win10 1903+; borde amarillo desactivable solo desde Win10 2104 / Win11 (`IsBorderRequired = false`) | **Principal** |
| DXGI Desktop Duplication (DDA) | Muy probado, Win8+ | Problemas en portátiles híbridos (iGPU + dGPU), solo monitor completo, a veces penaliza más en juegos | **Fallback** |
| Hook / inyección en el juego (game capture de OBS) | El método más rápido, captura pantalla completa exclusiva | Inyectar DLLs **dispara anti-cheats** (Vanguard, EAC, BattlEye) y puede causar baneos o crashes | **No usar** |

Decisiones:
- Capturar a la **frecuencia del clip** (30 o 60 fps), no a la del monitor: WGC permite fijar `MinUpdateInterval` (Win11 24H2+) y, si no, se descartan frames en el GPU antes de convertir.
- Fullscreen exclusivo: en Windows 10/11 modernos casi todos los juegos usan "fullscreen optimizations" (flip model), así que WGC los captura. Documentar que el modo exclusivo antiguo puede requerir modo "ventana sin bordes".
- Cursor: `IsCursorCaptureEnabled` configurable.

## 4. Conversión de color y escalado (en GPU)

La captura llega en BGRA; los encoders quieren NV12 (YUV 4:2:0). Hacerlo en CPU cuesta mucho. Se hace en la GPU con **ID3D11VideoProcessor** (lo implementan todos los drivers, incluido escalado a 1080p/720p) o con un pixel shader propio como respaldo. El resultado es una textura NV12 que se pasa directamente al encoder. **Ningún frame toca la RAM del sistema.**

## 5. Codificación

Orden de preferencia, detectado al arrancar con una prueba de 1 frame:

1. **NVENC** (NVIDIA; con los drivers actuales, en la práctica GTX 900 en adelante).
2. **AMF** (AMD GCN / RX 400 en adelante).
3. **QuickSync** (Intel HD 4000 en adelante, incluidas iGPU, útil en portátiles).
4. **Media Foundation H.264 MFT**: el encoder hardware genérico de Windows, cubre gráficas raras.
5. **Software (x264 `ultrafast` o OpenH264)** a 720p30 como último recurso, avisando al usuario.

Recomendación: **FFmpeg (libavcodec)** con `h264_nvenc`, `h264_amf`, `h264_qsv`, `h264_mf` y `hwframes` D3D11, porque un solo API cubre los cinco casos y además hace el remux a MP4. Licencia: compilar FFmpeg en modo **LGPL** (sin x264, que es GPL; usar OpenH264 como software).

Parámetros por defecto pensados para bajo impacto:
- Códec **H.264** (compatibilidad total al compartir clips). HEVC y AV1 como opción en gráficas que lo soporten (RTX 40+, RX 7000+, Arc).
- 1080p60, **CBR/VBR ~15-20 Mbps**, preset rápido del encoder (NVENC `p3`/`p4`, AMF `speed`), sin B-frames en gráficas antiguas.
- **GOP de 1 segundo** (keyframe cada 60 frames) para poder cortar el clip con precisión sin recodificar.
- Límites NVENC: las GeForce permiten ahora hasta 8 sesiones simultáneas, así que replay + grabación a la vez cabe; aun así se usa **una sola sesión** que alimenta ambos.

## 6. Replay buffer (clipear)

- El buffer guarda **paquetes ya codificados**, no frames. 60 s a 20 Mbps ≈ **150 MB**; 30 s ≈ 75 MB. Mucho menos que frames sin comprimir (60 s 1080p60 en NV12 serían ~11 GB).
- Anillo en RAM preasignado una vez (sin allocs en caliente), descartando por GOP completo.
- Opción "buffer en disco" para equipos con poca RAM: segmentos temporales de pocos segundos, con escritura secuencial.
- Al pulsar el atajo: se copia la lista de paquetes desde el último keyframe dentro del rango y un hilo de baja prioridad hace **remux a MP4** (milisegundos, sin recodificar, sin pico de CPU/GPU).
- La duración del clip (15 s, 30 s, 1 min, 2 min, 5 min) es configurable desde el overlay. Recorte posterior básico también por remux (cortes en keyframe) o recodificando solo si el usuario lo pide.
- Grabación manual: usa los mismos paquetes del encoder y los escribe a disco en streaming (MP4 fragmentado para que un crash no corrompa el archivo).

## 7. Audio

- **Salida (lo que suena)**: WASAPI loopback del dispositivo elegido.
- **Por aplicación** (solo el juego, sin Discord, por ejemplo): *Application Loopback* con `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, disponible desde Windows 10 2004. Es una diferencia clara frente a ShadowPlay.
- **Entrada (micrófono)**: WASAPI captura en modo compartido, dispositivo elegible, con push-to-talk opcional.
- Cada fuente en **pista separada** dentro del MP4 (juego, micro), para poder ajustarlas al editar. Mezcla a una pista adicional por compatibilidad.
- Códec **AAC** (Media Foundation AAC encoder, gratis y en todos los Windows) a 160 kbps. El coste de CPU es despreciable.
- Hilos de audio registrados en **MMCSS** ("Pro Audio") para que no haya cortes cuando el juego carga la CPU.
- Escuchar cambios de dispositivo (`IMMNotificationClient`) para no romperse cuando se conectan unos auriculares.

## 8. Cámara (webcam en segundo plano)

- Captura con **Media Foundation** (`IMFSourceReader`), pidiendo MJPEG o NV12 a 720p30 como máximo.
- Decodificación MJPEG por hardware cuando el driver la ofrece.
- Composición en la GPU (un quad sobre la textura antes de codificar) con posición y tamaño configurables, o grabada como archivo aparte.
- **Apagada por defecto**: la cámara solo se abre si el usuario la activa, porque es lo que más CPU añade.

## 9. Overlay y atajos (Alt+Z)

- Atajos globales con `RegisterHotKey` (cero coste, no es un hook de teclado). Configurables: Alt+Z menú, Alt+F10 guardar clip, Alt+F9 grabar, etc. Fallback con Raw Input para combinaciones que `RegisterHotKey` no permite.
- El overlay es **una ventana propia** sin bordes, transparente y siempre encima, **no se inyecta en el juego** (de nuevo, anti-cheat). Funciona sobre juegos en ventana sin bordes y fullscreen con flip model.
- Se marca con `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` para que **el menú no salga en los clips**.
- Se dibuja con **Dear ImGui** sobre el mismo `ID3D11Device` que la captura: unos pocos MB y solo se renderiza mientras está abierto; cerrado, la ventana se destruye y no consume nada.
- Notificaciones tipo "Clip guardado" como un toast pequeño que desaparece solo.

## 10. Cómo se consigue "que no laguee"

1. **Cero copias GPU↔CPU** en el camino de vídeo (WGC → VideoProcessor → encoder).
2. **Encoder dedicado**: NVENC/AMF/QSV son chips aparte de los shaders del juego.
3. **Prioridad de GPU**: igual que OBS, subir la prioridad de planificación GPU del proceso (`D3DKMTSetProcessSchedulingPriorityClass` / `SetGPUThreadPriority`) para que el encoder no se quede sin turno cuando el juego está al 100 % y se pierdan frames. Ojo: sin pasarse, para no quitarle FPS al juego.
4. **Sin trabajo en reposo de la UI**: bandeja del sistema, sin Chromium, sin animaciones de fondo.
5. **Memoria preasignada** y sin allocs por frame; objetivo de RAM total en reposo con buffer de 30 s: **< 150 MB** (la mayoría es el propio buffer).
6. **Hilos acotados**: captura, encoder, audio, escritor de disco. Nada de polling, todo por eventos.
7. **Modo "solo cuando juego"**: detectar el proceso en primer plano a pantalla completa y activar el buffer solo entonces (como ShadowPlay), apagándolo en el escritorio.
8. Medir siempre: overlay de depuración con ms de captura, ms de encode, frames perdidos y RAM, y pruebas de FPS con/sin Spectre en 2-3 juegos.

Objetivos medibles para la primera versión: < 3 % de FPS perdidos con NVENC/AMF a 1080p60, < 2 % de CPU en reposo con el buffer activo, < 150 MB de RAM con 30 s de buffer.

## 11. Stack recomendado

| Pieza | Elección | Alternativa |
|---|---|---|
| Lenguaje núcleo | **C++20** (MSVC, CMake) | Rust + `windows-rs` (más seguro, menos ejemplos de FFmpeg hw) |
| Captura | WGC (C++/WinRT) + DXGI Desktop Duplication | — |
| GPU | Direct3D 11 | D3D12 más adelante si hace falta |
| Encoder y MP4 | FFmpeg LGPL (libavcodec / libavformat) | SDKs nativos NVENC/AMF/oneVPL directamente |
| Audio | WASAPI + Media Foundation AAC | — |
| Cámara | Media Foundation | — |
| UI overlay y ajustes | Dear ImGui (backend D3D11 + Win32) | Win32 nativo puro |
| Ajustes | Un `config.json` (nlohmann/json) | — |
| Instalador | MSIX o Inno Setup | — |

Por qué no Electron, Tauri, WPF o Qt para la UI: Electron mete Chromium entero (cientos de MB, como Medal); Tauri usa WebView2, que es menos pesado pero sigue siendo un navegador con 50-100 MB cuando se abre; WPF/.NET y Qt son razonables pero añaden runtime y RAM permanente. Dear ImGui reaprovecha el dispositivo D3D11 que ya existe y su coste es casi nulo.

## 12. Arquitectura

```
                 ┌─────────────── proceso spectre.exe ────────────────┐
 Monitor/Ventana │ WGC ──► textura BGRA ──► VideoProcessor ──► NV12   │
                 │                                   │                │
 Webcam ─────────┤ MF reader ──► composición GPU ────┘                │
                 │                                   ▼                │
                 │                         Encoder HW (FFmpeg)        │
 Audio salida ───┤ WASAPI loopback ─┐               │ paquetes H.264 │
 Audio juego  ───┤ Process loopback ┼► AAC ──►──────┤                │
 Micrófono    ───┤ WASAPI capture ──┘               ▼                │
                 │                       Anillo de paquetes (RAM)     │
                 │                          │              │          │
                 │        atajo "clip" ─────┘              └── grabación
                 │                ▼                               ▼   │
                 │        remux MP4 (hilo bajo)          MP4 fragmentado
                 │                                                    │
                 │  Hotkeys (RegisterHotKey) ─► Overlay ImGui (Alt+Z) │
                 │  Bandeja del sistema, config.json                  │
                 └────────────────────────────────────────────────────┘
```

Módulos del código:
- `capture/` WGC y DDA detrás de una interfaz común.
- `gpu/` dispositivo D3D11 compartido, conversión NV12, composición de cámara.
- `encode/` detección de encoders y wrapper de FFmpeg.
- `audio/` fuentes WASAPI, mezcla, AAC.
- `buffer/` anillo de paquetes y lógica de corte por keyframe.
- `output/` remux de clips y grabación continua.
- `ui/` overlay ImGui, bandeja, toasts.
- `app/` hotkeys, configuración, detección de juego en primer plano.

## 13. Plan por fases

1. **MVP clip**: WGC monitor principal → NVENC/AMF/QSV/MF → anillo en RAM → Alt+F10 guarda MP4 de 30 s. Sin UI, configuración por JSON. Medir FPS y RAM.
2. **Audio**: loopback de salida y micrófono en pistas separadas.
3. **Overlay Alt+Z**: duración del clip, dispositivos de audio, calidad, carpeta, atajos.
4. **Grabación manual** con el mismo pipeline.
5. **Captura por ventana / por juego**, audio por aplicación, detección automática de juego.
6. **Cámara** y recorte simple de clips.
7. Instalador, autoarranque, actualizaciones.

## 14. Riesgos conocidos

- **Fullscreen exclusivo legacy** (juegos DX9 antiguos): WGC puede no verlo. Mitigación: recomendar ventana sin bordes o DDA.
- **Portátiles híbridos**: DDA falla si el monitor cuelga de otra GPU; WGC lo resuelve, y hay que crear el encoder en la GPU correcta.
- **Contenido protegido (DRM)** sale en negro, igual que en otras apps.
- **Licencias**: FFmpeg en LGPL enlazado dinámicamente; H.264 por hardware no requiere pagar licencias al desarrollador de la app en uso normal, pero conviene revisarlo antes de vender el producto.
- **Anti-cheat**: al no inyectar nada, el riesgo es el de cualquier grabador de escritorio (el mismo que OBS display capture o Game Bar).

## 15. Referencias

- [GraphicsCaptureSession.IsBorderRequired (Microsoft Learn)](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired)
- [Application loopback audio capture sample (Microsoft)](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- [ActivateAudioInterfaceAsync (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/desktop/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync)
- [Loopback recording (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [SetWindowDisplayAffinity (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity)
- [NVENC Application Note, Video Codec SDK 13](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html)
- [Nvidia lifts some video encoding limitations from consumer GPUs (Tom's Hardware)](https://tomshardware.com/news/nvidia-increases-concurrent-nvenc-sessions-on-consumer-gpus)
- [OBS: Windows Graphics Capture vs DXGI Desktop Duplication](https://obsproject.com/forum/threads/windows-graphics-capture-vs-dxgi-desktop-duplication.149320)
- [OBS: DXGI Desktop Duplication ruins performance on games](https://obsproject.com/forum/threads/using-capture-method-dxgi-desktop-duplication-ruins-performance-on-games.154062)
- [OBS 24.0.3 GPU priority fix testing](https://obsproject.com/forum/threads/obs-studio-24-0-3-gpu-priority-fix-testing.111669/post-422085)
- [OBS PR: WGC capture support](https://github.com/obsproject/obs-studio/pull/2208)
- [Steam Game Recording: opción de buffer solo en memoria](https://steamcommunity.com/discussions/forum/10/4625853520754552796/)
- [Medal vs NVIDIA ShadowPlay (Medal)](https://medal.tv/compare/medal-vs-shadowplay)
- [Insights Capture vs ShadowPlay](https://insights.gg/blog/insights-vs-shadowplay)
- [Electron y consumo de RAM](https://tech.yahoo.com/computing/articles/hate-windows-ram-usage-wait-163013543.html)
