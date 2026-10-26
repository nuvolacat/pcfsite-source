#!/usr/bin/env python3
"""Prepara build/web-site/: la página (web/index.html), pcf7.js/.wasm (build/web) y datos/ con los datos del juego
(enlaces a ~/pcf7-work) y su índice (datos/indice.txt: «d ruta» / «f ruta<TAB>fecha», lo lee platform/src/web.cpp).

  python3 tools/web_datos.py
  python3 tools/web_servir.py      → http://127.0.0.1:8793/
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "recomp"))
import juego as J  # noqa: E402  (PCF_JUEGO, por defecto pcf7)

BUILD = os.path.join(ROOT, "build") if J.ID == "pcf7" else os.path.join(ROOT, "build", J.ID)
OUT = os.path.join(BUILD, "web-site")
WEB = os.path.join(ROOT, "build", "web") if J.ID == "pcf7" else os.path.join(ROOT, "build", "web-" + J.ID)
WORK = J.TRABAJO

# nombre en datos/ → origen (los mismos que usa vfs.cpp en nativo)
DATOS = {
    "game": os.path.join(WORK, "game"),            # instalación: game/FUTBOL7 = C:\FUTBOL7
    "cd": os.path.join(WORK, "iso"),               # D:\ (el CD)
    "c": os.path.join(WORK, "drive_c"),            # resto de C:
    "fonts": os.path.join(ROOT, "platform", "fonts"),
    "mem.bin": os.path.join(J.gen_dir(J.PRINCIPAL.rsplit(".", 1)[0]), "mem.bin"),
}
# los demás programas del CD (gen-<programa>): su imagen de memoria es datos/mem-<programa>.bin
_gens = os.path.dirname(J.gen_dir(J.PRINCIPAL.rsplit(".", 1)[0]))
for _g in sorted(os.listdir(_gens)) if os.path.isdir(_gens) else []:
    if _g.startswith("gen-") and _g != "gen-proquini" and os.path.exists(os.path.join(_gens, _g, "mem.bin")):
        DATOS[f"mem-{_g[4:]}.bin"] = os.path.join(_gens, _g, "mem.bin")

# no se publican: la partida de prueba y lo que el juego deja al usarse (en la web se empieza como recién instalado)
# (juegos/<id>/juego.json, «web»: «excluir» y «cd», la parte del CD que se publica; sin «cd», todo)
EXCLUIR = set(J.CFG.get("web", {}).get("excluir", []))
CD_SOLO = J.CFG.get("web", {}).get("cd")


def enlaza(dst, src):
    if os.path.isdir(dst) and not os.path.islink(dst):
        shutil.rmtree(dst)
    elif os.path.islink(dst) or os.path.exists(dst):
        os.remove(dst)
    os.symlink(src, dst)


def main():
    os.makedirs(os.path.join(OUT, "datos"), exist_ok=True)
    progs = ["juego"] + ["juego-" + n[4:-4] for n in DATOS if n.startswith("mem-")]
    for n in [p + e for p in progs for e in (".js", ".wasm")]:   # juego.js / juego-<programa>.js (CMakeLists.txt)
        src = os.path.join(WEB, n)
        if not os.path.exists(src):
            sys.exit(f"falta {src}: compila antes (emcmake cmake -B build/web && cmake --build build/web)")
        enlaza(os.path.join(OUT, n), src)
    for n in ("index.html", "inicio.js", "partidas.js"):
        enlaza(os.path.join(OUT, n), os.path.join(ROOT, "web", n))
    for viejo in ("pcf7.js", "pcf7.wasm"):   # nombres de antes
        if os.path.islink(os.path.join(OUT, viejo)):
            os.remove(os.path.join(OUT, viejo))
    lineas, nf = [], 0
    for nombre, src in DATOS.items():
        dst = os.path.join(OUT, "datos", nombre)
        if nombre == "cd" and CD_SOLO:   # solo parte del CD: carpeta con enlaces a lo que se publica
            if os.path.islink(dst) or os.path.isfile(dst):
                os.remove(dst)
            elif os.path.isdir(dst):
                shutil.rmtree(dst)
            os.makedirs(dst)
            for parte in CD_SOLO:
                real = next((n for n in os.listdir(src) if n.lower() == parte.lower()), None)
                if real:
                    os.symlink(os.path.join(src, real), os.path.join(dst, real))
            src = dst
        else:
            enlaza(dst, src)
        if os.path.isfile(src):
            lineas.append(f"f {nombre}\t{int(os.path.getmtime(src))}")
            nf += 1
            continue
        lineas.append(f"d {nombre}")
        for dirpath, dirnames, filenames in os.walk(src, followlinks=True):
            dirnames.sort()
            rel = os.path.relpath(dirpath, src)
            base = nombre if rel == "." else f"{nombre}/{rel}"
            for d in dirnames:
                lineas.append(f"d {base}/{d}")
            for f in sorted(filenames):
                if f.startswith("._") or f == ".DS_Store" or f"{base}/{f}" in EXCLUIR:
                    continue
                lineas.append(f"f {base}/{f}\t{int(os.path.getmtime(os.path.join(dirpath, f)))}")
                nf += 1
    with open(os.path.join(OUT, "datos", "indice.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lineas) + "\n")
    print(f"{OUT}: {nf} ficheros en datos/indice.txt")
    precarga(lineas)


# Lo que la página descarga en segundo plano a la caché del navegador (web/inicio.js): primero lo que el juego pide
# al arrancar y en un recorrido típico (juegos/<id>/precarga-caliente.txt, en ese orden), después el resto de la instalación.
# Fuera quedan el CD (salvo lo de la lista) y los apartados que la web no abre.
PRECARGA_NO = (f"game/{J.CARPETA}/PROQUINI/", f"game/{J.CARPETA}/INFOFUT/", f"game/{J.CARPETA}/ACTUALIZ/", "cd/")


def precarga(lineas):
    tam = lambda f: os.path.getsize(os.path.join(OUT, "datos", f))
    ficheros = [l[2:].split("\t")[0] for l in lineas if l.startswith("f ")]
    existe = set(ficheros)
    p = os.path.join(ROOT, "juegos", J.ID, "precarga-caliente.txt")
    caliente = []
    if os.path.exists(p):
        with open(p, encoding="utf-8") as fh:
            caliente = [l.strip() for l in fh if l.strip() and not l.startswith("#")]
    orden = ["mem.bin"] + [f for f in ficheros if f.startswith("fonts/")]
    grandes = [f for f in caliente if f in existe and tam(f) > 50 << 20]   # los muy grandes, al final
    orden += [f for f in caliente if f in existe and f not in grandes]
    vistos = set(orden) | set(grandes)
    orden += [f for f in ficheros if f not in vistos and not f.startswith(PRECARGA_NO)] + grandes
    total = 0
    with open(os.path.join(OUT, "datos", "precarga.txt"), "w", encoding="utf-8") as fh:
        for f in orden:
            n = tam(f)
            total += n
            fh.write(f"{f}\t{n}\n")
    print(f"precarga: {len(orden)} ficheros, {total / 1048576:.0f} MB")


if __name__ == "__main__":
    main()
