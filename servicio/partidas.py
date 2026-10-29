#!/usr/bin/env python3
"""source.pcfsite.com — partidas guardadas de los juegos en el navegador, en la cuenta de pcfsite.com, y el estado
del sitio que administra el Admin (acceso, catálogo, aviso, mantenimiento, estadísticas).

Va detrás de nginx (location /partidas/ → 127.0.0.1:8870). Quién es el usuario lo dice el foro: se le pasa la cookie
de la petición a su /api/yo. Nada de esto guarda contraseñas ni datos personales: solo los ficheros que escribe el
juego y, para las estadísticas, qué juego se arrancó, cuándo y desde qué IP.

Público (por nginx):
  GET    /partidas/<juego>/              → {"ficheros": [{"ruta", "fecha", "tam", "sha", "borrado"}]}
  GET    /partidas/<juego>/f/<ruta>      → el fichero (tal como se subió: el cliente lo manda comprimido con gzip)
  PUT    /partidas/<juego>/f/<ruta>?fecha=ms&sha=…   (cuerpo = fichero)   → 409 si el servidor tiene una más nueva
  DELETE /partidas/<juego>/f/<ruta>?fecha=ms
  POST   /partidas/hit · /partidas/beat  (cuerpo «<juego> <id de navegador>»): arranque de un juego y latido por minuto
  GET    /estado.json                    → catálogo, aviso y mantenimiento (lo aplica la web, site/pcf/estado.js)
  GET    /acceso/comprobar (auth_request de nginx) → 204 pasa, 401 sin sesión (o /jugar/ sin cuenta: jugar exige
                                                     sesión aunque el sitio esté abierto), 403 cerrado o en mantenimiento
  GET    /acceso/pagina (error_page 403 de nginx)  → la página «cerrado» (403) o «mantenimiento» (503)
  GET    /salud

Admin (solo desde la propia máquina, con el JWT firmado del Admin: pcf_interno.py, aud «source»):
  GET    /internal/v1/estado                         (source.access:write)
  PUT    /internal/v1/acceso        {abierto, permitidos}            (source.access:write)
  PUT    /internal/v1/catalogo      {catalogo: {id: {oculto, nombre, nuevo, arreglado, orden}}}   (source.catalog:write)
  PUT    /internal/v1/aviso         {texto, enlace}                  (source.catalog:write)
  PUT    /internal/v1/mantenimiento {activo, mensaje}                (source.ops:write)
  GET    /internal/v1/stats                                          (stats:read)
  GET    /internal/v1/partidas · /internal/v1/partidas/<uid>         (source.saves:read)
  GET    /internal/v1/partidas/<uid>/<juego>/fichero?ruta=&version=  (source.saves:read)
  POST   /internal/v1/partidas/<uid>/<juego>/restaurar {ruta, version}   (source.saves:write)
  DELETE /internal/v1/partidas/<uid>/<juego>                         (source.saves:write; va a la papelera 30 días)

Disco: DATOS/<id usuario>/<juego>/actual/<ruta> + meta.json; cada versión sustituida o borrada se guarda en
historial/<ruta>/<fecha> (las 10 últimas), para poder recuperar una partida estropeada (Admin › Source › Partidas).
DATOS/acceso.json y DATOS/ajustes.json son el estado del sitio; DATOS/estadisticas.sqlite, los arranques."""
import datetime
import hashlib
import html
import http.client
import http.server
import json
import os
import re
import shutil
import sqlite3
import threading
import time
import urllib.parse
import zoneinfo

import pcf_interno   # copia exacta de Admin-PCFutbol/contracts/verificador/pcf_interno.py

DATOS = os.environ.get("PARTIDAS_DATOS", "/var/lib/source-partidas")
PUERTO = int(os.environ.get("PARTIDAS_PUERTO", "8870"))
FORO = ("127.0.0.1", int(os.environ.get("PARTIDAS_FORO_PUERTO", "8850")))
ORIGEN = "https://source.pcfsite.com"
AUD = "source"   # audiencia de los tokens del Admin
JUEGOS = {"pcf7", "pcf7plus", "pcf6", "pcf6oro", "pcf5", "pcf5oro", "pcf5arg", "pcf6arg", "pcf4", "pcf2000", "pcf2001"}   # los ids de juegos/<id> de core/ que están en el navegador (JUEGOS_WEB de deploy.sh)
NOMBRES = {"pcf4": "PC Fútbol 4.0", "pcf5": "PC Fútbol 5.0", "pcf5oro": "PC Fútbol 5.0 Edición de Oro",
           "pcf5arg": "PC Fútbol 5.0 Apertura 97", "pcf6arg": "PC Fútbol 6.0 Apertura 98", "pcf6": "PC Fútbol 6.0",
           "pcf6oro": "PC Fútbol 6.0 Edición de Oro", "pcf7": "PC Fútbol 7", "pcf7plus": "PC Fútbol 7 Plus",
           "pcf2000": "PC Fútbol 2000", "pcf2001": "PC Fútbol 2001"}
