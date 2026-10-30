"""Partidas de juegos.pcfsite.com (fichero <clave>.pcfsave de «Mis partidas») → copia de partidas de Source.

En juegos.pcfsite.com los PC Fútbol corren en un Windows 98 (o un MS-DOS) emulado, y la partida no son ficheros sino
los trozos del disco duro emulado que el juego ha escrito. Para sacar los ficheros se ponen esos trozos encima del
disco ORIGINAL (el mismo que usa juegos.pcfsite.com), se lee el FAT y se copian los ficheros de la carpeta del juego
que han cambiado, con las rutas de Source (game/<CARPETA>/…). Hay dos formatos:

- v86 (4.0, 5.0, 6.0, 7, 5.0 y 6.0 Apertura; clave v86_<juego> o legacy_<juego>): "DMSV2" + u32 n + n × u32 (nº de
  bloque de 256 B del disco) + n × 256 B. El disco es el de datos de la máquina v86 (o el C: en el 4.0 y el 5.0
  Apertura), ver online-pcfutbol/juegos/site/games.js.
- js-dos (2000 y 2001; y el 4.0-7 de los primeros días): encodeSockdriveChanges, repetido {u32 largo, URL del disco,
  u32 largo, bloque}; bloque = u32 nº de trozos + por trozo {u32 largo, datos}; trozo = [u32 nº de sector][512 bytes],
  tal cual si mide 516 o comprimido en LZ4 (bloque) si no. El 4.0 de js-dos guardaba un zip con los ficheros.

Los discos originales se leen por trozos de 1 MiB de la carpeta DISCOS (los deja discos.py, que lanza el despliegue) y,
si falta un trozo de un disco troceado (sockdrive), de R2. Solo biblioteca estándar.

Probado (30-oct-2026) con todas las partidas de las cuentas de juegos.pcfsite.com y, de una muestra de cada juego,
cargándolas en el juego traducido."""
import base64
import gzip
import io
import json
import os
import struct
import time
import urllib.error
import urllib.request
import zipfile

SECTOR = 512
BLOQUE_V86 = 256
TROZO = 1 << 20
R2 = "https://discos.pcfsite.com/"
ORIGEN_R2 = "https://juegos.pcfsite.com"   # la regla de Origin de R2 deja pasar este origen
DISCOS = os.environ.get("PARTIDAS_DISCOS", "/var/cache/source-partidas/discos")
MAX_TOTAL = 256 << 20   # de ficheros de partida (sin comprimir)
# no se llevan nunca: el código y los ajustes del Windows emulado (música, pantalla, tarjeta de sonido del 4.0…), que
# en Source son los suyos
NUNCA = (".EXE", ".DLL", ".COM", ".BAT", ".PIF", ".LNK", ".SCR", ".VXD", ".DRV", ".INI", "SETUP.INF")

