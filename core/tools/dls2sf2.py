#!/usr/bin/env python3
"""Convierte un banco DLS nivel 1 (p. ej. el banco General MIDI/GS de Windows o macOS) en una SoundFont 2 para
TinySoundFont (platform/src/dmusic.cpp).

  python3 tools/dls2sf2.py BANCO.dls SALIDA.sf2

DLS: instrumentos (insh: banco/programa; bit 31 del banco = percusión) con regiones (rgnh: rangos de tecla y
velocidad; wsmp: nota raíz, afinación, ganancia y bucle; wlnk: onda) y articulación (art1: bloques de conexión con
envolventes EG1/EG2 y LFO en 16.16). SF2: cada instrumento DLS → un preset + un instrumento con una zona por región;
las muestras se concatenan en sdta (16 bits mono)."""
import os
import struct
import sys

SKIP = os.environ.get("DLS2SF2_SKIP", "")

# ---------------------------------------------------------------- lectura RIFF

def chunks(d, off, end):
    while off + 8 <= end:
        tag = d[off:off + 4]
        size = struct.unpack_from("<I", d, off + 4)[0]
        body = off + 8
        if tag in (b"RIFF", b"LIST"):
            yield d[body:body + 4], body + 4, body + size
        else:
            yield tag, body, body + size
        off = body + size + (size & 1)


def parse_art1(d, off, end):
    """Bloques de conexión → dict (fuente, control, destino) → lScale (16.16)."""
    cb, n = struct.unpack_from("<II", d, off)
    out = {}
    p = off + cb
    for _ in range(n):
        src, ctl, dst, trn, scale = struct.unpack_from("<HHHHi", d, p)
        out[(src, ctl, dst)] = scale
        p += 12
    return out


def parse_wsmp(d, off):
    cb, unity, fine, gain, opts, nloops = struct.unpack_from("<IHhiII", d, off)
    loops = []
    p = off + cb
    for _ in range(nloops):
        lcb, ltype, lstart, llen = struct.unpack_from("<IIII", d, p)
        loops.append((ltype, lstart, llen))
        p += lcb
    return unity, fine, gain, loops


def parse_dls(data):
    assert data[:4] == b"RIFF" and data[8:12] == b"DLS ", "no es un DLS"
    ins_list, ptbl, wvpl = [], None, None
    for tag, a, b in chunks(data, 12, len(data)):
        if tag == b"lins":
            for t2, a2, b2 in chunks(data, a, b):
                if t2 == b"ins ":
                    ins_list.append(parse_instrument(data, a2, b2))
        elif tag == b"ptbl":
            cb, n = struct.unpack_from("<II", data, a)
            ptbl = list(struct.unpack_from("<%dI" % n, data, a + cb))
        elif tag == b"wvpl":
            wvpl = a
    waves = []
    for o in ptbl:
        waves.append(parse_wave(data, wvpl + o))
    return ins_list, waves


def parse_instrument(d, a, b):
    ins = {"regions": [], "art": {}, "name": ""}
    for tag, x, y in chunks(d, a, b):
        if tag == b"insh":
            nreg, bank, patch = struct.unpack_from("<III", d, x)
            ins["bank"], ins["patch"] = bank, patch
        elif tag == b"lrgn":
            for t2, x2, y2 in chunks(d, x, y):
                if t2 in (b"rgn ", b"rgn2"):
                    ins["regions"].append(parse_region(d, x2, y2))
        elif tag in (b"lart", b"lar2"):
            for t2, x2, y2 in chunks(d, x, y):
                if t2 in (b"art1", b"art2"):
                    ins["art"].update(parse_art1(d, x2, y2))
        elif tag == b"INFO":
            for t2, x2, y2 in chunks(d, x, y):
                if t2 == b"INAM":
                    ins["name"] = d[x2:y2].split(b"\0")[0].decode("latin-1")
    return ins


