# app — la app de escritorio (macOS, Windows y Linux, el mismo código)

«PCFSite - Source.app»: los PC Fútbol traducidos (`core/`) como programa de escritorio. **No incluye ningún fichero de los
juegos**: al abrirla pide la imagen ISO del CD original (o la carpeta con su contenido, o la carpeta del juego ya
instalado), reconoce qué versión es, prepara la carpeta del juego tal como la dejaba el instalador original y arranca
el juego nativo. Solo acepta las versiones traducidas (hoy, las nueve: 4.0, 5.0, 5.0 Oro, 6.0, 6.0 Oro, 7, 7 Plus,
2000 y 2001); al ampliar la compatibilidad se vuelve a generar `versiones.json` y se publica la app.

## La interfaz

Diseño «Industry» de Source PCFSite (`app/lanzador/ui.*`): fondo claro, un acento azul acero, Barlow Condensed para
títulos y Barlow para el texto (`app/fonts`, OFL), y los objetos como dibujos de plano (esquinas rectas, borde fino,
marcas «+» en las esquinas). La biblioteca es una rejilla de tarjetas con la carátula de cada juego (`app/assets/portadas`,
las de pcfsite.com/historia; las ediciones Oro y Plus llevan su etiqueta encima) y la de «Añadir juego»; un selector cambia a vista de lista (tabla con carátula, año, tamaño y origen; se recuerda en
`ajustes.json`). La ventana (1000×780, redimensionable) está pensada para que los nueve juegos quepan sin desplazamiento.
Al pulsar una tarjeta o una fila se abre la ficha: de dónde salió (ISO o carpeta), fecha, tamaño, carpeta de las partidas, «Jugar» y «Quitar».
«Configuración» permite cambiar la carpeta donde se preparan los juegos nuevos (`ajustes.json` en la carpeta base; los
ya preparados no se mueven y se siguen listando). «Acerca de» lleva la comunidad (www.pcfsite.com) y el autor.

## Versión

`1.AA.MMDD.HHMM` con la hora real del empaquetado, como en pcfsite.com: la pone `mac/empaquetar.sh`, que la escribe en
`VERSION`, en el `Info.plist` y en el lanzador (`-DPCF_VERSION`, se ve en «Acerca de»). Cada versión que se publica
lleva su entrada en `CHANGELOG.md` (si se vuelve a empaquetar sin publicar, la entrada va con la versión que de verdad
se distribuyó).

**Actualizaciones** (`lanzador/actualizar.cpp`): al abrir, en un hilo, `curl` (el de macOS y el de Windows 10 1803 o
posterior) pide `api.github.com/repos/nuvolacat/pcfsite-source/releases/latest`; si su `tag_name` es más nuevo que la
versión de la app, el diálogo ACTUALIZAR lo propone. Al aceptar se baja el paquete de la plataforma (por el final del
nombre: `-macos-apple-silicon.dmg`, `-macos-intel.dmg`, `-windows-x64.zip`, `-windows-arm64.zip`) a
`<base>/actualizacion`, se comprueba su `digest` (SHA-256) y la firma: en macOS `codesign --verify --deep --strict`,
Team ID V9Q7Y5J9R9 y `spctl` (notarizada); en Windows `Get-AuthenticodeSignature` válida y a nombre de Jordi Ruiz Bover.
Un ayudante (`aplicar.sh`; `aplicar.ps1` con su `aplicar.log`) espera a que la app se cierre, pone la nueva en su sitio
(macOS: la `.app` entera, y si falla vuelve la vieja; Windows: `robocopy` encima de la carpeta) y la abre. Si la app
está en una carpeta sin permiso de escritura, avisa y no toca nada. `PCF_ACT_API` cambia la dirección de la consulta
(pruebas con un `file://`); en terminal, `--buscar-actualizacion` y `--actualizar`. Se apaga con el ajuste
`actualizaciones: "no"` (casilla de «Acerca de»).

## PC Fútbol 2001: InstallShield y las 13 actualizaciones

