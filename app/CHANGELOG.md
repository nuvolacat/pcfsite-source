# Changelog de la app de escritorio

Versión = `1.AA.MMDD.HHMM` con la hora real del empaquetado (`app/mac/empaquetar.sh`, el mismo esquema que pcfsite.com).
Cada entrada lleva la versión que **de verdad se distribuyó**; dos empaquetados en el mismo minuto van en una sola entrada.

## 1.26.1029.2228 — 29-oct-2026

- **Mods con EXE modificado, ahora en todos los juegos.** En la 1.26.1024.2027 solo se retradujeron con «inmediatos
  vivos» el 2001, el 4.0 y los programas secundarios; el programa principal del 5.0, 5.0 Oro, 6.0, 6.0 Oro, 7, 7 Plus
  y 2000 seguía con los valores del código fijos, y de un `MANAGER.EXE` modificado solo entraban los datos. Ahora los
  siete están retraducidos. Caso real (foro, t/528): el mod de Houdini para el 7 Plus cambia las listas de equipos de
  Inglaterra, Italia, Alemania y Francia del modo Manager; antes salían las de fábrica y ahora las del mod.
- **Mods de otra edición.** La app sabe para qué edición es un mod (por su `pcfsite-mod.json`, que el editor Houdini
  ya mete en sus zip, o por el tamaño de sus EXE y DLL) y no deja importarlo, activarlo ni jugar con él en otra: un
  mod del 7 Plus en el 7 cogía los datos del mod con el EXE de fábrica. Si el EXE no es de ninguna edición, avisa de
  que no se aplicará.
