# recomp — el traductor x86 → C

Convierte los binarios del juego (EXE y DLL, PE de 32 bits) en código C que se compila con la plataforma. No es un
descompilador: no intenta recuperar el programa original, sino reproducir exactamente lo que hace cada instrucción.
El resultado es tan fiel como un emulador y tan rápido como código nativo, porque el C se compila con `-O2` y el
compilador optimiza el flujo real del juego.

```
build.py        orquesta: lee juego.json, carga los módulos (linker), traduce cada función (lift), descubre las
                entradas que Ghidra no vio, escribe build/<id>/gen*/ y mem.bin
lift.py         el lifter: de una función (bytes + tamaño) a C, instrucción a instrucción (Capstone)
linker.py       los módulos: PE relocalizados a sus bases, exportaciones, IAT resuelta, parches de bytes, TLS,
                secciones; da al lifter el destino real de cada `call [IAT]`
juego.py        la ficha del juego (PCF_JUEGO, juegos/<id>/juego.json) y las rutas de salida
winapi_sigs.py  genera platform/gen/winapi_sigs.h a partir de los .spec de Wine: cuántos dwords limpia cada API
```

## Uso

```bash
PCF_JUEGO=pcf2001 .venv/bin/python recomp/build.py                          # el programa principal → build/pcf2001/gen
PCF_JUEGO=pcf2001 PCF7_TARGET=dbasewin.exe .venv/bin/python recomp/build.py # otro programa del CD → gen-dbasewin
```

Necesita el descompilado de Ghidra en `<trabajo>/decomp/<binario>/functions.tsv` (`ghidra/run_headless.sh`) y los
binarios en `<trabajo>/bin/`. Tarda unos minutos (el 2001, con 91.000 funciones: unos 6) y solo reescribe los
ficheros que cambian, así que la compilación posterior es incremental. Si cambia `lift.py` hay que relanzarlo a mano:
ninja no sabe que el C depende de él.

## Qué genera (`build/<id>/gen/`)

| Fichero | Qué es |
|---|---|
| `chunk_XXXX.c` | las funciones traducidas, un trozo por cada 64 KB de direcciones (`f_<dirección>`) |
| `funcs.h` | prototipos de todas las funciones y de los stubs del sistema |
| `ftab.c` | tabla dirección → función para las llamadas indirectas (`icall`), y los trampolines de la IAT |
| `gp.c` | exportaciones de los módulos (`GetProcAddress`) |
| `stubs_default.c` | stubs de APIs que la plataforma aún no implementa: avisan una vez y limpian la pila según su firma |
| `juego.c` | lo propio del juego para la plataforma (id, carpeta, programa principal, lanzador, entorno) |
| `modinfo.c` | los módulos cargados (nombre, base, tamaño, punto de entrada, TLS) |
| `mem.bin` | la imagen inicial de la memoria: los módulos ya relocalizados y con la IAT resuelta, tal como quedan en `MEM` al arrancar; la plataforma la carga y la web la descarga |

## Cómo traduce (`lift.py`)

- **CPU de registros globales**: `eax`…`edi`, `esp`, `ebp`, las banderas (`CF`, `ZF`, `SF`, `OF`, `PF`), la pila x87
  (`ST`, `TOP`, `FPUSW`), MMX/SSE (`mm`/`xmm`) y `DF`, declaradas en `platform/include/rt.h`. Cada función es
  `void f_<addr>(void)` y lee sus argumentos de la pila emulada.
- `call f` → `esp -= 4; wr32(esp, retorno); f_x();`; `ret N` → `esp += 4 + N; return;`. Un `jmp` a otra función
  es una llamada de cola (`f_x(); return;`). Los saltos dentro de la función son `goto L_<addr>`.
- **Llamadas indirectas** (`call [mem]`, `call reg`, vtables): `icall_try(destino)` busca en la tabla `FTAB`; si no
  está, `js_missing` lo apunta en el registro («llamada indirecta a 0x… sin traducir») y la llamada se salta
  limpiando la dirección de retorno.
- **`call [IAT]`**: el enlazador sabe a qué apunta cada entrada de la IAT, así que el C llama directamente a la
  función de la otra DLL traducida o al stub `ext_<dll>_<nombre>` de la plataforma, con la convención de llamada
  correcta. Lo que el juego pide con `GetProcAddress` en tiempo de ejecución va por `gp.c` y trampolines.
- **Tablas de salto** (`jmp [tabla + reg*4]`) → `switch`; las tablas lejanas de `memcpy` de MSVCRT también.
- **SEH y C++**: los prólogos de MSVC (`push manejador; mov eax, fs:[0]` y sus variantes) se reconocen; los `catch` y
  los `__except` son «pistas de aterrizaje» (`setjmp`/`longjmp`) a las que la plataforma salta al despachar una
  excepción (`kernel32.cpp`, `RaiseException`/`RtlUnwind`).
- **x87 completa**, MMX, SSE básico, cadenas (`rep movs/stos/scas/cmps/lods`), `pushf/popf` de 16 y 32 bits,
  `loop/loope/loopne`, `bswap/shld/shrd/rcl/rcr`, `cpuid`, `rdtsc`…
