#!/usr/bin/env python3
"""Sube una carpeta al bucket R2 de los datos de los juegos (pcfsite-datos, servido en https://datos.pcfsite.com).

  CF_CUENTA=… CF_EMAIL=… CF_KEY=… python3 tools/subir_r2.py CARPETA PREFIJO
  p. ej.: python3 tools/subir_r2.py core/build/web-site/datos pcf7

Sigue los enlaces simbólicos. Solo sube lo que cambia (compara el MD5 con el ETag de R2) y borra del prefijo lo que
ya no está en la carpeta. Las credenciales (clave global de Cloudflare) van por el entorno: no se guardan."""
import concurrent.futures
import hashlib
import json
import os
import sys
import time
import urllib.parse
import urllib.request

CUENTA = os.environ.get("CF_CUENTA", "")   # el id de la cuenta de Cloudflare (va con las credenciales, no en el repo)
BUCKET = "pcfsite-datos"
API = f"https://api.cloudflare.com/client/v4/accounts/{CUENTA}/r2/buckets/{BUCKET}/objects"


def cabeceras():
    if not CUENTA or not os.environ.get("CF_EMAIL") or not os.environ.get("CF_KEY"):
        raise SystemExit("faltan CF_CUENTA, CF_EMAIL o CF_KEY en el entorno (ver tools/README.md)")
    return {"X-Auth-Email": os.environ["CF_EMAIL"], "X-Auth-Key": os.environ["CF_KEY"]}


def pide(metodo, url, datos=None, extra=None):
    req = urllib.request.Request(url, data=datos, method=metodo, headers={**cabeceras(), **(extra or {})})
    for intento in range(10):
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                return r.read()
        except Exception as e:
            if intento == 9:
                raise
            # 429: la API de Cloudflare limita las peticiones; se espera cada vez más
            time.sleep(min(60, 5 * (intento + 1)) if getattr(e, "code", 0) in (429, 500, 502, 503, 504) else 2)


def remotos(prefijo):
    out, cursor = {}, None
    while True:
        q = {"prefix": prefijo + "/", "per_page": "1000"}
        if cursor:
            q["cursor"] = cursor
        d = json.loads(pide("GET", API + "?" + urllib.parse.urlencode(q)))
        for o in d["result"]:
            out[o["key"]] = o.get("etag", "").strip('"')
        cursor = (d.get("result_info") or {}).get("cursor")
        if not cursor or not (d.get("result_info") or {}).get("is_truncated"):
            return out


def locales(carpeta):
    for raiz, dirs, ficheros in os.walk(carpeta, followlinks=True):
        dirs.sort()
        for f in sorted(ficheros):
            if f.startswith("._") or f == ".DS_Store":
                continue
            p = os.path.join(raiz, f)
            yield os.path.relpath(p, carpeta).replace(os.sep, "/"), p


def main():
    carpeta, prefijo = sys.argv[1], sys.argv[2].strip("/")
    hay = remotos(prefijo)
    print(f"en R2: {len(hay)} objetos con {prefijo}/")
    tareas, vistos = [], set()
    for rel, p in locales(carpeta):
        clave = f"{prefijo}/{rel}"
        vistos.add(clave)
        with open(p, "rb") as fh:
            md5 = hashlib.md5(fh.read()).hexdigest()
        if hay.get(clave) != md5:
            tareas.append((clave, p))
    total = sum(os.path.getsize(p) for _, p in tareas)
    print(f"a subir: {len(tareas)} ficheros, {total / 1048576:.0f} MB")

    def sube(t):
        clave, p = t
        texto = p.endswith(".txt")
        extra = {"Content-Type": "text/plain; charset=utf-8" if texto else "application/octet-stream",
                 "Cache-Control": "no-cache" if texto else "public, max-age=86400"}
        with open(p, "rb") as fh:
            pide("PUT", API + "/" + urllib.parse.quote(clave), fh.read(), extra)
        return os.path.getsize(p)

    hecho = 0
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        for i, n in enumerate(ex.map(sube, tareas), 1):
            hecho += n
            if i % 50 == 0 or i == len(tareas):
                print(f"  {i}/{len(tareas)} ({hecho / 1048576:.0f}/{total / 1048576:.0f} MB)", flush=True)
    sobran = [k for k in hay if k not in vistos]
    for k in sobran:
        pide("DELETE", API + "/" + urllib.parse.quote(k))
    print(f"listo; borrados {len(sobran)} que ya no estaban")


if __name__ == "__main__":
    main()
