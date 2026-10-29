# core — el núcleo: traductor, plataforma y reproductor web de los PC Fútbol

Aquí vive todo lo común a los juegos: el **traductor** que convierte los binarios x86 originales en C (`recomp/`), la
**plataforma** en C++ sobre SDL3 que imita Windows 98, MS-DOS y DirectX (`platform/`), el **reproductor web**
(`web/`), las herramientas (`tools/`, `ghidra/`) y la **ficha técnica de cada juego** (`juegos/<id>/juego.json`).
Nació como el repo `pcf7` (PC Fútbol 7, octubre de 2026) y se generalizó a toda la serie; `~/Projects/pcf7` es un
enlace a esta carpeta.

Los binarios y datos de los juegos **no están en el repo**: viven fuera, en la carpeta de trabajo de cada juego
(`~/<id>-work`, por ejemplo `~/pcf7-work` o `~/pcf2001src-work`; la dice `juego.json`, `trabajo`, y se puede cambiar
con `PCF7_WORK`). Tampoco está el C generado (`build/`, ignorado): es obra derivada del original.

## Cómo funciona, en una página

1. **Ghidra** analiza cada binario (EXE y DLL del juego, más las DLL de Microsoft que trae el CD: MFC42, MSVCRT…) y
   exporta la lista de funciones (`functions.tsv`) y el descompilado (`decomp_*.c`, solo para leerlo nosotros).
2. **El traductor** (`recomp/build.py` + `lift.py`) desensambla cada función con Capstone y la escribe en C: cada
   instrucción x86 se convierte en una o varias líneas de C que operan sobre una CPU emulada de **registros
   globales** (`eax`…`edi`, banderas, pila x87) y sobre un array `MEM` de 512 MB que es la memoria del proceso
   (una dirección x86 es un índice en `MEM`). Las llamadas entre funciones del juego son llamadas de C; las
   llamadas a Windows (`call [IAT]`) se resuelven en tiempo de traducción (`linker.py`) a una función de otra DLL
   traducida o a un **stub** de la plataforma (`ext_<dll>_<función>`).
3. **La plataforma** (`platform/`) implementa esos stubs: ficheros, ventanas, GDI, DirectDraw, Direct3D (rasterizador
   por software), DirectSound, DirectMusic, DirectShow, registro, COM, hilos… con SDL3 para la ventana, la entrada y
   el audio. Lo que el juego ve es un Windows 98 con un disco C: y un CD en D: (`vfs.cpp`).
4. **CMake** compila el C generado y la plataforma en un ejecutable nativo por programa del CD (`<id>`,
   `<id>-dbasewin`…) y, con Emscripten, en `juego.js` + `juego.wasm` para el navegador.
5. **En el navegador**, los datos se sirven desde R2 por trozos y las partidas se guardan en tres capas
   (`web/README.md`).

Criterio de todo el proyecto: **imitar lo que hace Windows, no parchear el juego**. Si algo falla, primero se
confirma qué hace el original (descompilado, traza) y se arregla en la plataforma o en el traductor; los parches de
bytes al binario (`parches` en `juego.json`) se reservan para lo que no tiene sentido imitar (la protección anticopia
del 4.0, por ejemplo).

## Qué hay en cada carpeta

| Carpeta | Qué es | Más |
|---|---|---|
| `recomp/` | el traductor: `build.py` (orquesta), `lift.py` (x86 → C), `linker.py` (módulos, IAT, relocaciones, parches), `juego.py` (lee `juego.json`), `winapi_sigs.py` (firmas de las APIs de Windows) | [`recomp/README.md`](recomp/README.md) |
| `platform/` | la plataforma C++/SDL3: un fichero por subsistema de Windows o DirectX, más `include/rt.h` (la CPU y la memoria que usa el C generado) y `fonts/` | [`platform/README.md`](platform/README.md) |
| `web/` | la página del juego (`index.html`, `inicio.js`, `partidas.js`) y el backend de ficheros por red de WasmFS | [`web/README.md`](web/README.md) |
| `juegos/` | `README.md` con la tabla de juegos y, por juego, `juego.json` (ficha técnica), `unresolved.txt` y `precarga-caliente.txt` | [`juegos/README.md`](juegos/README.md) |
| `tools/` | instalar desde el CD (`cpr.py`, `instalar_nfo.py`), `le2pe.py` (DOS), `web_datos.py`, `web_servir.py`, `web_probar.py`, `web_guion.py`, `dls2sf2.py` | [`tools/README.md`](tools/README.md) |
| `ghidra/` | `run_headless.sh` y los scripts de Ghidra (`FixGaps`, `ExportAll`…) | [`ghidra/README.md`](ghidra/README.md) |
| `docs/capturas/` | capturas de referencia del original (Windows 98 en QEMU) para comparar | |
| `CMakeLists.txt` | nativo y web; `-DPCF_JUEGO=<id>` elige el juego | |
| `build/` | (ignorado) C generado, compilaciones nativas y web, `web-site/` con los datos listos para publicar | |

