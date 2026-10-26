#!/usr/bin/env python3
"""Guarda en juegos/<id>/funciones/<binario>.txt lo que el traductor usa de la exportación de Ghidra
(<trabajo>/decomp/<binario>/functions.tsv): una línea por función, «dirección tamaño [marca]» en hexadecimal, con la
marca C (un Catch@ de C++) o 0 (FUN_ sin referencias). Con esas listas en el repo, recomp/build.py da el mismo C sin
Ghidra; sin ellas (ni Ghidra), las funciones se descubren al traducir y los cortes salen distintos.

  PCF_JUEGO=pcf4 python3 tools/funciones.py
"""
import csv
import glob
import json
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ID = os.environ.get("PCF_JUEGO", "pcf7")
CFG = json.load(open(os.path.join(ROOT, "juegos", ID, "juego.json")))
DEC = os.environ.get("PCF7_DECOMP", os.path.join(os.path.expanduser(CFG["trabajo"]), "decomp"))
SAL = os.path.join(ROOT, "juegos", ID, "funciones")

os.makedirs(SAL, exist_ok=True)
for f in sorted(glob.glob(os.path.join(DEC, "*", "functions.tsv"))):
    out = []
    for r in csv.reader(open(f, encoding="utf-8", errors="replace"), delimiter="\t"):
        if r[0] == "entry":
            continue
        marca = "C" if r[2].startswith("Catch@") else "0" if r[5:] and r[5].strip() == "0" and r[2].startswith("FUN_") else ""
        out.append(f"{r[0]} {int(r[1]):x} {marca}".rstrip())
    nombre = os.path.basename(os.path.dirname(f))
    with open(os.path.join(SAL, nombre + ".txt"), "w") as g:
        g.write("\n".join(out) + "\n")
    print(f"{ID}/{nombre}: {len(out)} funciones")
