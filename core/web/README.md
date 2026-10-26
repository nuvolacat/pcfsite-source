# web — los juegos en el navegador

El mismo C generado y la misma plataforma, compilados con Emscripten (`emcmake cmake -B build/web-<id>`): SDL3 de
Emscripten para la pantalla, la entrada y el audio; hilos con Web Workers y `SharedArrayBuffer` (por eso la página
necesita las cabeceras COOP/COEP); `longjmp` de WebAssembly para las pistas de aterrizaje SEH; llamadas de cola
reales para los `jmp` entre funciones. Sale `juego.js` + `juego.wasm` por programa (`juego-dbasewin.*` para los
demás programas del CD).

```
index.html              la página: el canvas, la pantalla de carga, el aviso de precarga y el registro (?log)
inicio.js               configura el módulo antes de cargar juego.js: URL de los datos, precarga, partidas, lanzador
partidas.js             las partidas guardadas en tres capas (navegador, cuenta, copia manual)
libwasmfs_fetch.js      copia del backend de WasmFS por red de Emscripten 6.0.12 con un arreglo (se enlaza con --js-library)
```

## Los datos

El navegador no descarga el juego entero. `platform/src/web.cpp` monta en `/pcf7/ro` un sistema de ficheros de WasmFS
cuyo contenido se pide al servidor **por trozos de 512 KB** (peticiones Range) a medida que el juego lee; el árbol se
crea al arrancar a partir de `indice.txt`. La URL base la da la página (`data-datos` de `/jugar/<id>/index.html`,
hoy `https://datos.pcfsite.com/<id>/`, el bucket R2 `pcfsite-datos`).

`tools/web_datos.py` prepara `build/<id>/web-site/datos/` con enlaces a la carpeta de trabajo:

```
datos/
  game/<CARPETA>/   la instalación (C:\<CARPETA>), menos web.excluir de juego.json
  cd/               la parte del CD que lee el juego (web.cd de juego.json; sin instalador, DirectX, vídeos de la intro…)
  c/                el resto del disco C (WINDOWS/SYSTEM)
  fonts/            las fuentes de la plataforma
  mem.bin           la imagen de memoria del programa principal; mem-<programa>.bin para los demás
  indice.txt        «d ruta» / «f ruta<TAB>fecha»: el árbol que monta web.cpp
  precarga.txt      «ruta<TAB>tamaño»: lo que la página descarga entero a la caché del navegador, en ese orden
```

**Precarga**: `inicio.js` descarga en segundo plano los ficheros de `precarga.txt` a la caché del navegador
(`<juego>-datos-<versión del índice>`), y el backend de WasmFS los lee de ahí en vez de esperar a la red. Lo primero
de la lista es lo que el juego necesita al arrancar (`juegos/<id>/precarga-caliente.txt` manda; el resto, por orden
de tamaño). Sin esto, los bucles de sonido mientras llega un fichero grande se notan (fue el primer problema de la
web del 7). Lo ya descargado sirve en las siguientes visitas; si no cabe (`QuotaExceededError`) se deja como está.

Los datos se suben a R2 con `tools/subir_r2.py` del repo raíz (sigue los enlaces, solo sube lo que cambia, borra lo
que ya no está). R2 sirve con CORS para `source.pcfsite.com`; la CSP de `/jugar/` permite `connect-src` a
`datos.pcfsite.com`.

## Las partidas

Todo lo que el juego escribe va a `/pcf7/rw` (en memoria; copy-on-write sobre `/pcf7/ro`). `web.cpp` vigila esa capa:
cuando un fichero lleva un rato sin cambiar y nadie lo tiene abierto para escribir, se lo pasa a la página, y
`partidas.js` lo guarda:

1. **Navegador**: IndexedDB (`ficheros`, por ruta, con historial de versiones).
2. **Cuenta de pcfsite**: si hay sesión, `PUT /partidas/<id>/f/<ruta>` al servicio (`servicio/partidas.py`),
   comprimido. Al arrancar se juntan las dos copias y gana la más nueva de cada fichero; lo pendiente de subir se sube.
3. **Copia manual**: `exportar()`/`importar()` (botones «Descargar mis partidas» y «Cargar partidas» de la ficha), un
   JSON con todos los ficheros.

Antes de arrancar el juego, `inicio.js` pone los ficheros restaurados en `/pcf7/rw` (`Module.pcf7Restaurar`) y el
registro dice «n de n ficheros de partidas restaurados». Los temporales de `WINDOWS\TEMP` no se guardan.

## El lanzador

En el original, el menú principal sale con el número del apartado y un lanzador (`PCF7.EXE`, `FUTBOL4.EXE`…) abre el
programa que toca (Historia, Base de datos…). En la web, `juego.js` avisa a la página (`Module.pcf7Lanzar(programa,
argumento)`) y `inicio.js` carga `juego-<programa>.js` con su `mem-<programa>.bin`, sobre los mismos datos y
partidas; al terminar, vuelve al principal.

## Versiones y caché

`deploy.sh --jugar` calcula una versión por compilación (sha de los `.js`, `.wasm`, `inicio.js` y `partidas.js`) y la
pone en `index.html` (`data-version`): la página pide `juego.js?v=…` y `juego.wasm?v=…`, así el navegador nunca mezcla
un `.js` guardado de un despliegue anterior con un `.wasm` nuevo (daba «El juego se ha detenido por un error»). Los
`.wasm` y `.js` se sirven también comprimidos (`.gz` precalculado).

## Probar en local

```bash
PCF_JUEGO=pcf2001 python3 tools/web_datos.py        # build/pcf2001/web-site/
PCF_JUEGO=pcf2001 python3 tools/web_servir.py 8822   # sirve web-site/ con COOP/COEP y Range → http://127.0.0.1:8822/
.venv/bin/python tools/web_guion.py SALIDA 300 20 "12000:click:650:418;…"   # Playwright: capturas y consola
```

`web_guion.py` usa el guion nativo (coordenadas del juego, ops `click/move/key/char/ifpx/shot/reload`), con
`PCF7_WEB_URL` para la URL y `PCF7_WEB_DESP` (ms) para desplazar todos los pasos (la web tarda más en arrancar).
`reload` recarga la página en la misma sesión: sirve para comprobar que las partidas sobreviven. Ojo: con `?log` el
registro se pinta sobre la parte baja del canvas y se traga los clics de esa zona; para los guiones, sin `?log`
(la consola se guarda igual en `consola.txt`).

## Qué cambia respecto al nativo

- El reloj y las esperas: `cooperative_sleep` cede al navegador; el audio sale por `SDL_AudioStream` desde un worker.
- Las lecturas de ficheros pueden tardar (red): `PCF7_LECTURA_LENTA` en nativo simula ese retardo para cazar bucles
  de sonido o de espera.
- `PCF7_HASHES` vuelca los hashes de la memoria por bloques de 64 KB: comparando nativo y web se encuentra dónde
  divergen dos ejecuciones con el mismo guion (`PCF7_RELOJ_VIRTUAL=1` para que sean deterministas).
