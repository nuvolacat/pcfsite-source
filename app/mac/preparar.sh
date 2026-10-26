#!/bin/bash
# Prepara lo que hace falta para compilar la app de macOS para las dos arquitecturas: SDL3 compilada desde el fuente
# para arm64 (Apple Silicon) y x86_64 (Intel) con macOS 12 como mínimo. La SDL3 de Homebrew no vale para distribuir:
# es solo arm64 y exige el macOS con el que se construyó la botella (la de macOS 27 pide macOS 27).
#
#   ~/pcfsite-mac/sdl3-arm64      SDL3 para Apple Silicon (lib/libSDL3.0.dylib, include, cmake)
#   ~/pcfsite-mac/sdl3-x86_64     SDL3 para Intel
#
#   app/mac/preparar.sh [--sdl3 3.4.18] [--minimo 12.0]
#
# Después, app/mac/empaquetar.sh --arch arm64|x86_64|todas compila y empaqueta.
set -euo pipefail
cd "$(dirname "$0")/../.."
SDL3=3.4.18
MINIMO=12.0
while [ $# -gt 0 ]; do
  case "$1" in
    --sdl3) SDL3="$2"; shift ;;
    --minimo) MINIMO="$2"; shift ;;
    *) echo "opción desconocida: $1"; exit 1 ;;
  esac
  shift
done
W="${PCF_MAC:-$HOME/pcfsite-mac}"
mkdir -p "$W/descargas"

if [ ! -d "$W/SDL3-$SDL3" ]; then
  echo "== SDL3 $SDL3 (fuentes)"
  F="$W/descargas/SDL3-$SDL3.tar.gz"
  [ -f "$F" ] || [ ! -f "$HOME/pcfsite-win/descargas/SDL3-$SDL3.tar.gz" ] || cp "$HOME/pcfsite-win/descargas/SDL3-$SDL3.tar.gz" "$F"   # la misma que para Windows
  [ -f "$F" ] || curl -sSL -o "$F" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3/SDL3-$SDL3.tar.gz"
  tar xzf "$F" -C "$W"
fi

for a in arm64 x86_64; do
  [ -f "$W/sdl3-$a/lib/libSDL3.0.dylib" ] && continue
  echo "== SDL3 para $a (macOS $MINIMO o posterior)"
  cmake -S "$W/SDL3-$SDL3" -B "$W/build-sdl3-$a" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=$a -DCMAKE_OSX_DEPLOYMENT_TARGET=$MINIMO -DCMAKE_INSTALL_PREFIX="$W/sdl3-$a" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF >/dev/null
  cmake --build "$W/build-sdl3-$a" -j"$(sysctl -n hw.ncpu)" >/dev/null
  cmake --install "$W/build-sdl3-$a" >/dev/null
  echo "   $(otool -l "$W/sdl3-$a/lib/libSDL3.0.dylib" | grep -A3 LC_BUILD_VERSION | grep minos | tr -s ' ') · $(lipo -archs "$W/sdl3-$a/lib/libSDL3.0.dylib")"
done
echo "listo: $W"
