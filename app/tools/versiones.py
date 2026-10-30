#!/usr/bin/env python3
"""Genera app/versiones.json: lo que la app necesita saber de cada versión soportada para reconocer un CD (o una
instalación) del usuario, reproducir la instalación y encontrar los binarios. Se calcula desde las carpetas de trabajo
(~/<id>-work: iso/, game/, drive_c/, bin/) de los juegos ya traducidos; no copia ningún fichero, solo nombres y SHA-256.

  python3 app/tools/versiones.py            → app/versiones.json
  python3 app/tools/versiones.py pcf7 pcf6  → solo esos (los demás se conservan del fichero actual)

Por versión:
  discos      los CD que hacen falta, con sus firmas (ficheros de la raíz y su SHA-256), cómo se instala desde cada uno
              (guiones .nfo/.NFC del instalador original, árbol entero, o nada) y qué parte se copia como D:
  instalada   firmas de una carpeta ya instalada (C:\\<carpeta>), para aceptarla en vez del CD
  binarios    los ficheros que la plataforma reconstruye en memoria (nombre, tamaño, SHA-256) y dónde están
  necesita_cd si con la carpeta instalada aún hace falta el CD (D:)
  solo_instalada  la instalación no se reproduce desde el CD (InstallShield, parches): hace falta la carpeta instalada
"""
import hashlib
import json
import os
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORE = os.path.join(ROOT, "core")
SALIDA = os.path.join(ROOT, "app", "versiones.json")
CACHE = os.path.join(CORE, "build", "firmas-cache.json")
H = os.path.expanduser("~")

