# La web: source.pcfsite.com

Vuelta al [README](../README.md).

Es una zona más de [pcfsite.com](https://pcfsite.com), con el diseño «Industry» del foro y la misma barra, pie, tema y
cuenta. `build.py` lee `data/juegos.json` y `site/` y escribe `dist/`: portada con cifras, una ficha por juego (resumen,
capturas reales, qué funciona, qué falta, botón «Jugar» por versión), `/como-funciona` y `/diario` (todos los hitos por
fecha). Las fichas se aíslan con COOP/COEP y cargan el juego desde `/jugar/<id>/` en un `iframe`.

Cada juego en el navegador tiene tres partes:

1. **El programa**: `juego.js` + `juego.wasm` por programa del CD, en la carpeta `source-jugar/<id>/` del servidor, con la versión de la
   compilación en la página para que el navegador nunca mezcle un `.js` viejo con un `.wasm` nuevo.
2. **Los datos** (instalación, la parte del CD que lee el juego, `WINDOWS\SYSTEM` y `mem.bin`): en el bucket R2
   `pcfsite-datos`, servido en `datos.pcfsite.com/<id>/` con CORS. El navegador no los descarga enteros: WasmFS los
   pide por trozos de 512 KB (Range) a medida que el juego lee, y precarga en caché lo que hace falta al arrancar.
   Los prepara `core/tools/web_datos.py` y los sube `tools/subir_r2.py` (solo lo que cambia).
3. **Las partidas**, en tres capas: IndexedDB del navegador, la cuenta de pcfsite (`servicio/partidas.py`, un servicio
   HTTP sin dependencias detrás de nginx que guarda por usuario y juego con historial de 10 versiones) y la copia
   manual (descargar y cargar un JSON desde la ficha).

El sitio se administra desde el Admin de pcfsite.com (sección **Source**, por la API interna firmada del servicio de
partidas, `servicio/README.md`): acceso (abierto, cerrado o limitado a unas cuentas; nginx pregunta por cada petición
al servicio con `auth_request`, y sin sesión responde un 302 al foro), catálogo y aviso de la portada (la web los
aplica con `site/pcf/estado.js`), mantenimiento, estadísticas de arranques (`core/web/inicio.js` avisa al servicio) y
las partidas de cada usuario. Detalles en [`deploy/README.md`](../deploy/README.md) y
[`core/web/README.md`](../core/web/README.md).
