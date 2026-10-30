#!/bin/bash
# Construye la app de macOS: «PCFSite - Source.app» con el lanzador, los juegos traducidos (un ejecutable por programa), SDL3,
# las fuentes, versiones.json y los ficheros extra. NO mete ningún fichero de los juegos.
#
#   app/mac/empaquetar.sh [--arch arm64|x86_64|todas] [--sin-compilar] [--juegos "pcf7 pcf6"] [--dmg] [--firma "…"]
#                         [--notarizar]
#
# Una app por arquitectura: arm64 (Apple Silicon) y x86_64 (Intel), las dos para macOS 12 o posterior (MINIMO), con la
# SDL3 de app/mac/preparar.sh (~/pcfsite-mac/sdl3-<arch>; la de Homebrew pide el macOS con que se construyó).
# Compila (salvo --sin-compilar) el lanzador (core/build/mac-<arch>/app-lanzador) y cada juego
# (core/build/mac-<arch>/app-<id>, con -DPCF_JUEGO=<id>; hace falta haber traducido antes con recomp/build.py, que deja
# también imagen.c) y deja la app en core/build/mac-<arch>/PCFSite - Source.app (y, con --dmg,
# core/build/mac-<arch>/PCFSite-Source-<versión>-macos-<apple-silicon|intel>.dmg). Firma con la identidad de --firma
# (o $APP_FIRMA), si no ad hoc («-»: Gatekeeper avisa la primera vez; para no avisar, Developer ID y notarizar).
# --notarizar (con --dmg y un Developer ID): manda cada .dmg a Apple con la clave de la API de App Store Connect de
# ~/.appstoreconnect/config (ASC_KEY_PATH, ASC_KEY_ID, ASC_ISSUER_ID; fuera del repo) y le grapa el tíquet.
set -euo pipefail
cd "$(dirname "$0")/../.."
REPO=$(pwd)
JUEGOS="pcf4 pcf5 pcf5oro pcf5arg pcf6 pcf6oro pcf6arg pcf7 pcf7plus pcf2000 pcf2001 pcfpatch"   # pcfpatch: el parcheador de las actualizaciones del 2001
COMPILAR=1; DMG=0; FIRMA="${APP_FIRMA:--}"; NOTARIZAR=0; ARCHS="arm64 x86_64"; MINIMO=12.0
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) [ "$2" = todas ] || ARCHS="$2"; shift ;;
    --sin-compilar) COMPILAR=0 ;;
    --juegos) JUEGOS="$2"; shift ;;
    --dmg) DMG=1 ;;
    --firma) FIRMA="$2"; shift ;;
    --notarizar) NOTARIZAR=1 ;;
    *) echo "opción desconocida: $1"; exit 1 ;;
  esac
  shift
done
# versión = 1.AA.MMDD.HHMM con la hora real del empaquetado (el esquema de pcfsite.com); queda en app/VERSION, en el
# Info.plist y en «Acerca de» (-DPCF_VERSION); cada versión publicada lleva su entrada en app/CHANGELOG.md
VERSION="${VERSION:-1.$(date +%y.%m%d.%H%M)}"   # (VERSION del entorno: la misma para macOS y Windows)
echo "$VERSION" > app/VERSION

W="${PCF_MAC:-$HOME/pcfsite-mac}"
for a in $ARCHS; do
B="$REPO/core/build/mac-$a"
APP="$B/PCFSite - Source.app"
C="$APP/Contents"
SDLD="$W/sdl3-$a"
[ -f "$SDLD/lib/libSDL3.0.dylib" ] || { echo "falta SDL3 para $a en $SDLD (ejecuta app/mac/preparar.sh)"; exit 1; }
TC=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=$a -DCMAKE_OSX_DEPLOYMENT_TARGET=$MINIMO
    -DCMAKE_PREFIX_PATH="$SDLD" -DCMAKE_FIND_FRAMEWORK=NEVER)