# lo que no se saca de juego.json: texto para el usuario (anio, corto, portada —carátula de app/assets/portadas— y etiqueta van en la tarjeta), discos y cómo se
# instala desde cada uno
VERSIONES = [
    ("pcf4", dict(detalle="versión 4.1 del CD (MS-DOS)", anio=1995, corto="4.0", portada="1995.jpg", necesita_cd=False,
                  discos=[dict(rol="cd", nombre="CD de PC Fútbol 4.0", iso="pcf4-work/iso", instalar=[["arbol", "FILES", ""]])])),
    ("pcf5", dict(detalle="diciembre de 1996", anio=1996, corto="5.0", portada="1996.jpg",
                  discos=[dict(rol="cd", nombre="CD de PC Fútbol 5.0", iso="pcf5-work/iso", instalar="nfo")])),
    ("pcf5oro", dict(detalle="Edición de Oro, octubre de 1997", anio=1997, corto="5.0 ORO", portada="1996.jpg", etiqueta="Edición de Oro",
                     discos=[dict(rol="cd", nombre="CD de PC Fútbol 5.0 Edición de Oro", iso="pcf5oro-work/iso", instalar="nfo")])),
    ("pcf5arg", dict(detalle="Apertura 97, edición argentina (noviembre de 1997)", anio=1997, corto="5.0 ARG", portada="1996.jpg", etiqueta="Argentina",
                     discos=[dict(rol="cd", nombre="CD de PC Fútbol 5.0 Apertura 97", iso="pcf5arg-work/iso", instalar="nfo")])),
    ("pcf6", dict(detalle="diciembre de 1997", anio=1997, corto="6.0", portada="1997.jpg",
                  discos=[dict(rol="cd", nombre="CD de PC Fútbol 6.0", iso="pcf6-work/iso", instalar="nfo")])),
    ("pcf6oro", dict(detalle="Edición de Oro (recopilatorio), noviembre de 1999", anio=1999, corto="6.0 ORO", portada="1997.jpg", etiqueta="Edición de Oro",
                     discos=[dict(rol="cd", nombre="CD de PC Fútbol 6.0 Edición de Oro", iso="pcf6oro-work/iso", instalar="nfo")])),
    ("pcf6arg", dict(detalle="Apertura 98, edición argentina (1998)", anio=1998, corto="6.0 ARG", portada="1997.jpg", etiqueta="Argentina",
                     discos=[dict(rol="cd", nombre="CD de PC Fútbol 6.0 Apertura 98", iso="pcf6arg-work/iso", instalar="nfo")])),
    ("pcf7", dict(detalle="CD del 28 de diciembre de 1998", anio=1998, corto="7", portada="1998.jpg",
                  discos=[dict(rol="cd", nombre="CD de PC Fútbol 7", iso="pcf7-work/iso", instalar="nfc")])),
    ("pcf7plus", dict(detalle="PC Fútbol 7 con el CD del Plus (septiembre de 1999)", anio=1999, corto="7 PLUS", portada="1998.jpg", etiqueta="Plus", extra="pcf7plus",
                      extra_copias=[["SIMULDAT/TEXTURAS/OTROS/vallas.bmp", "SIMULDAT/TEXTURAS/OTROS/vallas%s.bmp" % x] for x in ("esp", "mun", "bar", "arg", "fra", "ita", "ing")],
                      discos=[dict(rol="cd", nombre="CD de PC Fútbol 7", iso="pcf7-work/iso", instalar="nfc"),
                              dict(rol="cd2", nombre="CD de PC Fútbol 7 Plus", iso="pcf7plus-work/iso_plus", instalar="nfo")])),
    ("pcf2000", dict(detalle="con la Extensión (InstallShield)", anio=1999, corto="2000", portada="2000.jpg",
                     nota="Hacen falta los dos CD: el de PC Fútbol 2000 y el de su Extensión (valen sus ISO). Si ya lo tienes "
                          "instalado, también vale la carpeta C:\\PCF2000 de la instalación con la Extensión, junto con el CD de PC Fútbol 2000.",
                     # la instalación de InstallShield 5 de los dos CD (base y Extensión), deducida comparando con una instalación
                     # real en Windows 98: grupos de cada CAB a su carpeta, y la Utils.dll con MMX de cada CD (el instalador la
                     # copiaba si la CPU lo tenía: siempre, aquí)
                     discos=[dict(rol="cd", nombre="CD de PC Fútbol 2000", iso="pcf2000src-work/iso",
                                  instalar=[["is6", "Program DLLs", ""], ["is6", "Program Executable Files", ""], ["is6", "Help Files", "AYUDA"],
                                            ["copia", "MMX/UTILS.DLL", "Utils.dll"],
                                            ["carpeta", "Goles"], ["carpeta", "GOLESEXT"], ["carpeta", "Notas"], ["carpeta", "Save"], ["carpeta", "SAVEEXT"]],
                                  sistema_is6="Shared DLLs"),
                             # solo para instalar (el juego no lo lee como D:); con la carpeta instalada no hace falta
                             dict(rol="ext", nombre="CD de la Extensión de PC Fútbol 2000", iso="pcf2000src-work/iso_ext", solo_instalar=True,
                                  instalar=[["is6", "DataBase Files", "DBDAT"], ["is6", "Dll Extension", "Extension"], ["is6", "Exe Extension", "Extension"],
                                            ["is6", "SimulDat", "Simuldat"], ["is6", "Graphics Files", ""], ["is6", "Root Extension", ""],
                                            ["is6", "Help Files", "AYUDA"], ["copia", "MMX/Utils.dll", "Extension/Utils.dll"]])])),
    ("pcf2001", dict(detalle="con las 13 actualizaciones oficiales (versión 3.6)", anio=2000, corto="2001", portada="2001.jpg",
                     nota="Hacen falta el CD y la carpeta ACTUALIZ con las 13 actualizaciones oficiales (PCFACT01 a PCFACT12 y PCFPATCH; "
                          "vale su ISO). La app reproduce la instalación de InstallShield y aplica las actualizaciones con el parcheador "
                          "original (RTPatch) traducido.",
                     discos=[dict(rol="cd", nombre="CD de PC Fútbol 2001", iso="pcf2001src-work/iso",
                                  # la instalación de InstallShield 6 (lo que SETUP.INX hacía con DATA1.CAB y el CD), deducida comparando
                                  # con una instalación real: grupos del CAB a la raíz y carpetas del CD
                                  instalar=[["is6", "Exe & Dll", ""], ["is6", "Root", ""], ["arbol", "DBDAT", "DBDAT"], ["arbol", "SIMULDAT", "Simuldat"],
                                            ["arbol", "SFX", "SFX"], ["copia", "GRAFICOS.PAK", "GRAFICOS.PAK"], ["copia", "RC_DBASE.PAK", "rc_dbase.pak"],
                               ["carpeta", "Goles"], ["carpeta", "Notas"], ["carpeta", "Save/actual"], ["carpeta", "Save/Manager"]],
                                  sistema_is6="Shared DLLs")],
                     # las 13 actualizaciones oficiales, en su orden (cada EXE es un paquete con el .RTP y el parcheador); la 05
                     # trae además la Utils.dll con MMX, que el parcheador original copiaba si la CPU lo tenía (siempre, aquí), y al
                     # acabar queda update.ini con la versión
                     parches=dict(nombre="la carpeta ACTUALIZ con las 13 actualizaciones oficiales (o su ISO)", carpeta="pcfpatch-work/actualiz",
                                  orden=[("PCFACT01.exe", "PCFACT01.RTP"), ("PCFACT02.exe", "PCFACT02.RTP"), ("PCFACT03.exe", "PCFACT03.RTP"),
                                         ("PCFACT04.exe", "PCFACT04.RTP"), ("PCFPATCH.EXE", "PCFPATCH36.RTP"), ("PCFACT05.exe", "PCFACT05.RTP"),
                                         ("PCFACT06.exe", "PCFACT06.RTP"), ("PCFACT07.exe", "PCFACT07.RTP"), ("PCFACT08.exe", "PCFACT08.RTP"),
                                         ("PCFACT09.exe", "PCFACT09.RTP"), ("PCFACT10.exe", "PCFACT10.RTP"), ("PCFACT11.exe", "PCFACT11.RTP"),
                                         ("PCFACT12.exe", "PCFACT12.RTP")],
                                  copias={"PCFPATCH.EXE": [["UtilsMMX.dll", "Utils.dll"]]},
                                  escribe=[["update.ini", "V3.6\r\n"]]))),
]
NO_SE_INSTALA = {"proquini"}   # programas que la app no lanza (ProQuinielas: el lanzador del juego no lo abre)

