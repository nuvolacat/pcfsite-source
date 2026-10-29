#!/usr/bin/env python3
"""Sube una carpeta al bucket R2 de los datos de los juegos (pcfsite-datos, servido en https://datos.pcfsite.com).

  CF_CUENTA=… CF_TOKEN=… python3 tools/subir_r2.py CARPETA PREFIJO   (o CF_EMAIL=… CF_KEY=… en vez de CF_TOKEN)
  p. ej.: python3 tools/subir_r2.py core/build/web-site/datos pcf7

Sigue los enlaces simbólicos. Solo sube lo que cambia (compara el MD5 con el ETag de R2) y borra del prefijo lo que
ya no está en la carpeta. Las credenciales (token de API o clave global de Cloudflare) van por el entorno: no se
guardan. Con token, los ficheros de más de GRANDE (la API no acepta tanto de una vez) van por partes por la API S3 de
R2, con las claves que salen del token (id del token y SHA-256 del token)."""
import concurrent.futures
import datetime
import hashlib
import hmac
import re
import json
import os
import sys
import time
import urllib.parse
import urllib.request

CUENTA = os.environ.get("CF_CUENTA", "")   # el id de la cuenta de Cloudflare (va con las credenciales, no en el repo)
BUCKET = "pcfsite-datos"
API = f"https://api.cloudflare.com/client/v4/accounts/{CUENTA}/r2/buckets/{BUCKET}/objects"
S3 = f"https://{CUENTA}.r2.cloudflarestorage.com/{BUCKET}/"
GRANDE = 200 << 20
PARTE = 64 << 20


def cabeceras():
    if CUENTA and os.environ.get("CF_TOKEN"):   # token de API con permiso de R2 (o, si no, el email y la clave global)
        return {"Authorization": "Bearer " + os.environ["CF_TOKEN"]}
    if not CUENTA or not os.environ.get("CF_EMAIL") or not os.environ.get("CF_KEY"):
        raise SystemExit("faltan CF_CUENTA y CF_TOKEN (o CF_EMAIL y CF_KEY) en el entorno (ver tools/README.md)")
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


def s3_claves():
    if not hasattr(s3_claves, "c"):
        tok = os.environ["CF_TOKEN"]
        d = json.loads(pide("GET", f"https://api.cloudflare.com/client/v4/accounts/{CUENTA}/tokens/verify"))
        s3_claves.c = (d["result"]["id"], hashlib.sha256(tok.encode()).hexdigest())
    return s3_claves.c


def s3(metodo, clave, query="", datos=b"", extra=None):
    """Petición a la API S3 de R2 firmada con SigV4 (región «auto»; el cuerpo sin firmar)."""
    ak, sk = s3_claves()
    ahora = datetime.datetime.now(datetime.timezone.utc)
    fecha, dia = ahora.strftime("%Y%m%dT%H%M%SZ"), ahora.strftime("%Y%m%d")
    host = f"{CUENTA}.r2.cloudflarestorage.com"
    ruta = "/" + BUCKET + "/" + urllib.parse.quote(clave)
    cab = {"host": host, "x-amz-content-sha256": "UNSIGNED-PAYLOAD", "x-amz-date": fecha,
           **{k.lower(): v for k, v in (extra or {}).items()}}
    firmadas = ";".join(sorted(cab))
    canon = "\n".join([metodo, ruta, query, "".join(f"{k}:{cab[k]}\n" for k in sorted(cab)), firmadas, "UNSIGNED-PAYLOAD"])
    ambito = f"{dia}/auto/s3/aws4_request"
    texto = "\n".join(["AWS4-HMAC-SHA256", fecha, ambito, hashlib.sha256(canon.encode()).hexdigest()])
    k = ("AWS4" + sk).encode()
    for x in (dia, "auto", "s3", "aws4_request"):
        k = hmac.new(k, x.encode(), hashlib.sha256).digest()
    cab["Authorization"] = (f"AWS4-HMAC-SHA256 Credential={ak}/{ambito}, SignedHeaders={firmadas}, "
                            f"Signature={hmac.new(k, texto.encode(), hashlib.sha256).hexdigest()}")
    del cab["host"]
    req = urllib.request.Request("https://" + host + ruta + ("?" + query if query else ""), data=datos, method=metodo, headers=cab)
    for intento in range(10):
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                return r.read(), r.headers
        except Exception as e:
            if intento == 9:
                raise
            time.sleep(min(60, 5 * (intento + 1)))


def sube_por_partes(clave, p, extra):
    cuerpo, _ = s3("POST", clave, "uploads=", extra=extra)
    uid = re.search(rb"<UploadId>([^<]+)</UploadId>", cuerpo).group(1).decode()
    q = lambda **kw: "&".join(f"{k}={urllib.parse.quote(str(v), safe='')}" for k, v in sorted(kw.items()))
    etags = []
    with open(p, "rb") as fh:
        while trozo := fh.read(PARTE):
            _, h = s3("PUT", clave, q(partNumber=len(etags) + 1, uploadId=uid), trozo)
            etags.append(h["ETag"])
    xml = "<CompleteMultipartUpload>" + "".join(f"<Part><PartNumber>{i}</PartNumber><ETag>{e}</ETag></Part>"
                                                for i, e in enumerate(etags, 1)) + "</CompleteMultipartUpload>"
    s3("POST", clave, q(uploadId=uid), xml.encode())


def etag_local(p):
    """El ETag que tendrá en R2: el MD5, o, si va por partes, el MD5 de los MD5 de las partes y «-n»."""
    with open(p, "rb") as fh:
        if not (os.environ.get("CF_TOKEN") and os.path.getsize(p) > GRANDE):
            return hashlib.md5(fh.read()).hexdigest()
        partes = [hashlib.md5(t).digest() for t in iter(lambda: fh.read(PARTE), b"")]
    return f"{hashlib.md5(b''.join(partes)).hexdigest()}-{len(partes)}"


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
        if hay.get(clave) != etag_local(p):
            tareas.append((clave, p))
    total = sum(os.path.getsize(p) for _, p in tareas)
    print(f"a subir: {len(tareas)} ficheros, {total / 1048576:.0f} MB")

    def sube(t):
        clave, p = t
        texto = p.endswith(".txt")
        extra = {"Content-Type": "text/plain; charset=utf-8" if texto else "application/octet-stream",
                 "Cache-Control": "no-cache" if texto else "public, max-age=86400"}
        if os.environ.get("CF_TOKEN") and os.path.getsize(p) > GRANDE:
            sube_por_partes(clave, p, extra)
            return os.path.getsize(p)
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