# juego de Source → de dónde vienen sus partidas en juegos.pcfsite.com:
#   v86: (clave del juego en juegos, disco donde escribe la máquina v86, carpeta del juego en ese disco)
#   jsdos: {disco de js-dos: carpeta del juego en ese disco}
#   carpeta: la carpeta del juego en Source (game/<carpeta>/…)
JUEGOS = {
    "pcf7plus": {"v86": ("pcf7", "pcf7_data", "FUTBOL7"), "jsdos": {"pcf7_data": "FUTBOL7"}, "carpeta": "FUTBOL7"},
    "pcf5oro": {"v86": ("pcf5", "pcf5_data", "FUT5ORO"), "jsdos": {"pcf5_data": "FUT5ORO"}, "carpeta": "FUTBOL5"},
    "pcf6oro": {"v86": ("pcf6", "pcf6_data", "FUTBOL6"), "jsdos": {"pcf6_data": "FUTBOL6"}, "carpeta": "FUTBOL6"},
    "pcf6arg": {"v86": ("pcf6arg", "win98_pcf6arg_data.bin", "PCF6A"), "jsdos": {}, "carpeta": "APERTURA98"},
    "pcf5arg": {"v86": ("pcf5arg", "win98_pcf5arg.bin", "PCF5A"), "jsdos": {}, "carpeta": "APERTURA97"},
    "pcf4": {"v86": ("pcf4", "pcf4_disq.bin", "FUTBOL4"), "zip": "FUTBOL4", "jsdos": {}, "carpeta": "FUTBOL4"},
    # del 2000 y el 2001, solo las partidas (save; saveext en la Extensión del 2000) y las tácticas: el resto que
    # cambia en ese disco es del editor, de las opciones del menú (MainOpt.dat) y de Windows
    "pcf2001": {"jsdos": {"pcf2001": "PCF2001"}, "carpeta": "PCF2001", "solo": ("save", "tactics")},
    "pcf2000": {"jsdos": {"pcf2000": "PCF2000"}, "carpeta": "PCF2000", "solo": ("save", "saveext", "tactics")},
}
# tamaño de las imágenes enteras (las troceadas lo dicen en su sockdrive.metaj)
TAM_BIN = {"win98_pcf6arg_data.bin": 429496320, "win98_pcf5arg.bin": 523837440, "pcf4_disq.bin": 523837440}
# la clave de juegos.pcfsite.com → el juego de Source donde se carga (para decir a dónde llevarla si no es aquí)
DESTINO = {"pcf7": "pcf7plus", "pcf5": "pcf5oro", "pcf6": "pcf6oro", "pcf6arg": "pcf6arg", "pcf5arg": "pcf5arg",
           "pcf4": "pcf4", "pcf2000": "pcf2000", "pcf2001": "pcf2001"}
NOMBRES = {"pcf7plus": "PC Fútbol 7 Plus", "pcf5oro": "PC Fútbol 5.0 Edición de Oro",
           "pcf6oro": "PC Fútbol 6.0 Edición de Oro", "pcf6arg": "PC Fútbol 6.0 Apertura 98",
           "pcf5arg": "PC Fútbol 5.0 Apertura 97", "pcf4": "PC Fútbol 4.0", "pcf2000": "PC Fútbol 2000",
           "pcf2001": "PC Fútbol 2001"}


class Error(Exception):
    pass


def _u16(b, o):
    return b[o] | b[o + 1] << 8


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def lz4_bloque(src, tam):
    out, i, n = bytearray(), 0, len(src)
    while i < n:
        t = src[i]
        i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | src[i + 1] << 8
        i += 2
        ml = t & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        ini = len(out) - off
        if off == 0 or ini < 0:
            raise Error("LZ4 roto")
        for k in range(ml):
            out.append(out[ini + k])
    if len(out) != tam:
        raise Error("LZ4: tamaño")
    return bytes(out)


VALIDO = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 !#$%&'()-@^_`{}~.+,;=[]") | set("ñÑçÇáéíóúÁÉÍÓÚàèìòùÀÈÌÒÙäëïöüÄËÏÖÜ")


def _nombre_valido(rel):
    return all(p and p.strip() and p not in (".", "..") and set(p) <= VALIDO for p in rel.split("/"))


