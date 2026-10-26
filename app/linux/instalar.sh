#!/bin/bash
# Crea el acceso del menú de aplicaciones (~/.local/share/applications/pcfsite-source.desktop) apuntando a esta carpeta.
# Con --quitar lo borra. La app no copia nada a otro sitio: la carpeta se puede mover y volver a ejecutar esto.
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"
A="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
if [ "${1:-}" = "--quitar" ]; then rm -f "$A/pcfsite-source.desktop"; echo "acceso quitado"; exit 0; fi
mkdir -p "$A"
sed "s|@CARPETA@|$D|g" "$D/pcfsite-source.desktop" > "$A/pcfsite-source.desktop"
chmod +x "$A/pcfsite-source.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$A" 2>/dev/null || true
echo "acceso creado: $A/pcfsite-source.desktop → $D/pcfsite-source"