# ---------------------------------------------------------------- utilidades
_cache = {}


def sha(p):
    st = os.stat(p)
    k = (p, st.st_size, int(st.st_mtime))
    c = _cache.get(p)
    if c and c[0] == st.st_size and c[1] == int(st.st_mtime):
        return c[2]
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    _cache[p] = (st.st_size, int(st.st_mtime), h.hexdigest())
    return _cache[p][2]


def resuelve(base, rel):
    """ruta real bajo base sin distinguir mayúsculas (None si no existe)"""
    p = base
    for parte in rel.replace("\\", "/").split("/"):
        if not parte:
            continue
        try:
            nombres = os.listdir(p)
        except (NotADirectoryError, FileNotFoundError):
            return None
        m = next((n for n in nombres if n.lower() == parte.lower()), None)
        if m is None:
            return None
        p = os.path.join(p, m)
    return p


def ficheros(raiz):
    out = {}
    for d, _, fs in os.walk(raiz):
        for f in fs:
            p = os.path.join(d, f)
            out[os.path.relpath(p, raiz)] = os.path.getsize(p)
    return out


def cpr_miembros(path):
    """[(nombre, tamaño, crc)] de un contenedor .CPR (entradas ZIP locales, sin directorio central)"""
    data = open(path, "rb").read()
    off, out = 0, []
    while off + 30 <= len(data) and data[off:off + 4] == b"PK\x03\x04":
        (_, _, flags, method, _, _, crc, csize, usize, nlen, xlen) = struct.unpack_from("<4sHHHHHIIIHH", data, off)
        name = data[off + 30:off + 30 + nlen].decode("cp850")
        if not name.endswith(("/", "\\")):
            out.append((name.replace("\\", "/"), usize, crc))
        off = off + 30 + nlen + xlen + csize
    return out


def lee_guion(path):
    """líneas de un .nfo (cp1252) o .NFC (cifrado: u16 longitud + bytes XOR 0xA5)"""
    d = open(path, "rb").read()
    if path.lower().endswith(".nfc"):
        o, lineas = 0, []
        while o + 2 <= len(d):
            n = struct.unpack_from("<H", d, o)[0]
            o += 2
            lineas.append(bytes(b ^ 0xA5 for b in d[o:o + n]).decode("cp1252", "replace"))
            o += n
        return lineas
    return d.decode("cp1252", "replace").splitlines()


