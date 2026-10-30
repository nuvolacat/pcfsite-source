#!/bin/bash
# Parte del despliegue que corre como root en el servidor (la lanza deploy.sh con la carpeta como argumento).
set -euo pipefail
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

# 1c) servicio de partidas (servicio/partidas.py en 127.0.0.1:8870, detrás de /partidas/), su copia y su limpieza
if [ -d "$SRC/servicio" ]; then
  id source-partidas >/dev/null 2>&1 || useradd --system --home-dir /var/lib/source-partidas --shell /usr/sbin/nologin source-partidas
  install -d -m 750 -o source-partidas -g source-partidas /var/lib/source-partidas
  install -d -m 755 -o root -g root /opt/source-partidas
  install -m 644 -o root -g root "$SRC/servicio/partidas.py" "$SRC/servicio/pcfsave.py" "$SRC/servicio/discos.py" "$SRC/servicio/pcf_interno.py" "$SRC/servicio/limpiar.py" /opt/source-partidas/
  # discos originales de juegos.pcfsite.com para convertir sus partidas (pcfsave.py); las imágenes enteras de R2 piden el
  # token del portal de juegos, que solo puede leer root. La primera vez baja ~3,7 GB; luego solo comprueba
  install -d -m 750 -o source-partidas -g source-partidas /var/cache/source-partidas /var/cache/source-partidas/discos
  python3 /opt/source-partidas/discos.py /var/cache/source-partidas/discos || echo "discos: alguno no cuadra (las partidas de ese juego no se podrán convertir)"
  chown -R source-partidas:source-partidas /var/cache/source-partidas
  install -m 755 -o root -g root "$SRC/servicio/copia.sh" /usr/local/sbin/source-partidas-copia
  install -m 644 -o root -g root "$SRC/servicio/cron" /etc/cron.d/source-partidas
  install -m 644 -o root -g root "$SRC/servicio/source-partidas.service" /etc/systemd/system/source-partidas.service
  systemctl daemon-reload
  systemctl enable --quiet source-partidas
  systemctl restart source-partidas
  sleep 1
  systemctl is-active --quiet source-partidas && echo "partidas: activo" || { echo "partidas: NO arranca"; journalctl -u source-partidas -n 20 --no-pager; exit 1; }
fi

# 2) nginx (solo con --nginx): se comprueba antes de recargar y, si falla, se deja la de antes
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