# ------------------------------------------------------------------ el disco original
class Base:
    """Disco original de solo lectura, por trozos de 1 MiB: DISCOS/<nombre>/<n>.raw y, si falta y el disco es de los
    troceados de R2 (sockdrive), R2. Las imágenes enteras (.bin) solo se leen de DISCOS (en R2 piden un token)."""

    def __init__(self, nombre):
        self.nombre = nombre
        self.dir = os.path.join(DISCOS, nombre.replace("/", "_"))
        self.cache = {}
        self.dropped = set()
        self.sectores = TAM_BIN.get(nombre, 0) // SECTOR
        if not nombre.endswith(".bin"):
            meta = os.path.join(self.dir, "sockdrive.metaj")
            raw = open(meta, "rb").read() if os.path.exists(meta) else self._r2("sockdrive.metaj")
            m = json.loads(raw)
            if m.get("ahead_read") != TROZO:
                raise Error("disco original con trozos raros")
            self.dropped = set(m.get("dropped_ranges", []))
            self.sectores = m["range_count"] * TROZO // SECTOR

    def _r2(self, f):
        req = urllib.request.Request(R2 + self.nombre + "/" + f, headers={"Origin": ORIGEN_R2, "User-Agent": "pcfsite-source"})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return b""
            raise

    def trozo(self, n):
        if n not in self.cache:
            f = os.path.join(self.dir, "%d.raw" % n)
            if os.path.exists(f):
                buf = open(f, "rb").read()
            elif n in self.dropped:
                buf = b""
            elif self.nombre.endswith(".bin"):
                raise Error("falta el disco original %s en el servidor" % self.nombre)
            else:
                buf = self._r2("%d.raw" % n)
                try:   # se queda en DISCOS para la próxima (el disco original no cambia)
                    os.makedirs(self.dir, exist_ok=True)
                    tmp = "%s.%d.tmp" % (f, os.getpid())
                    with open(tmp, "wb") as o:
                        o.write(buf)
                    os.replace(tmp, f)
                except OSError:
                    pass
            if len(self.cache) > 24:   # el servicio tiene la memoria limitada (source-partidas.service)
                self.cache.clear()
            self.cache[n] = buf.ljust(TROZO, b"\0")
        return self.cache[n]

    def sector(self, s):
        r, o = divmod(s * SECTOR, TROZO)
        return self.trozo(r)[o:o + SECTOR]


class Disco:
    """El disco original con la partida encima. `parche(s)` da el sector s de la partida (o None si no lo toca)."""

    def __init__(self, base, parche, tocados):
        self.base, self.parche, self.tocados = base, parche, tocados
        if tocados and max(tocados) >= base.sectores:
            # la partida es de otro disco más grande: el de los primeros días de juegos.pcfsite.com
            raise Error("la partida es de una versión antigua de juegos.pcfsite.com y no se puede convertir")

    def _sector(self, s):
        p = self.parche(s) if s in self.tocados else None
        return self.base.sector(s) if p is None else p

    def read(self, off, n):
        s0, s1 = off // SECTOR, (off + n - 1) // SECTOR
        buf = b"".join(self._sector(s) for s in range(s0, s1 + 1))
        return buf[off - s0 * SECTOR:off - s0 * SECTOR + n]


def disco_original(base):
    return Disco(base, lambda s: None, set())


