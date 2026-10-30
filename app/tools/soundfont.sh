#!/bin/bash
# Baja la soundfont General MIDI libre que usa la música MIDI del PC Fútbol 2000 (DirectMusic, platform/src/dmusic.cpp):
# GeneralUser GS 2.0.3 de S. Christian Collins (uso y redistribución libres: core/platform/fonts/LICENSE-GeneralUser-GS.txt).
# Queda en core/platform/fonts/gm.sf2 (fuera de git por tamaño); los empaquetadores la copian a la app.
set -euo pipefail
cd "$(dirname "$0")/../.."
F=core/platform/fonts/gm.sf2
SHA=9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe
URL=https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/97049183643d5fc5a9322a69c5b09efb667c6c3a/GeneralUser-GS.sf2
suma() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
if [ -f "$F" ] && [ "$(suma "$F")" = "$SHA" ]; then echo "gm.sf2: ya está (GeneralUser GS 2.0.3)"; exit 0; fi
curl -fsSL -o "$F.tmp" "$URL"
[ "$(suma "$F.tmp")" = "$SHA" ] || { rm -f "$F.tmp"; echo "gm.sf2: el SHA-256 no cuadra"; exit 1; }
mv "$F.tmp" "$F"; echo "gm.sf2: GeneralUser GS 2.0.3 ($(du -h "$F" | cut -f1))"
