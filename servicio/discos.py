"""Deja en DISCOS (/var/cache/source-partidas/discos) los discos originales de juegos.pcfsite.com que necesita pcfsave.py
para convertir las partidas, por trozos de 1 MiB (<disco>/<n>.raw), y comprueba su SHA-256.

Las imágenes enteras (.bin) de R2 piden el token rotativo del portal de juegos (lo lee de su disk_tokens.json, por eso
se ejecuta como root, desde deploy/remote.sh); los discos troceados (sockdrive) son públicos y, si faltan, pcfsave.py
los baja solo, pero se dejan ya aquí para que la primera conversión no dependa de R2. Solo biblioteca estándar.

  python3 discos.py [DISCOS]          (no vuelve a bajar lo que ya está y cuadra)"""
import hashlib
import json
import os
import sys
import urllib.request

R2 = "https://discos.pcfsite.com/"
ORIGEN = "https://juegos.pcfsite.com"
TOKENS = "/opt/smbportal/instance/disk_tokens.json"
TROZO = 1 << 20
# disco → (tamaño en bytes, SHA-256 de esos bytes): los de juegos.pcfsite.com (online-pcfutbol/juegos/site/games.js)
DISCOS_V86 = {
    "pcf7_data": (1073741824, "6c797ff000063cd6652669d55b6fe16e7a30435deb7c64f7fd42ec63cf906742"),
    "pcf5_data": (536870912, "cacc548eb1b4cd80e32722c6a4f627b330438f82c08b639b6f753f566e1cb4a7"),
    "pcf6_data": (751619072, "3f6094c389b84db3c337530b3779691d2aaee2cfef186d7ed94904bd644a77a3"),
    "win98_pcf6arg_data.bin": (429496320, "9f3f1689c2e5d30c436f6184ab98cb0ed599f5b38a573fe91dccc3657fbefff9"),
    "win98_pcf5arg.bin": (523837440, "922bd074b82ab5bb16001b47bf0a212ebf3776414fbb2c659be39832978c8e82"),
    "pcf4_disq.bin": (523837440, "68509e3c206f7c63b775ada7a08eaa88dfae9aab805df6f0cc0e39d1cc328574"),
}


def baja(url, rango=None):
    h = {"Origin": ORIGEN, "User-Agent": "pcfsite-source"}
    if rango:
        h["Range"] = "bytes=%d-%d" % rango
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=120) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return b""
        raise


def trozos(nombre, tam, base):
    d = os.path.join(base, nombre.replace("/", "_"))
    os.makedirs(d, exist_ok=True)
    n = (tam + TROZO - 1) // TROZO
    if nombre.endswith(".bin"):
        token = json.load(open(TOKENS)).get("cur", "")
        url = R2 + nombre + "?k=" + token
        quitados = set()
    else:
        meta = baja(R2 + nombre + "/sockdrive.metaj")
        open(os.path.join(d, "sockdrive.metaj"), "wb").write(meta)
        quitados = set(json.loads(meta).get("dropped_ranges", []))
    for i in range(n):
        f = os.path.join(d, "%d.raw" % i)
        if os.path.exists(f) or i in quitados:
            continue
        buf = baja(url, (i * TROZO, min(tam, (i + 1) * TROZO) - 1)) if nombre.endswith(".bin") else baja(R2 + nombre + "/%d.raw" % i)
        open(f + ".tmp", "wb").write(buf)
        os.replace(f + ".tmp", f)
    return d, n


def suma(d, n, tam):
    h = hashlib.sha256()
    for i in range(n):
        f = os.path.join(d, "%d.raw" % i)
        buf = open(f, "rb").read() if os.path.exists(f) else b""
        h.update(buf.ljust(TROZO, b"\0")[:min(TROZO, tam - i * TROZO)])
    return h.hexdigest()


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "/var/cache/source-partidas/discos"
    mal = 0
    for nombre, (tam, sha) in DISCOS_V86.items():
        d, n = trozos(nombre, tam, base)
        ok = suma(d, n, tam) == sha
        print("%-24s %s" % (nombre, "bien" if ok else "¡NO CUADRA!"))
        if not ok:   # se borra para que la próxima vez se baje de nuevo
            for f in os.listdir(d):
                os.remove(os.path.join(d, f))
            mal += 1
    sys.exit(1 if mal else 0)


if __name__ == "__main__":
    main()