- **MS-DOS** (`dos: true`): `int n` → `dos_int(n)`, `in`/`out` → `port_in`/`port_out`, registros de segmento con base
  (`sbase`), y la ventana de la VGA (`0xA0000-0xBFFFF`) desviada a `vga_rd8`/`vga_wr8`.

Cada línea de C lleva la instrucción original como comentario (`L_21e7f30: /* mov eax, dword ptr fs:[0] */`), que es
la forma de depurar: la función `f_x` del registro se busca en `chunk_*.c` y se lee junto al desensamblado.

Tres detalles que salieron al traducir `patchw32.dll` (RTPatch, código antiguo de 16 bits mezclado) y que valen para
todo: `push r16`/`pop r16` (prefijo 66) mueven 2 bytes de pila, no 4; el epílogo de `__chkstk` (`mov esp, ecx; mov eax,
[eax+4]; jmp eax`) salta a la dirección de retorno y se traduce como `return` (como salto indirecto, el resto del llamante
corría anidado y luego otra vez); y `bt`/`bts`/`btr`/`btc` con el operando en memoria y el bit en un registro direccionan
más allá del operando (`[base + (bit >> 5) * 4]`), que es como `strpbrk` de MSVCRT monta su mapa de 256 bits en la pila.

## Inmediatos vivos (mods)

Los mods de PC Fútbol traen el EXE con valores cambiados dentro del código: `mov word [tabla], 0x14b`, `push 0x162`,
`mov edx, 0x13a` (fechas, tamaños de competición, identificadores); el mod «Actualización 26-27» del 2001 cambia 6.000
inmediatos así y 1.000 bytes de datos. Como el código traducido es el original, esos valores se emiten leyéndolos en
tiempo de ejecución de los bytes de la propia instrucción en `MEM` (`IMM8/16/32`, `IMMS8/16` en `rt.h`; `_imm_vivo` en
`lift.py`) en vez de como constantes de C, solo en `mov`, `push`, `cmp`, `test`, `add`, `sub`, `adc`, `sbb`, `and`,
`or`, `xor` e `imul` con un único inmediato. La app aplica sobre la imagen reconstruida las diferencias del EXE del mod
(`platform/src/imagen.cpp`) y esos valores cambian; los saltos, `ret n`, `int`, desplazamientos de bits y el resto del
código siguen siendo constantes. Coste: una lectura de memoria por inmediato (los bytes del código están en caché).

## Entradas que Ghidra no vio

Ghidra define las funciones que alcanza por llamadas directas; las que solo se llaman por puntero (vtables, tablas,
`atexit`, callbacks) a veces no. `build.py` las descubre por varias vías: punteros a código en las secciones de
datos (vtables), continuaciones de `catch`, manejadores SEH, plantillas de código, los trocitos `int XX; ret` de
Watcom, y **`juegos/<id>/unresolved.txt`**: direcciones que solo se ven llamar en ejecución. El bucle es: ejecutar,
copiar del registro las direcciones de «sin traducir» que sean código real (no punteros basura), añadirlas,
retraducir. `PCF7_UNRESOLVED=<fichero>` hace que la plataforma las vaya apuntando sola.

## Parches de bytes

`parches` en `juego.json` (`{binario: [[dirección, bytes, motivo]]}`) los aplica `linker.py` sobre la imagen antes de
traducir. Se usan lo mínimo: la protección anticopia del 4.0 (`b8 01 00 00 00 c3`: siempre correcta, como en las
reediciones). Todo lo demás se arregla imitando mejor a Windows.

## Firmas de las APIs (`winapi_sigs.py`)

`platform/gen/winapi_sigs.h` dice, para cada API de Windows, cuántos dwords de argumentos lleva y si es stdcall:
con eso `stubs_default.c` limpia bien la pila aunque la API no esté implementada (y avisa), en vez de
descuadrarla en silencio. Se genera a partir de los `.spec` de Wine. Las que no están ahí (controles de
ProQuinielas, etc.) van en `EXTRA_SIGS` de `build.py`.

## Verificar una traducción

El método original del proyecto es el **oráculo diferencial**: ejecutar la misma función en Unicorn con el binario
original y comparar retorno y memoria byte a byte con la traducida (el arnés `oracle/recomp` del proyecto PCFWeb, el antecesor de este núcleo). En la
práctica, los errores del traductor se cazan con las trazas de la plataforma (`PCF7_WATCH`, `PCF7_TRAZA`,
`PCF7_STACKCHECK`) y comparando con el descompilado de Ghidra. Ejemplos ya resueltos que ilustran el tipo de fallo:
`pushf` de 16 bits (`66 9C`) apilado como de 32 (el mezclador del 4.0 leía los punteros de las voces por
desplazamiento desde `esp` y metía ceros: petardazos en cada efecto); el límite de `_follow_flow` tras una tabla de
saltos; `ftst`/`fxam` para el `sqrt` de Watcom y MSVCRT.
