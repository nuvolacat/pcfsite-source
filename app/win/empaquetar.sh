#!/bin/bash
# Construye la app de Windows desde macOS (compilación cruzada con llvm-mingw, ver app/win/preparar.sh): la carpeta
# «PCFSite - Source» con el lanzador (el único .exe), los juegos traducidos (una DLL por programa, en programas/),
# SDL3.dll, las fuentes, versiones.json
# y los ficheros extra, y su zip. Para x64 (PC) y ARM64 (Windows ARM). NO mete ningún fichero de los juegos.
#
#   app/win/empaquetar.sh [--arch x86_64|aarch64|todas] [--sin-compilar] [--juegos "pcf7 pcf6"]
#
# Deja core/build/win-<arch>/PCFSite - Source/ y core/build/win-<arch>/PCFSite-Source-<versión>-windows-<x64|arm64>.zip.
# La versión es la misma que en macOS (1.AA.MMDD.HHMM, app/VERSION); con VERSION en el entorno se reutiliza una.
set -euo pipefail
cd "$(dirname "$0")/../.."
REPO=$(pwd)
JUEGOS="pcf4 pcf5 pcf5oro pcf5arg pcf6 pcf6oro pcf6arg pcf7 pcf7plus pcf2000 pcf2001 pcfpatch"
ARCHS="x86_64 aarch64"; COMPILAR=1
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) [ "$2" = todas ] || ARCHS="$2"; shift ;;
    --sin-compilar) COMPILAR=0 ;;
    --juegos) JUEGOS="$2"; shift ;;
    *) echo "opción desconocida: $1"; exit 1 ;;
  esac
  shift
done
W="${PCF_WIN:-$HOME/pcfsite-win}"
[ -x "$W/llvm-mingw/bin/x86_64-w64-mingw32-clang" ] || { echo "falta llvm-mingw en $W (ejecuta app/win/preparar.sh)"; exit 1; }
VERSION="${VERSION:-1.$(date +%y.%m%d.%H%M)}"
echo "$VERSION" > app/VERSION

for a in $ARCHS; do
  B="$REPO/core/build/win-$a"
  SDL="$W/sdl3-$a"
  [ -f "$SDL/bin/SDL3.dll" ] || { echo "falta SDL3 para $a en $SDL (ejecuta app/win/preparar.sh)"; exit 1; }
  TC=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$REPO/core/cmake/mingw.cmake" -DPCF_WIN_ARCH=$a -DPCF_WIN_TOOLS="$W/llvm-mingw" -DPCF_WIN_SDL3="$SDL" -DCMAKE_BUILD_TYPE=Release)
  if [ "$COMPILAR" = 1 ]; then
    echo "== $a: lanzador"
    cmake -S app -B "$B/app-lanzador" "${TC[@]}" -DPCF_VERSION="$VERSION" >/dev/null
    cmake --build "$B/app-lanzador" | grep -E "error|FAILED" || true
    for j in $JUEGOS; do
      echo "== $a: $j"
      cmake -S core -B "$B/app-$j" "${TC[@]}" -DPCF_JUEGO="$j" >/dev/null
      cmake --build "$B/app-$j" -j8 | grep -E "error|FAILED|Linking" || true
    done
  fi

  D="$B/PCFSite - Source"
  rm -rf "$D"
  mkdir -p "$D/fonts" "$D/assets" "$D/extra" "$D/programas"
  [ -f "$B/app-lanzador/lanzador.exe" ] || { echo "no está $B/app-lanzador/lanzador.exe"; exit 1; }
  cp "$B/app-lanzador/lanzador.exe" "$D/PCFSite - Source.exe"
  N=0   # cada programa traducido es una DLL en programas/ (el único .exe es el lanzador, que las carga con --programa)
  for j in $JUEGOS; do
    for dll in "$B/app-$j/$j.dll" "$B/app-$j/$j"-*.dll; do
      [ -f "$dll" ] || continue
      cp "$dll" "$D/programas/"; N=$((N + 1))
    done
  done
  [ "$N" -gt 0 ] || { echo "no hay ningún juego compilado en $B/app-<id>"; exit 1; }
  cp "$SDL/bin/SDL3.dll" "$D/"
  cp core/platform/fonts/*.ttf core/platform/fonts/LICENSE "$D/fonts/"
  cp app/fonts/*.ttf app/fonts/OFL.txt "$D/fonts/"   # Barlow (el lanzador)
  cp -R app/assets/. "$D/assets/"                     # logo y carátulas
  app/tools/soundfont.sh >/dev/null && cp core/platform/fonts/gm.sf2 core/platform/fonts/LICENSE-GeneralUser-GS.txt "$D/fonts/"   # música MIDI del 2000
  cp app/versiones.json "$D/"
  for d in core/juegos/*/datos_extra; do [ -d "$d" ] && { id=$(basename "$(dirname "$d")"); mkdir -p "$D/extra/$id"; cp -R "$d/." "$D/extra/$id/"; }; done
  echo "$VERSION" > "$D/VERSION"
  cp app/win/LEEME.txt "$D/LEEME.txt"

  case "$a" in x86_64) SUF=x64 ;; aarch64) SUF=arm64 ;; *) SUF=$a ;; esac
  Z="$B/PCFSite-Source-$VERSION-windows-$SUF.zip"
  rm -f "$B"/PCFSite-Source-*-windows-$SUF.zip
  (cd "$B" && zip -qr "$Z" "PCFSite - Source")
  echo "$D: $N programas, $(du -sh "$D" | cut -f1), versión $VERSION → $(basename "$Z") ($(du -sh "$Z" | cut -f1))"
done