FICHAS = ["pcf4", "pcf5", "pcf6", "pcf7", "pcf2000", "pcf2001", "pcf5arg", "pcf6arg"]   # las tarjetas de la portada (data/juegos.json); las ediciones van dentro
RUTA_OK = re.compile(r"^[A-Za-z0-9_\-][A-Za-z0-9_.\- /]{0,199}$")
MAX_FICHERO = 16 << 20         # comprimido; una partida de PC Fútbol 7 son ~1,2 MB
MAX_TOTAL = 300 << 20          # por usuario y juego
MAX_FICHEROS = 2000
HISTORIAL = 10
MADRID = zoneinfo.ZoneInfo("Europe/Madrid")

_cerrojos, _cerrojo = {}, threading.Lock()
_sesiones, _sesiones_mx = {}, threading.Lock()   # cookie → (id, caduca)


def cerrojo(clave):
    with _cerrojo:
        return _cerrojos.setdefault(clave, threading.Lock())


def usuario(cookie):
    """Id del usuario de la sesión (preguntando al foro; 60 s de caché), o None."""
    u = cuenta(cookie)
    return u["id"] if u else None


def cuenta(cookie):
    """{id, name, admin} del usuario de la sesión (preguntando al foro; 60 s de caché), o None."""
    if not cookie:
        return None
    ahora = time.time()
    with _sesiones_mx:
        c = _sesiones.get(cookie)
        if c and c[1] > ahora:
            return c[0]
    try:
        con = http.client.HTTPConnection(*FORO, timeout=5)
        con.request("GET", "/api/yo", headers={"Host": "foro.pcfsite.com", "Cookie": cookie})
        r = con.getresponse()
        u = json.loads(r.read()).get("user") if r.status == 200 else None
        con.close()
    except Exception:
        return None   # foro caído: sin cuenta (el juego sigue guardando en el navegador)
    r = {"id": int(u["id"]), "name": str(u.get("name", "")), "admin": bool(u.get("admin") or u.get("super"))} \
        if u and str(u.get("id", "")).isdigit() else None
    with _sesiones_mx:
        if len(_sesiones) > 5000:
            _sesiones.clear()
        _sesiones[cookie] = (r, ahora + 60)
    return r


def ruta_valida(r):
    return bool(RUTA_OK.match(r)) and ".." not in r.split("/") and "//" not in r and not r.endswith("/")


def escribe_atomico(p, datos):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + ".tmp"
    with open(tmp, "wb") as f:
        f.write(datos)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, p)


def lee_json(p, defecto):
    try:
        with open(p, encoding="utf-8") as f:
            v = json.load(f)
        return v if isinstance(v, dict) else defecto
    except (FileNotFoundError, ValueError):
        return defecto


class Juego:
    def __init__(self, uid, juego):
        self.base = os.path.join(DATOS, str(uid), juego)
        self.meta_p = os.path.join(self.base, "meta.json")

    def meta(self):
        return lee_json(self.meta_p, {})

    def guarda_meta(self, m):
        escribe_atomico(self.meta_p, json.dumps(m, ensure_ascii=False, sort_keys=True).encode())

    def actual(self, ruta):
        return os.path.join(self.base, "actual", ruta)

    def historial(self, ruta):
        return os.path.join(self.base, "historial", ruta)

    def versiones(self, ruta):
        """Fechas (ms) de las versiones guardadas en el historial, de la más nueva a la más vieja."""
        try:
            return sorted((int(x) for x in os.listdir(self.historial(ruta)) if x.isdigit()), reverse=True)
        except FileNotFoundError:
            return []

    def al_historial(self, ruta, fecha):
        """La versión que se va a sustituir o borrar pasa al historial (se guardan las HISTORIAL últimas)."""
        p = self.actual(ruta)
        if not os.path.exists(p):
            return
        h = self.historial(ruta)
        os.makedirs(h, exist_ok=True)
        shutil.copy2(p, os.path.join(h, str(fecha)))
        viejas = sorted(os.listdir(h), key=lambda x: int(x) if x.isdigit() else 0)
        for v in viejas[:-HISTORIAL]:
            os.remove(os.path.join(h, v))


# ---- control de acceso a source.pcfsite.com (nginx auth_request → /acceso/comprobar) y quién lo usa
ACCESO = os.path.join(DATOS, "acceso.json")   # {"abierto": bool, "permitidos": [nombres del foro en minúsculas]}
AJUSTES = os.path.join(DATOS, "ajustes.json")  # catálogo, aviso y mantenimiento (Admin › Source)
_vistos, _vistos_mx = {}, threading.Lock()    # usuario o IP → última visita
ACTIVO_S = 300


