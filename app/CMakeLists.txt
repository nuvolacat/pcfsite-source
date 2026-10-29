cmake_minimum_required(VERSION 3.24)
project(pcfsite_source_app CXX)

# El lanzador de la app de escritorio (app/lanzador): SDL3 + stb_truetype (texto) + stb_image (el inflate de los .CPR y las
# imágenes); inflar.cpp (deflate propio), is6.cpp (cabinets de InstallShield 6) y mscab.cpp (cabinets de Microsoft).
# Los juegos se compilan desde core/ (uno por juego, -DPCF_JUEGO=<id>); app/mac/empaquetar.sh lo junta todo en el .app.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL3 REQUIRED CONFIG)

add_executable(lanzador
  lanzador/main.cpp lanzador/ui.cpp lanzador/util.cpp lanzador/json.cpp lanzador/versiones.cpp lanzador/instalar.cpp
  lanzador/inflar.cpp lanzador/is6.cpp lanzador/mscab.cpp lanzador/zip.cpp lanzador/actualizar.cpp)
target_compile_options(lanzador PRIVATE -Wall -Wno-unused-function)
# en desarrollo (sin .app) las fuentes y versiones.json se leen del repo
target_compile_definitions(lanzador PRIVATE PCF_APP_SRC="${CMAKE_CURRENT_SOURCE_DIR}")
# la versión de la app (la pone app/mac/empaquetar.sh; sin ella, «desarrollo»)
if(PCF_VERSION)
  target_compile_definitions(lanzador PRIVATE PCF_VERSION="${PCF_VERSION}")
endif()
target_link_libraries(lanzador PRIVATE SDL3::SDL3)
if(APPLE)   # objc_msgSend: la app se retira del Dock mientras corre el juego
  target_link_libraries(lanzador PRIVATE objc)
endif()
if(WIN32)   # icono y manifiesto (UTF-8 en las APIs ANSI, DPI por monitor); sin consola (el modo terminal se engancha a la del padre)
  target_sources(lanzador PRIVATE win/lanzador.rc ../core/platform/src/compat_win.cpp)
  target_link_options(lanzador PRIVATE -mwindows -Wl,--stack,16777216)   # 16 MB de pila (como en macOS; Windows da 1 MB)
endif()
