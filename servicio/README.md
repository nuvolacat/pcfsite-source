# servicio — las partidas en la cuenta de pcfsite

`partidas.py` es un servicio HTTP pequeño (Python, sin dependencias) en `127.0.0.1:8870`, detrás de nginx
(`/partidas/`). Guarda los ficheros que escribe cada juego en el navegador, por usuario y por juego, para que las
partidas se recuperen desde cualquier ordenador. Quién es el usuario lo dice el foro: se le pasa la cookie de la
petición a su `/api/yo`. No guarda contraseñas ni datos personales: solo los ficheros del juego.

```
GET    /partidas/<juego>/              → {"ficheros": [{ruta, fecha, tam, sha, borrado}]}
GET    /partidas/<juego>/f/<ruta>      → el fichero (tal como se subió: el cliente lo manda con gzip)
PUT    /partidas/<juego>/f/<ruta>?fecha=ms&sha=…   (cuerpo = fichero) → 409 si el servidor tiene una más nueva
DELETE /partidas/<juego>/f/<ruta>?fecha=ms
POST   /partidas/hit · /partidas/beat  («<juego> <id>», sendBeacon de core/web/inicio.js): arranques y latidos
GET    /estado.json                    → catálogo, aviso y mantenimiento (site/pcf/estado.js lo aplica en cada página)
GET    /acceso/comprobar (auth_request de nginx) → 204/401/403; /acceso/pagina (error_page 403) → cerrado (403) o mantenimiento (503)
GET    /internal/v1/…                  → la API del Admin (admin.pcfsite.com), solo local y con su JWT (pcf_interno.py)
```

**El Admin (Source)** administra el sitio por `/internal/v1/` con el mismo contrato firmado que el foro y el portal
(`pcf_interno.py` es la copia exacta de `Admin-PCFutbol/contracts/verificador/pcf_interno.py`; la clave pública va en
`source-partidas.service`): acceso (abierto o cerrado, usuarios autorizados), catálogo de la portada (ocultar, renombrar,
«Nuevo», «Arreglado», orden), aviso, mantenimiento (solo pasan los administradores del foro; los demás ven la página
503 con el mensaje), estadísticas (arranques por juego y día, jugando ahora, récord; `estadisticas.sqlite`) y las partidas
de cualquier usuario (ver, descargar, restaurar una versión del historial, mandar a `_papelera/`, que `limpiar.py`
vacía a los 30 días). Estado del sitio en `acceso.json` y `ajustes.json`.

`JUEGOS` es la lista de ids admitidos (los `JUEGOS_WEB` de `deploy.sh`): hay que añadir cada juego nuevo.

Disco: `/var/lib/source-partidas/<id usuario>/<juego>/actual/<ruta>` + `meta.json`; cada versión sustituida o borrada
va a `historial/<ruta>/<fecha>` (las 10 últimas), para recuperar a mano una partida estropeada.

| Fichero | Qué es |
|---|---|
| `partidas.py` | el servicio (`PARTIDAS_DATOS`, `PARTIDAS_PUERTO`, `PARTIDAS_FORO_PUERTO`, `PCF_ADMIN_FIRMA_PUB`, `PCF_INTERNO_JTI_DIR` por el entorno) |
| `pcf_interno.py` | verificador de los tokens del Admin (copia canónica en el repo del Admin; no se edita aquí) |
| `limpiar.py` | borra las partidas de quien ha borrado su cuenta del foro (lo que promete pcfsite.com/legal) y vacía `_papelera/` a los 30 días; lo lanza el cron |
| `copia.sh` | copia diaria: un `tar.gz` por día en `/var/backups/source-partidas` (14 días) y otro cifrado con la clave pública de copias en `/var/backups/foro-cifradas`, de donde se lo lleva el servidor de copias cada noche |
| `cron` | la copia y la limpieza, en `/etc/cron.d/source-partidas` |
| `source-partidas.service` | la unidad de systemd (usuario de sistema `source-partidas`, sin shell) |

Lo instala `deploy/remote.sh` en cada despliegue y reinicia el servicio.
