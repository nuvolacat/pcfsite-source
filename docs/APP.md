# La app de escritorio: «PCFSite - Source»

Vuelta al [README](../README.md).

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
[`app/README.md`](../app/README.md).

## En Linux

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
# 3) traducir los juegos (hace falta la carpeta de trabajo de cada uno con el CD, la instalación y los binarios de la
#    edición soportada, core/juegos/README.md); o copiar core/build/<id>/gen* de una máquina donde ya estén traducidos
python3 -m venv core/.venv && core/.venv/bin/pip install capstone pefile
PCF_JUEGO=pcf7 core/.venv/bin/python core/recomp/build.py
# 4) compilar y empaquetar
app/linux/empaquetar.sh            # core/build/linux/PCFSite - Source/ y PCFSite-Source-<versión>-linux-<arch>.tar.gz
# 5) ejecutar (el lanzador prepara cada juego desde su CD; los ejecutables de core/build/linux/app-<id>/ sueltos usan la
#    carpeta de trabajo, como los de core/build/native-<id>/)
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
