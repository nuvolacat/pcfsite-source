# Juegos

Cada carpeta es un PC Fútbol traducido con el núcleo común (`recomp/` traduce, `platform/` reimplementa Windows y
DirectX, `web/` es el reproductor del navegador). Aquí va solo lo propio de cada juego:

- `juego.json`: la ficha técnica (abajo, clave a clave). Las claves `*_notas` son documentación: lo que se averiguó
  del juego al portarlo (instalación, lanzador, DLL, sonido, CD) y el estado.
- `unresolved.txt`: direcciones (hex, una por línea) de funciones que Ghidra no definió y el juego llama por puntero;
  el traductor las añade como entradas. `unresolved-<programa>.txt` para los demás programas del CD.
- `funciones/<binario>.txt`: las funciones que encontró Ghidra («dirección tamaño [marca]», `tools/funciones.py`). El
  traductor las usa cuando no está la exportación de Ghidra (`decomp/<binario>/functions.tsv`): así se traduce igual
  sin Ghidra. Sin ninguna de las dos, las funciones se descubren al traducir y los cortes salen distintos (no funciona).
- `precarga-caliente.txt`: ficheros que la web descarga primero (ver `tools/web_datos.py`).

Se elige con `PCF_JUEGO=<id>` (por defecto `pcf7`) al traducir (`recomp/build.py`) y con `-DPCF_JUEGO=<id>` en CMake.
El C traducido va a `build/gen*` para `pcf7` y a `build/<id>/gen*` para los demás.

| id | Juego | Estado |
|---|---|---|
| `pcf4` | PC Fútbol 4.0 (versión 4.1 del CD, MS-DOS con DOS/4GW) | completado: Manager, Promanager, Historia, Base de datos; VGA 640x480x16, Sound Blaster; también en el navegador |
| `pcf5` | PC Fútbol 5.0 (dic-1996) | completado, también en el navegador |
| `pcf5oro` | PC Fútbol 5.0 Edición de Oro (oct-1997) | completado, también en el navegador |
| `pcf6` | PC Fútbol 6.0 (dic-1997) | completado, también en el navegador |
| `pcf6oro` | PC Fútbol 6.0 Edición de Oro (nov-1999) | completado, también en el navegador |
| `pcf7` | PC Fútbol 7 (CD del 28-dic-1998) | completado, también en el navegador |
| `pcf7plus` | PC Fútbol 7 Plus (sept-1999) | completado, también en el navegador |
| `pcf2000` | PC Fútbol 2000 + Extensión (dic-1999/sept-2000) | completado: menú, Euromanager, oficina y partido 3D (motor nuevo: DirectX 7); Historia, Base de datos, Seguimiento y FutbolQuiz (DBASEWIN, encadenado con `salida_menu`); música MIDI (DirectMusic, con reverberación) y panel de ayuda; en el navegador, datos en R2 (datos.pcfsite.com/pcf2000) |
| `pcf2001` | PC Fútbol 2001 con las 13 actualizaciones (3.6, jul-2001) | completado: menú, Euromanager (ofertas y contrato), oficina, partido en modo resultado y en 3D, música MP3 por DirectShow (dshow.cpp), partidas; en el navegador, datos en R2 (datos.pcfsite.com/pcf2001) |

| `pcfpatch` | (no es un juego) el parcheador RTPatch de las actualizaciones del 2001: `patchw32.dll` traducida, que la app llama con `PCF7_RTPATCH` (`platform/src/pcfpatch.cpp`) |

## La carpeta de trabajo (`trabajo`, fuera del repo)

```
~/<id>-work/
  iso/            el contenido del CD tal cual (lo que el juego ve como D:)
  game/<CARPETA>/ la instalación (C:\<CARPETA>): lo que el instalador original copiaba, reproducido con las
                  herramientas de tools/ (cpr.py, instalar_nfo.py, unshield) o sacado de una instalación real
  drive_c/        el resto del disco C: (WINDOWS/SYSTEM con las DLL compartidas que copiaba el instalador)
  bin/            los binarios que se traducen (EXE, DLL propias, MFC42/MSVCRT… del CD), en el nombre que usa juego.json
  decomp/<bin>/   la exportación de Ghidra (functions.tsv, imports.tsv, strings.tsv, decomp_*.c, callgraph.json)
  ghidra/proj/    el proyecto de Ghidra
  <id>.log        el registro de la última ejecución (PCF7_LOG lo cambia)
```