El CD del 2001 no lleva guiones de Dinamic sino InstallShield 6 (`DATA1.HDR`, `DATA1.CAB`, `DATA2.CAB`). `lanzador/is6.cpp`
lee ese formato (reescrito a partir de lo que documenta unshield): los grupos de ficheros, los descriptores, los volúmenes
y los trozos deflate, y comprueba el MD5 de cada fichero. Las reglas (qué grupo va a dónde y qué carpetas del CD se
copian) se dedujeron comparando con una instalación real y están en `versiones.json` (`is6`, `arbol`, `copia`,
`carpeta` para las carpetas vacías que creaba el guion de instalación, `sistema` «is6:Shared DLLs»). Las 13 actualizaciones oficiales (`PCFACT01…12.exe` y `PCFPATCH.EXE`) son envoltorios
PackageForTheWeb: un CAB de Microsoft (`lanzador/mscab.cpp`, MSZIP con `lanzador/inflar.cpp`) con un `.RTP` de RTPatch
(diferencias binarias, formato propietario) y el parcheador (`PCFPATCH.exe` + `patchw32.dll`). La app no entiende el
`.RTP`: traduce `patchw32.dll` con el mismo traductor que los juegos (`core/juegos/pcfpatch`, programa `pcfpatch` de la
app) y la plataforma llama a su `RTPatchApply32` con `PCF7_RTPATCH` (`core/platform/src/pcfpatch.cpp`), sin ventana, una
vez por actualización, en orden. El resultado se verificó fichero a fichero contra la instalación de referencia (la de
un Windows 98 real con las 13 aplicadas): idéntico. Por eso el 2001 pide dos fuentes: la ISO del CD y la carpeta
`ACTUALIZ` (o su ISO) con los 13 paquetes, que se reconocen por tamaño y SHA-256, con el nombre que tengan (circulan
renombrados, «01 - PCFACT01.exe»).

## Mods

Un mod es una carpeta con ficheros del juego modificados (plantillas, escudos, sonidos, textos…), con la misma estructura
que la carpeta del juego. Se importa desde la ficha del juego (zip o carpeta; se localiza dentro la carpeta que replica
la del juego aunque venga con una de más o con el nombre `FUTBOL7`) a `<juego>/mods/<nombre>` y se activa con su
casilla; el orden de la lista es la prioridad. Al jugar, el lanzador pasa a la plataforma las carpetas de los mods
activos (`PCF7_CAPAS`) y una carpeta de escritura propia de esa combinación (`PCF7_ESCRITURA`,
`<juego>/perfiles/<mods>`): el juego lee cada fichero de la primera capa que lo tenga (perfil → mods → instalación) y
escribe siempre en el perfil, copiando antes allí lo que abre para modificar. Así las partidas de cada combinación de
mods van aparte y la instalación no se toca. Los mods de PC Fútbol suelen traer también el EXE con los textos cambiados:
como el programa se reconstruye en memoria a partir del EXE original, la plataforma aplica encima las diferencias del
EXE del mod: los textos (datos) y los valores de las instrucciones (`mov`, `push`, `cmp`… con inmediato, que el
traductor emite leyéndolos de la imagen: «inmediatos vivos», `core/recomp/README.md`); el código en sí no cambia. En la terminal: `--mod <id> <zip|carpeta>`
y `--mods <id> [a,b…]`.

## Cómo funciona

1. **Reconocer**: `versiones.json` lleva, por versión, las *firmas* de cada CD (unos ficheros de su raíz con tamaño y
   SHA-256) y las de una carpeta instalada. El lanzador monta la ISO (`hdiutil`) y comprueba las firmas. Un mismo CD
   puede valer para varias versiones (el de PC Fútbol 7 sirve para el 7 y para el 7 Plus): se pregunta cuál.
