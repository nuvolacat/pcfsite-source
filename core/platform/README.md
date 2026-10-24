# platform — la plataforma: Windows 98, MS-DOS y DirectX imitados en C++ sobre SDL3

El C que genera el traductor llama a funciones `ext_<dll>_<nombre>` por cada API de Windows que el juego importa, y
lee y escribe la memoria del proceso (`MEM`) directamente. Esta carpeta implementa esas funciones y todo lo que hay
detrás: la memoria y la CPU emuladas, el cargador, los hilos, la pantalla, la entrada, el audio y el sistema de
ficheros. Un mismo código compila nativo (SDL3) y a WebAssembly (Emscripten con su SDL3).

```
include/rt.h          lo que ve el C generado: MEM (512 MB), los registros globales, rd/wr, RET/arg, icall, SEH
src/plat.h            utilidades comunes de la plataforma (plog, heap, guest_call, host_fn, gstr, mapa de memoria)
src/handles.h         la tabla de HANDLE de Windows (ficheros, eventos, mapeos, hilos…)
src/com.h             objetos COM del host en MEM ([vtable][refcount][…])
src/ctlutil.h, win.h, display.h, text.h, cursor.h, threads.h, ddraw.h, dshow.h, reverb.h   interfaces internas
fonts/                Liberation (métricas de Arial, Times y Courier) para el texto de GDI; gm.sf2 (no versionado)
third_party/          stb_truetype, pl_mpeg (MPEG-1 + MP2), tsf/tml (SoundFont y MIDI), minimp3
gen/winapi_sigs.h     firmas de las APIs (lo genera recomp/winapi_sigs.py)
```

## Mapa de los fuentes (`src/`)

| Subsistema | Fichero | Qué hace |
|---|---|---|
| Núcleo | `core.cpp` | `MEM`, registros, `guest_call`/`call_stub`/`host_fn`, montículo (`heap_alloc`), registro (`plog`), trazas y vigilantes, pistas de aterrizaje SEH |
| | `loader.cpp`, `imagen.cpp` | pone `mem.bin` en `MEM` (o, sin él, la reconstruye con los ficheros del juego: `imagen.c`, `sha256.h`), TEB/PEB/TLS/pila, `DllMain` de las DLL y salta al punto de entrada |
| | `main.cpp` | el host SDL3: ventana, escalado, entrada → eventos del juego, capturas, guion de pruebas, lanzador de programas hermanos |
| | `threads.cpp` | `CreateThread` y la multitarea cooperativa: solo un hilo del invitado corre a la vez (registros globales); las esperas ceden el turno |
| | `compat.h`, `compat_win.cpp` | lo que le falta a Windows (llvm-mingw) de POSIX: `pread`/`pwrite`, `futimens`, `setenv`, `mkdir`, `rename` que sustituye, hilos con la pila reservada (no comprometida); el mismo código compila en macOS, Linux y Windows (`core/cmake/mingw.cmake`, ver `app/README.md`) |
| | `vfs.cpp` | rutas de Windows → host: `C:\<carpeta>` (instalación), `D:` (CD), resto de `C:` (disco aislado); en la web, capas `/pcf7/ro` y `/pcf7/rw` |
| | `web.cpp` | solo Emscripten: WasmFS por red (Range), restaurar y persistir las partidas |
| KERNEL32 | `kernel32.cpp` | proceso, módulos (`LoadLibrary`/`GetProcAddress` de lo traducido y de lo del host), memoria virtual, TLS, eventos y esperas, tiempo, locale, SEH (`RaiseException`, `RtlUnwind`), recursos |
| | `kfile.cpp` | ficheros, directorios, unidades, ficheros mapeados, `.INI` |
| | `registry.cpp` | ADVAPI32 `Reg*`: un árbol en memoria guardado en texto (`registro.txt` de la carpeta de trabajo) |
| | `winextra.cpp` | variantes W, ficheros de 16 bits, fechas, perfiles, VERSION, SHELL32 (los usa el runtime de VB de ProQuinielas) |
| USER32 | `user32.cpp` | clases, ventanas, mensajes, ganchos, entrada, temporizadores, cursor, rectángulos |
| | `controls.cpp`, `edit.cpp`, `listbox.cpp`, `menu.cpp`, `msgbox.cpp`, `commctrl.cpp` | los controles estándar (BUTTON, STATIC, SCROLLBAR, EDIT, LISTBOX, COMBOBOX), menús y aceleradores, MessageBox y barra de estado, COMCTL32 |
| GDI | `gdi32.cpp`, `text.cpp` | DC, bitmaps de 8 bits sobre la paleta del sistema, dibujo, paletas, texto con fuentes TrueType libres en monocromo |
| DirectX | `ddraw.cpp` | DirectDraw 1-7: superficies en `MEM`, paletas, Blt/Flip, color alto para el 2000/2001 |
| | `d3d.cpp` | Direct3D 3/5/6/7 por software: dispositivos, viewports, luces, texturas, búferes de vértices; el rasterizador que pinta los partidos 3D (y, en el 2000/2001, todo, menús incluidos) |
| | `dsound.cpp` | DirectSound sobre SDL: búferes en `MEM`, mezcla con remuestreo, volumen, pan, 3D posicional |
| | `dinput.cpp` | DirectInput 5 (teclado y ratón; lo usa el 7 Plus en el partido) |
| | `dmusic.cpp`, `reverb.h` | DirectMusic 7 para la música MIDI del 2000: puertos, Performance, Loader, Segment; sintetizador TinySoundFont con banco GS; reverberación por `IKsControl` |
| | `video.cpp` | DirectShow para los vídeos MPEG-1 de los CD (pl_mpeg): FilterGraph, IMediaControl, IVideoWindow… |
| | `dshow.cpp` | DirectShow para la música MP3 del 2001: el grafo con la fuente IAsyncReader y el renderer PCM del juego, decodificando con minimp3 |
| COM/OLE | `ole32.cpp` | COM en proceso sobre el registro (los OCX de ProQuinielas), BSTR/VARIANT, flujos; reparte las clases propias (FilterGraph, DirectMusic, WebBrowser, filtros MPEG) |
| | `webbrowser.cpp` | control WebBrowser falso para el panel de ayuda del 2000/2001: dibuja el texto de los `.htm` |
| WINMM | `misc.cpp` | tiempo, `mmio`, joystick; COMDLG32 |
| MS-DOS | `dos.cpp`, `vga.cpp`, `sb.cpp` | DOS/4GW e int 21h, el lanzador por `COMFILE.DAT`, la VGA (planos, latches, modo 12h) y la Sound Blaster con su DMA e IRQ |
| Por juego | `pcf2000.cpp`, `pcf2001.cpp` | lo que importa ese juego y no tenía la plataforma (OLEAUT32, mmio de escritura, diálogos de MFC, WinInet, Winsock) |
| Vídeo/entrada | `display.h`, `cursor.cpp` | el framebuffer en `MEM` y la cola de entrada compartidos con el hilo del host; cursores `.CUR` pintados por el host |