## Flujo de trabajo de un juego

Todo se hace con `PCF_JUEGO=<id>` (por defecto `pcf7`). Ejemplo con el 2001:

```bash
cd core
# 0) carpeta de trabajo (ver juegos/README.md): ~/pcf2001src-work/{iso,game/PCF2001,bin,drive_c}
# 1) Ghidra: funciones y descompilado de cada binario → ~/pcf2001src-work/decomp/<binario>/
PCF7_WORK=~/pcf2001src-work ghidra/run_headless.sh pcf2001 ~/pcf2001src-work/bin/manager.exe ~/pcf2001src-work/bin/gfxwin.dll …
# 2) traducir el programa principal (build/pcf2001/gen) y los demás programas del CD (gen-<programa>)
PCF_JUEGO=pcf2001 .venv/bin/python recomp/build.py
PCF_JUEGO=pcf2001 PCF7_TARGET=dbasewin.exe .venv/bin/python recomp/build.py
# 3) nativo
cmake -G Ninja -S . -B build/native-pcf2001 -DPCF_JUEGO=pcf2001 -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-pcf2001 -j8
build/native-pcf2001/pcf2001                      # ventana SDL; registro en ~/pcf2001src-work/pcf2001.log
# 4) web
emcmake cmake -S . -B build/web-pcf2001 -DPCF_JUEGO=pcf2001 -DCMAKE_BUILD_TYPE=Release
cmake --build build/web-pcf2001 -j6
PCF_JUEGO=pcf2001 python3 tools/web_datos.py      # build/pcf2001/web-site/ (datos/, indice.txt, precarga.txt)
PCF_JUEGO=pcf2001 python3 tools/web_servir.py 8822 # prueba local: http://127.0.0.1:8822/
# 5) publicar: ../tools/subir_r2.py (datos a R2) y ../deploy.sh --jugar (programa y página)
```

Para `pcf7` el C va a `build/gen` y la compilación a `build/native` y `build/web` (sin sufijo). `.venv` es un entorno
de Python con `capstone` y `pefile` (`python3 -m venv .venv && .venv/bin/pip install capstone pefile`); en los
worktrees de otras ramas se enlaza al del repo principal.

Si el traductor cambia (`lift.py`), hay que volver a lanzar `build.py`: ninja no sabe que el C generado depende de él.
`build.py` solo reescribe los `chunk_*.c` que cambian, así que la compilación es incremental.

### Pruebas sin ventana y guiones

El ejecutable nativo se puede correr sin ventana, con tiempo límite, capturas periódicas y un guion de entrada, que es
como se recorren los juegos y se sacan las capturas de las fichas:

```bash
PCF7_HEADLESS=1 PCF7_TIMEOUT=60 PCF7_SHOT=/tmp/s PCF7_SHOT_MS=5000 \
PCF7_SCRIPT="12000:click:650:418;20000:char:74;21000:key:13:28" build/native-pcf2001/pcf2001
```

Pasos del guion (`ms:op:a:b[:c]`, milisegundos desde el arranque): `click`/`rclick`/`move`/`down`/`up x y`,
`key vk scan`, `keydown`/`keyup`, `char código`, `shot n`, `wait x y RRGGBB` (espera a que ese píxel tenga ese
color y desplaza los pasos siguientes), `ifpx x y RRGGBB` (el paso siguiente solo si el píxel tiene ese color),
`poke dirección valor`. Cada programa puede llevar el suyo (`PCF7_SCRIPT_DBASEWIN`). La versión web se prueba igual con
`tools/web_guion.py` (Playwright). Todas las variables de diagnóstico están en [`platform/README.md`](platform/README.md).

## Los juegos

