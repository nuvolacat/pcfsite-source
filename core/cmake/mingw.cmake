# Compilación cruzada para Windows desde macOS (o Linux) con llvm-mingw (clang + runtime mingw-w64, UCRT).
#
#   cmake -S core -B core/build/win-x86_64/app-pcf7 -G Ninja -DCMAKE_TOOLCHAIN_FILE=core/cmake/mingw.cmake \
#         -DPCF_WIN_ARCH=x86_64 -DPCF_JUEGO=pcf7 -DCMAKE_BUILD_TYPE=Release
#
# PCF_WIN_ARCH: x86_64 (PC) o aarch64 (Windows ARM). PCF_WIN_TOOLS: la carpeta de llvm-mingw (por defecto
# ~/pcfsite-win/llvm-mingw) y PCF_WIN_SDL3: la SDL3 compilada para esa arquitectura (~/pcfsite-win/sdl3-<arch>);
# app/win/preparar.sh descarga y compila las dos cosas.
set(CMAKE_SYSTEM_NAME Windows)
if(NOT PCF_WIN_ARCH)
  set(PCF_WIN_ARCH x86_64)
endif()
if(PCF_WIN_ARCH STREQUAL "aarch64")
  set(CMAKE_SYSTEM_PROCESSOR ARM64)
else()
  set(CMAKE_SYSTEM_PROCESSOR AMD64)
endif()
if(NOT PCF_WIN_TOOLS)
  set(PCF_WIN_TOOLS "$ENV{HOME}/pcfsite-win/llvm-mingw")
endif()
if(NOT PCF_WIN_SDL3)
  set(PCF_WIN_SDL3 "$ENV{HOME}/pcfsite-win/sdl3-${PCF_WIN_ARCH}")
endif()
set(_t ${PCF_WIN_ARCH}-w64-mingw32)
set(CMAKE_C_COMPILER "${PCF_WIN_TOOLS}/bin/${_t}-clang")
set(CMAKE_CXX_COMPILER "${PCF_WIN_TOOLS}/bin/${_t}-clang++")
set(CMAKE_RC_COMPILER "${PCF_WIN_TOOLS}/bin/${_t}-windres")
set(CMAKE_AR "${PCF_WIN_TOOLS}/bin/llvm-ar")
set(CMAKE_RANLIB "${PCF_WIN_TOOLS}/bin/llvm-ranlib")
set(CMAKE_FIND_ROOT_PATH "${PCF_WIN_SDL3}" "${PCF_WIN_TOOLS}/${_t}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(SDL3_DIR "${PCF_WIN_SDL3}/cmake" CACHE PATH "")
# ficheros de más de 2 GB (stat/off_t de 64 bits), localtime_r/gmtime_r, y nada de min/max de windows.h
# __USE_MINGW_SETJMP_NON_SEH: longjmp sin desenrollar la pila (el fin del programa salta desde muy dentro del código traducido)
add_compile_definitions(_FILE_OFFSET_BITS=64 _POSIX_THREAD_SAFE_FUNCTIONS=200112L NOMINMAX WIN32_LEAN_AND_MEAN __USE_MINGW_SETJMP_NON_SEH)
# runtime de C++ y winpthreads dentro de cada ejecutable y DLL (solo dependen de las DLL del sistema y de SDL3.dll);
# variables normales, no _INIT: el fichero de toolchain se lee en cada configuración y así vale también para un
# directorio de build que ya existía
set(CMAKE_EXE_LINKER_FLAGS "-static")
set(CMAKE_SHARED_LINKER_FLAGS "-static")