if [ "$COMPILAR" = 1 ]; then
  echo "== $a: lanzador"
  cmake -S app -B "$B/app-lanzador" "${TC[@]}" -DPCF_VERSION="$VERSION" >/dev/null
  cmake --build "$B/app-lanzador" | grep -E "error|FAILED" || true
  for j in $JUEGOS; do
    echo "== $a: $j"
    cmake -S core -B "$B/app-$j" "${TC[@]}" -DPCF_JUEGO="$j" >/dev/null
    cmake --build "$B/app-$j" -j"$(sysctl -n hw.ncpu)" | grep -E "error|FAILED|Linking" || true
  done
fi

rm -rf "$APP"
mkdir -p "$C/MacOS" "$C/Resources/fonts" "$C/Frameworks" "$C/Resources/extra"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@MINIMO@/$MINIMO/g" app/mac/Info.plist > "$C/Info.plist"
printf 'APPL????' > "$C/PkgInfo"
cp "$B/app-lanzador/lanzador" "$C/MacOS/"
N=0
for j in $JUEGOS; do
  for exe in "$B/app-$j/$j" "$B/app-$j/$j"-*; do
    [ -f "$exe" ] && [ -x "$exe" ] || continue
    cp "$exe" "$C/MacOS/"; N=$((N + 1))
  done
done
[ "$N" -gt 0 ] || { echo "no hay ningún juego compilado en $B/app-<id>"; exit 1; }
cp core/platform/fonts/*.ttf core/platform/fonts/LICENSE "$C/Resources/fonts/"
cp app/fonts/*.ttf app/fonts/OFL.txt "$C/Resources/fonts/"   # Barlow (el lanzador)
mkdir -p "$C/Resources/assets" && cp -R app/assets/. "$C/Resources/assets/"   # logo y carátulas
app/tools/soundfont.sh >/dev/null && cp core/platform/fonts/gm.sf2 core/platform/fonts/LICENSE-GeneralUser-GS.txt "$C/Resources/fonts/"   # música MIDI del 2000
cp app/versiones.json "$C/Resources/"
for d in core/juegos/*/datos_extra; do [ -d "$d" ] && { id=$(basename "$(dirname "$d")"); mkdir -p "$C/Resources/extra/$id"; cp -R "$d/." "$C/Resources/extra/$id/"; }; done
[ -f app/mac/pcfsite.icns ] && cp app/mac/pcfsite.icns "$C/Resources/"