def simula_guion(cd, path):
    """qué deja un guion del instalador: {destino relativo (minúsculas): (origen en el CD, miembro de CPR o None)}"""
    out = {}
    for linea in lee_guion(path):
        linea = linea.strip()
        if not linea or linea.startswith("*"):
            continue
        orden, resto = linea[0].upper(), linea[1:].strip()
        if orden == "D" or orden not in "FX":
            continue
        src, _, dst = (x.strip() for x in resto.partition(">"))
        dst = (dst or src).replace("\\", "/")
        real = resuelve(cd, src)
        if not real:
            continue
        if src.lower().endswith(".cpr"):
            carpeta = "" if orden == "F" else os.path.splitext(os.path.basename(dst))[0]
            for nombre, usize, crc in cpr_miembros(real):
                out[(os.path.join(carpeta, nombre)).lower()] = (os.path.relpath(real, cd), nombre)
            continue
        if os.path.isdir(real):
            for raiz, _, fs in os.walk(real):
                for f in fs:
                    o = os.path.join(raiz, f)
                    out[os.path.join(dst, os.path.relpath(o, real)).lower()] = (os.path.relpath(o, cd), None)
        else:
            out[dst.lower()] = (os.path.relpath(real, cd), None)
    return out


def guiones_del_cd(cd):
    """los guiones del instalador en la raíz del CD (INST_1xx, Ins6_1xx, InsF_…: por edición), en orden de nombre"""
    return sorted((n for n in os.listdir(cd) if n.lower().endswith((".nfo", ".nfc")) and "_" in n), key=str.lower)


