#!/bin/bash
# Copia diaria de las partidas de source.pcfsite.com (/var/lib/source-partidas, con su historial de versiones):
# un tar.gz en /var/backups/source-partidas (solo el último) y otro cifrado con la clave PÚBLICA de copias, en
# /var/backups/foro-cifradas (solo el último), de donde el servidor de copias se lo lleva cada noche (como foro-copia-cifrada).
# Ya pasan de 14 GB: con 14 días de copias el disco se llenó el 03-nov y tumbó el foro. Corre como root.
set -euo pipefail
cd /
SRC=/var/lib/source-partidas
LOCAL=/var/backups/source-partidas
DEST=/var/backups/foro-cifradas
FPR=7BB42A47580B3BAB5E965EBB1CBCA426683B0811
install -d -m 700 "$LOCAL"
F="$LOCAL/source-partidas-$(date +%F).tar.gz"
# las de días anteriores fuera antes de crear la nueva (la de ayer ya la tiene el servidor de copias)
find "$LOCAL" -name 'source-partidas-*.tar.gz' ! -path "$F" -delete
trap 'rm -f "$F.tmp"' EXIT
(umask 077; tar -C "$(dirname "$SRC")" -czf "$F.tmp" "$(basename "$SRC")")
mv "$F.tmp" "$F"
if [ -d "$DEST" ] && [ -f /etc/foro/copias-pub.asc ]; then
  G=$(mktemp -d); trap 'rm -rf "$G" "$F.tmp" "$DEST/$(basename "$F").gpg.tmp"' EXIT
  gpg --homedir "$G" --batch --quiet --import /etc/foro/copias-pub.asc
  (umask 022; gpg --homedir "$G" --batch --yes --quiet --trust-model always -r "$FPR" -o "$DEST/$(basename "$F").gpg.tmp" --encrypt "$F")
  mv "$DEST/$(basename "$F").gpg.tmp" "$DEST/$(basename "$F").gpg"
  find "$DEST" -name 'source-partidas-*.tar.gz.gpg' ! -name "$(basename "$F").gpg" -delete
fi
