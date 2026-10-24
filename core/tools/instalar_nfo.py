#!/usr/bin/env python3
"""Instala un PC Fútbol desde su CD con los guiones del instalador original (los .nfo de la raíz del CD).

  python3 tools/instalar_nfo.py CD DESTINO GUION.nfo [GUION.nfo…]
  p. ej.: python3 tools/instalar_nfo.py ~/pcf6oro-work/iso ~/pcf6oro-work/game/FUTBOL6 Ins6_10{0,1,2,3,4,5,6}.nfo

Formato (cp1252, CRLF), una orden por línea; «*» es comentario:
  F origen [> destino]   copia un fichero del CD (origen relativo al CD; destino relativo a la instalación;
                         sin «>», el mismo camino)
  X origen [> destino]   copia una carpeta entera, mezclándola con la que ya haya
  D carpeta              crea una carpeta vacía
Las rutas del CD no distinguen mayúsculas (el CD es ISO 9660 y la copia puede tener otras). Copia con clones
de APFS cuando se puede (cp -c), así una instalación de cientos de MB no ocupa disco."""
import os
import shutil
import subprocess
import sys


def resuelve(base, rel):
    """Ruta real dentro de base sin distinguir mayúsculas (None si no existe)."""
    p = base
    for parte in rel.replace("\\", "/").split("/"):
        if not parte:
            continue
        try:
            nombres = os.listdir(p)
        except NotADirectoryError:
            return None
        m = next((n for n in nombres if n.lower() == parte.lower()), None)
        if m is None:
            return None
        p = os.path.join(p, m)
    return p


def copia(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if subprocess.run(["cp", "-cR" if os.path.isdir(src) else "-c", src, dst]).returncode != 0:
        (shutil.copytree if os.path.isdir(src) else shutil.copy2)(src, dst)


def main():
    cd, destino, guiones = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(destino, exist_ok=True)
    faltan = 0
    for g in guiones:
        ruta = resuelve(cd, g)
        if not ruta:
            sys.exit(f"no está el guion {g} en {cd}")
        for linea in open(ruta, encoding="cp1252").read().splitlines():
            linea = linea.strip()
            if not linea or linea.startswith("*"):
                continue
            orden, resto = linea[0].upper(), linea[1:].strip()
            if orden == "D":
                os.makedirs(os.path.join(destino, resto.replace("\\", "/")), exist_ok=True)
                continue
            if orden not in "FX":
                print(f"{g}: orden desconocida: {linea}")
                continue
            src, _, dst = (x.strip() for x in resto.partition(">"))
            dst = (dst or src).replace("\\", "/")
            real = resuelve(cd, src)
            if not real:
                print(f"{g}: no está en el CD: {src}")
                faltan += 1
                continue
            final = os.path.join(destino, dst)
            if orden == "X":   # carpeta: se mezcla con la que haya (una actualización solo añade o sustituye ficheros)
                for raiz, _, ficheros in os.walk(real):
                    for f in ficheros:
                        o = os.path.join(raiz, f)
                        d = os.path.join(final, os.path.relpath(o, real))
                        # mismo nombre con otras mayúsculas (el CD es ISO 9660): se sustituye
                        dd = os.path.dirname(d)
                        if os.path.isdir(dd):
                            for n in os.listdir(dd):
                                if n.lower() == f.lower():
                                    os.remove(os.path.join(dd, n))
                        copia(o, d)
                continue
            if os.path.exists(final):
                os.remove(final)
            copia(real, final)
    print(f"{destino}: instalado" + (f" ({faltan} que faltan en el CD)" if faltan else ""))


if __name__ == "__main__":
    main()