def acceso():
    a = lee_json(ACCESO, {})
    return {"abierto": bool(a.get("abierto", True)), "permitidos": sorted(set(a.get("permitidos", [])))}


def guarda_acceso(a):
    nuevo = {"abierto": bool(a.get("abierto", True)),
             "permitidos": sorted({str(x).strip().lower()[:60] for x in a.get("permitidos", []) if str(x).strip()})}
    escribe_atomico(ACCESO, json.dumps(nuevo, ensure_ascii=False).encode())
    return nuevo


def _catalogo(c):
    """Solo las fichas que existen y los campos que entiende la web."""
    out = {}
    for jid in FICHAS:
        v = c.get(jid) if isinstance(c, dict) else None
        v = v if isinstance(v, dict) else {}
        try:
            orden = max(0, min(999, int(v.get("orden") or 0)))
        except (TypeError, ValueError):
            orden = 0
        out[jid] = {"oculto": bool(v.get("oculto")), "nombre": str(v.get("nombre") or "")[:80],
                    "nuevo": bool(v.get("nuevo")), "arreglado": bool(v.get("arreglado")), "orden": orden}
    return out


def ajustes():
    a = lee_json(AJUSTES, {})
    m, av = a.get("mantenimiento") or {}, a.get("aviso") or {}
    return {"catalogo": _catalogo(a.get("catalogo") or {}),
            "mantenimiento": {"activo": bool(m.get("activo")), "mensaje": str(m.get("mensaje") or "")[:300]},
            "aviso": {"texto": str(av.get("texto") or "")[:300], "enlace": str(av.get("enlace") or "")[:300]}}


def guarda_ajustes(a):
    escribe_atomico(AJUSTES, json.dumps(a, ensure_ascii=False, sort_keys=True).encode())


def visto(clave):
    with _vistos_mx:
        _vistos[clave] = time.time()


def activos():
    lim = time.time() - ACTIVO_S
    with _vistos_mx:
        for k in [k for k, t in _vistos.items() if t < lim]:
            del _vistos[k]
        return sorted(_vistos)


def pagina_cerrada(mantenimiento, mensaje=""):
    """La página que ve quien no puede pasar (cerrado o en mantenimiento): mismo aspecto que la de nginx de antes."""
    if mantenimiento:
        titulo, texto = "En mantenimiento", mensaje or "source.pcfsite.com está en mantenimiento. Volvemos en un rato."
    else:
        titulo, texto = "Acceso cerrado", "source.pcfsite.com está cerrado de momento. Si te han dado acceso, entra con tu cuenta del foro."
    return (f'<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<title>source.pcfsite.com</title><body style="font:16px system-ui;background:#0e1420;color:#dfe6f1;display:grid;'
            f'place-items:center;min-height:100vh;margin:0"><div style="max-width:440px;padding:24px;text-align:center">'
            f'<h1 style="font-size:22px">{titulo}</h1><p>{html.escape(texto)}</p>'
            f'<p><a style="color:#8fb4ff" href="https://foro.pcfsite.com/">foro.pcfsite.com</a></p></div>').encode()


# ---- estadísticas: arranques (sqlite) y latidos (memoria)
BD = os.path.join(DATOS, "estadisticas.sqlite")
DEDUPE_S = 1800      # el mismo navegador y juego no cuenta dos veces en media hora (recargas, cambios de programa)
LATIDO_S = 150       # sin latido en este tiempo ya no está jugando (el jugador manda uno por minuto)
_bd_mx = threading.Lock()
_latidos, _latidos_mx = {}, threading.Lock()   # (quién, juego) → último latido


def bd():
    c = sqlite3.connect(BD, timeout=5)
    c.execute("CREATE TABLE IF NOT EXISTS lanzamientos (id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, juego TEXT NOT NULL,"
              " uid INTEGER, ip TEXT, sid TEXT)")
    c.execute("CREATE INDEX IF NOT EXISTS lanzamientos_ts ON lanzamientos (ts)")
    c.execute("CREATE INDEX IF NOT EXISTS lanzamientos_quien ON lanzamientos (sid, juego, ts)")
    c.execute("CREATE TABLE IF NOT EXISTS meta (clave TEXT PRIMARY KEY, valor TEXT)")
    return c


def quien(uid, sid, ip):
    return f"u:{uid}" if uid else (f"s:{sid}" if sid else f"ip:{ip}")