# SDL3: la biblioteca dentro de la app, y cada ejecutable apunta a ella
otool -L "$C/MacOS/lanzador" | grep -q 'libSDL3' || { echo "el lanzador no enlaza con libSDL3"; exit 1; }
cp "$SDLD/lib/libSDL3.0.dylib" "$C/Frameworks/libSDL3.0.dylib"   # la de app/mac/preparar.sh (ya va como @rpath)
chmod 644 "$C/Frameworks/libSDL3.0.dylib"
install_name_tool -id "@rpath/libSDL3.0.dylib" "$C/Frameworks/libSDL3.0.dylib"
for exe in "$C/MacOS"/*; do
  dep=$(otool -L "$exe" | awk '/libSDL3/{print $1; exit}')
  [ -n "$dep" ] && [ "$dep" != "@rpath/libSDL3.0.dylib" ] && install_name_tool -change "$dep" "@rpath/libSDL3.0.dylib" "$exe"
  install_name_tool -delete_rpath "$SDLD/lib" "$exe" 2>/dev/null || true   # el rpath de compilación (una ruta de este Mac)
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$exe" 2>/dev/null || true
done

# firma (ad hoc por defecto)
SELLO=(); [ "$FIRMA" = - ] || SELLO=(--timestamp)   # (con Developer ID, sello de tiempo: la notarización lo exige)
codesign --force --sign "$FIRMA" ${SELLO[@]+"${SELLO[@]}"} "$C/Frameworks/libSDL3.0.dylib"
# primero los juegos y luego el lanzador (el ejecutable principal: al firmarlo, codesign mira los demás; en x86_64 el
# enlazador no los firma solo como en arm64)
for exe in "$C/MacOS"/*; do [ "$(basename "$exe")" = lanzador ] || codesign --force --sign "$FIRMA" ${SELLO[@]+"${SELLO[@]}"} --options runtime --entitlements app/mac/entitlements.plist "$exe"; done
codesign --force --sign "$FIRMA" ${SELLO[@]+"${SELLO[@]}"} --options runtime --entitlements app/mac/entitlements.plist "$C/MacOS/lanzador"
codesign --force --sign "$FIRMA" ${SELLO[@]+"${SELLO[@]}"} --options runtime --entitlements app/mac/entitlements.plist "$APP"
codesign --verify --deep --strict "$APP" && echo "firmada ($FIRMA)"
# todo de la arquitectura pedida y para el macOS mínimo (si no, en otros Mac no abre: «no es compatible»)
for exe in "$C/MacOS"/* "$C/Frameworks/libSDL3.0.dylib"; do
  [ "$(lipo -archs "$exe")" = "$a" ] || { echo "$exe no es $a: $(lipo -archs "$exe")"; exit 1; }
  m=$(otool -l "$exe" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
  [ "$m" = "$MINIMO" ] || { echo "$exe pide macOS $m (no $MINIMO)"; exit 1; }
done
echo "$APP: $a, macOS $MINIMO+, $N programas, $(du -sh "$APP" | cut -f1), versión $VERSION"

if [ "$DMG" = 1 ]; then
  case "$a" in arm64) SUF=apple-silicon ;; x86_64) SUF=intel ;; *) SUF=$a ;; esac
  DMGF="$B/PCFSite-Source-$VERSION-macos-$SUF.dmg"
  rm -f "$B"/PCFSite-Source-*-macos-$SUF.dmg
  rm -f "$DMGF"
  # ventana del .dmg: fondo (app/mac/dmg/fondo.py), la app a la izquierda y Aplicaciones a la derecha, a 128 px, y el
  # disco con el icono de la app. Se monta en lectura y escritura para que Finder guarde la vista en .DS_Store y luego
  # se comprime.
  VOL="PCFSite - Source"
  [ -d "/Volumes/$VOL" ] && { echo "desmonta antes /Volumes/$VOL (Finder no distinguiría los dos discos)"; exit 1; }
  T=$(mktemp -d); cp -R "$APP" "$T/"; ln -s /Applications "$T/Aplicaciones"
  mkdir "$T/.background"
  tiffutil -cathidpicheck app/mac/dmg/fondo.png app/mac/dmg/fondo@2x.png -out "$T/.background/fondo.tiff" >/dev/null 2>&1
  cp app/mac/pcfsite.icns "$T/.VolumeIcon.icns"
  RW="$B/dmg-rw.dmg"; rm -f "$RW"
  hdiutil create -volname "$VOL" -srcfolder "$T" -fs HFS+ -format UDRW -ov "$RW" >/dev/null 2>&1
  rm -rf "$T"
  hdiutil attach -readwrite -noverify -noautoopen "$RW" >/dev/null
  SetFile -a C "/Volumes/$VOL"
  osascript >/dev/null <<OSA
tell application "Finder"
  tell disk "$VOL"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set the bounds of container window to {200, 120, 860, 548}
    set opts to the icon view options of container window
    set arrangement of opts to not arranged
    set icon size of opts to 128
    set text size of opts to 13
    set background picture of opts to file ".background:fondo.tiff"
    set position of item "PCFSite - Source.app" of container window to {170, 205}
    set position of item "Aplicaciones" of container window to {490, 205}
    close
    open
    delay 1
    close
  end tell
end tell
OSA
  sync; sleep 1
  hdiutil detach "/Volumes/$VOL" >/dev/null || { sleep 3; hdiutil detach -force "/Volumes/$VOL" >/dev/null; }
  # UDZO (zlib): ULZO ya no lo crea hdiutil en macOS 27 y UDZO lo abre cualquier Mac
  hdiutil convert "$RW" -format UDZO -o "$DMGF" >/dev/null
  rm -f "$RW"
  if [ "$FIRMA" != - ]; then codesign --force --sign "$FIRMA" --timestamp "$DMGF"; fi
  if [ "$NOTARIZAR" = 1 ]; then
    ( source ~/.appstoreconnect/config
      xcrun notarytool submit "$DMGF" --key "$ASC_KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER_ID" --wait )
    xcrun stapler staple "$DMGF"
    spctl -a -t open --context context:primary-signature -v "$DMGF"
  fi
  echo "$DMGF: $(du -sh "$DMGF" | cut -f1)"
fi
done
