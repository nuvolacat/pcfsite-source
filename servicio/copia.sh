#!/bin/bash
# Copia diaria de las partidas de source.pcfsite.com (/var/lib/source-partidas): cifrada con la clave PÚBLICA de copias
# en /var/backups/foro-cifradas (solo la última), de donde el servidor de copias se la lleva cada noche (como
# foro-copia-cifrada). Corre como root.
# - Va de tar a gpg sin tar.gz local en medio: con un tar.gz local de cada día el disco se llenó el 03-nov y tumbó el
#   foro, y la noche siguiente iba a volver a pasar (32 GB de copias locales con el disco al 97 %).
# - Sin el historial de versiones (<uid>/<juego>/historial): ocupa el triple que las partidas (14 GB frente a 4,7 el
#   03-nov) y ya es la copia para recuperar una partida estropeada (Admin › Source › Partidas); fuera solo hace falta
#   la partida de cada uno.
# - La anterior no se borra hasta que la nueva está entera.
set -euo pipefail
cd /
SRC=/var/lib/source-partidas
DEST=/var/backups/foro-cifradas
FPR=7BB42A47580B3BAB5E965EBB1CBCA426683B0811
[ -d "$DEST" ] && [ -f /etc/foro/copias-pub.asc ] || { echo "falta $DEST o la clave de copias" >&2; exit 1; }
N=source-partidas-$(date +%F).tar.gz.gpg
G=$(mktemp -d); trap 'rm -rf "$G" "$DEST/$N.tmp"' EXIT
gpg --homedir "$G" --batch --quiet --import /etc/foro/copias-pub.asc
# tar sale con 1 si un fichero cambia mientras lo lee (alguien guardando a esa hora): la copia vale igual
(umask 022
 { tar -C "$(dirname "$SRC")" --anchored --no-wildcards-match-slash --exclude="$(basename "$SRC")/*/*/historial" \
       --warning=no-file-changed -czf - "$(basename "$SRC")" || [ $? -eq 1 ]; } |
   gpg --homedir "$G" --batch --yes --quiet --trust-model always --compress-algo none -r "$FPR" -o "$DEST/$N.tmp" --encrypt)
mv "$DEST/$N.tmp" "$DEST/$N"
find "$DEST" -name 'source-partidas-*.tar.gz.gpg' ! -name "$N" -delete
# las copias locales sin cifrar de antes (hasta el 03-nov-2026)
rm -rf /var/backups/source-partidas