def registra_lanzamiento(juego, uid, ip, sid):
    ahora = int(time.time())
    with _bd_mx, bd() as c:
        q = quien(uid, sid, ip)
        ya = c.execute("SELECT 1 FROM lanzamientos WHERE juego = ? AND ts > ? AND "
                       "((? <> '' AND sid = ?) OR (uid IS NOT NULL AND uid = ?) OR (? = '' AND sid = '' AND ip = ?)) LIMIT 1",
                       (juego, ahora - DEDUPE_S, sid, sid, uid, sid, ip)).fetchone()
        if ya:
            return False
        c.execute("INSERT INTO lanzamientos (ts, juego, uid, ip, sid) VALUES (?, ?, ?, ?, ?)", (ahora, juego, uid, ip, sid))
    latido(q, juego)
    return True


def latido(q, juego):
    with _latidos_mx:
        _latidos[(q, juego)] = time.time()


def jugando():
    """[(quién, juego)] con latido reciente; de paso apunta el récord de jugadores a la vez."""
    lim = time.time() - LATIDO_S
    with _latidos_mx:
        for k in [k for k, t in _latidos.items() if t < lim]:
            del _latidos[k]
        ahora = sorted(_latidos)
    n = len({q for q, _j in ahora})
    if n:
        with _bd_mx, bd() as c:
            r = c.execute("SELECT valor FROM meta WHERE clave = 'record'").fetchone()
            rec = json.loads(r[0]) if r else {"max": 0}
            if n > int(rec.get("max") or 0):
                c.execute("INSERT OR REPLACE INTO meta (clave, valor) VALUES ('record', ?)",
                          (json.dumps({"max": n, "ts": int(time.time())}),))
    return ahora


def _dia(ts):
    return datetime.datetime.fromtimestamp(ts, MADRID).date()


def estadisticas():
    ahora = int(time.time())
    hoy = _dia(ahora)
    vivos = jugando()   # antes de leer el récord: lo apunta
    with _bd_mx, bd() as c:
        tot = c.execute("SELECT count(*), count(DISTINCT CASE WHEN uid IS NOT NULL THEN 'u' || uid ELSE 's' || sid END),"
                        " count(DISTINCT uid), count(DISTINCT ip) FROM lanzamientos").fetchone()
        semana = c.execute("SELECT count(*) FROM lanzamientos WHERE ts > ?", (ahora - 7 * 86400,)).fetchone()[0]
        por_juego = c.execute("SELECT juego, count(*), count(DISTINCT CASE WHEN uid IS NOT NULL THEN 'u' || uid ELSE 's' || sid END)"
                              " FROM lanzamientos GROUP BY juego ORDER BY 2 DESC").fetchall()
        filas = c.execute("SELECT ts, juego, uid, sid, ip FROM lanzamientos WHERE ts > ? ORDER BY ts",
                          (ahora - 31 * 86400,)).fetchall()
        recientes = c.execute("SELECT ts, juego, uid, ip FROM lanzamientos ORDER BY id DESC LIMIT 50").fetchall()
        r = c.execute("SELECT valor FROM meta WHERE clave = 'record'").fetchone()
    record = json.loads(r[0]) if r else None
    dias = {}
    for ts, juego, uid, sid, _ip in filas:
        d = dias.setdefault(_dia(ts), {"n": 0, "quien": set(), "juegos": {}})
        d["n"] += 1
        d["quien"].add(f"u{uid}" if uid else f"s{sid}")
        d["juegos"][juego] = d["juegos"].get(juego, 0) + 1
    SEM = ["lun", "mar", "mié", "jue", "vie", "sáb", "dom"]
    tabla, por_dia = [], []
    for i in range(29, -1, -1):
        dia = hoy - datetime.timedelta(days=i)
        d = dias.get(dia, {"n": 0, "quien": set(), "juegos": {}})
        top = max(d["juegos"].items(), key=lambda x: x[1]) if d["juegos"] else ("", 0)
        tabla.append({"label": dia.strftime("%d/%m/%Y"), "weekday": SEM[dia.weekday()], "n": d["n"], "players": len(d["quien"]),
                      "top_game": NOMBRES.get(top[0], top[0]), "top_n": top[1]})
        if i < 14:
            por_dia.append({"label": dia.strftime("%d/%m"), "count": d["n"]})
    tabla.reverse()
    top = {}
    for _q, j in vivos:
        top[j] = top.get(j, 0) + 1
    return {
        "totales": {"lanzamientos": tot[0], "jugadores": tot[1], "con_cuenta": tot[2], "ips": tot[3], "ultimos_7_dias": semana},
        "por_juego": [{"juego": j, "name": NOMBRES.get(j, j), "n": n, "players": p} for j, n, p in por_juego],
        "por_dia": por_dia, "tabla_diaria": tabla,
        "recientes": [{"ts": ts, "juego": j, "name": NOMBRES.get(j, j), "uid": uid, "ip": ip} for ts, j, uid, ip in recientes],
        "vivo": {"jugando": len({q for q, _j in vivos}), "top": [{"juego": j, "name": NOMBRES.get(j, j), "c": n} for j, n in sorted(top.items(), key=lambda x: -x[1])],
                 "ts": datetime.datetime.now(MADRID).strftime("%H:%M:%S")},
        "record": record, "activos": len(activos()), "ventana_s": ACTIVO_S, "partidas": resumen_partidas(),
        "actualizado": ahora,
    }