2. **Preparar** (lo que en Windows hacía `INSTALAR.EXE`): en `~/Documents/PCFSite - Source/<id>/` (la carpeta
   Documentos del usuario, en macOS y en Windows; se puede cambiar en Configuración, y `ajustes.json` y los puntos de
   montaje van en `~/Library/Application Support/PCFSite - Source`)
   - `game/<CARPETA>/`: la instalación, `C:\<CARPETA>`. Se reproduce con los **guiones del instalador original** que
     están en el CD (`INST_1xx.NFO`; en el 7, `.NFC` cifrados con XOR 0xA5): órdenes `F` (copiar fichero), `X` (copiar
     carpeta) y `D` (crear carpeta); los contenedores `.CPR` (entradas ZIP con deflate) se descomprimen. El 4.0 copia
     la carpeta `FILES` del CD. Qué guiones reproducen exactamente la instalación probada lo decide
     `tools/versiones.py` comparando con las carpetas de trabajo. Aquí es donde el juego escribe sus partidas.
   - el CD (`D:`): si la fuente es una ISO y el juego usa un solo disco, **no se copia**: la app monta la ISO al jugar
     (`<id>/cd`) y se la pasa al juego (`PCF7_CD`). Con una carpeta o con dos discos (el 7 Plus mezcla los dos) se
     copia a `<id>/iso/` lo que el juego lee (`web.cd` de `juego.json`, o todo).
   - `drive_c/WINDOWS/SYSTEM/`: las DLL compartidas que el instalador copiaba (MFC42, MSVCRT…), sacadas del CD.
   - `instalado.json`: versión, fecha, de dónde salió cada parte (`origenes`: la ISO o la carpeta elegida; se enseña en
     la lista) y qué binarios faltan (si falta alguno se avisa).
3. **Jugar**: el lanzador arranca como proceso hijo el programa traducido `<id>` que va junto a él, con `PCF7_WORK`
   apuntando a esa carpeta; mientras tanto oculta su ventana y, cuando el juego se cierra, vuelve a la biblioteca
   (desde la terminal, `--jugar` lo ejecuta con `execv`). Los programas hermanos (`<id>-dbasewin`…) se encadenan como siempre (`core/platform/src/main.cpp`).
4. **La memoria inicial sin `mem.bin`**: en desarrollo el programa carga `build/<gen>/mem.bin` (el EXE y las DLL
   originales ya colocados en memoria). La app no lo lleva: `recomp/build.py` emite además `imagen.c`, una *receta*
   (qué trozos de cada fichero van a qué dirección, tal como los mapea `pefile`, y encima los bytes que cambian:
   relocaciones, IAT resuelta y parches), con el SHA-256 de cada fichero y de la imagen entera. `platform/src/imagen.cpp`
   busca los ficheros en la instalación, `WINDOWS\SYSTEM` y el CD, los comprueba y reconstruye la imagen; si algo no
   coincide, el juego no arranca y dice qué fichero no es el esperado. En el 4.0 los ficheros son los `.DAT` (LE) del
   CD: `tools/le2pe.py` deja `<programa>.le.map.json` con el mapa de páginas para la receta. `PCF7_MEMCHECK=1` compara
   la reconstrucción con `mem.bin` (se ha comprobado idéntica en todos los juegos).

La única versión que aún necesita la carpeta instalada además del CD (`solo_instalada` en `versiones.json`) es el
**2000** (InstallShield 5: CD base y CD Extensión; pendiente de reproducir sus CAB). Sus DLL de sistema (MFC42, MSVCRT,
MSVCP60, MSVCIRT) tampoco vienen sueltas en el CD: el lanzador avisa de cuáles faltan y dónde ponerlas. El **2001** se
prepara desde el CD y la carpeta `ACTUALIZ` (ver más abajo).

## Qué hay

```
versiones.json        las versiones soportadas (lo genera tools/versiones.py desde las carpetas de trabajo ~/<id>-work)
tools/versiones.py    el generador: firmas de cada CD, guiones que reproducen la instalación, binarios y dónde están
lanzador/             el lanzador (C++17, SDL3): main.cpp (pantallas y modo terminal), ui.cpp (texto con stb_truetype
                      y las Liberation, botones), versiones.cpp (manifiesto y reconocimiento), instalar.cpp (guiones,
                      CPR, copias, comprobación de binarios), util.cpp (ficheros, procesos, rutas), json.cpp
                      third_party/stb_image.h: solo su inflate (los .CPR)
CMakeLists.txt        el lanzador
lanzador/inflar.*     deflate propio (ventana persistente para los bloques MSZIP)
lanzador/is6.*        cabinets de InstallShield 6 (DATA1.HDR / DATAn.CAB), con MD5
lanzador/mscab.*      cabinets de Microsoft (MSZIP), también pegados a un EXE (PackageForTheWeb)
lanzador/zip.*        extractor de .zip (los mods)
mac/empaquetar.sh     construye «PCFSite - Source.app» (y el .dmg); mac/Info.plist, mac/entitlements.plist
win/empaquetar.sh     construye la carpeta y el zip de Windows (x64 y ARM64) desde el Mac; win/preparar.sh baja la
                      cadena de herramientas; win/lanzador.rc, manifest.xml, pcfsite.ico (recursos), LEEME.txt
```

