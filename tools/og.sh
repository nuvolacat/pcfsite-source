#!/bin/bash
# Genera las imágenes para redes (1200×630) con el navegador headless: og.jpg (general) y og-pcf7.jpg (ficha).
# Si se rehacen, subir OG_V en build.py: Facebook, WhatsApp y X guardan la imagen en caché.
set -euo pipefail
cd "$(dirname "$0")/.."
E="${NAVEGADOR:-/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge}"
T=$(mktemp -d); trap 'rm -rf "$T"; kill $PID 2>/dev/null || true' EXIT
cp -R site/pcf "$T/pcf"; cp docs/og.html "$T/og.html"
python3 -m http.server 8792 -d "$T" >/dev/null 2>&1 & PID=$!
sleep 1
for v in ":og" "?j=pcf7:og-pcf7"; do
  q="${v%%:*}"; n="${v##*:}"; [ "$q" = "" ] && q=""
  "$E" --headless=new --disable-gpu --hide-scrollbars --window-size=1200,630 --virtual-time-budget=3000 \
    --screenshot="$T/$n.png" "http://127.0.0.1:8792/og.html$q" >/dev/null 2>&1
  sips -s format jpeg -s formatOptions 88 "$T/$n.png" --out "site/pcf/img/$n.jpg" >/dev/null
  echo "site/pcf/img/$n.jpg"
done
