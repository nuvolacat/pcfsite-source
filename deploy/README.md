# deploy — lo que corre en el servidor

`deploy.sh` (en la raíz) sube una carpeta temporal a `~/source-deploy/` del usuario de despliegue y ejecuta
`remote.sh` como root con `sudo -S` (la contraseña va por la entrada estándar, no por la línea de órdenes). Accesos en
`deploy.env` (no versionado): `DEPLOY_HOST`, `DEPLOY_PASS`, `DEPLOY_SSH_OPTS` (lleva el salto `-J`; para un ssh a
mano hay que hacer `eval` con esas opciones).

## remote.sh

1. **Web estática** → `/var/www/source/` (`rsync --delete` de `dist/`). Imprime la versión (`_version.txt`).
2. **Juegos en el navegador** (solo con `--jugar`) → `/var/www/source-jugar/<id>/`, **juego a juego**: un despliegue
   solo toca los juegos que trae, así una rama que no tenga compilado un juego no borra el de otra.
3. **Servicio de partidas**: instala `servicio/partidas.py` y `limpiar.py` en `/opt/source-partidas/`, la copia
   diaria en `/usr/local/sbin/source-partidas-copia`, el cron en `/etc/cron.d/source-partidas` y la unidad
   `source-partidas.service` (usuario de sistema `source-partidas`, datos en `/var/lib/source-partidas`); la reinicia y
   comprueba que arranca.
4. **nginx** (solo con `--nginx`): instala `nginx-source.pcfsite.com.conf` en `sites-available`, lo enlaza, `nginx -t`
   y recarga; si falla, deja la configuración anterior.

La carpeta de despliegue tiene que estar en el home de quien despliega y en `700` (no en `/tmp`).

## nginx-source.pcfsite.com.conf

| Ruta | Qué sirve |
|---|---|
| `/` | `dist/` (las páginas `/<juego>` son `<juego>.html`), con las cabeceras comunes de pcfsite y la CSP del sitio |
| `/pcf/` | el diseño (css, js, imágenes, capturas), con caché larga |
| `/pcf[0-9]+` (fichas) | aisladas con COOP/COEP para que el `iframe` del juego también lo esté |
| `/jugar/` | `/var/www/source-jugar/`: `juego.js`/`.wasm` (con `.gz` precalculado), COOP/COEP, CSP con `wasm-unsafe-eval`, `worker-src blob:` y `connect-src` a `datos.pcfsite.com` |
| `/partidas/` | el servicio de partidas en `127.0.0.1:8870` (valida la sesión él mismo) |
| `/cuenta/yo` | el foro dice quién es (mismo origen para que viaje la cookie de pcfsite.com) |
| `/_acceso` | `auth_request` por cada petición: el servicio de partidas decide si el sitio está abierto, autorizado para esa cuenta o solo para administradores (Admin › Source); sin sesión, 302 a `foro.pcfsite.com/entrar`. Jugar (`/jugar/…`) exige sesión aunque el sitio esté abierto |
| `/_cerrado` | a donde van los 403 del control de acceso: el servicio pinta «Acceso cerrado» (403) o «En mantenimiento» con su mensaje (503) |
| `/estado.json` | catálogo, aviso y mantenimiento que cambia el Admin (sin `auth_request`; `site/pcf/estado.js` lo aplica en cada página) |

Los datos de los juegos no pasan por este servidor: están en el bucket R2 `pcfsite-datos` (Cloudflare), servido en
`https://datos.pcfsite.com/<id>/` con CORS para `source.pcfsite.com`. Se suben con `tools/subir_r2.py`.

## Comprobar un despliegue

El sitio devuelve 302 sin sesión (control de acceso), así que se comprueba por ssh:

```bash
source deploy.env; export SSHPASS="$DEPLOY_PASS"
eval "sshpass -e ssh $DEPLOY_SSH_OPTS -o PreferredAuthentications=password -o PubkeyAuthentication=no $DEPLOY_HOST \
  'cat /var/www/source/_version.txt; ls /var/www/source-jugar/; systemctl is-active source-partidas'"
```
