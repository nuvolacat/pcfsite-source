# tools — herramientas del núcleo

Todas se lanzan desde `core/` con `PCF_JUEGO=<id>` cuando dependen del juego.

| Herramienta | Qué hace |
|---|---|
| `cpr.py` | Extrae los contenedores de instalación `.CPR` de los PC Fútbol de Dinamic (flujos ZIP sin directorio central; el `.HDR` hermano es el índice). Con ellos se reproduce la instalación sin `INSTALAR.EXE`: `RECURSOS.CPR` → raíz, `DBDAT.CPR` → `DBDAT/`, `SIMULDAT.CPR` → `SIMULDAT/`… |
| `instalar_nfo.py` | Instala un PC Fútbol desde su CD con los guiones del instalador original (los `.nfo` de la raíz del CD): qué contenedor va a qué carpeta y qué se queda en el CD. |
| `funciones.py` | Guarda en `juegos/<id>/funciones/` las funciones de la exportación de Ghidra (dirección, tamaño y si es un `Catch@` o una `FUN_` sin referencias): lo único que el traductor usa de Ghidra. Se vuelve a ejecutar tras analizar de nuevo un binario (`PCF_JUEGO=<id> python3 tools/funciones.py`). |
| `le2pe.py` | Convierte un ejecutable LE de DOS/4GW (Watcom, modelo plano de 32 bits) en un PE equivalente, para que Ghidra y el traductor lo traten como cualquier otro. Deja también `<programa>.le.txt` con los punteros a código de los datos del LE (entradas para el traductor). Para el 4.0. |
| `web_datos.py` | Prepara `build/<id>/web-site/`: la página, `juego*.js/.wasm` y `datos/` con enlaces a la carpeta de trabajo, `indice.txt` (el árbol que monta `web.cpp`) y `precarga.txt` (qué descargar primero; `juegos/<id>/precarga-caliente.txt` manda). Respeta `web.excluir` y `web.cd` de `juego.json`. |
| `web_servir.py [puerto]` | Servidor local de `web-site/` con las cabeceras COOP/COEP (hilos de WebAssembly) y peticiones Range, como nginx y R2. |
| `web_probar.py SALIDA SEGUNDOS [CADA] [GUION]` | Prueba automática de la web en Chromium (Playwright): consola a `SALIDA/consola.txt`, capturas cada `CADA` s, guion en coordenadas 640×480. |
| `web_guion.py SALIDA SEGUNDOS CADA "guion"` | Lo mismo con el guion nativo (coordenadas del juego, ops `click/move/key/char/ifpx/shot/reload`). `PCF7_WEB_URL` (por defecto `http://127.0.0.1:8797/`) y `PCF7_WEB_DESP` (ms que se suman a todos los pasos). |
| `dls2sf2.py` | Convierte un banco DLS nivel 1 (el General MIDI/GS de Windows 98, `GM.DLS`) en SoundFont 2 (regiones, bucles, ganancia, envolventes, LFO): el banco `platform/fonts/gm.sf2` del DirectMusic del 2000. |

Playwright va en el `.venv` del núcleo (`.venv/bin/pip install playwright && .venv/bin/playwright install chromium`).

## Recetas

Capturas para una ficha, en nativo:

```bash
PCF7_HEADLESS=1 PCF7_TIMEOUT=90 PCF7_SHOT=/tmp/c PCF7_SHOT_MS=10000 PCF7_SCRIPT="12000:click:650:418;…" build/native-<id>/<id>
sips -s format jpeg -s formatOptions 85 /tmp/c60000.bmp --out ../site/pcf/img/capturas/<id>/<captura>.jpg
```

Comprobar las partidas en la web (grabar, recargar, cargar):

```bash
.venv/bin/python tools/web_guion.py /tmp/p 640 20 "…;300000:click:660:483;…;336400:click:546:337;356400:reload:0:0;446400:click:175:88"
grep "partidas" /tmp/p/consola.txt      # «n ficheros guardados» y «n de n ficheros de partidas restaurados»
```

Qué lee el juego del CD (para `web.cd`):

```bash
PCF7_TRACE_FILES=1 PCF7_HEADLESS=1 PCF7_TIMEOUT=120 PCF7_SCRIPT=… build/native-<id>/<id> > /tmp/t.out
grep "^fichero:" /tmp/t.out | grep "/iso/"
```