# ---- partidas de todos los usuarios (Admin › Source › Partidas)
PAPELERA = os.path.join(DATOS, "_papelera")


def _uids():
    try:
        return sorted((int(n) for n in os.listdir(DATOS) if n.isdigit()), key=int)
    except FileNotFoundError:
        return []


def _juegos_de(uid):
    base = os.path.join(DATOS, str(uid))
    try:
        return sorted(j for j in os.listdir(base) if j in JUEGOS and os.path.isfile(os.path.join(base, j, "meta.json")))
    except FileNotFoundError:
        return []


def _resumen_juego(uid, juego):
    m = Juego(uid, juego).meta()
    vivos = [v for v in m.values() if not v.get("borrado")]
    return {"juego": juego, "name": NOMBRES.get(juego, juego), "ficheros": len(vivos), "borrados": len(m) - len(vivos),
            "bytes": sum(int(v.get("tam") or 0) for v in vivos),
            "ultima": max((int(v.get("fecha") or 0) for v in m.values()), default=0)}


def usuarios_con_partidas():
    out = []
    for uid in _uids():
        js = [_resumen_juego(uid, j) for j in _juegos_de(uid)]
        if js:
            out.append({"uid": uid, "juegos": js, "bytes": sum(j["bytes"] for j in js), "ficheros": sum(j["ficheros"] for j in js),
                        "ultima": max(j["ultima"] for j in js)})
    out.sort(key=lambda u: -u["ultima"])
    return out


def resumen_partidas():
    us = usuarios_con_partidas()
    por_juego = {}
    for u in us:
        for j in u["juegos"]:
            d = por_juego.setdefault(j["juego"], {"juego": j["juego"], "name": j["name"], "usuarios": 0, "ficheros": 0, "bytes": 0})
            d["usuarios"] += 1
            d["ficheros"] += j["ficheros"]
            d["bytes"] += j["bytes"]
    return {"usuarios": len(us), "ficheros": sum(u["ficheros"] for u in us), "bytes": sum(u["bytes"] for u in us),
            "por_juego": sorted(por_juego.values(), key=lambda d: -d["usuarios"])}


def detalle_usuario(uid):
    juegos = []
    for j in _juegos_de(uid):
        g = Juego(uid, j)
        m = g.meta()
        fich = [dict(ruta=r, **v, versiones=g.versiones(r)) for r, v in sorted(m.items())]
        juegos.append({"juego": j, "name": NOMBRES.get(j, j), "ficheros": fich,
                       "bytes": sum(int(v.get("tam") or 0) for v in m.values() if not v.get("borrado"))})
    return {"uid": uid, "juegos": juegos}


def fichero_de(uid, juego, ruta, version):
    """Ruta en disco del fichero actual (version vacía) o de una versión del historial; None si no existe."""
    g = Juego(uid, juego)
    if version:
        p = os.path.join(g.historial(ruta), str(int(version)))
    else:
        m = g.meta()
        if ruta not in m or m[ruta].get("borrado"):
            return None
        p = g.actual(ruta)
    return p if os.path.isfile(p) else None


def restaurar(uid, juego, ruta, version):
    """Una versión del historial pasa a ser la actual (la actual, al historial). Devuelve el error o None."""
    g = Juego(uid, juego)
    origen = os.path.join(g.historial(ruta), str(int(version)))
    if not os.path.isfile(origen):
        return "no existe esa versión"
    with cerrojo(g.base):
        m = g.meta()
        ya = m.get(ruta)
        if ya and not ya.get("borrado"):
            g.al_historial(ruta, ya["fecha"])
        with open(origen, "rb") as f:
            datos = f.read()
        escribe_atomico(g.actual(ruta), datos)
        m[ruta] = {"fecha": int(time.time() * 1000), "tam": len(datos), "sha": hashlib.sha256(datos).hexdigest()}
        g.guarda_meta(m)
    return None


def a_la_papelera(uid, juego):
    """Las partidas de un juego de un usuario, a DATOS/_papelera/<uid>-<juego>-<fecha> (limpiar.py las borra a los 30 días)."""
    base = Juego(uid, juego).base
    if not os.path.isdir(base):
        return "no hay partidas"
    os.makedirs(PAPELERA, exist_ok=True)
    with cerrojo(base):
        shutil.move(base, os.path.join(PAPELERA, f"{uid}-{juego}-{int(time.time())}"))
    return None


