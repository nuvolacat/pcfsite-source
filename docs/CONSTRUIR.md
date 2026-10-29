# Construir

Requisitos, traducción de un juego, app de escritorio, sitio, pruebas y ramas (antes en el README). Vuelta al [README](../README.md).

## Requisitos

- macOS (Apple Silicon es lo probado; el código es portable) o Linux. CMake, Ninja, SDL3 (`brew install cmake ninja sdl3`).
- Python 3 con `capstone` y `pefile` en `core/.venv` (`python3 -m venv core/.venv && core/.venv/bin/pip install capstone pefile`);
  Playwright para las pruebas web.
- Ghidra 12 (Homebrew, `JAVA_HOME` openjdk@21) solo para analizar un binario nuevo: las funciones que encontró en los ya
  traducidos van en el repo (`core/juegos/<id>/funciones/`, `core/tools/funciones.py`), y sin Ghidra sale el mismo C.
- Emscripten (`emcmake`) para la web; llvm-mingw y SDL3 cruzada para Windows (los baja `app/win/preparar.sh`).
- La carpeta de trabajo de cada juego, con el CD, la instalación y los binarios (`core/juegos/README.md`). No está en el repo.

## Un juego, de principio a fin

```bash
cd core
# 1) Ghidra: funciones y descompilado → ~/pcf2001src-work/decomp/<binario>/ (unos 20 min para el 2001)
PCF7_WORK=~/pcf2001src-work ghidra/run_headless.sh pcf2001 ~/pcf2001src-work/bin/manager.exe ~/pcf2001src-work/bin/gfxwin.dll …
# 2) traducir el programa principal y los demás programas del CD (unos 6 min para las 91.000 funciones del 2001)
PCF_JUEGO=pcf2001 .venv/bin/python recomp/build.py
PCF_JUEGO=pcf2001 PCF7_TARGET=dbasewin.exe .venv/bin/python recomp/build.py
# 3) nativo
cmake -G Ninja -S . -B build/native-pcf2001 -DPCF_JUEGO=pcf2001 -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-pcf2001 -j8
build/native-pcf2001/pcf2001                       # ventana SDL; registro en ~/pcf2001src-work/pcf2001.log
# 4) web
emcmake cmake -S . -B build/web-pcf2001 -DPCF_JUEGO=pcf2001 -DCMAKE_BUILD_TYPE=Release
cmake --build build/web-pcf2001 -j6
PCF_JUEGO=pcf2001 python3 tools/web_datos.py       # build/pcf2001/web-site/ (datos/, indice.txt, precarga.txt)
PCF_JUEGO=pcf2001 python3 tools/web_servir.py 8822 # http://127.0.0.1:8822/ con COOP/COEP y Range
```

`build.py` solo reescribe los `chunk_*.c` que cambian, así que la compilación es incremental; si cambia `lift.py` hay
que relanzarlo a mano. Una «llamada indirecta a 0x… sin traducir» en el registro es una función que Ghidra no definió:
se añade a `juegos/<id>/unresolved.txt` y se retraduce.

Cada juego es la traducción de una edición concreta del CD (`sha256` en `juego.json`; `build.py` se para con otra).
El 4.0, de MS-DOS, se traduce desde los `.DAT` de la instalación (la carpeta `FILES` del CD de la 4.1, que el instalador
copiaba entera en `C:\FUTBOL4`; `PCFUTBOL.DAT` de 333.923 bytes), convertidos antes en PE, y cada uno es un programa:

```bash
for p in pcfutbol pcmanag promanag historia; do
  python3 tools/le2pe.py ~/pcf4-work/game/FUTBOL4/$(echo $p | tr a-z A-Z).DAT ~/pcf4-work/bin/$p.exe
  PCF_JUEGO=pcf4 PCF7_TARGET=$p.exe .venv/bin/python recomp/build.py
done
```

## La app de escritorio

```bash
app/mac/preparar.sh                     # una vez: SDL3 para arm64 y x86_64, macOS 12+, en ~/pcfsite-mac
app/mac/empaquetar.sh                   # compila los 36 programas para Apple Silicon e Intel: core/build/mac-<arch>/PCFSite - Source.app
app/mac/empaquetar.sh --dmg             # además los .dmg (UDZO): …-macos-apple-silicon.dmg y …-macos-intel.dmg
app/mac/empaquetar.sh --arch x86_64     # solo una arquitectura
app/win/preparar.sh                     # una vez: llvm-mingw y SDL3 para x86_64 y aarch64 en ~/pcfsite-win
app/win/empaquetar.sh                   # core/build/win-<arch>/PCFSite - Source/ y su zip, x64 y ARM64
VERSION=1.26.1024.2027 app/win/empaquetar.sh --sin-compilar   # reutilizar una versión (la misma para todos los paquetes)
app/linux/preparar.sh                   # en Linux, una vez: SDL3 estática en ~/pcfsite-linux
app/linux/empaquetar.sh                 # en Linux: core/build/linux/PCFSite - Source/ y su tar.gz (x86_64 o arm64, el de la máquina)
```

La versión es `1.AA.MMDD.HHMM` con la hora del empaquetado (`app/VERSION`); cada versión publicada lleva su entrada en
[`app/CHANGELOG.md`](../app/CHANGELOG.md) y se publica como *release* de GitHub con los cuatro paquetes (`.dmg` de Apple Silicon y de
Intel, zip x64, zip ARM64). Los binarios no van al repo.

## El sitio

```bash
python3 build.py                 # → dist/
python3 tools/servir.py 8791     # http://127.0.0.1:8791/ con las mismas rutas que nginx (/pcf7 → pcf7.html)
./deploy.sh                      # el sitio y el servicio de partidas (accesos en deploy.env)
./deploy.sh --jugar              # además los juegos en el navegador compilados en core/build/web-<id>
./deploy.sh --nginx              # además la configuración de nginx
```

## Pruebas y depuración

El ejecutable nativo corre sin ventana, con tiempo límite, capturas periódicas y un guion de entrada; así se recorren
los juegos, se sacan las capturas de las fichas y se comprueban las partidas:

```bash
PCF7_HEADLESS=1 PCF7_TIMEOUT=60 PCF7_SHOT=/tmp/s PCF7_SHOT_MS=5000 \
PCF7_SCRIPT="12000:click:650:418;20000:char:74;21000:key:13:28" core/build/native-pcf2001/pcf2001
```

El guion (`ms:op:a:b`) admite clics, teclas, esperas a que un píxel tenga un color, capturas y escrituras en memoria;
`core/tools/web_guion.py` ejecuta el mismo guion en el navegador con Playwright. Para depurar hay trazas por subsistema
(`PCF7_WATCH`, `PCF7_TRAZA`, `PCF7_MEMWATCH`, `PCF7_STACKCHECK`, `PCF7_TRACE_FILES`…), un volcado al morir (anillo de
funciones, registros, pila por hilo) y `PCF7_HASHES` para comparar la memoria entre nativo y web paso a paso. La lista
completa está en [`core/platform/README.md`](../core/platform/README.md). El método de fondo es comparar siempre con el
original: el descompilado dice qué espera el juego de cada API, y la plataforma se arregla hasta dárselo.

## Trabajar en varias ramas

El núcleo es común: un cambio en `core/platform` o `core/recomp` afecta a todos los juegos, así que tras tocarlo se
retraducen y arrancan los nueve. Cada frente grande se ha hecho en una rama con un worktree propio
(`../pcfsite-source-<rama>`, al lado del clon principal) y se funde en `main` al terminar. El despliegue del sitio es de la rama que lo
lanza, y `deploy.sh --jugar` solo toca los juegos que trae compilados.
