#!/bin/bash
# Prepara en este Mac (o Linux) lo que hace falta para compilar la app de Windows: llvm-mingw (clang + runtime
# mingw-w64 con UCRT, para x86_64 y aarch64) y SDL3 compilada para las dos arquitecturas. Todo en ~/pcfsite-win:
#
#   ~/pcfsite-win/llvm-mingw        la cadena de herramientas (https://github.com/mstorsjo/llvm-mingw)
#   ~/pcfsite-win/sdl3-x86_64       SDL3 para Windows x64 (bin/SDL3.dll, lib, include, cmake)
#   ~/pcfsite-win/sdl3-aarch64      SDL3 para Windows ARM64
#
#   app/win/preparar.sh [--llvm-mingw 20261020] [--sdl3 3.4.18]
#
# Después, app/win/empaquetar.sh compila y empaqueta. core/cmake/mingw.cmake es el fichero de toolchain de CMake.
set -euo pipefail
cd "$(dirname "$0")/../.."
REPO=$(pwd)
LLVM_MINGW=20261020
SDL3=3.4.18
while [ $# -gt 0 ]; do
  case "$1" in
    --llvm-mingw) LLVM_MINGW="$2"; shift ;;
    --sdl3) SDL3="$2"; shift ;;
    *) echo "opción desconocida: $1"; exit 1 ;;
  esac
  shift
done
W="$HOME/pcfsite-win"
mkdir -p "$W/descargas"
case "$(uname -s)" in
  Darwin) SO=macos-universal ;;
  *) SO=ubuntu-22.04-$(uname -m) ;;
esac

if [ ! -x "$W/llvm-mingw/bin/x86_64-w64-mingw32-clang" ]; then
  F="llvm-mingw-$LLVM_MINGW-ucrt-$SO.tar.xz"
  echo "== llvm-mingw $LLVM_MINGW"
  [ -f "$W/descargas/$F" ] || curl -sSL -o "$W/descargas/$F" "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW/$F"
  rm -rf "$W/llvm-mingw-$LLVM_MINGW-ucrt-$SO"
  tar xf "$W/descargas/$F" -C "$W"
  rm -rf "$W/llvm-mingw"
  mv "$W/llvm-mingw-$LLVM_MINGW-ucrt-$SO" "$W/llvm-mingw"
fi

if [ ! -d "$W/SDL3-$SDL3" ]; then
  echo "== SDL3 $SDL3 (fuentes)"
  [ -f "$W/descargas/SDL3-$SDL3.tar.gz" ] || curl -sSL -o "$W/descargas/SDL3-$SDL3.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3/SDL3-$SDL3.tar.gz"
  tar xzf "$W/descargas/SDL3-$SDL3.tar.gz" -C "$W"
fi

for a in x86_64 aarch64; do
  [ -f "$W/sdl3-$a/bin/SDL3.dll" ] && continue
  echo "== SDL3 para $a"
  cmake -S "$W/SDL3-$SDL3" -B "$W/build-sdl3-$a" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/core/cmake/mingw.cmake" -DPCF_WIN_ARCH=$a -DCMAKE_INSTALL_PREFIX="$W/sdl3-$a" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF >/dev/null
  cmake --build "$W/build-sdl3-$a" -j8 >/dev/null
  cmake --install "$W/build-sdl3-$a" >/dev/null
done
echo "listo: $W"
