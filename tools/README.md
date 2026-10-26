# tools — herramientas del sitio

| Herramienta | Qué hace |
|---|---|
| `servir.py [puerto]` | Vista previa local de `dist/` con las mismas rutas que nginx (`/pcf7` → `pcf7.html`). `python3 build.py && python3 tools/servir.py 8791`. |
| `subir_r2.py CARPETA PREFIJO` | Sube los datos de un juego al bucket R2 `pcfsite-datos` (`https://datos.pcfsite.com/<prefijo>/`). Sigue los enlaces simbólicos, solo sube lo que cambia (MD5 contra el ETag) y borra del prefijo lo que ya no está en la carpeta. Credenciales por el entorno: `CF_CUENTA` (id de la cuenta), `CF_EMAIL` y `CF_KEY` (la clave global de Cloudflare); **nunca se guardan** en el repo ni en ficheros. `CF_CUENTA=… CF_EMAIL=… CF_KEY=… python3 tools/subir_r2.py core/build/pcf2001/web-site/datos pcf2001`. |
| `og.sh` | Genera las imágenes para redes (1200×630) con el navegador sin cabeza: `og.jpg` (general) y `og-pcf7.jpg` (ficha). Si se rehacen, subir `OG_V` en `build.py`: Facebook, WhatsApp y X guardan la imagen en caché. |

Las herramientas del núcleo (instalar desde el CD, preparar y probar la versión web) están en
[`core/tools/`](../core/tools/README.md).
