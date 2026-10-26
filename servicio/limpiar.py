#!/usr/bin/env python3
"""Borra las partidas de source.pcfsite.com de quien ha borrado su cuenta del foro (lo que promete pcfsite.com/legal
para las partidas en la cuenta). Igual que /opt/smbportal/limpiar_partidas.py: el foro da los ids de las cuentas
borradas en /api/cuentas-borradas (solo desde la propia máquina). Lo lanza cada noche /etc/cron.d/source-partidas.
También vacía la papelera (DATOS/_papelera, lo que el Admin borra desde Source › Partidas) a los 30 días."""
import json
import os
import shutil
import sys
import time
import urllib.request

DATOS = os.environ.get("PARTIDAS_DATOS", "/var/lib/source-partidas")
FORO = "http://127.0.0.1:8850/api/cuentas-borradas"


PAPELERA_DIAS = 30


def papelera():
    p = os.path.join(DATOS, "_papelera")
    lim = time.time() - PAPELERA_DIAS * 86400
    n = 0
    for nombre in (os.listdir(p) if os.path.isdir(p) else []):
        partes = nombre.rsplit("-", 1)   # <uid>-<juego>-<fecha>
        if len(partes) == 2 and partes[1].isdigit() and int(partes[1]) < lim:
            shutil.rmtree(os.path.join(p, nombre), ignore_errors=True)
            n += 1
    if n:
        print(f"papelera: {n} carpetas de más de {PAPELERA_DIAS} días borradas")


def main():
    papelera()
    rq = urllib.request.Request(FORO, headers={"Host": "foro.pcfsite.com"})
    with urllib.request.urlopen(rq, timeout=20) as r:
        ids = {int(x) for x in json.load(r)["ids"]}
    borradas = 0
    for nombre in os.listdir(DATOS):
        if nombre.isdigit() and int(nombre) in ids:
            shutil.rmtree(os.path.join(DATOS, nombre), ignore_errors=True)
            borradas += 1
    print(f"partidas de {borradas} cuentas borradas ({len(ids)} cuentas borradas en el foro)")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001  (foro caído: lo intenta la noche siguiente)
        print(f"sin limpiar: {e}", file=sys.stderr)
        sys.exit(1)