## Construir (macOS)

```bash
# 1) traducir cada juego (deja también imagen.c): PCF_JUEGO=<id> [PCF7_TARGET=<programa>] .venv/bin/python core/recomp/build.py
# 2) compilar todo y empaquetar:
app/mac/preparar.sh                   # una vez: SDL3 para arm64 y x86_64 (macOS 12+) en ~/pcfsite-mac
app/mac/empaquetar.sh                 # core/build/mac-<arm64|x86_64>/PCFSite - Source.app (firma ad hoc)
app/mac/empaquetar.sh --dmg --firma "Developer ID Application: …" --notarizar   # para distribuir
app/mac/empaquetar.sh --sin-compilar --juegos "pcf7 pcf6"           # solo empaquetar lo ya compilado
```

Necesita CMake y Ninja (`brew install cmake ninja`). Hay una app por arquitectura, arm64 (Apple Silicon) y x86_64
(Intel), las dos con **macOS 12 como mínimo** (`CMAKE_OSX_DEPLOYMENT_TARGET`, `LSMinimumSystemVersion`): la SDL3 sale de
`app/mac/preparar.sh`, no de Homebrew, porque la de Homebrew es solo arm64 y pide el macOS con que se construyó (con
ella la app pedía macOS 27 y no abría en macOS 26). El script comprueba al final que cada binario es de su arquitectura
y pide macOS 12. Los juegos se compilan en `core/build/mac-<arch>/app-<id>` (uno por juego, `-DPCF_JUEGO=<id>`); la app
lleva `libSDL3.0.dylib` dentro (`Frameworks/`) y las fuentes Liberation en `Resources/fonts/`. La firma ad hoc hace
que Gatekeeper avise la primera vez. Para distribuir se firma con Developer ID (con sello de tiempo y hardened runtime)
y `--notarizar` manda cada `.dmg` a Apple con la clave de la API de App Store Connect de `~/.appstoreconnect/config`
(fuera del repo) y le grapa el tíquet: así se abre sin aviso.

## Construir (Windows, desde el Mac)

```bash
app/win/preparar.sh                   # una vez: llvm-mingw y SDL3 para x86_64 y aarch64, en ~/pcfsite-win
app/win/empaquetar.sh                 # core/build/win-<arch>/PCFSite - Source/ y su zip, para x64 y ARM64
app/win/empaquetar.sh --arch x86_64 --sin-compilar --juegos "pcf7"
VERSION=1.26.1024.1825 app/mac/empaquetar.sh   # la misma versión para los dos sistemas
```