def parse_region(d, a, b):
    r = {"wsmp": None, "art": {}}
    for tag, x, y in chunks(d, a, b):
        if tag == b"rgnh":
            klo, khi, vlo, vhi, opts, keygroup = struct.unpack_from("<HHHHHH", d, x)
            r.update(klo=klo, khi=khi, vlo=vlo, vhi=vhi, keygroup=keygroup)
        elif tag == b"wsmp":
            r["wsmp"] = parse_wsmp(d, x)
        elif tag == b"wlnk":
            opts, phase, chan, idx = struct.unpack_from("<HHII", d, x)
            r["wave"] = idx
        elif tag in (b"lart", b"lar2"):
            for t2, x2, y2 in chunks(d, x, y):
                if t2 in (b"art1", b"art2"):
                    r["art"].update(parse_art1(d, x2, y2))
    return r


def parse_wave(d, a):
    assert d[a:a + 4] == b"LIST" and d[a + 8:a + 12] == b"wave"
    size = struct.unpack_from("<I", d, a + 4)[0]
    w = {"wsmp": None, "name": ""}
    for tag, x, y in chunks(d, a + 12, a + 8 + size):
        if tag == b"fmt ":
            fmt, ch, rate, _, align, bits = struct.unpack_from("<HHIIHH", d, x)
            w.update(fmt=fmt, channels=ch, rate=rate, bits=bits)
        elif tag == b"data":
            w["data"] = (x, y)
        elif tag == b"wsmp":
            w["wsmp"] = parse_wsmp(d, x)
        elif tag == b"INFO":
            for t2, x2, y2 in chunks(d, x, y):
                if t2 == b"INAM":
                    w["name"] = d[x2:y2].split(b"\0")[0].decode("latin-1")
    return w


# ---------------------------------------------------------------- escritura SF2

def gen(gens, op, val):
    gens.append((op, val & 0xFFFF))


def tc(scale, cero_instantaneo=False):   # 16.16 time cents → timecents SF2 (acotado); 0x80000000 y, en ataques, 0 = instantáneo
    if scale == -0x80000000 or (cero_instantaneo and scale == 0):
        return -12000
    v = scale / 65536.0
    return int(max(-12000, min(8000, round(v))))


def sustain_cb(scale):   # DLS: 0,1 % × 65536 → SF2: atenuación en cB respecto al nivel máximo
    if "sus100" in SKIP and scale == 0:
        return 0
    pct = scale / 65536.0 / 10.0
    if pct >= 100:
        return 0
    if pct <= 0:
        return 1440
    import math
    return int(min(1440, round(-200 * math.log10(pct / 100.0))))


