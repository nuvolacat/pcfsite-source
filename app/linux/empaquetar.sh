#!/bin/bash
# Construye la app de Linux, en Linux (x86_64 o aarch64, el de la máquina): la carpeta «PCFSite - Source» con el lanzador
# (pcfsite-source), los juegos traducidos (un ejecutable por programa, como en macOS), las fuentes, versiones.json, los
# ficheros extra, el icono, el .desktop y LEEME.txt, y su tar.gz. NO mete ningún fichero de los juegos.
#
#   app/linux/empaquetar.sh [--sin-compilar] [--juegos "pcf7 pcf6"]
#
# Hace falta app/linux/preparar.sh (SDL3 estática en ~/pcfsite-linux) y haber traducido los juegos (core/build/<id>/gen,
# con recomp/build.py: necesita las carpetas de trabajo con los binarios originales, core/juegos/README.md).
# Deja core/build/linux/PCFSite - Source/ y core/build/linux/PCFSite-Source-<versión>-linux-<x86_64|arm64>.tar.gz.
# La versión es la misma que en macOS y Windows (1.AA.MMDD.HHMM, app/VERSION); con VERSION en el entorno se reutiliza.
# Los binarios dependen de la glibc de la máquina que compila: para que valgan en más distribuciones, compilar en la más
# antigua que se quiera soportar.
set -euo pipefail
cd "$(dirname "$0")/../.."
REPO=$(pwd)
JUEGOS="pcf4 pcf5 pcf5oro pcf5arg pcf6 pcf6oro pcf6arg pcf7 pcf7plus pcf2000 pcf2001 pcfpatch"
COMPILAR=1
while [ $# -gt 0 ]; do
  case "$1" in
    --sin-compilar) COMPILAR=0 ;;
    --juegos) JUEGOS="$2"; shift ;;
    *) echo "opción desconocida: $1"; exit 1 ;;
  esac
  shift
done
W="${PCF_LINUX:-$HOME/pcfsite-linux}"
[ -f "$W/sdl3/lib/libSDL3.a" ] || { echo "falta SDL3 en $W/sdl3 (ejecuta app/linux/preparar.sh)"; exit 1; }
VERSION="${VERSION:-1.$(date +%y.%m%d.%H%M)}"
echo "$VERSION" > app/VERSION
B="$REPO/core/build/linux"
TC=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$W/sdl3")

if [ "$COMPILAR" = 1 ]; then
  echo "== lanzador"
  cmake -S app -B "$B/app-lanzador" "${TC[@]}" -DPCF_VERSION="$VERSION" >/dev/null
  cmake --build "$B/app-lanzador" | grep -E "error|FAILED" || true
  for j in $JUEGOS; do
    echo "== $j"
    cmake -S core -B "$B/app-$j" "${TC[@]}" -DPCF_JUEGO="$j" >/dev/null
    cmake --build "$B/app-$j" -j"$(nproc)" | grep -E "error|FAILED|Linking" || true
  done
fi

D="$B/PCFSite - Source"
rm -rf "$D"
mkdir -p "$D/fonts" "$D/assets" "$D/extra"
[ -f "$B/app-lanzador/lanzador" ] || { echo "no está $B/app-lanzador/lanzador"; exit 1; }
cp "$B/app-lanzador/lanzador" "$D/pcfsite-source"
N=0
for j in $JUEGOS; do
  for exe in "$B/app-$j/$j" "$B/app-$j/$j"-*; do
    [ -f "$exe" ] && [ -x "$exe" ] || continue
    cp "$exe" "$D/"; N=$((N + 1))
  done
done
[ "$N" -gt 0 ] || { echo "no hay ningún juego compilado en $B/app-<id>"; exit 1; }
cp core/platform/fonts/*.ttf core/platform/fonts/LICENSE "$D/fonts/"
cp app/fonts/*.ttf app/fonts/OFL.txt "$D/fonts/"   # Barlow (el lanzador)
cp -R app/assets/. "$D/assets/"                     # logo y carátulas
[ -f core/platform/fonts/gm.sf2 ] && cp core/platform/fonts/gm.sf2 "$D/fonts/" || echo "aviso: sin core/platform/fonts/gm.sf2 (música MIDI del 2000)"
cp app/versiones.json "$D/"
for d in core/juegos/*/datos_extra; do [ -d "$d" ] && { id=$(basename "$(dirname "$d")"); mkdir -p "$D/extra/$id"; cp -R "$d/." "$D/extra/$id/"; }; done
echo "$VERSION" > "$D/VERSION"
cp app/linux/LEEME.txt app/linux/pcfsite.png app/linux/pcfsite-source.desktop app/linux/instalar.sh "$D/"
chmod +x "$D/instalar.sh"

case "$(uname -m)" in x86_64) SUF=x86_64 ;; aarch64|arm64) SUF=arm64 ;; *) SUF=$(uname -m) ;; esac
Z="$B/PCFSite-Source-$VERSION-linux-$SUF.tar.gz"
rm -f "$B"/PCFSite-Source-*-linux-$SUF.tar.gz
(cd "$B" && tar czf "$Z" "PCFSite - Source")
echo "$D: $N programas, $(du -sh "$D" | cut -f1), versión $VERSION → $(basename "$Z") ($(du -sh "$Z" | cut -f1))"
