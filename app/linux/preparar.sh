#!/bin/bash
# Prepara en Linux lo que hace falta para compilar la app: SDL3 estática (así el paquete no depende de la SDL3 de cada
# distribución; SDL carga X11, Wayland, PulseAudio y PipeWire en tiempo de ejecución, estén o no). Todo en ~/pcfsite-linux:
#
#   ~/pcfsite-linux/sdl3            SDL3 estática (lib/libSDL3.a, include, cmake)
#
#   app/linux/preparar.sh [--sdl3 3.4.18]
#
# Antes hacen falta las herramientas y las cabeceras del sistema (Debian/Ubuntu):
#   sudo apt install build-essential cmake ninja-build pkg-config git curl \
#     libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxtst-dev \
#     libxss-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libdbus-1-dev libudev-dev
# Para que la app monte las ISO sin privilegios: fuseiso (recomendado) o udisks2 (lo trae cualquier escritorio).
# Después, app/linux/empaquetar.sh compila y empaqueta.
set -euo pipefail
cd "$(dirname "$0")/../.."
SDL3=3.4.18
while [ $# -gt 0 ]; do
  case "$1" in
    --sdl3) SDL3="$2"; shift ;;
    *) echo "opción desconocida: $1"; exit 1 ;;
  esac
  shift
done
W="${PCF_LINUX:-$HOME/pcfsite-linux}"
mkdir -p "$W/descargas"
if [ ! -d "$W/SDL3-$SDL3" ]; then
  echo "== SDL3 $SDL3 (fuentes)"
  [ -f "$W/descargas/SDL3-$SDL3.tar.gz" ] || curl -sSL -o "$W/descargas/SDL3-$SDL3.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3/SDL3-$SDL3.tar.gz"
  tar xzf "$W/descargas/SDL3-$SDL3.tar.gz" -C "$W"
fi
if [ ! -f "$W/sdl3/lib/libSDL3.a" ]; then
  echo "== SDL3 estática"
  cmake -S "$W/SDL3-$SDL3" -B "$W/build-sdl3" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$W/sdl3" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF >/dev/null
  cmake --build "$W/build-sdl3" -j"$(nproc)" >/dev/null
  cmake --install "$W/build-sdl3" >/dev/null
fi
echo "listo: $W (cmake con -DCMAKE_PREFIX_PATH=$W/sdl3)"