# ---------------------------------------------------------------- por versión
def version(jid, cfg):
    J = json.load(open(os.path.join(CORE, "juegos", jid, "juego.json"), encoding="utf-8"))
    W = os.path.join(H, os.path.basename(os.path.expanduser(J["trabajo"])))
    game = os.path.join(W, "game", J["carpeta"])
    gf = ficheros(game)
    gl = {k.lower(): k for k in gf}
    principal = J["principal"].lower()
    # los programas: el principal y los que tienen C generado (build/<id>/gen-<programa>)
    gen = os.path.join(CORE, "build") if jid == "pcf7" else os.path.join(CORE, "build", jid)
    otros = sorted(n[4:] for n in os.listdir(gen) if n.startswith("gen-") and os.path.exists(os.path.join(gen, n, "funcs.h"))
                   and n[4:] not in NO_SE_INSTALA)
    programas = [jid] + [f"{jid}-{p}" for p in otros]
    v = dict(id=jid, nombre=J["nombre"], detalle=cfg.get("detalle", ""), anio=cfg.get("anio", 0), corto=cfg.get("corto", ""),
             portada=cfg.get("portada", ""), etiqueta=cfg.get("etiqueta", ""),
             carpeta=J["carpeta"], programas=programas,
             dos=bool(J.get("dos")), necesita_cd=cfg.get("necesita_cd", True), solo_instalada=cfg.get("solo_instalada", False))
    if cfg.get("nota"):
        v["nota"] = cfg["nota"]
    if cfg.get("extra"):
        v["extra"] = cfg["extra"]
        v["extra_copias"] = cfg.get("extra_copias", [])   # (origen, destino) dentro de la instalación, tras copiar extra
    # ---- binarios: los módulos que la plataforma reconstruye en memoria: el EXE de cada programa y las DLL (bin/)
    bindir = os.path.join(W, "bin")
    binarios = []
    for n in [principal] + [p + ".exe" for p in otros] + [d for d, _ in J["dlls"]]:
        p = resuelve(bindir, n)
        if not p:
            raise SystemExit(f"{jid}: falta {n} en {bindir}")
        mapa = p + ".le.map.json"
        if os.path.exists(mapa):   # 4.0: el PE es la conversión del LE original (.DAT)
            mp = json.load(open(mapa))
            binarios.append(dict(nombre=mp["origen"], tam=mp["tam"], sha256=mp["sha256"]))
        else:
            binarios.append(dict(nombre=os.path.basename(p), tam=os.path.getsize(p), sha256=sha(p)))
    # ---- discos: primero se simulan todos los guiones de todos los discos (en orden), para saber qué tapa qué
    cds = [os.path.join(H, d["iso"]) for d in cfg["discos"]]
    todos = []   # (índice de disco, guion, salidas)
    for di, d in enumerate(cfg["discos"]):
        if d.get("instalar") in ("nfo", "nfc"):
            for g in guiones_del_cd(cds[di]):
                if g.lower().endswith("." + d["instalar"]):
                    sim = simula_guion(cds[di], os.path.join(cds[di], g))
                    if sim and not all("proquini" in k for k in sim):
                        todos.append((di, g, sim))
    final = {}   # destino → (cd, (origen, miembro)) tras los guiones elegidos, en orden
    elegidos = {di: [] for di in range(len(cfg["discos"]))}
    for i, (di, g, sim) in enumerate(todos):
        cd = cds[di]
        hay = [k for k in sim if k in gl]
        sobra = [k for k in sim if k not in gl]
        # ficheros distintos de los de la instalación valen si un guion posterior (sin sobras) los vuelve a dejar
        tapados = set()
        for dj, g2, sim2 in todos[i + 1:]:
            if all(k in gl for k in sim2):
                tapados |= set(sim2)
        distinto = [k for k in hay if sim[k][1] is None and k not in tapados
                    and sha(os.path.join(cd, sim[k][0])) != sha(os.path.join(game, gl[k]))]
        ok = len(hay) >= 0.8 * len(sim) and not sobra and not distinto
        nuevo = any(k not in final or final[k][1] != val for k, val in sim.items())
        motivo = "" if ok and nuevo else " (no aporta nada)" if ok else f" ({len(sobra)} que la instalación no tiene, {len(distinto)} distintos)"
        print(f"  {jid}: {g}: {len(sim)} salidas, {len(hay)} en la instalación → {'SÍ' if ok and nuevo else 'no'}{motivo}")
        if ok and nuevo:
            elegidos[di].append([cfg["discos"][di]["instalar"], g])
            for k, val in sim.items():
                final[k] = (cd, val)
    if final:   # comprobación: lo que dejan los guiones elegidos es lo que hay en la instalación de trabajo
        mal = [k for k, (cd, (src, miembro)) in final.items()
               if k in gl and miembro is None and sha(os.path.join(cd, src)) != sha(os.path.join(game, gl[k]))]
        faltan = [k for k in final if k not in gl]
        sin = [k for k in gl if k not in final and not k.startswith("proquini/")]
        print(f"  {jid}: guiones: {len(final)} ficheros; {len(mal)} distintos de la instalación, {len(faltan)} que la instalación no tiene, "
              f"{len(sin)} de la instalación que no salen de los guiones")
        for k in mal[:10]:
            print(f"     distinto: {k}")
        for k in faltan[:10]:
            print(f"     sobra: {k}")
        for k in sin[:12]:
            print(f"     falta: {k}")
    discos = []
    for di, d in enumerate(cfg["discos"]):
        cd = cds[di]
        cf = ficheros(cd)
        por_tam = {}
        for rel, tam in cf.items():
            por_tam.setdefault(tam, []).append(rel)

        def en_cd(tam, h):
            for rel in por_tam.get(tam, []):
                if sha(os.path.join(cd, rel)) == h:
                    return rel
            return None
        # binarios que están en este CD (por hash); las DLL se copian a WINDOWS\SYSTEM
        sistema = []
        en_este = {}
        for b in binarios:
            rel = en_cd(b["tam"], b["sha256"])
            if rel:
                en_este[b["nombre"].lower()] = rel
                b.setdefault("origen", []).append([d["rol"], rel])
                if b["nombre"].lower().endswith(".dll"):
                    sistema.append([rel, b["nombre"].upper()])
        # firmas: el identificador del disco, el programa principal si está en el CD y el primer guion
        raiz = [n for n in os.listdir(cd) if os.path.isfile(os.path.join(cd, n))]
        cand = [n for n in raiz if n.lower() == "disk.id"]
        if principal in en_este:
            cand.append(en_este[principal])
        elif binarios[0]["nombre"].lower() in en_este:
            cand.append(en_este[binarios[0]["nombre"].lower()])
        cand += [n for n in raiz if n.lower() in ("inst_100.nfo", "inst_100.nfc", "install.dat", "layout.bin", "autorun.inf")]
        firmas = []
        for r in cand:
            if len(firmas) < 3 and r not in [f[0] for f in firmas]:
                firmas.append([r, sha(os.path.join(cd, r)), os.path.getsize(os.path.join(cd, r))])
        inst = d.get("instalar")
        if d.get("sistema_is6"):   # las DLL compartidas van dentro del CAB de InstallShield: el grupo entero a WINDOWS\SYSTEM
            sistema = [["is6:" + d["sistema_is6"], ""]]
        if d.get("solo_instalar"):   # un disco que solo hace falta para instalar: nada a D: ni a WINDOWS\SYSTEM
            sistema = []
        disco = dict(rol=d["rol"], nombre=d["nombre"], firmas=firmas,
                     instalar=elegidos[di] if inst in ("nfo", "nfc") else (inst or []),
                     copiar=[] if d.get("solo_instalar") else J.get("web", {}).get("cd", "*") if d["rol"] == "cd" else "*", sistema=sistema)
        if d.get("solo_instalar"):
            disco["solo_instalar"] = True
        discos.append(disco)
    v["discos"] = discos
    if cfg.get("parches"):   # las actualizaciones oficiales: nombre, SHA-256 y tamaño de cada paquete, y qué hacer con cada uno
        pc = cfg["parches"]
        pdir = os.path.join(H, pc["carpeta"])
        lista_p = []
        for exe, rtp in pc["orden"]:
            r = resuelve(pdir, exe)
            if not r:
                print(f"  {jid}: AVISO: falta {exe} en {pdir}")
                continue
            lista_p.append(dict(nombre=exe, sha256=sha(r), tam=os.path.getsize(r), rtp=rtp, copias=pc.get("copias", {}).get(exe, [])))
        v["parches"] = dict(nombre=pc["nombre"], ficheros=lista_p, escribe=pc.get("escribe", []))
    # binarios que deja la instalación (o que vienen en ella) y los de WINDOWS\SYSTEM
    gpor_tam = {}
    for rel, tam in gf.items():
        gpor_tam.setdefault(tam, []).append(rel)
    dc = os.path.join(W, "drive_c")
    for b in binarios:
        for rel in gpor_tam.get(b["tam"], []):
            if sha(os.path.join(game, rel)) == b["sha256"]:
                b.setdefault("origen", []).append(["instalacion", rel])
                break
        r = resuelve(dc, "WINDOWS/SYSTEM/" + b["nombre"])
        if r and os.path.getsize(r) == b["tam"] and sha(r) == b["sha256"]:
            b.setdefault("origen", []).append(["sistema", os.path.basename(r)])
        if not any(o[0] != "sistema" for o in b.get("origen", [])):
            print(f"  {jid}: AVISO: {b['nombre']} no está en ningún CD ni en la instalación: el usuario tendrá que aportarlo")
    v["binarios"] = binarios
    # firmas de una carpeta instalada: el programa principal y el siguiente binario que deje la instalación
    inst_firmas = []
    for b in binarios:
        o = next((x for x in b.get("origen", []) if x[0] == "instalacion"), None)
        if o and len(inst_firmas) < 2:
            inst_firmas.append([o[1], b["sha256"], b["tam"]])
    v["instalada"] = dict(firmas=inst_firmas)
    return v


def main():
    global _cache
    if os.path.exists(CACHE):
        _cache = {k: tuple(v) for k, v in json.load(open(CACHE)).items()}
    solo = set(sys.argv[1:])
    actual = {}
    if os.path.exists(SALIDA):
        actual = {v["id"]: v for v in json.load(open(SALIDA, encoding="utf-8"))["versiones"]}
    out = []
    for jid, cfg in VERSIONES:
        if solo and jid not in solo:
            if jid in actual:
                out.append(actual[jid])
            continue
        print(f"== {jid}")
        t = time.time()
        out.append(version(jid, cfg))
        print(f"   {time.time() - t:.0f} s")
        json.dump({k: list(v) for k, v in _cache.items()}, open(CACHE, "w"))
    with open(SALIDA, "w", encoding="utf-8") as f:
        json.dump({"generado": time.strftime("%Y-%m-%d"), "versiones": out}, f, ensure_ascii=False, indent=1)
    print(f"{SALIDA}: {len(out)} versiones")


if __name__ == "__main__":
    main()