En Windows el único ejecutable es «PCFSite - Source.exe»: cada programa traducido es una DLL en `programas/`
(`<id>.dll`, `<id>-<programa>.dll`, `pcfpatch.dll`; `PCF_DLL` en `core/CMakeLists.txt`, exporta `pcf_main`) que el
lanzador carga en un proceso hijo suyo («PCFSite - Source.exe --programa <nombre> [argumento]», `modo_programa`), y lo
mismo hace la plataforma para pasar de un programa a su hermano (`exec_program`). Así el juego sigue aislado de la app
sin que haya más .exe. Compilación cruzada con [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang + runtime
mingw-w64 con UCRT);
`core/cmake/mingw.cmake` es el fichero de toolchain de CMake (`-DPCF_WIN_ARCH=x86_64|aarch64`). El mismo código
compila para los tres sistemas: lo que Windows no tiene de POSIX (`pread`, `futimens`, `setenv`, `mkdir` con modo,
`rename` que sustituya…) está en `core/platform/src/compat.h` + `compat_win.cpp`, que también usa el lanzador. Lo
que cambia en el lanzador: los procesos (`CreateProcess`, `util::lanzar/esperar`), las ISO (las monta Windows con
`Mount-DiskImage` desde PowerShell, que les da una letra de unidad; no monta imágenes de carpetas de red, así que esas
se copian antes a `%LOCALAPPDATA%\PCFSite - Source\isos`), las rutas (`Documentos\PCFSite - Source` por
`SHGetKnownFolderPath`, ajustes en `%LOCALAPPDATA%\PCFSite - Source`) y los recursos de cada .exe (icono y un
manifiesto con UTF-8 como página de códigos, para que las APIs ANSI acepten rutas con acentos: por eso hace falta
Windows 10 1903 o posterior). Los ejecutables van sin consola (`-mwindows`); el modo terminal del lanzador se engancha a
la consola del padre o escribe en el fichero de `PCF_SALIDA`. Los hilos de pila grande (1 GB el del juego, 256 MB cada
`CreateThread` del invitado) se crean con `STACK_SIZE_PARAM_IS_A_RESERVATION`: con winpthreads se comprometería toda
la pila. Los `_setjmp`/`_longjmp` del código traducido van a los de mingw sin desenrollar la pila
(`__USE_MINGW_SETJMP_NON_SEH`, `rt.h`). En Windows la ISO del CD se desmonta al cerrar el juego. Para distribuir, `app/win/firmar.sh`
firma con Authenticode el `.exe`, `SDL3.dll` y las DLL de `programas/` en una máquina Windows con el certificado de
firma de código (por SSH: `FIRMA_SSH`, `FIRMA_HUELLA`; el certificado en la nube no sale de allí) y rehace el zip.

Desde la terminal, sin ventana (pruebas): `lanzador --listar`, `lanzador --instalar FUENTE… [--version id]`,
`lanzador --jugar id`. En desarrollo, `PCF_EJECUTABLES` dice dónde están los juegos compilados y `PCF_DATOS` cambia la
carpeta de datos.

## Construir (Linux, en Linux)

```bash
app/linux/preparar.sh                 # una vez: SDL3 estática en ~/pcfsite-linux (antes, las cabeceras del sistema: ver el script)
app/linux/empaquetar.sh               # core/build/linux/PCFSite - Source/ y PCFSite-Source-<versión>-linux-<x86_64|arm64>.tar.gz
```

No hay compilación cruzada desde el Mac: se compila en la máquina Linux (x86_64 o arm64), con los juegos ya traducidos
en `core/build/<id>/gen*` (o copiados de donde lo estén). El paquete es una carpeta como la de Windows, con el lanzador
`pcfsite-source`, un ejecutable por programa (como en macOS), SDL3 dentro (estática: carga X11 o Wayland, PulseAudio o
PipeWire del sistema al arrancar), `LEEME.txt`, el icono, un `.desktop` e `instalar.sh` (crea el acceso del menú en
`~/.local/share/applications` apuntando a la carpeta; `--quitar` lo deshace). Lo propio de Linux en el lanzador: las
ISO se montan sin privilegios con `fuseiso` en `<juego>/cd` o, si no está, con `udisksctl` (udisks2: `loop-setup` +
`mount`, en `/run/media/<usuario>/`), apuntando en `<juego>/cd-montado.txt` qué se montó para desmontarlo al cerrar el
juego o al quitarlo; Documentos es la carpeta XDG (`xdg-user-dir DOCUMENTS`, si no `~/Documentos` o `~/Documents`); los
ajustes van en `~/.local/share/pcfsite-source`; los mods .rar/.7z se abren con `bsdtar`, `7z` o `unrar`. El lanzador
se compila en GitHub Actions (`.github/workflows/linux.yml`, Ubuntu x86_64 y arm64) en cada cambio de `app/`; los
juegos no, porque su C sale de los binarios originales. Los binarios dependen de la glibc de la máquina que compila.
