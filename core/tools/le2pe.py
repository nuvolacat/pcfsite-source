#!/usr/bin/env python3
"""Convierte un ejecutable LE de DOS4GW (Watcom, modelo plano de 32 bits) en un PE equivalente, para que el traductor
(recomp/, hecho para PE) y Ghidra lo traten como un programa más.

  python3 tools/le2pe.py ENTRADA.DAT SALIDA.exe

- Cada objeto del LE es una sección; el PE va en 0x400000 y los objetos se colocan seguidos desde 0x401000 (en el LE
  el objeto de datos suele ir en 0xA0000, que en un PC es la memoria de vídeo VGA: no puede quedarse ahí).
- Las reparaciones de 32 bits (tipo 7) se aplican y además se dejan como reubicaciones HIGHLOW del PE.
- Las de selector (tipo 2) llevan el selector plano de datos (SELECTOR_DATOS): en el modelo plano todos los
  segmentos son la misma memoria.
- Los objetos de 16 bits (sin el bit «big») se marcan como datos: el traductor solo sabe de código de 32 bits.
- En el PE no hay importaciones: el programa habla con DOS/DPMI por interrupciones (lo atiende platform/).
Además escribe SALIDA.le.txt con lo que hace falta saber en ejecución (pila inicial, objetos) y SALIDA.le.map.json con
qué trozos del LE original van a qué dirección (recomp/build.py lo usa para la receta de la imagen de memoria, imagen.c:
la app reconstruye la memoria con el fichero original del usuario, sin llevar mem.bin)."""
import hashlib
import json
import os
import struct
import sys

BASE = 0x400000
SELECTOR_CODIGO = 0x0008   # selectores planos que da la capa DOS (platform)
SELECTOR_DATOS = 0x0010


def lee_le(d):
    le = struct.unpack_from("<I", d, 0x3C)[0]
    if d[le:le + 2] not in (b"LE", b"LX"):
        le = d.find(b"LE\x00\x00")
    if le < 0:
        raise SystemExit("no es un ejecutable LE")
    h = lambda o, f="<I": struct.unpack_from(f, d, le + o)[0]
    info = dict(le=le, npages=h(0x14), eip_obj=h(0x18), eip=h(0x1C), esp_obj=h(0x20), esp=h(0x24), pgsz=h(0x28),
                lastpg=h(0x2C), objtab=le + h(0x40), nobj=h(0x44), pgmap=le + h(0x48), fixpg=le + h(0x68),
                fixrec=le + h(0x6C), datapg=h(0x80))
    objs = []
    for i in range(info["nobj"]):
        vsz, base, fl, pidx, pcnt, _ = struct.unpack_from("<6I", d, info["objtab"] + i * 24)
        objs.append(dict(vsz=vsz, base=base, flags=fl, pidx=pidx, pcnt=pcnt))
    return info, objs


def pagina(d, info, p):
    """Contenido de la página p (1…n) del fichero (la última puede ser más corta)."""
    ent = d[info["pgmap"] + (p - 1) * 4: info["pgmap"] + p * 4]
    num = (ent[0] << 16) | (ent[1] << 8) | ent[2]   # LE: número de página de 24 bits, big endian, y un byte de tipo
    tipo = ent[3]
    if tipo not in (0,):
        raise SystemExit(f"página {p}: tipo {tipo} no soportado")
    off = info["datapg"] + (num - 1) * info["pgsz"]
    n = info["lastpg"] if num == info["npages"] else info["pgsz"]
    return d[off:off + n]


def pagina_pos(d, info, p):
    """(desplazamiento en el fichero, bytes) de la página p."""
    ent = d[info["pgmap"] + (p - 1) * 4: info["pgmap"] + p * 4]
    num = (ent[0] << 16) | (ent[1] << 8) | ent[2]
    off = info["datapg"] + (num - 1) * info["pgsz"]
    n = info["lastpg"] if num == info["npages"] else info["pgsz"]
    return off, min(n, len(d) - off)