class H(http.server.BaseHTTPRequestHandler):
    server_version = "partidas"
    sys_version = ""

    def log_message(self, fmt, *a):
        pass

    def responde(self, codigo, cuerpo=b"", tipo="application/json", cabeceras=None):
        if isinstance(cuerpo, (dict, list)):
            cuerpo = json.dumps(cuerpo, ensure_ascii=False).encode()
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(cuerpo)))
        if "Cache-Control" not in (cabeceras or {}):
            self.send_header("Cache-Control", "no-store")
        for k, v in (cabeceras or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(cuerpo)

    def local(self):
        return self.client_address[0] == "127.0.0.1" and not self.headers.get("X-Forwarded-For")

    def cuerpo(self, maximo=1 << 20):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if 0 <= n <= maximo else None

    def destino(self):
        """(Juego, ruta o None, query) si la petición es válida y hay sesión; si no, responde y devuelve None."""
        u = urllib.parse.urlsplit(self.path)
        partes = u.path.split("/", 4)   # ['', 'partidas', juego, '' | 'f', ruta]
        if len(partes) < 3 or partes[1] != "partidas" or partes[2] not in JUEGOS:
            self.responde(404, {"error": "no existe"})
            return None
        if self.command in ("PUT", "DELETE") and self.headers.get("Origin") != ORIGEN:
            self.responde(403, {"error": "origen"})
            return None
        uid = usuario(self.headers.get("Cookie", ""))
        if uid is None:
            self.responde(401, {"error": "sin sesión"})
            return None
        ruta = None
        if len(partes) == 5 and partes[3] == "f":
            ruta = urllib.parse.unquote(partes[4])
            if not ruta_valida(ruta):
                self.responde(400, {"error": "ruta"})
                return None
        elif not (len(partes) == 4 and partes[3] == "") and self.command != "GET":
            self.responde(404, {"error": "no existe"})
            return None
        return Juego(uid, partes[2]), ruta, urllib.parse.parse_qs(u.query)

    # ---- público: acceso, estado del sitio, salud
    def publico_get(self):
        ruta = urllib.parse.urlsplit(self.path).path
        if ruta == "/acceso/comprobar":   # nginx auth_request: 204 pasa, 401 sin sesión, 403 sin permiso o mantenimiento
            u = cuenta(self.headers.get("Cookie", ""))
            a = acceso()
            ip = self.headers.get("X-Real-IP", "?")
            if ajustes()["mantenimiento"]["activo"] and not (u and u["admin"]):
                return self.responde(403)
            if not u and self.headers.get("X-Original-URI", "").startswith("/jugar/"):   # jugar exige la cuenta de pcfsite
                return self.responde(401)
            if a["abierto"] or (u and (u["admin"] or u["name"].lower() in a["permitidos"])):
                visto(f"u:{u['name']}" if u else f"ip:{ip}")
                return self.responde(204)
            return self.responde(403 if u else 401)
        if ruta == "/acceso/pagina":      # nginx error_page 403: cerrado (403) o mantenimiento (503), según el caso
            u = cuenta(self.headers.get("Cookie", ""))
            m = ajustes()["mantenimiento"]
            if m["activo"] and not (u and u["admin"]):
                return self.responde(503, pagina_cerrada(True, m["mensaje"]), "text/html; charset=utf-8", {"Retry-After": "600"})
            return self.responde(403, pagina_cerrada(False), "text/html; charset=utf-8")
        if ruta == "/estado.json":
            a = ajustes()
            return self.responde(200, {"ok": True, "catalogo": a["catalogo"], "aviso": a["aviso"], "mantenimiento": a["mantenimiento"]},
                                 cabeceras={"Cache-Control": "public, max-age=20"})
        if ruta == "/salud":
            return self.responde(200, {"ok": True, "activos": len(activos()), "jugando": len({q for q, _j in jugando()})})
        if ruta == "/interno/acceso" and self.local():   # el Admin anterior al 25-oct-2026; se quita cuando ya no lo use
            return self.responde(200, dict(acceso(), activos=activos(), ventana_s=ACTIVO_S))
        return self.responde(404, {"error": "no existe"})

    def marca(self, que):
        """POST /partidas/hit · /partidas/beat: «<juego> <id de navegador>» (sendBeacon desde el jugador)."""
        datos = (self.cuerpo(200) or b"").decode("utf-8", "replace").split()
        juego = datos[0] if datos else ""
        sid = re.sub(r"[^A-Za-z0-9_\-]", "", datos[1])[:40] if len(datos) > 1 else ""
        if juego not in JUEGOS:
            return self.responde(204)
        uid = usuario(self.headers.get("Cookie", ""))
        ip = (self.headers.get("X-Forwarded-For") or "?").split(",")[0].strip()
        if que == "hit":
            registra_lanzamiento(juego, uid, ip, sid)
        else:
            latido(quien(uid, sid, ip), juego)
        self.responde(204)

    # ---- el Admin: /internal/v1/*, con el JWT firmado (pcf_interno.py), solo desde la propia máquina
    def interno(self):
        if not self.local():
            return self.responde(404, {"error": {"code": "NO_ENCONTRADO", "message": "No encontrado."}})
        cuerpo = self.cuerpo(4 << 20) if self.command in ("PUT", "POST", "DELETE") else b""
        if cuerpo is None:
            return self.responde(413, {"error": {"code": "GRANDE", "message": "Cuerpo demasiado grande."}})
        try:
            claims = pcf_interno.verificar(self.headers.get("Authorization", ""), aud=AUD, metodo=self.command, htu=self.path, cuerpo=cuerpo)
        except pcf_interno.TokenNoValido as e:
            return self.responde(401, {"error": {"code": e.codigo, "message": e.mensaje}})
        u = urllib.parse.urlsplit(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        partes = u.path.split("/")[3:]   # tras /internal/v1
        try:
            datos = json.loads(cuerpo or b"{}") if cuerpo else {}
            if not isinstance(datos, dict):
                raise ValueError
        except ValueError:
            return self.responde(400, {"error": {"code": "JSON", "message": "Cuerpo no válido."}})
        try:
            return self.interno_ruta(claims, partes, q, datos)
        except pcf_interno.TokenNoValido as e:
            return self.responde(403, {"error": {"code": e.codigo, "message": e.mensaje}})

    def interno_ruta(self, claims, partes, q, datos):
        m, r = self.command, partes
        exige = lambda p: pcf_interno.exige(claims, p)   # noqa: E731
        if r == ["estado"] and m == "GET":
            exige("source.access:write")
            return self.responde(200, dict(acceso(), activos=activos(), ventana_s=ACTIVO_S, **ajustes(),
                                           fichas=[{"id": j, "name": NOMBRES[j]} for j in FICHAS]))
        if r == ["acceso"] and m == "PUT":
            exige("source.access:write")
            return self.responde(200, guarda_acceso(datos))
        if r == ["catalogo"] and m == "PUT":
            exige("source.catalog:write")
            a = ajustes()
            a["catalogo"] = _catalogo(datos.get("catalogo") or {})
            guarda_ajustes(a)
            return self.responde(200, {"ok": True, "catalogo": a["catalogo"]})
        if r == ["aviso"] and m == "PUT":
            exige("source.catalog:write")
            a = ajustes()
            enlace = str(datos.get("enlace") or "")[:300]
            a["aviso"] = {"texto": str(datos.get("texto") or "")[:300], "enlace": enlace if enlace.startswith(("https://", "/")) else ""}
            guarda_ajustes(a)
            return self.responde(200, {"ok": True, "aviso": a["aviso"]})
        if r == ["mantenimiento"] and m == "PUT":
            exige("source.ops:write")
            a = ajustes()
            a["mantenimiento"] = {"activo": bool(datos.get("activo")), "mensaje": str(datos.get("mensaje") or "")[:300]}
            guarda_ajustes(a)
            return self.responde(200, {"ok": True, "mantenimiento": a["mantenimiento"]})
        if r == ["stats"] and m == "GET":
            exige("stats:read")
            return self.responde(200, estadisticas())
        if r == ["partidas"] and m == "GET":
            exige("source.saves:read")
            return self.responde(200, {"usuarios": usuarios_con_partidas(), "resumen": resumen_partidas()})
        if len(r) >= 2 and r[0] == "partidas" and r[1].isdigit():
            uid = int(r[1])
            if len(r) == 2 and m == "GET":
                exige("source.saves:read")
                return self.responde(200, detalle_usuario(uid))
            if len(r) >= 3 and r[2] in JUEGOS:
                juego = r[2]
                if len(r) == 4 and r[3] == "fichero" and m == "GET":
                    exige("source.saves:read")
                    ruta, version = q.get("ruta", ""), q.get("version", "")
                    if not ruta_valida(ruta) or (version and not version.isdigit()):
                        return self.responde(400, {"error": {"code": "RUTA", "message": "Ruta o versión no válidas."}})
                    p = fichero_de(uid, juego, ruta, version)
                    if not p:
                        return self.responde(404, {"error": {"code": "NO_ENCONTRADO", "message": "No existe ese fichero."}})
                    with open(p, "rb") as f:
                        return self.responde(200, f.read(), "application/octet-stream")
                if len(r) == 4 and r[3] == "restaurar" and m == "POST":
                    exige("source.saves:write")
                    ruta, version = str(datos.get("ruta") or ""), str(datos.get("version") or "")
                    if not ruta_valida(ruta) or not version.isdigit():
                        return self.responde(400, {"error": {"code": "RUTA", "message": "Ruta o versión no válidas."}})
                    err = restaurar(uid, juego, ruta, version)
                    if err:
                        return self.responde(404, {"error": {"code": "NO_ENCONTRADO", "message": err}})
                    return self.responde(200, {"ok": True})
                if len(r) == 3 and m == "DELETE":
                    exige("source.saves:write")
                    err = a_la_papelera(uid, juego)
                    if err:
                        return self.responde(404, {"error": {"code": "NO_ENCONTRADO", "message": err}})
                    return self.responde(200, {"ok": True})
        return self.responde(404, {"error": {"code": "NO_ENCONTRADO", "message": "No encontrado."}})

    def do_GET(self):
        if self.path.startswith("/internal/v1/"):
            return self.interno()
        if self.path.startswith(("/acceso/", "/interno/", "/estado.json", "/salud")):
            return self.publico_get()
        d = self.destino()
        if not d:
            return
        j, ruta, _ = d
        m = j.meta()
        if ruta is None:
            return self.responde(200, {"ficheros": [dict(ruta=r, **v) for r, v in sorted(m.items())]})
        if ruta not in m or m[ruta].get("borrado"):
            return self.responde(404, {"error": "no existe"})
        with open(j.actual(ruta), "rb") as f:
            self.responde(200, f.read(), "application/octet-stream")

    def do_POST(self):
        if self.path.startswith("/internal/v1/"):
            return self.interno()
        ruta = urllib.parse.urlsplit(self.path).path
        if ruta in ("/partidas/hit", "/partidas/beat"):
            return self.marca(ruta.rsplit("/", 1)[1])
        self.responde(404, {"error": "no existe"})

    def do_PUT(self):
        if self.path.startswith("/internal/v1/"):
            return self.interno()
        if urllib.parse.urlsplit(self.path).path == "/interno/acceso":   # el Admin anterior al 25-oct-2026
            if not self.local():
                return self.responde(403, {"error": "solo local"})
            try:
                a = json.loads(self.cuerpo() or b"{}")
            except ValueError:
                return self.responde(400, {"error": "json"})
            return self.responde(200, guarda_acceso(a))
        d = self.destino()
        if not d:
            return
        j, ruta, q = d
        if ruta is None:
            return self.responde(400, {"error": "falta la ruta"})
        n = int(self.headers.get("Content-Length") or -1)
        if n < 0 or n > MAX_FICHERO:
            return self.responde(413, {"error": "demasiado grande"})
        datos = self.rfile.read(n)
        fecha = int((q.get("fecha") or ["0"])[0] or 0)
        sha = hashlib.sha256(datos).hexdigest()
        with cerrojo(j.base):
            m = j.meta()
            ya = m.get(ruta)
            if ya and ya["fecha"] > fecha:
                return self.responde(409, {"error": "hay una más nueva", "actual": dict(ruta=ruta, **ya)})
            if ya and not ya.get("borrado") and ya.get("sha") == sha:
                ya["fecha"] = fecha
                j.guarda_meta(m)
                return self.responde(200, {"ok": True, "igual": True})
            total = sum(v["tam"] for r, v in m.items() if r != ruta and not v.get("borrado")) + len(datos)
            if total > MAX_TOTAL or len(m) >= MAX_FICHEROS and ruta not in m:
                return self.responde(507, {"error": "sin espacio en la cuenta"})
            if ya:
                j.al_historial(ruta, ya["fecha"])
            escribe_atomico(j.actual(ruta), datos)
            m[ruta] = {"fecha": fecha, "tam": len(datos), "sha": sha}
            j.guarda_meta(m)
        self.responde(200, {"ok": True})

    def do_DELETE(self):
        if self.path.startswith("/internal/v1/"):
            return self.interno()
        d = self.destino()
        if not d:
            return
        j, ruta, q = d
        if ruta is None:
            return self.responde(400, {"error": "falta la ruta"})
        fecha = int((q.get("fecha") or ["0"])[0] or 0)
        with cerrojo(j.base):
            m = j.meta()
            ya = m.get(ruta)
            if ya and ya["fecha"] > fecha:
                return self.responde(409, {"error": "hay una más nueva", "actual": dict(ruta=ruta, **ya)})
            if ya and not ya.get("borrado"):
                j.al_historial(ruta, ya["fecha"])
                os.remove(j.actual(ruta))
            m[ruta] = {"fecha": fecha, "tam": 0, "sha": "", "borrado": True}
            j.guarda_meta(m)
        self.responde(200, {"ok": True})


if __name__ == "__main__":
    os.makedirs(DATOS, exist_ok=True)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PUERTO), H)
    srv.daemon_threads = True
    srv.serve_forever()