Las rutas se pueden cambiar sin tocar `juego.json`: `PCF7_WORK` (toda la carpeta), `PCF7_GAME`, `PCF7_CD`,
`PCF7_DRIVE_C`.

## `juego.json`, clave a clave

| Clave | Qué es |
|---|---|
| `id`, `nombre` | el identificador (carpeta, `PCF_JUEGO`, ejecutable, URL `/jugar/<id>/`) y el nombre |
| `carpeta` | la carpeta de instalación: `C:\<carpeta>` (el directorio de trabajo del juego al arrancar) |
| `trabajo` | la carpeta de trabajo (`~/…`) |
| `principal` | el programa principal (`manager.exe`, `pcfutbol.exe`); su C va a `gen/`, su ejecutable se llama `<id>` |
| `dos` | `true` en los juegos de MS-DOS: activa `dos.cpp`, `vga.cpp`, `sb.cpp` y la traducción de `int`/`in`/`out` |
| `dlls` | `[[nombre, base]]`: las DLL que se traducen con el programa y dónde se cargan (apiladas desde `0x2000000`, alineadas a 64 KB según `SizeOfImage`). El orden es el de carga (`DllMain`) |
| `programas` | los demás programas del CD que se traducen (`PCF7_TARGET=<programa>`): `{}` si usan las mismas DLL que el principal, o `{"carpeta", "apiladas": [...]}` si traen las suyas (ProQuinielas del 7, en Visual Basic) |
| `salida_menu` | dirección donde el menú deja el apartado elegido al salir: el lanzador la lee y arranca el programa que toque |
| `lanzador` | `{apartado: [programa, argumento]}`: la tabla del lanzador original (`PCF7.EXE`, `FUTBOL4.EXE`…); `""` termina |
| `entorno` | variables de entorno propias del juego, además de las de un Windows 98 recién instalado |
| `pila` | (hex, opcional) tamaño de la pila del hilo principal si el juego necesita más |
| `parches` | `{binario: [[dirección, bytes hex, motivo]]}`: parches de bytes al binario antes de traducir (lo mínimo: anticopia) |
| `web.excluir` | ficheros de los datos que no se publican (la partida de prueba, lo que el juego deja al usarse) |
| `web.cd` | la parte del CD que se publica (sin instalador, DirectX, vídeos de la intro…); sin la clave, todo |
| `sha256` | `{binario de bin/: SHA-256}`: los de la edición soportada. `recomp/build.py` se para si el binario es otro (otra edición del CD: necesita su propio `juegos/<id>/`); `PCF7_OTRA_EDICION=1` traduce igualmente |
| `*_notas` | documentación: `instalacion_notas`, `lanzador_notas`, `dlls_notas`, `sonido_notas`, `cd_notas`, `parches_notas`, `origen_notas`, `estado_notas` |

Las bases de las DLL las calcula uno con `SizeOfImage` de cada PE (`pefile`), 64 KB entre imágenes, y las escribe en
`dlls`; el montículo empieza por encima de la última (`PCF7_HEAP_LO`, lo fija CMake).

## Añadir un juego

1. Carpeta de trabajo con el CD, la instalación y los binarios (la instalación se reproduce con las herramientas del
   instalador original: `.CPR`/`.nfo` en los de Dinamic, `unshield` en los InstallShield del 2000 y 2001; los RTPatch del
   2001 los aplica la propia `patchw32.dll` traducida (`juegos/pcfpatch`), verificado contra una instalación real).
2. `juegos/<id>/juego.json` con lo de arriba. Las DLL propias se traducen con el EXE; las de Microsoft del CD (MFC42,
   MSVCRT, MSVCP60, MSVCIRT) también, porque el juego las llama de verdad.
3. Ghidra, traducción, compilación nativa (ver `core/README.md`). Arrancar sin ventana y leer el registro: lo
   primero que sale son las APIs que faltan (`stubs_default.c` avisa de cada una) y las funciones sin traducir.
4. Lo que el juego pide de Windows y no existe va en un fichero propio de la plataforma (`platform/src/<id>.cpp`), para
   no tocar el núcleo si no hace falta; lo que es general (un subsistema entero: DirectShow, DirectMusic) va en el suyo.
5. Versión web, datos (`tools/web_datos.py`, `web.cd`), prueba con `tools/web_guion.py`, R2 y despliegue.
6. Ficha en `data/juegos.json` del sitio con capturas reales, y una fila en esta tabla.