def fixups(d, info, objs):
    """(objeto origen, desplazamiento en el objeto, tipo, objeto destino, desplazamiento destino), sin repetir las que
    cruzan de página (salen en las dos)."""
    def obj_de_pagina(p):
        for i, o in enumerate(objs):
            if o["pidx"] <= p < o["pidx"] + o["pcnt"]:
                return i + 1, (p - o["pidx"]) * info["pgsz"]
        raise SystemExit(f"página {p} fuera de los objetos")

    vistas = {}
    for p in range(1, info["npages"] + 1):
        a = struct.unpack_from("<I", d, info["fixpg"] + 4 * (p - 1))[0]
        b = struct.unpack_from("<I", d, info["fixpg"] + 4 * p)[0]
        i, e = info["fixrec"] + a, info["fixrec"] + b
        sobj, sbase = obj_de_pagina(p)
        while i < e:
            src, flg = d[i], d[i + 1]
            i += 2
            if src & 0x20:
                cnt = d[i]
                i += 1
            else:
                srcoff = struct.unpack_from("<h", d, i)[0]
                i += 2
            if flg & 3:
                raise SystemExit(f"reparación con destino externo (flags {flg:x}): no soportada")
            if flg & 0x40:
                tobj = struct.unpack_from("<H", d, i)[0]
                i += 2
            else:
                tobj = d[i]
                i += 1
            st = src & 0xF
            tgt = 0
            if st != 2:
                if flg & 0x10:
                    tgt = struct.unpack_from("<I", d, i)[0]
                    i += 4
                else:
                    tgt = struct.unpack_from("<H", d, i)[0]
                    i += 2
            offs = struct.unpack_from("<%dh" % cnt, d, i) if src & 0x20 else (srcoff,)
            if src & 0x20:
                i += 2 * cnt
            if st not in (2, 7):
                raise SystemExit(f"reparación de tipo {st:x} no soportada")
            for o in offs:
                vistas[(sobj, sbase + o)] = (st, tobj, tgt)
    return [(so, off, st, to, tg) for (so, off), (st, to, tg) in sorted(vistas.items())]