| id | Juego | Particularidades |
|---|---|---|
| `pcf4` | PC Fútbol 4.0 (4.1 del CD, MS-DOS) | DOS/4GW: los LE de Watcom se convierten en PE (`tools/le2pe.py`); VGA 640x480x16 planar, Sound Blaster, int 21h (`dos.cpp`, `vga.cpp`, `sb.cpp`) |
| `pcf5`, `pcf5oro`, `pcf5arg` | PC Fútbol 5.0, Edición de Oro y Apertura 97 (Argentina) | Windows de 16 colores/256; menú y programas del CD encadenados por el lanzador |
| `pcf6`, `pcf6oro`, `pcf6arg` | PC Fútbol 6.0, Edición de Oro y Apertura 98 (Argentina) | DirectDraw, simulador 3D por software (Direct3D 5/6), música S3M |
| `pcf7`, `pcf7plus` | PC Fútbol 7 y 7 Plus | la línea base; el Plus usa DirectInput en el partido |
| `pcf2000` | PC Fútbol 2000 + Extensión | motor nuevo (gfxwin/d2s2): todo por Direct3D 7, música MIDI por DirectMusic, panel de ayuda (WebBrowser) |
| `pcf2001` | PC Fútbol 2001 + 13 actualizaciones | el mismo motor; música MP3 por DirectShow (`dshow.cpp`), MSVCP60/MSVCIRT traducidas, online fuera |

La tabla con el estado de cada uno y las notas de instalación están en [`juegos/README.md`](juegos/README.md).

## Lo que conviene saber antes de tocar el núcleo

- **Convención de los stubs.** `STUB(ext_kernel32_dll_CreateFileA) { … RET(valor, n); }`: los argumentos se leen de la
  pila emulada con `arg(i)` y `RET(v, n)` deja `v` en `eax` y limpia la dirección de retorno más `n` dwords (stdcall).
  Las cdecl limpian 0. **En los objetos COM del host, `n` cuenta también el `this`** (`arg(0)`): un `n` de menos deja
  la pila descuadrada 4 bytes y el juego lee basura en los argumentos siguientes, de forma silenciosa y lejos del
  culpable.
- **Objetos COM** (`platform/src/com.h`): `[vtable][refcount][…]` en `MEM`, con la vtable apuntando a funciones del
  host (`host_fn`). Un `QueryInterface` que no conozca la interfaz debe devolver `E_NOINTERFACE` y `*out = 0`.
- **Llamar al juego desde el host**: `guest_call(fn, {args})` (stdcall/cdecl; no toca `ecx`, así que para `__thiscall`
  se pone `ecx = this` antes). Las vtables del juego se leen con `rd32(rd32(obj) + 4 * slot)`.
- **Hilos**: cooperativos (`threads.cpp`). El C generado usa registros globales, así que solo un hilo del invitado corre
  a la vez; `WaitForSingleObject`, `Sleep`… ceden el turno. Un stub que devuelva «listo» sin mirar nada (como hacía
  `WaitForMultipleObjects` hasta el 2001) rompe las máquinas de estado del juego de formas muy raras.
- **Memoria**: EXE en su base (`0x400000`), DLL apiladas desde `0x2000000` (64 KB entre imágenes), pila del hilo
  principal bajo `0x1F00000`, montículo desde `PCF7_HEAP_LO` hasta `0x1C000000`, vistas de ficheros mapeados desde ahí
  hasta el final de `MEM` (512 MB). El TEB está en la dirección 0 (`fs:[x]` lee `MEM[x]`).
- **Una función que Ghidra no vio** aparece en el registro como «llamada indirecta a 0x… sin traducir»: se añade su
  dirección a `juegos/<id>/unresolved.txt` y se retraduce (`recomp/README.md`).
- **Registro**: cada ejecución escribe `~/<trabajo>/<id>.log` (`plog`); `plog_once(clave, …)` para los avisos que se
  repetirían.

## Origen y método (PC Fútbol 7, octubre de 2026)

El proyecto arrancó el 2026-10-17 con PC Fútbol 7 (CD del 28-dic-1998) y el método de PCFWeb (Ghidra → lifter x86→C
→ oráculo diferencial): primero una **línea base** que corre en todas partes (el EXE completo traducido, más MFC42 y
MSVCRT, con la plataforma en C++ sobre SDL3 en lugar de JS), después el resto de juegos sobre el mismo núcleo. En un
día el 7 llegaba al menú; el 18 se jugaba una liga con el simulador 3D; después vinieron el 7 Plus, los dos 6.0, los dos
5.0, el 4.0 (MS-DOS), el 2000 y el 2001. El plan a largo plazo sigue abierto: reconstruir el fuente legible función a
función (clases de RTTI y vtables, estructuras ya conocidas por Houdini) comprobando cada una contra el original con
el oráculo, sustituir MFC por una capa propia y, con el 100 % del fuente, el remaster (resoluciones modernas, recursos
HD, editores).

Formatos de datos (FDI de jugadores y equipos, PKF, textos): ya documentados en el proyecto Houdini (el editor de
los PC Fútbol), no se vuelven a destripar.
