#!/bin/bash
# Publica source.pcfsite.com (dist/, generada por build.py); con --nginx, también su configuración de nginx.
# Accesos en deploy.env (no versionado): los mismos que el deploy.env del foro.
set -euo pipefail
cd "$(dirname "$0")"
source deploy.env
: "${DEPLOY_HOST:?}" "${DEPLOY_PASS:?}"
SSHOPTS="${DEPLOY_SSH_OPTS:-} -o PreferredAuthentications=password -o PubkeyAuthentication=no"
export SSHPASS="$DEPLOY_PASS"
# --nginx: también la configuración de nginx; --jugar: también los juegos en el navegador (desde core/, compilado
# con emcmake; los datos van aparte a R2 con tools/subir_r2.py)
NG=0; JUGAR=0
# id | carpeta de la compilación web en core/ | título de la página | programa principal si no es «manager» (PCF4: el menú PCFUTBOL)
JUEGOS_WEB=("pcf7|build/web|PC Fútbol 7" "pcf7plus|build/web-pcf7plus|PC Fútbol 7 Plus" "pcf6|build/web-pcf6|PC Fútbol 6.0"
            "pcf6oro|build/web-pcf6oro|PC Fútbol 6.0 Edición de Oro" "pcf5|build/web-pcf5|PC Fútbol 5.0" "pcf5oro|build/web-pcf5oro|PC Fútbol 5.0 Edición de Oro"
            "pcf5arg|build/web-pcf5arg|PC Fútbol 5.0 Apertura 97|managarg" "pcf6arg|build/web-pcf6arg|PC Fútbol 6.0 Apertura 98|managarg"
            "pcf4|build/web-pcf4|PC Fútbol 4.0|pcfutbol"
            "pcf2000|build/web-pcf2000|PC Fútbol 2000|manager"
            "pcf2001|build/web-pcf2001|PC Fútbol 2001|manager")
# --jugar=<id>[,<id>…]: solo esos juegos (los demás se quedan como estén en el servidor)
SOLO=""
for a in "$@"; do case "$a" in --nginx) NG=1 ;; --jugar) JUGAR=1 ;; --jugar=*) JUGAR=1; SOLO=",${a#--jugar=}," ;; *) echo "opción desconocida: $a"; exit 1 ;; esac; done
python3 build.py
for f in dist/topbar.js dist/pcf/*.js; do node --check "$f"; done
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/web" "$T/nginx" "$T/deploy"
rsync -a dist/ "$T/web/"
cp deploy/nginx-*.conf "$T/nginx/"
cp deploy/remote.sh "$T/deploy/"
mkdir -p "$T/servicio"
cp servicio/partidas.py servicio/pcfsave.py servicio/pcf_interno.py servicio/limpiar.py servicio/copia.sh servicio/cron servicio/source-partidas.service "$T/servicio/"
if [ "$JUGAR" = 1 ]; then
  # cada juego en el navegador: /jugar/<id>/ con su programa (compilado en core/) y los datos en R2 (datos.pcfsite.com/<id>)
  P7=core
  for J in "${JUEGOS_WEB[@]}"; do
    IFS='|' read -r id web titulo principal datos <<< "$J"; principal=${principal:-manager}; datos=${datos:-https://datos.pcfsite.com/$id}
    [ -z "$SOLO" ] || [[ "$SOLO" == *",$id,"* ]] || continue
    mkdir -p "$T/jugar/$id"
    cp "$P7/$web"/juego*.js "$P7/$web"/juego*.wasm "$P7/web/inicio.js" "$P7/web/partidas.js" "$T/jugar/$id/"
    rm -f "$T/jugar/$id/juego-proquini."*   # ProQuinielas está desfasado
    # versión de esta compilación: los .js y .wasm se piden con ?v=<versión>, así el navegador nunca mezcla un
    # juego.js guardado de un despliegue anterior con el .wasm nuevo
    ver=$(cat "$T/jugar/$id"/juego*.js "$T/jugar/$id"/juego*.wasm "$T/jugar/$id"/inicio.js "$T/jugar/$id"/partidas.js | shasum | cut -c1-12)
    sed -e "s|<html lang=\"es\" data-juego=\"pcf7\">|<html lang=\"es\" data-juego=\"$id\" data-datos=\"$datos\" data-version=\"$ver\" data-principal=\"$principal\">|" \
        -e "s|<script src=\"partidas.js\">|<script src=\"partidas.js?v=$ver\">|" -e "s|<script src=\"inicio.js\">|<script src=\"inicio.js?v=$ver\">|" \
        -e "s|<title>PC Fútbol 7</title>|<title>$titulo</title>|" "$P7/web/index.html" > "$T/jugar/$id/index.html"
    grep -q "data-datos=\"$datos\" data-version=\"$ver\"" "$T/jugar/$id/index.html"
    grep -q "inicio.js?v=$ver" "$T/jugar/$id/index.html"
    grep -q "<title>$titulo</title>" "$T/jugar/$id/index.html"
    gzip -9 -k "$T/jugar/$id"/juego*.wasm "$T/jugar/$id"/juego*.js
  done
fi   # sin --jugar, remote.sh no toca lo que ya haya en el servidor
sshpass -e rsync -rlt --delete --rsync-path="mkdir -p -m 700 source-deploy && chmod 700 source-deploy && rsync" \
  -e "ssh $SSHOPTS" "$T/" "$DEPLOY_HOST:source-deploy/"
# la contraseña de sudo va por la entrada estándar (en la orden se podía leer en /proc)
printf '%s\n' "$DEPLOY_PASS" | sshpass -e ssh $SSHOPTS "$DEPLOY_HOST" \
  "sudo -S -p '' bash \"\$HOME/source-deploy/deploy/remote.sh\" \"\$HOME/source-deploy\" $NG"
