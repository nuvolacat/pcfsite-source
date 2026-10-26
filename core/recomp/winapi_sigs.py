"""Tabla de firmas de las APIs de Windows que importa el juego (nº de dwords en pila y convención).

Fuente: los ficheros `.spec` de Wine (`dlls/<dll>/<dll>.spec`), que declaran cada exportación con sus
tipos (`@ stdcall CreateFileA(str long long ptr long long long)`). Solo se usa el número y el tamaño
de los argumentos: con eso cada stub sabe cuántos bytes limpiar (stdcall) y el despachador puede
fallar de forma ruidosa cuando una API no está implementada, en vez de desequilibrar la pila.

Uso: python3 winapi_sigs.py <carpeta_specs> <os_funcs.txt> > platform/gen/winapi_sigs.h
"""
import os
import re
import sys

SIZES = {"int64": 2, "double": 2, "int128": 4}
LINE = re.compile(r"^@\s+(stdcall|cdecl|varargs|thiscall)\s+(?:-\S+\s+)*([A-Za-z0-9_]+)\s*\(([^)]*)\)")


def load(specdir):
    sigs = {}
    for f in os.listdir(specdir):
        if not f.endswith(".spec"):
            continue
        dll = f[:-5]
        for line in open(os.path.join(specdir, f), encoding="utf-8", errors="replace"):
            m = LINE.match(line)
            if not m:
                continue
            cc, name, args = m.groups()
            n = sum(SIZES.get(a, 1) for a in args.split())
            sigs.setdefault((dll, name), (cc, n))
    return sigs


def main():
    sigs = load(sys.argv[1])
    out, missing = [], []
    for fn in open(sys.argv[2]).read().split():
        m = re.match(r"ext_([a-z0-9]+)_dll_(.+)", fn)
        dll, name = m.group(1), m.group(2)
        s = sigs.get((dll, name)) or sigs.get(("kernelbase", name))
        if s is None:
            missing.append(fn)
            continue
        cc, n = s
        out.append((fn, 0 if cc in ("cdecl", "varargs") else n, n, cc))
    print("/* Generado por recomp/winapi_sigs.py a partir de los .spec de Wine. No editar. */")
    print("/* { stub, dwords que limpia el callee (stdcall), dwords de argumentos, convención } */")
    for fn, clean, n, cc in out:
        print(f'WINAPI_SIG({fn}, {clean}, {n}, "{cc}")')
    for fn in missing:
        print(f"/* SIN FIRMA: {fn} */")
    print(f"{len(out)} firmas, {len(missing)} sin firma: {missing}", file=sys.stderr)


if __name__ == "__main__":
    main()