## Convenciones

- **Stub**: `STUB(ext_user32_dll_SetWindowTextA) { … RET(v, n); }`. `arg(i)` lee el argumento `i` de la pila emulada,
  `RET(v, n)` devuelve `v` en `eax` y limpia la dirección de retorno más `n` dwords (stdcall; cdecl: 0). `build.py`
  descubre los stubs implementados buscando `STUB(` en estos fuentes; los que faltan van a `stubs_default.c` con la
  firma de `winapi_sigs.h`.
- **COM del host** (`com.h`): `com_vtbl({f1, f2…})` crea la vtable en `MEM` con `host_fn`, `com_object(vt, extra)` el
  objeto; `arg(0)` es `this`. **`RET(v, n)` cuenta el `this`**: `AddRef(this)` es `RET(…, 1)`, `QueryInterface(this,
  iid, out)` es `RET(…, 3)`. `QueryInterface` devuelve `E_NOINTERFACE` y `*out = 0` para lo que no conoce.
- **Llamar al juego**: `guest_call(fn, {args})` apila los argumentos y una dirección de retorno ficticia y entra por la
  tabla de funciones; devuelve `eax`. No toca `ecx` (para `__thiscall`, `ecx = this` antes). `call_stub(f, {args})`
  llama a un stub desde el host como si lo llamara el juego.
- **Memoria**: `rd8/16/32/64`, `wr8/16/32/64` sobre `MEM`; `heap_alloc(n, zero)`/`heap_free` es el montículo del
  host dentro de `MEM` (el de `HeapAlloc`, `GlobalAlloc`, `CoTaskMemAlloc`…; el de `malloc` es el de MSVCRT traducido).
  Mapa: EXE en `0x400000`; DLL desde `0x2000000`; pila del hilo principal bajo `0x1F00000`; montículo desde
  `PCF7_HEAP_LO` (CMake) hasta `0x1C000000`; vistas de ficheros mapeados de ahí a `0x20000000`. TEB en 0.
