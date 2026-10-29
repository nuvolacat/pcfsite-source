# Cómo funciona la traducción

Detalle técnico del núcleo (antes en el README). Vuelta al [README](../README.md).

```
CD original ──► Ghidra (sin interfaz) ──► functions.tsv        ┐
                                                                ├─► recomp/build.py + lift.py ──► C generado (chunk_*.c, imagen.c…)
EXE y DLL (PE 32 bits; LE→PE en el 4.0) ──► linker.py (bases, IAT, parches) ┘                              │
                                                                                                            ▼
                                   platform/ (C++17, SDL3): Win98 + MS-DOS + DirectX ◄──── CMake ──► nativo (macOS, Windows, Linux)
                                                                                                └──► Emscripten: juego.js + juego.wasm
```

1. **Análisis.** [Ghidra](../core/ghidra/README.md) en modo headless define las funciones de cada binario (con un script
   propio que cierra los huecos que deja el análisis automático) y exporta su lista y el descompilado, que solo sirve
   para leerlo al depurar.
2. **Traducción** ([`core/recomp/`](../core/recomp/README.md)). `lift.py` desensambla cada función con Capstone y escribe
   una función C por función x86. La CPU son **registros globales** (`eax`…`edi`, banderas, pila x87, MMX/SSE) y la
   memoria del proceso es un array `MEM` de 512 MB: una dirección x86 es un índice. `call` y `ret` mueven la pila
   emulada; los saltos internos son `goto`; las tablas de salto, `switch`; los prólogos SEH de MSVC y los `catch` se
   reconocen y se convierten en pistas de aterrizaje `setjmp`/`longjmp`. `linker.py` coloca cada módulo en su base,
   resuelve la IAT en tiempo de traducción (una llamada a `kernel32!CreateFileA` es una llamada C directa al stub de
   la plataforma, o a la función de la DLL traducida si es MFC42) y aplica los pocos parches de bytes que hay
   (anticopia del 4.0). Cada línea de C lleva la instrucción original como comentario. Como no es un descompilador,
   la traducción es tan fiel como un emulador, y como se compila con `-O2` corre como código nativo.
3. **Plataforma** ([`core/platform/`](../core/platform/README.md), 38 fuentes, unas 20.000 líneas). Implementa lo que los
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
