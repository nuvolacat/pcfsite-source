# ghidra — análisis de los binarios

El traductor necesita saber dónde empieza y acaba cada función de cada binario. Eso lo da Ghidra (12.1.3, de
Homebrew; `JAVA_HOME` openjdk@21), sin interfaz, con dos scripts propios: `FixGaps.java` crea las funciones que el
análisis automático deja sin definir (huecos de código entre funciones), y `ExportAll.java` exporta todo a texto.

```bash
PCF7_WORK=~/pcf2001src-work ghidra/run_headless.sh pcf2001 ~/pcf2001src-work/bin/manager.exe ~/pcf2001src-work/bin/gfxwin.dll …
```

Crea (o reutiliza) el proyecto `<trabajo>/ghidra/proj/<proyecto>`, importa cada binario, lo analiza con hasta 6
núcleos y escribe `<trabajo>/decomp/<binario>/`:

| Fichero | Qué es |
|---|---|
| `functions.tsv` | `entry size name cc signature nrefs`: lo que usa el traductor |
| `imports.tsv`, `strings.tsv` | importaciones y cadenas |
| `decomp_NNN.c` | el descompilado, por lotes (`// ==== <dirección> <nombre>` delante de cada función): para leerlo nosotros al depurar |
| `callgraph.json` | quién llama a quién |

El registro va a `<trabajo>/ghidra/<proyecto>.log`. El 2001 (12 binarios, 8 MB de EXE) tarda unos 20 minutos; el 7, unos 10.

- `run_fast.sh`: igual pero exportando solo `functions.tsv` (`ExportFuncs.java`), mucho más rápido, para retraducir
  tras cambiar un binario.
- `DecompAt.java`: descompila una función suelta por dirección.
- `FixGaps2.java`: variante de `FixGaps` para los casos en que el primero deja huecos.

Lo que Ghidra no ve (funciones alcanzadas solo por puntero) lo descubre el traductor o se apunta a mano en
`juegos/<id>/unresolved.txt` (`recomp/README.md`). Para explorar un binario de forma interactiva está REA (MCP sobre
Ghidra, con `node@24`): referencias cruzadas, pseudocódigo y búsquedas sin abrir la interfaz.