def disco_v86(base, blob):
    """La partida v86 (DMSV2) sobre su disco: bloques de 256 B (dos por sector)."""
    if blob[:5] != b"DMSV2" or len(blob) < 9:
        raise Error("no es una partida de juegos.pcfsite.com")
    n = _u32(blob, 5)
    datos = 9 + 4 * n
    if len(blob) != datos + n * BLOQUE_V86:
        raise Error("la partida está incompleta")
    nums = struct.unpack_from("<%dI" % n, blob, 9)
    pos = {b: datos + i * BLOQUE_V86 for i, b in enumerate(nums)}   # si un bloque sale dos veces, manda el último
    tocados = {b // 2 for b in pos}

    def parche(s):
        sec = bytearray(base.sector(s))
        for k in (0, 1):
            p = pos.get(s * 2 + k)
            if p is not None:
                sec[k * BLOQUE_V86:(k + 1) * BLOQUE_V86] = blob[p:p + BLOQUE_V86]
        return bytes(sec)
    return Disco(base, parche, tocados)


def jsdos_discos(blob):
    """{nombre del disco en R2: (índice {sector: (inicio, largo)})} de una partida de js-dos (sockdrive)."""
    out, off = {}, 0
    try:
        while off < len(blob):
            ul = _u32(blob, off)
            if ul > 4096:
                raise Error("no es una partida de juegos.pcfsite.com")
            url = blob[off + 4:off + 4 + ul].decode()
            off += 4 + ul
            pl = _u32(blob, off)
            off += 4
            fin = off + pl
            idx = {}
            n, o = _u32(blob, off), off + 4
            for _ in range(n):
                tam = _u32(blob, o)
                o += 4
                if tam == SECTOR + 4:
                    idx[_u32(blob, o)] = (o + 4, -1)
                else:
                    idx[_u32(lz4_bloque(blob[o:o + tam], SECTOR + 4), 0)] = (o, tam)
                o += tam
            nombre = url.split("?")[0].rstrip("/")
            nombre = nombre[len(R2):] if nombre.startswith(R2) else nombre.rsplit("/", 1)[-1]
            out[nombre] = idx
            off = fin
    except (struct.error, UnicodeDecodeError, IndexError):
        raise Error("no es una partida de juegos.pcfsite.com")
    return out


def disco_jsdos(base, blob, idx):
    def parche(s):
        ini, tam = idx[s]
        return blob[ini:ini + SECTOR] if tam < 0 else lz4_bloque(blob[ini:ini + tam], SECTOR + 4)[4:]
    return Disco(base, parche, set(idx))


# ------------------------------------------------------------------ FAT16/FAT32 (lectura, con nombres largos)
class Fat:
    def __init__(self, d):
        self.d = d
        mbr = d.read(0, SECTOR)
        self.part = _u32(mbr, 446 + 8) * SECTOR if mbr[510] == 0x55 and mbr[511] == 0xAA and mbr[446 + 4] else 0
        v = d.read(self.part, SECTOR)
        self.bps, self.spc, rsvd, self.nfat = _u16(v, 11), v[13], _u16(v, 14), v[16]
        if self.bps != SECTOR or not self.spc:
            raise Error("disco sin FAT")
        root_ent, spf = _u16(v, 17), _u16(v, 22)
        self.fat16 = spf != 0
        if not self.fat16:
            spf, self.root_clus = _u32(v, 36), _u32(v, 44)
        self.fat_off = self.part + rsvd * self.bps
        self.fat_bytes = spf * self.bps
        self.root_off = self.fat_off + self.nfat * self.fat_bytes
        self.root_bytes = root_ent * 32
        self.data_off = self.root_off + (self.root_bytes + self.bps - 1) // self.bps * self.bps
        self.cb = self.spc * self.bps
        self.end = 0xFFF8 if self.fat16 else 0x0FFFFFF8
        self._fat = None

    def fat_get(self, c):
        if self._fat is None:
            self._fat = self.d.read(self.fat_off, self.fat_bytes)
        if (c + 1) * (2 if self.fat16 else 4) > len(self._fat):
            return self.end
        return _u16(self._fat, c * 2) if self.fat16 else _u32(self._fat, c * 4) & 0x0FFFFFFF

    def chain(self, c):
        out = []
        while 2 <= c < self.end and len(out) < 1 << 20:
            out.append(c)
            c = self.fat_get(c)
        return out

    def clus_off(self, c):
        return self.data_off + (c - 2) * self.cb

    def read_dir(self, clus):
        if clus is None and self.fat16:
            regiones = [(self.root_off, self.root_bytes)]
        else:
            regiones = [(self.clus_off(c), self.cb) for c in self.chain(self.root_clus if clus is None else clus)]
        out, lfn = [], []
        for off, tam in regiones:
            raw = self.d.read(off, tam)
            for o in range(0, tam, 32):
                e = raw[o:o + 32]
                if e[0] == 0:
                    return out
                if e[0] == 0xE5:
                    lfn = []
                    continue
                attr = e[11]
                if attr == 0x0F:
                    lfn.append(e)
                    continue
                if attr & 0x08:
                    lfn = []
                    continue
                nm, ex = e[0:8].decode("cp437").rstrip(), e[8:11].decode("cp437").rstrip()
                if e[12] & 0x08:   # minúsculas sin nombre largo (NT)
                    nm = nm.lower()
                if e[12] & 0x10:
                    ex = ex.lower()
                largo = ""
                if lfn:
                    largo = "".join((x[1:11] + x[14:26] + x[28:32]).decode("utf-16-le", "replace")
                                    for x in sorted(lfn, key=lambda x: x[0] & 0x3F)).split("\0")[0].rstrip("￿")
                    lfn = []
                out.append({"name": largo or nm + ("." + ex if ex else ""), "dir": bool(attr & 0x10),
                            "clus": _u16(e, 20) << 16 | _u16(e, 26), "size": _u32(e, 28), "fecha": bytes(e[22:26])})
        return out

    def busca(self, entradas, nombre):
        return next((e for e in entradas if e["name"].upper() == nombre.upper()), None)

    def carpeta(self, nombre):
        e = self.busca(self.read_dir(None), nombre)
        return e if e and e["dir"] and e["clus"] >= 2 else None

    def walk(self, clus, prefijo="", hondo=0):
        if hondo > 8:
            return
        for e in self.read_dir(clus):
            if e["name"] in (".", "..") or not _nombre_valido(e["name"]):
                # basura: el resto de una carpeta nueva que la partida no llegó a guardar entera (Windows la limpia al
                # crearla, pero esos sectores no están en la partida y se ve lo que había antes en el disco)
                continue
            rel = prefijo + e["name"]
            if e["dir"]:
                if e["clus"] >= 2:
                    yield from self.walk(e["clus"], rel + "/", hondo + 1)
            else:
                yield rel, e

    def sectores(self, e):
        spc = self.cb // SECTOR
        return {self.clus_off(c) // SECTOR + k for c in self.chain(e["clus"]) for k in range(spc)}

    def read_file(self, e):
        out, falta = bytearray(), e["size"]
        for c in self.chain(e["clus"]):
            if falta <= 0:
                break
            k = min(self.cb, falta)
            out += self.d.read(self.clus_off(c), k)
            falta -= k
        if len(out) != e["size"]:
            raise Error("fichero cortado en la partida")
        return bytes(out)


def cambiados(base, disco, carpeta):
    """[(ruta relativa a la carpeta del juego, estado, leer)] de los ficheros que la partida ha escrito o cambiado.
    estado: "ok", "vacio" o "roto" (la partida no llegó a guardar entero el fichero); leer() da sus bytes."""
    fat0 = Fat(disco_original(base))
    e0 = fat0.carpeta(carpeta)
    antes = {}
    if e0:
        for rel, fe in fat0.walk(e0["clus"]):
            antes[rel.upper()] = (fe["clus"], fe["size"], fe["fecha"])
    fat = Fat(disco)
    e = fat.carpeta(carpeta)
    if not e:
        raise Error("la partida no es de este juego")
    out = []
    for rel, fe in fat.walk(e["clus"]):
        if rel.upper().endswith(NUNCA):
            continue
        cambio = antes.get(rel.upper()) != (fe["clus"], fe["size"], fe["fecha"])
        if not cambio and not (fe["size"] and fat.sectores(fe) & disco.tocados):
            continue
        if not fe["size"]:
            estado = "vacio"
        elif fe["clus"] < 2 or len(fat.chain(fe["clus"])) * fat.cb < fe["size"]:
            estado = "roto"
        else:
            estado = "ok"
        out.append((rel, estado, lambda fe=fe: fat.read_file(fe)))
    return out


def _limpia(ficheros, solo=None):
    """Quita las partidas a medio grabar: si en una carpeta de ranura (save/<modo>/<ranura>/) hay algún fichero vacío o
    roto, se cerró el navegador mientras el juego grababa y fuera la ranura entera; fuera de las ranuras, solo ese
    fichero. `solo`: las subcarpetas que cuentan (2000/2001)."""
    if solo:
        ficheros = [x for x in ficheros if x[0].split("/")[0].lower() in solo]
    rotas = {r.rsplit("/", 1)[0].upper() for r, estado, _ in ficheros if estado != "ok" and r.count("/") >= 3}
    return [(r, leer) for r, estado, leer in ficheros
            if estado == "ok" and not (r.count("/") >= 3 and r.rsplit("/", 1)[0].upper() in rotas)]


def _de_zip(blob, carpeta):
    """El 4.0 de js-dos guardaba un zip con los ficheros cambiados (FUTBOL4/…)."""
    try:
        z = zipfile.ZipFile(io.BytesIO(blob))
        out = []
        for i in z.infolist():
            n = i.filename.replace("\\", "/")
            if i.is_dir() or not n.upper().startswith(carpeta.upper() + "/") or ".." in n.split("/"):
                continue
            rel = n[len(carpeta) + 1:]
            if not rel.upper().endswith(NUNCA):
                out.append((rel, "ok" if i.file_size else "vacio", lambda i=i: z.read(i)))
        return out
    except (zipfile.BadZipFile, KeyError, OSError):
        raise Error("no es una partida de juegos.pcfsite.com")


def clave_de(nombre):
    """«v86_pcf7.pcfsave» / «legacy_pcf7 (1).pcfsave» → «pcf7» (el juego de juegos.pcfsite.com), o None."""
    n = os.path.basename(nombre or "").lower()
    n = n.split(".pcfsave")[0].split(" ")[0].split("(")[0]
    for p in ("v86_", "legacy_"):
        if n.startswith(p):
            n = n[len(p):]
    return n if n in DESTINO else None


def ficheros_de(juego, blob, nombre=""):
    """[(ruta relativa a la carpeta del juego en Source, bytes)] de un .pcfsave de juegos.pcfsite.com."""
    clave = clave_de(nombre)
    if clave and DESTINO[clave] != juego:
        raise Error("esta partida es del %s: cárgala en ese juego" % NOMBRES[DESTINO[clave]])
    if juego not in JUEGOS:
        raise Error("de esta edición no hay partidas en juegos.pcfsite.com")
    J = JUEGOS[juego]
    if blob[:5] == b"DMSV2":
        if "v86" not in J:
            raise Error("la partida no es de este juego")
        _, disco, carpeta = J["v86"]
        base = Base(disco)
        ficheros = cambiados(base, disco_v86(base, blob), carpeta)
    elif blob[:4] == b"PK\x03\x04":
        if "zip" not in J:
            raise Error("la partida no es de este juego")
        ficheros = _de_zip(blob, J["zip"])
    else:
        discos = jsdos_discos(blob)
        propios = [d for d in discos if d in J["jsdos"]]
        if not propios:
            raise Error("la partida no es de este juego")
        ficheros = []
        for d in propios:
            base = Base(d)
            ficheros += cambiados(base, disco_jsdos(base, blob, discos[d]), J["jsdos"][d])
    ficheros = _limpia(ficheros, J.get("solo"))
    if not ficheros:
        raise Error("en esa partida no hay ninguna partida guardada del juego")
    return ficheros


def a_source(juego, blob, nombre=""):
    """La copia de partidas de Source (el JSON de «Descargar partidas») con las partidas de un .pcfsave."""
    carpeta, ahora, total, ficheros = JUEGOS.get(juego, {}).get("carpeta"), int(time.time() * 1000), 0, []
    for rel, leer in ficheros_de(juego, blob, nombre):
        datos = leer()
        total += len(datos)
        if total > MAX_TOTAL:
            raise Error("partida demasiado grande")
        ficheros.append({"ruta": "game/%s/%s" % (carpeta, rel), "fecha": ahora,
                         "gz": base64.b64encode(gzip.compress(datos, 6)).decode()})
    return {"formato": juego + "-partidas", "version": 1, "origen": "juegos.pcfsite.com",
            "fecha": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "ficheros": ficheros}


if __name__ == "__main__":   # prueba: pcfsave.py <juego> <fichero.pcfsave> [salida.json]
    import sys
    t = time.time()
    r = a_source(sys.argv[1], open(sys.argv[2], "rb").read(), sys.argv[2])
    if len(sys.argv) > 3:
        json.dump(r, open(sys.argv[3], "w"))
    print(len(r["ficheros"]), "ficheros en %.1f s" % (time.time() - t))
    for f in r["ficheros"]:
        print(" ", f["ruta"])
