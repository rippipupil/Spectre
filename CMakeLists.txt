cmake_minimum_required(VERSION 3.24)
project(Spectre VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SPECTRE_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/ffmpeg" CACHE PATH "FFmpeg (shared, LGPL) con include/, lib/ y bin/")

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(imgui json)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE ${SPECTRE_FFMPEG_DIR}/include)
target_link_libraries(ffmpeg INTERFACE
  ${SPECTRE_FFMPEG_DIR}/lib/avcodec.lib
  ${SPECTRE_FFMPEG_DIR}/lib/avformat.lib
  ${SPECTRE_FFMPEG_DIR}/lib/avutil.lib
  ${SPECTRE_FFMPEG_DIR}/lib/swresample.lib
  ${SPECTRE_FFMPEG_DIR}/lib/swscale.lib)

set(SPECTRE_BUILD_VERSION "${PROJECT_VERSION}" CACHE STRING "Versión que muestra la app (CI pone la de la release)")
# "Buscar actualizaciones" compara esta versión con la etiqueta de la release:
# mejor fallar aquí que publicar un exe con una versión rota.
if(NOT SPECTRE_BUILD_VERSION MATCHES "^[0-9]+(\\.[0-9]+)+$")
  message(FATAL_ERROR "SPECTRE_BUILD_VERSION no válida: '${SPECTRE_BUILD_VERSION}'")
endif()
file(GLOB_RECURSE SPECTRE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(spectre WIN32 ${SPECTRE_SOURCES} res/spectre.rc)
target_include_directories(spectre PRIVATE src)
target_compile_definitions(spectre PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
  SPECTRE_VERSION="${SPECTRE_BUILD_VERSION}")
target_compile_options(spectre PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus /EHsc)
target_link_libraries(spectre PRIVATE imgui nlohmann_json::nlohmann_json ffmpeg
  d3d11 dxgi dcomp windowsapp mfplat mfreadwrite mf mfuuid ole32 uuid
  avrt shell32 shcore user32 gdi32 dwmapi advapi32 mmdevapi winhttp bcrypt delayimp
  windowscodecs oleaut32 winmm)
# El manifiesto va en el .rc; evitamos que el linker genere otro.
# Media Foundation (solo la webcam) se carga en diferido: en las ediciones N de
# Windows no existe y, enlazada directamente, Spectre ni arrancaría.
target_link_options(spectre PRIVATE /MANIFEST:NO /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll /DELAYLOAD:mf.dll)

install(TARGETS spectre RUNTIME DESTINATION .)
install(DIRECTORY ${SPECTRE_FFMPEG_DIR}/bin/ DESTINATION . FILES_MATCHING
  PATTERN "avcodec-*.dll" PATTERN "avformat-*.dll" PATTERN "avutil-*.dll"
  PATTERN "swresample-*.dll" PATTERN "swscale-*.dll")
install(FILES THIRD_PARTY.md DESTINATION .)
install(FILES ${SPECTRE_FFMPEG_DIR}/LICENSE.txt DESTINATION . RENAME FFMPEG-LICENSE.txt OPTIONAL)