def main():
    ent, sal = sys.argv[1], sys.argv[2]
    d = open(ent, "rb").read()
    info, objs = lee_le(d)
    # colocación: seguidos desde 0x1000, alineados a 4 KiB
    rva = 0x1000
    copias = []   # (desplazamiento en el fichero LE, RVA, bytes): de dónde sale cada trozo de la imagen
    for o in objs:
        o["rva"] = rva
        o["big"] = bool(o["flags"] & 0x2000)
        o["exec"] = bool(o["flags"] & 0x4)
        datos = b"".join(pagina(d, info, o["pidx"] + k) for k in range(o["pcnt"]))
        o["img"] = bytearray(datos[:o["vsz"]])
        acc = 0
        for k in range(o["pcnt"]):
            off, n = pagina_pos(d, info, o["pidx"] + k)
            toma = min(n, o["vsz"] - acc)
            if toma > 0:
                copias.append((off, rva + acc, toma))
            acc += n
        rva += (o["vsz"] + 0xFFF) & ~0xFFF
    tam_img = rva

    fx = fixups(d, info, objs)
    relocs = []
    for so, off, st, to, tg in fx:
        o = objs[so - 1]
        if off < 0 or off >= len(o["img"]):
            continue   # cae en la parte sin datos (bss): no hay nada que reparar
        if st == 7:
            v = BASE + objs[to - 1]["rva"] + tg
            if off + 4 <= len(o["img"]):
                struct.pack_into("<I", o["img"], off, v)
                relocs.append(o["rva"] + off)
            else:   # al final de los datos del objeto: lo que quepa
                o["img"][off:] = struct.pack("<I", v)[:len(o["img"]) - off]
        else:
            if off + 2 <= len(o["img"]):
                sel = SELECTOR_CODIGO if objs[to - 1]["exec"] and objs[to - 1]["big"] else SELECTOR_DATOS
                struct.pack_into("<H", o["img"], off, sel)

    # reubicaciones del PE (bloques por página de 4 KiB)
    bloques = {}
    for r in relocs:
        bloques.setdefault(r & ~0xFFF, []).append(r & 0xFFF)
    reloc = bytearray()
    for pg in sorted(bloques):
        ents = [(3 << 12) | x for x in sorted(bloques[pg])]
        if len(ents) % 2:
            ents.append(0)
        reloc += struct.pack("<II", pg, 8 + 2 * len(ents)) + struct.pack("<%dH" % len(ents), *ents)
    reloc_rva = tam_img
    tam_img += (len(reloc) + 0xFFF) & ~0xFFF

    secciones = []
    for i, o in enumerate(objs):
        nombre = b".text" if o["exec"] and o["big"] else (b".t16" if o["exec"] else b".data")
        if nombre in [s[0] for s in secciones]:
            nombre = nombre[:5] + b"%d" % (i + 1)
        ch = (0x60000020 if o["exec"] and o["big"] else 0xC0000040)   # código: lectura/ejecución; resto: datos
        secciones.append((nombre, o["vsz"], o["rva"], bytes(o["img"]), ch))
    secciones.append((b".reloc", len(reloc), reloc_rva, bytes(reloc), 0x42000040))

    # PE: cabecera DOS mínima + NT + secciones; datos alineados a 0x200 en el fichero
    nsec = len(secciones)
    cab = 0x40 + 4 + 20 + 224 + 40 * nsec
    falin = 0x200
    pos = (cab + falin - 1) // falin * falin
    raw = []
    for nombre, vsz, srva, img, ch in secciones:
        tam = (len(img) + falin - 1) // falin * falin
        raw.append((pos, tam))
        pos += tam
    out = bytearray(pos)
    out[0:2] = b"MZ"
    struct.pack_into("<I", out, 0x3C, 0x40)
    out[0x40:0x44] = b"PE\x00\x00"
    struct.pack_into("<HHIIIHH", out, 0x44, 0x14C, nsec, 0, 0, 0, 224, 0x0102 | 0x0020)
    eobj = objs[info["eip_obj"] - 1]
    entrada = eobj["rva"] + info["eip"]
    code = next(o for o in objs if o["exec"] and o["big"])
    opt = struct.pack("<HBBIIIIIII", 0x10B, 6, 0, len(code["img"]), 0, 0, entrada, code["rva"], 0, BASE)
    opt += struct.pack("<IIHHHHHHIIIIHHIIIIII", 0x1000, falin, 4, 0, 0, 0, 4, 0, 0, tam_img,
                       (cab + falin - 1) // falin * falin, 0, 3, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    dirs = [(0, 0)] * 16
    dirs[5] = (reloc_rva, len(reloc))
    opt += b"".join(struct.pack("<II", a, b) for a, b in dirs)
    out[0x58:0x58 + len(opt)] = opt
    p = 0x58 + 224
    for (nombre, vsz, srva, img, ch), (fpos, ftam) in zip(secciones, raw):
        struct.pack_into("<8sIIIIIIHHI", out, p, nombre, vsz, srva, ftam, fpos, 0, 0, 0, 0, ch)
        out[fpos:fpos + len(img)] = img
        p += 40
    open(sal, "wb").write(out)

    sobj = objs[info["esp_obj"] - 1]
    with open(sal + ".le.txt", "w") as f:
        f.write(f"entrada 0x{BASE + entrada:x}\n")
        f.write(f"pila 0x{BASE + sobj['rva'] + info['esp']:x}\n")
        for i, o in enumerate(objs):
            f.write(f"objeto {i + 1} 0x{o['base']:x} -> 0x{BASE + o['rva']:x} tam 0x{o['vsz']:x} "
                    f"{'código32' if o['exec'] and o['big'] else 'código16' if o['exec'] else 'datos'}\n")
        # punteros a código guardados en los datos (tablas de inicialización de Watcom, punteros a funciones…):
        # entradas de función que solo se llaman por puntero (el traductor las añade a las de Ghidra)
        codigo = {i + 1 for i, o in enumerate(objs) if o["exec"] and o["big"]}
        dests = sorted({BASE + objs[to - 1]["rva"] + tg for so, off, st, to, tg in fx
                        if st == 7 and to in codigo and so not in codigo})
        for v in dests:
            f.write(f"puntero 0x{v:x}\n")
        # y los guardados dentro del propio código (tablas de manejadores de Watcom; también tablas de saltos, que
        # el traductor descarta porque caen dentro de una función conocida)
        dcod = sorted({BASE + objs[to - 1]["rva"] + tg for so, off, st, to, tg in fx
                       if st == 7 and to in codigo and so in codigo})
        for v in dcod:
            f.write(f"puntero_cod 0x{v:x}\n")
    with open(sal + ".le.map.json", "w") as f:
        json.dump({"origen": os.path.basename(ent).upper(), "ruta": os.path.abspath(ent), "tam": len(d),
                   "sha256": hashlib.sha256(d).hexdigest(), "copias": copias}, f)
    n7 = sum(1 for x in fx if x[2] == 7)
    print(f"{ent} -> {sal}: {len(objs)} objetos, {n7} reparaciones de 32 bits ({len(relocs)} en el PE), "
          f"{len(fx) - n7} de selector; entrada 0x{BASE + entrada:x}")


if __name__ == "__main__":
    main()