def convert(dls_path, sf2_path):
    data = open(dls_path, "rb").read()
    ins_list, waves = parse_dls(data)
    # muestras: 16 bits mono, con 46 ceros de separación (lo exige SF2)
    sdta = bytearray()
    shdr = []
    for i, w in enumerate(waves):
        x, y = w["data"]
        raw = data[x:y]
        if w["bits"] == 8:
            pcm = bytearray()
            for b in raw:
                pcm += struct.pack("<h", (b - 128) << 8)
            raw = bytes(pcm)
        if w["channels"] == 2:   # a mono (media)
            n = len(raw) // 4
            mono = bytearray()
            for k in range(n):
                l, r = struct.unpack_from("<hh", raw, 4 * k)
                mono += struct.pack("<h", (l + r) // 2)
            raw = bytes(mono)
        start = len(sdta) // 2
        sdta += raw
        end = len(sdta) // 2
        sdta += b"\0" * 92
        unity, fine, gain, loops = w["wsmp"] if w["wsmp"] else (60, 0, 0, [])
        if loops:
            lt, ls, ll = loops[0]
            lstart, lend = start + ls, start + ls + ll
        else:
            lstart, lend = start, end
        semis = int(round(fine / 100.0))   # la afinación fina de SF2 va de -99 a 99 centésimas: el resto, en semitonos de la nota raíz
        shdr.append((w["name"][:19] or "s%d" % i, start, end, lstart, lend, w["rate"], (unity - semis) & 0xFF, fine - semis * 100, len(loops) > 0))

    phdr, pbag, pgen, inst, ibag, igen = [], [], [], [], [], []
    ins_list.sort(key=lambda i: ((i["bank"] >> 31) & 1, (i["bank"] >> 8) & 0x7F, i["patch"] & 0x7F))
    for ins in ins_list:
        drums = (ins["bank"] >> 31) & 1
        bank = 128 if drums else (ins["bank"] >> 8) & 0x7F
        patch = ins["patch"] & 0x7F
        name = (ins["name"] or "ins%d" % patch)[:19]
        phdr.append((name, patch, bank, len(pbag)))
        pbag.append((len(pgen), 0))
        gen(pgen, 41, len(inst))   # instrument
        inst.append((name, len(ibag)))
        # zona global del instrumento: articulación del instrumento
        gl = []
        art_to_gens(ins["art"], gl)
        if gl:
            ibag.append((len(igen), 0))
            igen.extend(gl)
        for r in ins["regions"]:
            gs = []
            gen(gs, 43, (r["khi"] << 8) | r["klo"])        # keyRange
            gen(gs, 44, (r["vhi"] << 8) | r["vlo"])        # velRange
            art_to_gens(r["art"], gs)
            wsmp = r["wsmp"] or waves[r["wave"]]["wsmp"]
            loop = False
            if wsmp:
                unity, fine, gain, loops = wsmp
                semis = int(round(fine / 100.0))
                gen(gs, 58, (unity - semis) & 0x7F)         # overridingRootKey (con los semitonos de la afinación)
                if fine - semis * 100:
                    gen(gs, 52, fine - semis * 100)         # fineTune (centésimas, -99..99)
                if gain < 0 and "atten" not in SKIP:
                    gen(gs, 48, min(1440, int(round(-gain / 65536.0))))   # lGain en 1/65536 de 0,1 dB → initialAttenuation (cB)
                if loops:
                    loop = True
                    lt, ls, ll = loops[0]
                    sh = shdr[r["wave"]]
                    for op_fino, op_grueso, delta in ((2, 45, sh[1] + ls - sh[3]), (3, 50, sh[1] + ls + ll - sh[4])):   # bucle propio de la región
                        if delta:
                            if delta // 32768: gen(gs, op_grueso, delta // 32768)   # *loopAddrsCoarseOffset (×32768)
                            gen(gs, op_fino, delta % 32768)                        # *loopAddrsOffset
            if r.get("keygroup"):
                gen(gs, 57, r["keygroup"])                  # exclusiveClass
            gen(gs, 54, 1 if loop else 0)                   # sampleModes
            gen(gs, 53, r["wave"])                          # sampleID (último)
            ibag.append((len(igen), 0))
            igen.extend(gs)
    phdr.append(("EOP", 0, 0, len(pbag)))
    pbag.append((len(pgen), 0))
    inst.append(("EOI", len(ibag)))
    ibag.append((len(igen), 0))
    shdr.append(("EOS", 0, 0, 0, 0, 0, 0, 0, False))

    def ck(tag, body):
        return tag + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

    def lst(tag, body):
        return ck(b"LIST", tag + body)

    def zstr(s, n):
        b = s.encode("latin-1", "replace")[:n - 1]
        return b + b"\0" * (n - len(b))

    info = lst(b"INFO", ck(b"ifil", struct.pack("<HH", 2, 1)) + ck(b"isng", b"EMU8000\0") + ck(b"INAM", zstr("DLS convertido", 32)))
    sdta_ck = lst(b"sdta", ck(b"smpl", bytes(sdta)))
    phdr_b = b"".join(zstr(n, 20) + struct.pack("<HHHIII", p, bk, bg, 0, 0, 0) for n, p, bk, bg in phdr)
    pbag_b = b"".join(struct.pack("<HH", g, m) for g, m in pbag)
    pgen_b = b"".join(struct.pack("<HH", op, v) for op, v in pgen) + struct.pack("<HH", 0, 0)
    inst_b = b"".join(zstr(n, 20) + struct.pack("<H", bg) for n, bg in inst)
    ibag_b = b"".join(struct.pack("<HH", g, m) for g, m in ibag)
    igen_b = b"".join(struct.pack("<HH", op, v) for op, v in igen) + struct.pack("<HH", 0, 0)
    shdr_b = b"".join(zstr(n, 20) + struct.pack("<IIIIIBbHH", s, e, ls, le, rate, pitch & 0xFF, corr, 0, 1 if n != "EOS" else 0)
                      for n, s, e, ls, le, rate, pitch, corr, lp in shdr)
    pdta = lst(b"pdta", ck(b"phdr", phdr_b) + ck(b"pbag", pbag_b) + ck(b"pmod", struct.pack("<HHhHH", 0, 0, 0, 0, 0)) + ck(b"pgen", pgen_b)
                + ck(b"inst", inst_b) + ck(b"ibag", ibag_b) + ck(b"imod", struct.pack("<HHhHH", 0, 0, 0, 0, 0)) + ck(b"igen", igen_b) + ck(b"shdr", shdr_b))
    body = b"sfbk" + info + sdta_ck + pdta
    open(sf2_path, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
    print("%s: %d instrumentos, %d muestras, %.1f MB" % (sf2_path, len(ins_list), len(waves), len(body) / 1e6))


def art_to_gens(art, gs):
    if "art" in SKIP:
        return
    """Conexiones DLS → generadores SF2 (solo las de fuente fija: envolventes, LFO, pan)."""
    for (src, ctl, dst), scale in art.items():
        if ("eg1" in SKIP and 0x206 <= dst <= 0x20A) or ("eg2" in SKIP and (0x30A <= dst <= 0x30E or src == 5)) or \
           ("lfo" in SKIP and (dst in (0x104, 0x105) or src == 1)) or ("pan" in SKIP and dst == 4):
            continue
        if (src, ctl) == (0, 0):
            if dst == 0x206: gen(gs, 34, tc(scale, True))      # EG1 attack → attackVolEnv
            elif dst == 0x207: gen(gs, 36, tc(scale))          # decay
            elif dst == 0x209: gen(gs, 38, tc(scale))          # release
            elif dst == 0x20A: gen(gs, 37, sustain_cb(scale))  # sustain
            elif dst == 0x30A: gen(gs, 26, tc(scale, True))    # EG2 attack → attackModEnv
            elif dst == 0x30B: gen(gs, 28, tc(scale))          # decay
            elif dst == 0x30D: gen(gs, 30, tc(scale))          # release
            elif dst == 0x30E:                                 # sustain (0,1 %) → sustainModEnv (0,1 %)
                gen(gs, 29, int(max(0, min(1000, 1000 - round(scale / 65536.0)))))
            elif dst == 0x104: gen(gs, 22, int(max(-16000, min(4500, round(scale / 65536.0)))))  # LFO freq → freqModLFO (cents absolutos)
            elif dst == 0x105: gen(gs, 21, tc(scale, True))    # LFO start delay → delayModLFO
            elif dst == 0x004: gen(gs, 17, int(max(-500, min(500, round(scale / 65536.0 / 0.1)))))   # pan (0,1 %) → centésimas
        elif src == 1 and ctl == 0 and dst == 0x001:           # LFO → ganancia (trémolo, 0,1 dB) → modLfoToVolume (cB)
            gen(gs, 13, int(max(-960, min(960, round(scale / 65536.0)))))
        elif src == 1 and ctl == 0 and dst == 0x003:           # LFO → pitch (cents)
            gen(gs, 5, int(max(-12000, min(12000, round(scale / 65536.0)))))
        elif src == 5 and ctl == 0 and dst == 0x003:           # EG2 → pitch
            gen(gs, 7, int(max(-12000, min(12000, round(scale / 65536.0)))))


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
