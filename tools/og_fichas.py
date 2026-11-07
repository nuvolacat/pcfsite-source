#!/usr/bin/env python3
"""Tarjetas para compartir enlaces de source.pcfsite.com: una por ficha, por versión jugable, /diario y /como-funciona.

Quedan en site/pcf/img/og/ y build.py las enlaza con la huella del fichero. Volver a lanzarlo al cambiar data/juegos.json
(hace falta Pillow): core/.venv/bin/python tools/og_fichas.py
"""
import json
import os
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(RAIZ, "tools"))
from tarjeta import guarda, pinta

PCF = os.path.join(RAIZ, "site", "pcf")
BASE, FUENTES, SALIDA = os.path.join(PCF, "img", "og.jpg"), os.path.join(PCF, "fonts"), os.path.join(PCF, "img", "og")


def tarjeta(nombre, *arg, **kw):
    guarda(pinta(BASE, FUENTES, *arg, **kw), os.path.join(SALIDA, nombre + ".jpg"))


def main():
    with open(os.path.join(RAIZ, "data", "juegos.json"), encoding="utf-8") as f:
        juegos = json.load(f)["juegos"]
    captura = lambda j: os.path.join(PCF, "img", "capturas", j["id"], j["portada"] + ".jpg")
    for j in juegos:
        kicker = f'Temporada {j["temporada"]} · {j["anio"]}'
        tarjeta(j["id"], kicker, j["nombre"], j["resumen"], foto=captura(j))
        for x in j.get("versiones", []):
            if x.get("jugar") and x["id"] != j["id"]:
                tarjeta(x["id"], kicker, x["nombre"] if x["nombre"].startswith("PC") else f'{j["nombre"]} · {x["nombre"]}', j["resumen"], foto=captura(j))
    hitos = sorted(((h["fecha"], j, h) for j in juegos for h in j.get("hitos", [])), key=lambda t: t[0])
    _, j, h = hitos[-1]
    tarjeta("diario", "Diario", "Los avances de cada juego", f'{j["nombre"]}: {h["texto"]}', foto=captura(j))
    tarjeta("como-funciona", "Cómo funciona", "Del programa original a una app moderna",
            "Cómo se traducen los PC Fútbol desde su código original, sin emuladores.", foto=captura(juegos[-1]))


if __name__ == "__main__":
    main()
