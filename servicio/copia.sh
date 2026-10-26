#!/bin/bash
# Copia diaria de las partidas de source.pcfsite.com (/var/lib/source-partidas, con su historial de versiones):
# un tar.gz por día en /var/backups/source-partidas (14 días) y otro cifrado con la clave PÚBLICA de copias, en
# /var/backups/foro-cifradas, de donde el servidor de copias se lo lleva cada noche (como foro-copia-cifrada). Corre como root.
set -euo pipefail
cd /
SRC=/var/lib/source-partidas
LOCAL=/var/backups/source-partidas
DEST=/var/backups/foro-cifradas
FPR=7BB42A47580B3BAB5E965EBB1CBCA426683B0811
install -d -m 700 "$LOCAL"
F="$LOCAL/source-partidas-$(date +%F).tar.gz"
(umask 077; tar -C "$(dirname "$SRC")" -czf "$F" "$(basename "$SRC")")
find "$LOCAL" -name 'source-partidas-*.tar.gz' -mtime +14 -delete
if [ -d "$DEST" ] && [ -f /etc/foro/copias-pub.asc ]; then
  G=$(mktemp -d); trap 'rm -rf "$G"' EXIT
  gpg --homedir "$G" --batch --quiet --import /etc/foro/copias-pub.asc
  (umask 022; gpg --homedir "$G" --batch --yes --quiet --trust-model always -r "$FPR" -o "$DEST/$(basename "$F").gpg" --encrypt "$F")
fi
