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
POST   /partidas/<juego>/convertir     (cuerpo = <clave>.pcfsave de juegos.pcfsite.com; X-Pcfsave-Nombre = su nombre) → copia de partidas de Source (pcfsave.py)
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
| `pcfsave.py` | convierte la partida de «Mis partidas» de juegos.pcfsite.com (`<clave>.pcfsave`) en una copia de partidas de Source: pone lo que el juego escribió en el disco emulado (bloques de 256 B de v86 en el 4.0, 5.0, 6.0, 7 y los Apertura; sectores de js-dos en el 2000 y el 2001) sobre el disco original, lee el FAT y saca los ficheros cambiados de la carpeta del juego (sin código ni ajustes del Windows emulado; fuera las ranuras a medio grabar). El 7 de allí va al 7 Plus de Source; el 5.0 y el 6.0, a las Ediciones de Oro |
| `discos.py` | deja en `/var/cache/source-partidas/discos` (fuera de la copia diaria) los discos originales de juegos.pcfsite.com que usa `pcfsave.py`, por trozos de 1 MiB, y comprueba su SHA-256; las imágenes enteras de R2 piden el token del portal de juegos, por eso lo lanza `deploy/remote.sh` como root. La primera vez baja ~3,7 GB |
| `pcf_interno.py` | verificador de los tokens del Admin (copia canónica en el repo del Admin; no se edita aquí) |
| `limpiar.py` | borra las partidas de quien ha borrado su cuenta del foro (lo que promete pcfsite.com/legal) y vacía `_papelera/` a los 30 días; lo lanza el cron |
| `copia.sh` | copia diaria: un `tar.gz` por día en `/var/backups/source-partidas` (14 días) y otro cifrado con la clave pública de copias en `/var/backups/foro-cifradas`, de donde se lo lleva el servidor de copias cada noche |
| `cron` | la copia y la limpieza, en `/etc/cron.d/source-partidas` |
| `source-partidas.service` | la unidad de systemd (usuario de sistema `source-partidas`, sin shell) |

Lo instala `deploy/remote.sh` en cada despliegue y reinicia el servicio.