- **Hilos**: cada hilo del invitado es un hilo del host, pero solo el que tiene «el testigo» ejecuta código del juego.
  Las esperas (`WaitForSingleObject`, `WaitForMultipleObjects`, `Sleep`, secciones críticas) hacen
  `cooperative_sleep` y ceden. Un stub de espera que devuelva «listo» sin comprobar nada rompe la lógica del juego.
- **Pantalla**: el framebuffer es memoria del juego (`MEM[fb]`, `display.h`); el hilo del host lo presenta escalado.
  Los juegos de 8 bits pintan índices sobre la paleta; los de DX7, color alto. La ventana puede ir a pantalla completa.
- **Registro**: `plog("…")` escribe en `<trabajo>/<id>.log` (y en la consola en la web); `plog_once(clave, …)` solo la
  primera vez. `fatal(…)` vuelca el anillo de las últimas funciones, los registros y las pilas de los hilos, y sale.

## Variables de entorno

Rutas y arranque:

| Variable | Qué hace |
|---|---|
| `PCF7_WORK`, `PCF7_GAME`, `PCF7_CD`, `PCF7_DRIVE_C` | la carpeta de trabajo (la app de escritorio la pone en Application Support), la instalación (`C:\<carpeta>`), el CD (`D:`; la app le pasa la ISO montada) y el disco C aislado |
| `PCF7_LOG` | fichero de registro (por defecto `<trabajo>/<id>.log`) |
| `PCF7_MEMBIN`, `PCF7_GENDIR` | otra imagen de memoria / carpeta del C generado. Sin `mem.bin` (la app), la imagen se reconstruye con los ficheros del juego siguiendo la receta `imagen.c` que emite `recomp/build.py` (`imagen.cpp`: busca cada EXE y DLL en la instalación, `WINDOWS\SYSTEM` y el CD, comprueba su SHA-256 y el de la imagen). `PCF7_MEMCHECK=1` reconstruye además de cargar `mem.bin` y compara |
| `PCF7_REGISTRY` | fichero del registro de Windows (por defecto en la carpeta de trabajo) |
| `PCF7_ARGS` | argumentos del programa en los juegos de DOS (por defecto `0 C C`) |
| `PCF7_NOINTRO`, `PCF7_NOLAUNCHER`, `PCF7_RELAUNCHED` | saltar la intro, no encadenar programas, (interno: vuelta del lanzador) |
| `PCF7_FECHA` | fecha fija para el juego (`GetLocalTime`); `PCF7_RELOJ_VIRTUAL=1` reloj determinista (pruebas repetibles) |
| `PCF7_CAPAS=<carpeta>\n<carpeta>…`, `PCF7_ESCRITURA=<carpeta>` | mods (la app): capas por encima de la instalación de `C:\<carpeta del juego>`, de más a menos prioridad, y la carpeta donde se escribe (las partidas de ese perfil; lo que el juego abre para modificar se copia antes allí). Se lee de la primera capa que tenga el fichero; al listar se mezclan. Un EXE o DLL distinto en una capa aplica sobre la imagen reconstruida sus diferencias de datos (textos), nunca de código (`imagen.cpp`) |
| `PCF7_RTPATCH=<ruta Windows del .RTP>` | (programa `pcfpatch`) en vez de arrancar el programa, aplica esa actualización RTPatch llamando a `RTPatchApply32` de la `patchw32.dll` traducida (`pcfpatch.cpp`); devuelve 0 si no hubo errores |

Pruebas sin ventana:

| Variable | Qué hace |
|---|---|
| `PCF7_HEADLESS=1` | sin ventana; `PCF7_MSGBOX=1` para que las cajas de mensaje esperen de verdad |
| `PCF7_TIMEOUT=s` | cerrar a los `s` segundos, volcando dónde estaba el juego |
| `PCF7_SHOT=prefijo`, `PCF7_SHOT_MS=ms` | capturas BMP periódicas (`<prefijo><ms>.bmp` y `<prefijo>final.bmp`) |
| `PCF7_SCRIPT="ms:op:a:b;…"`, `PCF7_SCRIPT_<PROGRAMA>` | guion de entrada (ops en `core/README.md`) |

Diagnóstico (todo opcional; lo habitual es `PCF7_WATCH` y `PCF7_TRACE_FILES`):

