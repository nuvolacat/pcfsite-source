# PCFSite - Source

Los PCF de Dinamic Multimedia (del 4.0 de 1995 al 2001) **recompilados estáticamente desde su propio código**: cada
binario x86 de 32 bits (el `MANAGER.EXE`, las DLL propias de Dinamic y las de Microsoft que venían en el CD) se traduce
instrucción a instrucción a código C, y ese C se compila junto con una **plataforma propia** en C++ que reimplementa lo
que los juegos piden de Windows 98, MS-DOS y DirectX sobre SDL3. No hay emulador ni máquina virtual ni Wine: el
resultado es un programa nativo (macOS y Windows; el código compila igual en Linux) y, con Emscripten, un módulo
WebAssembly que corre en el navegador.

Del mismo núcleo salen tres cosas:

| Qué | Dónde | Para quién |
|---|---|---|
| **La web** [source.pcfsite.com](https://source.pcfsite.com) | `build.py`, `site/`, `data/`, `deploy/`, `servicio/` | jugar en el navegador con la cuenta de pcfsite; las partidas se guardan en la cuenta |
| **La app de escritorio** «PCFSite - Source» | `app/` | macOS 12 o posterior (Apple Silicon e Intel), Windows (x64 y ARM64) y Linux (x86_64 y arm64, compilada desde el fuente): pide el CD original, reproduce la instalación y arranca el juego nativo; mods |
| **El núcleo** | `core/` | el traductor x86→C, la plataforma, el reproductor web, las herramientas y la ficha técnica de cada juego |

El repo **no contiene ningún fichero de los juegos** ni el C generado a partir de ellos: todo eso es obra derivada del
original y vive fuera (`core/build/`, ignorado; las carpetas de trabajo `~/<id>-work`). La app pide la ISO del CD al
usuario y la web sirve los datos desde un bucket aparte. Hecho por fans, sin afiliación con Dinamic Multimedia.

## Los juegos

Nueve versiones jugables, seis juegos, todos completados (arrancan, se recorren y se juegan: manager, partidos, partidas
guardadas) en nativo y en el navegador. Cada arreglo se anota en el Diario de la ficha (`data/juegos.json`, `hitos`).

| id | Juego | Año | Programa principal | Qué tiene de particular |
|---|---|---|---|---|
| `pcf4` | PCF 4.0 (versión 4.1 del CD) | 1995 | `PCFUTBOL.EXE` (MS-DOS, DOS/4GW) | ejecutables LE de Watcom convertidos a PE (`core/tools/le2pe.py`); VGA 640×480×16 planar, Sound Blaster con DMA e IRQ, int 21h; Manager, Promanager, Historia y Base de datos encadenados por `COMFILE.DAT` |
| `pcf5`, `pcf5oro` | PCF 5.0 y Edición de Oro | 1996, 1997 | `MANAGER.EXE` + 3 DLL | Windows de 16 y 256 colores por GDI; menú y programas del CD encadenados por el lanzador original |
| `pcf6`, `pcf6oro` | PCF 6.0 y Edición de Oro | 1997, 1999 | `MANAGER.EXE` + 2 DLL | DirectDraw, simulador 3D por software (Direct3D 5/6), música S3M |
| `pcf7`, `pcf7plus` | PCF 7 y 7 Plus | 1998, 1999 | `MANAGER.EXE` + 2 DLL, ProQuinielas (Visual Basic) | la línea base del proyecto; el Plus usa DirectInput en el partido |
| `pcf2000` | PCF 2000 con la Extensión | 1999/2000 | `MANAGER.EXE` + 8 DLL, `DBASEWIN` | motor nuevo (gfxwin/d2s2): todo por Direct3D 7, menús incluidos; música MIDI por DirectMusic (sintetizador SoundFont con reverberación); panel de ayuda sobre un WebBrowser falso |
| `pcf2001` | PCF 2001 con las 13 actualizaciones (3.6) | 2000/2001 | `MANAGER.EXE` + 10 DLL, `DBASEWIN` | el mismo motor; música MP3 por DirectShow; MSVCP60 y MSVCIRT traducidas; las actualizaciones se aplican con la propia `patchw32.dll` (RTPatch) traducida (`pcfpatch`) |

Las DLL de Microsoft del CD (MFC42, MSVCRT, MSVCP60, MSVCIRT) se traducen igual que las del juego, porque el juego las
llama de verdad. La ficha técnica de cada uno (binarios, bases de carga, lanzador, parches, notas de instalación) está en
`core/juegos/<id>/juego.json` y su tabla en [`core/juegos/README.md`](core/juegos/README.md).

## Cómo funciona

```
CD original ──► Ghidra (sin interfaz) ──► functions.tsv        ┐
                                                                ├─► recomp/build.py + lift.py ──► C generado (chunk_*.c, imagen.c…)
EXE y DLL (PE 32 bits; LE→PE en el 4.0) ──► linker.py (bases, IAT, parches) ┘                              │
                                                                                                            ▼
                                   platform/ (C++17, SDL3): Win98 + MS-DOS + DirectX ◄──── CMake ──► nativo (macOS, Windows, Linux)
                                                                                                └──► Emscripten: juego.js + juego.wasm
```

1. **Análisis.** [Ghidra](core/ghidra/README.md) en modo headless define las funciones de cada binario (con un script
   propio que cierra los huecos que deja el análisis automático) y exporta su lista y el descompilado, que solo sirve
   para leerlo al depurar.
2. **Traducción** ([`core/recomp/`](core/recomp/README.md)). `lift.py` desensambla cada función con Capstone y escribe
   una función C por función x86. La CPU son **registros globales** (`eax`…`edi`, banderas, pila x87, MMX/SSE) y la
   memoria del proceso es un array `MEM` de 512 MB: una dirección x86 es un índice. `call` y `ret` mueven la pila
   emulada; los saltos internos son `goto`; las tablas de salto, `switch`; los prólogos SEH de MSVC y los `catch` se
   reconocen y se convierten en pistas de aterrizaje `setjmp`/`longjmp`. `linker.py` coloca cada módulo en su base,
   resuelve la IAT en tiempo de traducción (una llamada a `kernel32!CreateFileA` es una llamada C directa al stub de
   la plataforma, o a la función de la DLL traducida si es MFC42) y aplica los pocos parches de bytes que hay
   (anticopia del 4.0). Cada línea de C lleva la instrucción original como comentario. Como no es un descompilador,
   la traducción es tan fiel como un emulador, y como se compila con `-O2` corre como código nativo.
3. **Plataforma** ([`core/platform/`](core/platform/README.md), 38 fuentes, unas 20.000 líneas). Implementa lo que los
   juegos importan: más de mil funciones de 16 DLL de Windows (KERNEL32, USER32 con sus controles y menús, GDI32 con
   texto TrueType, ADVAPI32 con un registro en memoria, OLE32 y OLEAUT32 con COM en proceso, WINMM, WSOCK32,
   WININET…) y los objetos COM de DirectX: DirectDraw 1-7, Direct3D 3-7 con un rasterizador por software, DirectSound
   con mezcla y 3D posicional, DirectInput, DirectMusic, DirectShow (vídeos MPEG-1 y música MP3). Para el 4.0, un
   DOS/4GW con int 21h, una VGA con planos y latches y una Sound Blaster. Los hilos del juego son cooperativos (solo
   uno corre a la vez, porque los registros son globales) y las esperas ceden el turno. Lo que el juego ve es un
   Windows 98 con un `C:` y un CD en `D:` (`vfs.cpp`). Criterio del proyecto: **imitar lo que hace Windows, no parchear
   el juego**.
4. **Compilación.** `core/CMakeLists.txt` con `-DPCF_JUEGO=<id>` compila el C generado y la plataforma en un ejecutable
   por programa del CD (`pcf2001`, `pcf2001-dbasewin`…); con `emcmake`, en `juego.js` + `juego.wasm` (hilos con Web
   Workers y `SharedArrayBuffer`, `longjmp` de WebAssembly, llamadas de cola reales).
5. **Imagen de memoria.** En desarrollo el programa carga `mem.bin` (los módulos ya colocados y relocalizados). La app
   de escritorio no lo lleva, porque contiene el juego: `build.py` emite además `imagen.c`, una *receta* (qué trozos de
   qué fichero van a qué dirección y, encima, los bytes que cambian: relocaciones, IAT, parches) con el SHA-256 de cada
   fichero y de la imagen entera, y `imagen.cpp` la reconstruye en el arranque a partir de los ficheros del CD y de la
   instalación del usuario. Comprobado idéntico a `mem.bin` en los nueve juegos (`PCF7_MEMCHECK=1`).
6. **Inmediatos vivos.** Los mods de PCF traen el EXE con valores del juego cambiados dentro del código (`push
   0x162`, `mov word [tabla], 0x14b`: fechas, tamaños de competición, identificadores). Para que esos EXE surtan efecto
   sin retraducir, el traductor emite los inmediatos de `mov`, `push`, `cmp`, `test`, `add`, `sub`, `and`, `or`,
   `xor`, `imul`… leyéndolos en tiempo de ejecución de los bytes de la propia instrucción en `MEM`, y la app aplica
   sobre la imagen reconstruida las diferencias del EXE del mod. Saltos, `ret n` y el resto del código siguen siendo
   constantes.

## Mapa del repo

```
README.md               este fichero
LICENSE                 GPL-3.0
core/                   el núcleo (core/README.md)
  recomp/               traductor: build.py (orquesta), lift.py (x86→C), linker.py (módulos, IAT, parches), juego.py,
                        winapi_sigs.py (firmas de las APIs a partir de los .spec de Wine)
  platform/             la plataforma C++/SDL3: include/rt.h (CPU y MEM que ve el C generado), src/ (un fichero por
                        subsistema: kernel32, user32, gdi32, ddraw, d3d, dsound, dmusic, dshow, dos/vga/sb, vfs, imagen,
                        compat para Windows…), fonts/ (Liberation), third_party/ (stb_truetype, pl_mpeg, tsf/tml, minimp3)
  web/                  el reproductor del navegador: index.html, inicio.js, partidas.js, backend WasmFS por red
  juegos/<id>/          juego.json (ficha técnica), unresolved.txt (funciones que Ghidra no vio), precarga-caliente.txt,
                        datos_extra/ (ficheros propios que la app añade); pcfpatch/ es el parcheador RTPatch del 2001
  tools/                cpr.py e instalar_nfo.py (reproducir la instalación desde el CD), le2pe.py (DOS/4GW),
                        web_datos.py, web_servir.py, web_probar.py, web_guion.py (Playwright), dls2sf2.py
  ghidra/               run_headless.sh, run_fast.sh y los scripts (FixGaps, ExportAll, DecompAt)
  cmake/mingw.cmake     toolchain de CMake para compilar Windows desde macOS o Linux (llvm-mingw)
  CMakeLists.txt        nativo, web y DLL de Windows; -DPCF_JUEGO=<id>
  build/                (ignorado) C generado, compilaciones y datos preparados para la web
app/                    la app de escritorio (app/README.md, app/CHANGELOG.md, app/VERSION)
  lanzador/             C++17 + SDL3: main.cpp (pantallas, modo terminal, ISO, mods), ui.cpp (interfaz propia con
                        stb_truetype), versiones.cpp (reconocer CD e instalaciones por firmas), instalar.cpp (guiones del
                        instalador, mods), is6.cpp (InstallShield 6), mscab.cpp (cabinets de Microsoft), inflar.cpp
                        (deflate), zip.cpp, json.cpp, util.cpp
  versiones.json        las versiones soportadas: firmas de cada CD, guiones de instalación, binarios (tools/versiones.py)
  tools/versiones.py    genera versiones.json desde las carpetas de trabajo
  mac/                  empaquetar.sh (.app y .dmg), Info.plist, entitlements.plist, pcfsite.icns
  win/                  preparar.sh (llvm-mingw y SDL3 para x86_64 y aarch64), empaquetar.sh (carpeta y zip),
                        lanzador.rc, manifest.xml, pcfsite.ico, LEEME.txt
  assets/, fonts/       logo, carátulas, Barlow (OFL)
build.py                genera dist/ del sitio (portada, /<juego>, /como-funciona, /diario); sin dependencias
data/juegos.json        los juegos en el sitio: estado, resumen, capturas, qué funciona, qué falta, versiones, Diario
site/                   diseño «Industry» (ds.css del foro, comun.css, source.css), topbar.src.js, cuenta.js, tema.js,
                        juego.js (el juego incrustado), visor.js, como-funciona.frag, img/capturas/<id>/
tools/                  servir.py (vista previa), subir_r2.py (datos de los juegos a R2), og.sh (imágenes OG)
deploy.sh, deploy/      despliegue del sitio: nginx-source.pcfsite.com.conf y remote.sh (lo que corre en el servidor)
deploy.env.example      plantilla de los accesos (deploy.env no se versiona)
servicio/               partidas.py: las partidas guardadas en la cuenta de pcfsite; unidad de systemd, cron, copia
docs/og.html            plantilla de las imágenes para redes
```

Cada carpeta lleva su README: [`core/`](core/README.md), [`core/recomp/`](core/recomp/README.md),
[`core/platform/`](core/platform/README.md), [`core/web/`](core/web/README.md), [`core/juegos/`](core/juegos/README.md),
[`core/tools/`](core/tools/README.md), [`core/ghidra/`](core/ghidra/README.md), [`app/`](app/README.md),
[`deploy/`](deploy/README.md), [`servicio/`](servicio/README.md), [`tools/`](tools/README.md).

## Construir

### Requisitos

- macOS (Apple Silicon es lo probado; el código es portable) o Linux. CMake, Ninja, SDL3 (`brew install cmake ninja sdl3`).
- Python 3 con `capstone` y `pefile` en `core/.venv` (`python3 -m venv core/.venv && core/.venv/bin/pip install capstone pefile`);
  Playwright para las pruebas web.
- Ghidra 12 (Homebrew, `JAVA_HOME` openjdk@21) para analizar los binarios la primera vez.
- Emscripten (`emcmake`) para la web; llvm-mingw y SDL3 cruzada para Windows (los baja `app/win/preparar.sh`).
- La carpeta de trabajo de cada juego, con el CD, la instalación y los binarios (`core/juegos/README.md`). No está en el repo.

### Un juego, de principio a fin

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

### La app de escritorio

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
[`app/CHANGELOG.md`](app/CHANGELOG.md) y se publica como *release* de GitHub con los cuatro paquetes (`.dmg` de Apple Silicon y de
Intel, zip x64, zip ARM64). Los binarios no van al repo.

### El sitio

```bash
python3 build.py                 # → dist/
python3 tools/servir.py 8791     # http://127.0.0.1:8791/ con las mismas rutas que nginx (/pcf7 → pcf7.html)
./deploy.sh                      # el sitio y el servicio de partidas (accesos en deploy.env)
./deploy.sh --jugar              # además los juegos en el navegador compilados en core/build/web-<id>
./deploy.sh --nginx              # además la configuración de nginx
```

## La web: source.pcfsite.com

Es una zona más de [pcfsite.com](https://pcfsite.com), con el diseño «Industry» del foro y la misma barra, pie, tema y
cuenta. `build.py` lee `data/juegos.json` y `site/` y escribe `dist/`: portada con cifras, una ficha por juego (resumen,
capturas reales, qué funciona, qué falta, botón «Jugar» por versión), `/como-funciona` y `/diario` (todos los hitos por
fecha). Las fichas se aíslan con COOP/COEP y cargan el juego desde `/jugar/<id>/` en un `iframe`.

Cada juego en el navegador tiene tres partes:

1. **El programa**: `juego.js` + `juego.wasm` por programa del CD, en `/var/www/source-jugar/<id>/`, con la versión de la
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
las partidas de cada usuario. Detalles en [`deploy/README.md`](deploy/README.md) y
[`core/web/README.md`](core/web/README.md).

## La app de escritorio: «PCFSite - Source»

Un lanzador en C++17 y SDL3 con interfaz propia (biblioteca de carátulas o lista, ficha por juego, Configuración, Acerca
de) que lleva dentro los 36 programas traducidos y **ningún fichero de los juegos**. El usuario aporta su CD; la app hace
lo que en 1998 hacía el instalador, y después arranca el juego nativo. Paso a paso:

1. **Reconocer el CD.** «Añadir juego» pide la imagen ISO (o la carpeta con el contenido del CD, o la carpeta del juego
   ya instalado). La app monta la ISO (`hdiutil` en macOS, `Mount-DiskImage` en Windows, `fuseiso` o `udisksctl` en
   Linux) y compara unos ficheros de su raíz, por tamaño y SHA-256, con las *firmas* de `app/versiones.json`. Un CD puede
   valer para varias versiones (el del 7 sirve para el 7 y el 7 Plus): se pregunta. El 2001 pide además la carpeta
   `ACTUALIZ` con los 13 paquetes de actualización, que se reconocen por tamaño y SHA-256 aunque vengan renombrados.
2. **Preparar** (lo que hacía `INSTALAR.EXE`), en `Documentos/PCFSite - Source/<id>/`:
   - `game/<CARPETA>/`, la instalación (`C:\<CARPETA>`), reproducida con los **guiones del instalador original** que hay en
     el CD (`INST_1xx.NFO`; en el 7, cifrados con XOR): órdenes de copiar fichero, copiar carpeta y crear carpeta, y los
     contenedores `.CPR` (entradas ZIP con deflate) descomprimidos. En el 2001 no hay guiones de Dinamic sino
     InstallShield 6: la app lee sus cabinets (`DATA1.HDR`, `DATAn.CAB`, con el MD5 de cada fichero) y aplica las reglas
     de qué grupo va a dónde. El 4.0 copia la carpeta `FILES`. Aquí es donde el juego escribirá sus partidas.
   - `drive_c/WINDOWS/SYSTEM/`, las DLL compartidas que el instalador copiaba (MFC42, MSVCRT…), sacadas del CD.
   - las **13 actualizaciones del 2001**: cada paquete es un envoltorio PackageForTheWeb (un cabinet de Microsoft con un
     `.RTP` de RTPatch y el parcheador). La app no entiende el formato `.RTP`: extrae el `.RTP` y lo aplica con la propia
     `patchw32.dll` traducida (programa `pcfpatch`), en orden, sin ventana. El resultado es idéntico fichero a fichero a
     una instalación real con las 13 aplicadas.
   - el CD no se copia: la ISO se vuelve a montar al jugar y se le pasa al juego como `D:`. Con una carpeta o con dos
     discos (el 7 Plus mezcla los dos) se copia a `<id>/iso/` solo lo que el juego lee.
   - `instalado.json`: versión, fecha, de dónde salió cada parte y qué binarios faltan.
3. **Reconstruir el programa en memoria.** El ejecutable traducido no lleva `mem.bin` (contendría el juego): sigue la
   receta `imagen.c`, busca cada EXE y DLL en la instalación, en `WINDOWS\SYSTEM` y en el CD, comprueba su SHA-256 y
   coloca sus secciones donde estaban, con las relocaciones, la IAT y los parches encima. Si un fichero no es el esperado
   lo dice y no arranca.
4. **Jugar.** El lanzador arranca el programa traducido como proceso hijo con `PCF7_WORK` apuntando a esa carpeta (en
   Windows, una DLL que carga el propio exe con `--programa`, para que haya un solo ejecutable), oculta su ventana (en
   macOS se retira del Dock) y vuelve a la biblioteca al cerrarse. Los programas hermanos (Base de datos, Historia…) se
   encadenan como en el original.
5. **Mods.** Un mod es una carpeta con ficheros del juego modificados (plantillas, escudos, sonidos, textos, y
   normalmente el EXE). Se importa desde la ficha (zip, rar, 7z o carpeta; la app localiza dentro la carpeta que replica
   la del juego) a `<juego>/mods/<nombre>` y se activa con su casilla; el orden es la prioridad. Al jugar, la plataforma
   recibe las carpetas de los mods activos (`PCF7_CAPAS`) y una carpeta de escritura propia de esa combinación
   (`PCF7_ESCRITURA`, `<juego>/perfiles/<mods>`): lee cada fichero de la primera capa que lo tenga (perfil → mods →
   instalación) y escribe siempre en el perfil, copiando antes lo que abre para modificar. Las partidas con y sin mods
   van aparte y la instalación no se toca. El EXE del mod se aplica sobre la imagen reconstruida: sus diferencias en los
   datos (textos) y en los inmediatos del código (fechas, tamaños, identificadores: los «inmediatos vivos»).
6. **Terminal**, sin ventana, para pruebas y guiones: `--listar`, `--instalar FUENTE… [--version id]`, `--jugar id`,
   `--mod id <zip|carpeta>`, `--mods id [a,b]`.

El código es el mismo para los tres sistemas. Lo que a Windows le falta de POSIX está en `core/platform/src/compat.h` y
`compat_win.cpp`, y Windows se compila desde el Mac con llvm-mingw (`core/cmake/mingw.cmake`). Lo que cambia por sistema:
cómo se montan las ISO, dónde están Documentos y los ajustes, y cómo se lanzan los procesos. Todo el detalle en
[`app/README.md`](app/README.md).

### En Linux

La app se publica compilada para macOS y Windows; para Linux se compila desde el fuente, en una máquina Linux, con el
mismo script que las otras dos. El lanzador se comprueba en cada cambio con GitHub Actions en Ubuntu x86_64 y arm64
(`.github/workflows/linux.yml`); los juegos no se pueden compilar ahí porque su C sale de los binarios del CD.

```bash
# 1) herramientas y cabeceras (Debian/Ubuntu; en otras distribuciones, los paquetes equivalentes)
sudo apt install build-essential cmake ninja-build pkg-config git curl python3-venv \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxtst-dev \
  libxss-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libdbus-1-dev libudev-dev \
  fuseiso libarchive-tools
# 2) SDL3 estática (una vez; el paquete no depende de la SDL3 de la distribución)
app/linux/preparar.sh
# 3) traducir los juegos (hace falta la carpeta de trabajo de cada uno con el CD y los binarios, core/juegos/README.md);
#    o copiar core/build/<id>/gen* de una máquina donde ya estén traducidos
PCF_JUEGO=pcf7 core/.venv/bin/python core/recomp/build.py
# 4) compilar y empaquetar
app/linux/empaquetar.sh            # core/build/linux/PCFSite - Source/ y PCFSite-Source-<versión>-linux-<arch>.tar.gz
# 5) ejecutar
"core/build/linux/PCFSite - Source/pcfsite-source"
"core/build/linux/PCFSite - Source/instalar.sh"   # (opcional) el acceso en el menú de aplicaciones; --quitar lo deshace
```

Qué hace la app en Linux: monta las ISO sin privilegios con `fuseiso` (en `<juego>/cd`) o, si no está, con `udisksctl`
(udisks2, lo trae cualquier escritorio; monta en `/run/media/<usuario>/`), y las desmonta al cerrar el juego; sin
ninguno de los dos avisa, y siempre se puede elegir la carpeta con el contenido del CD en vez de la ISO. Los juegos se
preparan en la carpeta Documentos de XDG (`xdg-user-dir DOCUMENTS`; si no, `~/Documentos` o `~/Documents`) y los ajustes
van en `~/.local/share/pcfsite-source`. Los mods en .rar y .7z se descomprimen con `bsdtar`, `7z` o `unrar`, el que
haya. SDL3 va enlazada estáticamente y carga X11 o Wayland, PulseAudio o PipeWire del sistema en tiempo de ejecución;
los binarios dependen de la glibc de la máquina que compila (para repartirlos, compilar en la distribución más antigua
que se quiera soportar). El paquete lleva `LEEME.txt`, el icono, un `.desktop` y `instalar.sh`, que crea el acceso en
`~/.local/share/applications` apuntando a la carpeta donde esté.

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
completa está en [`core/platform/README.md`](core/platform/README.md). El método de fondo es comparar siempre con el
original: el descompilado dice qué espera el juego de cada API, y la plataforma se arregla hasta dárselo.

## Trabajar en varias ramas

El núcleo es común: un cambio en `core/platform` o `core/recomp` afecta a todos los juegos, así que tras tocarlo se
retraducen y arrancan los nueve. Cada frente grande se ha hecho en una rama con un worktree propio
(`~/Projects/pcfsite-source-<rama>`) y se funde en `main` al terminar. El despliegue del sitio es de la rama que lo
lanza, y `deploy.sh --jugar` solo toca los juegos que trae compilados.

## Participar

- **Fallos y propuestas**: [issues](https://github.com/nuvolacat/pcfsite-source/issues/new/choose), con las
  plantillas (juego, sistema, pantalla, registro). Nunca ficheros de los juegos.
- **Preguntas, instalación, novedades**: el [hilo oficial del foro](https://foro.pcfsite.com/t/526).
- **Código**: [CONTRIBUTING.md](CONTRIBUTING.md) (qué hace falta, el criterio del proyecto, pull requests).
- **Seguridad**: [SECURITY.md](SECURITY.md) (aviso en privado).
- **Convivencia**: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## Licencia

El código de este repositorio es software libre bajo la **GPL-3.0** (`LICENSE`): se puede usar, estudiar, modificar y
redistribuir, con la condición de que las versiones modificadas se publiquen con la misma licencia. Cubre el traductor,
la plataforma, el reproductor web, el lanzador, las herramientas y el sitio. **No cubre los juegos** (su código, sus
datos, sus marcas), que son de sus titulares y no están en el repo, ni el C generado a partir de ellos.

Código de terceros incluido, con sus licencias: `pl_mpeg` (MIT), `stb_truetype` y `stb_image` (dominio público),
TinySoundFont y TinyMidiLoader (MIT), `minimp3` (CC0), el backend de ficheros de WasmFS de Emscripten (MIT), las fuentes
Liberation y Barlow (SIL Open Font License). El lector de InstallShield 6 está reescrito a partir de lo que documenta
unshield; las firmas de las APIs de Windows se generan a partir de los `.spec` de Wine.

Comunidad: [pcfsite.com](https://www.pcfsite.com). Creado por [@jordirubo](https://github.com/jordirubo).