- **Ediciones argentinas.** PC Fútbol 5.0 Apertura 97 y PC Fútbol 6.0 Apertura 98.
- **Linux** (issue #1): el lanzador ya no se cuelga al montar la ISO con `fuseiso`, reutiliza un montaje solo si es de
  la misma ISO y desmonta los que quedaron de otra vez; el 4.0 deja de relanzarse en bucle si un programa termina sin
  dejar `COMFILE.DAT`.
- El registro del juego ya no repite «8253: temporizador» si el valor no cambia.
- **Se actualiza sola.** Al abrirse mira si hay una versión nueva en GitHub y propone actualizar: la baja, comprueba que
  es la oficial y está firmada, se cierra y se vuelve a abrir ya actualizada (los juegos, partidas y mods no se tocan).
  En «Acerca de», el botón «Buscar actualizaciones» y la casilla para no mirarlo al abrir. Esta es la última versión
  que hay que bajar a mano.
- **Firmadas.** macOS: con Developer ID y notarizada por Apple (se abre sin «Abrir de todas formas»). Windows: el
  ejecutable y las DLL con certificado de firma de código (Authenticode).

## 1.26.1025.1529 — 25-oct-2026

- **macOS Intel.** Además de la de Apple Silicon hay una app para los Mac con procesador Intel (x86_64), con su propio
  `.dmg` (`…-macos-intel.dmg`; el de Apple Silicon pasa a `…-macos-apple-silicon.dmg`).
- **macOS 12 o posterior.** Las versiones anteriores pedían macOS 27 sin querer (se compilaban con el SDK y la SDL3 de
  Homebrew del Mac que las construía) y no abrían en macOS 26 («no es compatible»). Ahora todo, SDL3 incluida, se
  compila para macOS 12 (`app/mac/preparar.sh`) y el empaquetado comprueba la arquitectura y el mínimo de cada binario.
- Windows: los mismos juegos, con el lanzador recompilado con la versión nueva.

## 1.26.1025.0103 — 25-oct-2026

- **Linux.** El lanzador monta las ISO sin privilegios (`fuseiso`, o `udisksctl` de udisks2) y las desmonta al cerrar el
  juego; Documentos por XDG; los mods .rar/.7z con `bsdtar`, `7z` o `unrar`. Scripts `app/linux/preparar.sh` (SDL3
  estática) y `app/linux/empaquetar.sh` (carpeta y tar.gz con icono, `.desktop` e `instalar.sh`), y una compilación del
  lanzador en GitHub Actions (Ubuntu x86_64 y arm64) en cada cambio. No se publica paquete de Linux: se compila en Linux
  desde el fuente (README).
- macOS: el `.dmg` en formato UDZO (el `hdiutil` de macOS 27 ya no crea ULZO, y el script fallaba al hacerlo).
- Los paquetes de macOS y Windows de esta versión son los de la 1.26.1024.2027 con el lanzador recompilado (los juegos
  no cambian).

## 1.26.1024.2027 — 24-oct-2026

- **Mods con EXE modificado, de verdad.** Los mods de PC Fútbol cambian dentro del EXE valores del juego (fechas,
  tamaños de competición, identificadores: `mov [tabla], valor`, `push valor`…), no solo textos. Ahora el programa
  traducido lee esos valores de la imagen en memoria («inmediatos vivos»), así que el EXE de un mod los cambia igual que
  los textos. Probado con «Actualización 26-27» del 2001: 1.003 bytes de datos y 7.202 de código aplicados.
- Mods en .rar y .7z (con el tar del sistema) además de zip y carpeta.
- macOS: mientras corre el juego la app se retira del Dock, así no hay dos iconos (el del juego es el mismo).

## 1.26.1024.2011 — 24-oct-2026

- **Mods.** En la ficha de cada juego: importar un mod (zip o carpeta con ficheros del juego modificados: plantillas,
  escudos, sonidos, textos…), activarlo con su casilla y ordenarlos (el primero manda). Al jugar, los ficheros se leen
  primero de los mods activos y, si no están, de la instalación; lo que el juego escribe (partidas, ajustes) va a un
  perfil propio de esa combinación de mods, así que las partidas con y sin mods van aparte y la instalación no se toca.
  Los EXE y DLL modificados de los mods (los textos) también se aplican: sus diferencias en los datos se ponen sobre el
  programa reconstruido en memoria; las de código se ignoran. En la terminal, `--mod` y `--mods`.

## 1.26.1024.1920 — 24-oct-2026

- **Windows: un solo ejecutable.** Los programas traducidos ya no son 36 .exe sueltos: cada uno es una DLL en
  `programas/` y «PCFSite - Source.exe» es el único ejecutable (carga la DLL del programa en un proceso hijo suyo,
  `--programa <nombre>`). Sustituye a la 1.26.1024.1825.

## 1.26.1024.1825 — 24-oct-2026

- **Windows** (x64 y ARM64): la misma app, compilada desde el Mac con llvm-mingw (`app/win/empaquetar.sh`), como
  carpeta con zip. Las ISO las monta Windows (`Mount-DiskImage`); las que estén en carpetas de red se copian antes a
  `AppData\Local\PCFSite - Source\isos`. Juegos en `Documentos\PCFSite - Source`. Hace falta Windows 10 1903 o
  posterior. Sin firma de código todavía (SmartScreen avisa).
- Para los tres sistemas: `compat.h` (lo que Windows no tiene de POSIX) y los hilos de pila grande con la pila solo
  reservada.

## 1.26.1024.1741 — 24-oct-2026

- Los 13 paquetes de actualización del 2001 se reconocen por tamaño y SHA-256 con el nombre que tengan (en las ISO que
  circulan van como «01 - PCFACT01.exe», «05 - PCFPATCH.EXE»…); antes solo valían con el nombre original.

## 1.26.1024.1737 — 24-oct-2026

- **PC Fútbol 2001 desde el CD y las 13 actualizaciones oficiales**: ya no hace falta la carpeta instalada. La app pide
  la ISO del CD y la carpeta `ACTUALIZ` (o su ISO) con `PCFACT01…12.exe` y `PCFPATCH.EXE`, reproduce la instalación de
  InstallShield 6 (lector propio, con el MD5 de cada fichero), extrae de cada actualización su `.RTP` y lo aplica con el
  parcheador original (`patchw32.dll`, RTPatch) traducido con el mismo traductor que los juegos. El resultado es
  idéntico, fichero a fichero, a una instalación real con las 13 aplicadas (versión 3.6).
- Diálogo de fuentes: las rutas largas se parten en las barras en vez de salirse del cuadro.
- Las carpetas vacías que creaba el instalador (`Goles`, `Notas`, `Save`) se crean también.
- Traductor y plataforma (afecta a todos los juegos, arranque comprobado en los nueve): `push`/`pop` de 16 bits,
  epílogo de `__chkstk`, `bt`/`bts`/`btr`/`btc` con el bit fuera del operando; vistas de fichero de escritura
  (`MapViewOfFile`) que se vuelcan al disco, `SetFileTime`, `MoveFileEx`.

## 1.26.1024.1619 — 24-oct-2026

Primera versión de «PCFSite - Source» para macOS.

- **Sin ficheros originales**: la app pide la imagen ISO del CD (o la carpeta con su contenido, o la del juego ya
  instalado), reconoce la versión por las firmas de `versiones.json` y prepara la carpeta del juego reproduciendo el
  instalador original (guiones `.NFO`/`.NFC`, contenedores `.CPR`). Los EXE y DLL se reconstruyen en memoria con la receta
  `imagen.c` a partir de esos mismos ficheros. La ISO se queda donde está: se monta como el CD al jugar.
- **Nueve versiones aceptadas**: PC Fútbol 4.0, 5.0, 5.0 Edición de Oro, 6.0, 6.0 Edición de Oro, 7, 7 Plus, 2000 y 2001
  (2000 y 2001 necesitan además la carpeta instalada).
- **Diseño «Industry»** (Source PCFSite): biblioteca en rejilla con las carátulas de pcfsite.com/historia (etiqueta
  «Edición de Oro»/«Plus»), vista de lista, ficha de cada juego (de dónde salió, fecha, tamaño, carpeta de las partidas,
  Jugar y Quitar), «Añadir juego», Configuración (carpeta de instalación, espacio libre) y «Acerca de» (comunidad
  www.pcfsite.com, creada por @jordirubo).
- Los juegos se preparan por defecto en `Documentos/PCFSite - Source`; los ajustes van en
  `~/Library/Application Support/PCFSite - Source`.
- El juego corre como proceso hijo: el lanzador se oculta y vuelve a la biblioteca al cerrarse. Todas las ventanas
  (lanzador y juegos) se abren centradas y en primer plano.
- Ventana 1000×780 redimensionable (los nueve juegos sin desplazamiento); versión `1.AA.MMDD.HHMM` en `VERSION`,
  `Info.plist` y «Acerca de».
