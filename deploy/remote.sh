#!/bin/bash
# Parte del despliegue que corre como root en el servidor (la lanza deploy.sh con la carpeta como argumento).
set -euo pipefail
# Corre como root en el servidor Doku (desde el 05-nov-2026): /var/www/source → /var/www/vhosts/pcfsite.com/web/source, nginx interno en 127.0.0.1:8099
SRC="${1:?falta la carpeta del despliegue}"; NG="${2:-0}"
# la carpeta está en el home de quien despliega y en 700 (no en /tmp: ahí otro usuario podía colar ficheros)
[ -d "$SRC" ] && [ "$(stat -c %a "$SRC")" = 700 ] || { echo "carpeta de despliegue insegura: $SRC"; exit 1; }

# 1) web estática
install -d -m 755 -o root -g root /var/www/source
rsync -rlt --chmod=D755,F644 --delete "$SRC/web/" /var/www/source/
chown -R root:root /var/www/source
echo "source: $(cat /var/www/source/_version.txt)"

# 1b) juegos en el navegador (solo si el despliegue los trae: deploy.sh --jugar)
if [ -d "$SRC/jugar" ]; then
  install -d -m 755 -o root -g root /var/www/source-jugar
  # juego a juego: un despliegue solo toca los juegos que trae (otra rama puede tener otros publicados)
  for J in "$SRC"/jugar/*/; do
    [ -d "$J" ] || continue
    rsync -rlt --chmod=D755,F644 --delete "$J" "/var/www/source-jugar/$(basename "$J")/"
  done
  chown -R root:root /var/www/source-jugar
  echo "jugar: $(ls /var/www/source-jugar | tr '\n' ' ')"
fi

# 1c) servicio de partidas (servicio/partidas.py en 127.0.0.1:8902, detrás de /partidas/), su copia y su limpieza
if [ -d "$SRC/servicio" ]; then
  # Servidor Doku (05-nov-2026): el servicio es la app pcf-source del panel (doku-pcf-source, usuario pcfsite, puerto
  # 8902, entorno PARTIDAS_*), y la copia y la limpieza nocturnas son los temporizadores pcf-source-copia / pcf-source-limpiar
  install -d -m 750 -o pcfsite -g pcfsite /var/lib/source-partidas
  install -d -m 755 -o root -g root /opt/source-partidas
  install -m 644 -o root -g root "$SRC"/servicio/*.py /opt/source-partidas/
  install -m 755 -o root -g root "$SRC/servicio/copia.sh" /usr/local/sbin/source-partidas-copia
  systemctl restart doku-pcf-source
  sleep 1
  systemctl is-active --quiet doku-pcf-source && echo "partidas: activo" || { echo "partidas: NO arranca"; journalctl -u doku-pcf-source -n 20 --no-pager; exit 1; }
fi
if [ "$NG" = 1 ]; then
  C=/etc/nginx/sites-available/source.pcfsite.com.conf
  B=""
  if [ -f "$C" ]; then B="$C.bak-$(date +%m%d%H%M)"; cp -a "$C" "$B"; fi
  install -m 644 -o root -g root "$SRC/nginx/nginx-source.pcfsite.com.conf" "$C"
  ln -sf "$C" /etc/nginx/sites-enabled/source.pcfsite.com.conf
  if nginx -t; then
    systemctl reload nginx; echo nginx-recargado
  else
    echo "nginx -t ha fallado: se deja la configuración anterior"
    if [ -n "$B" ]; then cp -a "$B" "$C"; else rm -f "$C" /etc/nginx/sites-enabled/source.pcfsite.com.conf; fi
    exit 1
  fi
fi