| Variable | Qué hace |
|---|---|
| `PCF7_WATCH=dir,dir…` | anota las 5 primeras entradas a esas funciones con `ecx`, argumentos y retornos probables |
| `PCF7_TRAZA=paso:desde:cuantas` | traza de entradas a funciones a partir de un paso, con registros y cadenas |
| `PCF7_TRACE_RANGE=lo-hi` (+ `PCF7_TRACE_LISTA`, `PCF7_TRACE_IMG`, `PCF7_TRACE_ARGMEM`, `PCF7_TRACE_OFF`, `PCF7_TRACE_MAX`) | cada entrada a una función de ese rango, con detalles de sus argumentos (hasta `PCF7_TRACE_MAX`, 400000) |
| `PCF7_TRACE_KFILE=1`, `PCF7_NO_DELETE=1` | traza de las APIs de ficheros (CreateFile, ReadFile, mapeos, SetFileTime…); no borrar ficheros (para ver lo que un programa deja temporalmente) |
| `PCF7_MEMWATCH=dir[,val]`, `PCF7_MEMWATCH_VAL`, `PCF7_MEMWATCH_DESDE`, `PCF7_MEMWATCH_REL` | vigilar escrituras en una dirección (absoluta o relativa a `ecx`) |
| `PCF7_STACKCHECK=1`, `PCF7_SEHCHECK=1` | descuadres de pila por llamada; integridad de la cadena SEH en cada entrada |
| `PCF7_TRACE_FILES=1` | cada fichero que el juego busca, una vez (para elegir qué publicar y precargar en la web) |
| `PCF7_TRACE_SEH`, `PCF7_TRACE_COM`, `PCF7_TRACE_REG`, `PCF7_TRACE_GPA`, `PCF7_TRACE_VMEM`, `PCF7_TRACE_KEYS`, `PCF7_TRACE_CURSOR`, `PCF7_TRACE_MSGBOX` | trazas por subsistema (excepciones, COM, registro, `GetProcAddress`, memoria virtual, teclado, cursor, cajas de mensaje) |
| `PCF7_TRACE_AUDIO`, `PCF7_AUDIO_DUMP=fichero`, `PCF7_TRACE_SB`, `PCF7_SB_DUMP=fichero` | DirectSound y Sound Blaster: traza y volcado del PCM mezclado |
| `PCF7_TRACE_DOS`, `PCF7_TRACE_DOS_DESDE`, `PCF7_VGA_TRAZA`, `PCF7_VGA_TRAZA_DESDE/MAX/TODAS`, `PCF7_VGA_ESTADO` | juegos de DOS: int 21h, puertos de la VGA, estado de la tarjeta |
| `PCF7_TEXTWATCH=texto`, `PCF7_TEXTLOG=1` | quién dibuja ese texto; cada `TextOut` |
| `PCF7_HEAPTRACE=n`, `PCF7_LECTURA_LENTA=ms`, `PCF7_NO_SURF_FREE`, `PCF7_SIN_KS`, `PCF7_SIN_IRQ` | montículo, simular lecturas lentas (como la web), no liberar superficies, sin `IKsControl` (reverberación), sin IRQ de la Sound Blaster |
| `PCF7_FATAL_LISTA`, `PCF7_DUMPMEM`, `PCF7_HASHES`, `PCF7_MEMFIND` | al morir: volcar una lista, la memoria entera, los hashes por bloque (comparar nativo y web), buscar un valor |
| `PCF7_UNRESOLVED=fichero` | ir apuntando las llamadas indirectas sin traducir (para `juegos/<id>/unresolved.txt`) |

## Cómo se depura un fallo

1. Leer el registro: lo último que sale antes del error, y el volcado de `fatal` (anillo de funciones, registros,
   pila por hilo). Las direcciones `f_xxxxxxx` se buscan en `build/<id>/gen/chunk_*.c` (con el desensamblado como
   comentario) y, restando la base de la DLL y sumando su base original, en el descompilado de Ghidra
   (`<trabajo>/decomp/<binario>/decomp_*.c`).
2. Un «sin traducir» → `unresolved.txt`. Un «sin implementar» de `stubs_default.c` → implementar el stub.
3. Un comportamiento raro sin error → comparar con lo que haría Windows: el descompilado de la función que llama al
   stub dice qué espera (valor de retorno, campos de una estructura, orden de llamadas). `PCF7_WATCH` en esa función
   para ver con qué la llaman, `PCF7_MEMWATCH` para ver quién escribe un campo.
4. Para la web, la misma traza sale en la consola del navegador (`?log` la muestra en la página); `PCF7_HASHES` compara
   la memoria entre nativo y web paso a paso si divergen.
