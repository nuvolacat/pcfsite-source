"""Genera el C de la línea base: traduce MANAGER.EXE + MFC42.DLL + MSVCRT.DLL a C y deja en
`build/gen/` todo lo que CMake compila junto con la capa de plataforma (`platform/`).

Salida:
  chunk_XXXX.c    funciones traducidas (un trozo por cada 64 KB de direcciones)
  funcs.h         prototipos de todas las funciones traducidas y de los stubs del sistema
  ftab.c          tabla dirección → función (despacho de llamadas indirectas), trampolines de IAT
  gp.c            exportaciones de los módulos (GetProcAddress)
  stubs_default.c stubs del sistema AÚN NO implementados en platform/src: avisan una vez y limpian la
                  pila según su firma (recomp/winapi_sigs.py), en vez de desequilibrarla
  modinfo.c       bases, puntos de entrada y TLS de los módulos (para el cargador)
  mem.bin         imagen inicial de la memoria (módulos relocalizados con la IAT resuelta)
  imagen.c        la receta para reconstruir mem.bin en ejecución con los ficheros del propio juego (la app no lleva
                  mem.bin): trozos de cada fichero → dirección, y encima los bytes que cambian (relocaciones, IAT,
                  parches); con el SHA-256 de cada fichero y de la imagen. La aplica platform/src/imagen.cpp

Adaptado de PCFWeb (oracle/recomp/runtime.py @ 8f237df). Uso: .venv/bin/python recomp/build.py
"""
from __future__ import annotations

import bisect
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lift as L  # noqa: E402
from linker import Linker, TARGET  # noqa: E402
import juego as J  # noqa: E402

STEM = TARGET.rsplit(".", 1)[0]
OUT = os.environ.get("PCF7_GEN", J.gen_dir(STEM))
SIGS = os.path.join(ROOT, "platform", "gen", "winapi_sigs.h")
PLATFORM_SRC = os.path.join(ROOT, "platform", "src")
# Funciones del host con dirección de invitado (GetProcAddress de ddraw/dsound, métodos COM…): la
# plataforma las registra en tiempo de ejecución en HOSTFN[i] y el invitado las llama en
# HOSTFN_BASE + 16*i. Va entre los trampolines de la IAT (0x2300000) y el montículo (0x2400000).
HOSTFN_N = 4096   # HOSTFN_BASE = TRAMP_BASE + 0x80000 (ver main)


def orphan_end(mod, a, hard_end):
    """Fin real de un bloque huérfano (entrada que Ghidra no reconoció como función): el primer
    `ret`/`jmp` seguido de relleno (int3/nop), acotado por el siguiente punto de entrada."""
    import capstone as cs
    img = bytes(mod.image)
    lo, hi = a - mod.base, hard_end - mod.base
    if lo < 0 or hi > len(img):
        return hard_end - a
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
    for ins in md.disasm(img[lo:hi], a):
        if ins.mnemonic == "ret" or ins.mnemonic.startswith("jmp"):
            nxt = ins.address + ins.size - mod.base
            if 0 <= nxt < len(img) and img[nxt] in (0xCC, 0x90):
                return ins.address + ins.size - a
    return hard_end - a


def cluster_of(lk, a):
    for m in lk.mods:
        for lo, hi in getattr(m, "clusters", []):
            if lo <= a < hi:
                return m, lo, hi
    return None


def lift_clusters(lk, iat, codes, ents):
    """Traduce cada grupo (ver linker: ENGINE de MSVBVM50) con todas sus entradas conocidas. El código
    va en la primera entrada y el resto quedan vacías (sus f_x las define el grupo)."""
    out = []
    for (m, lo, hi), es in ents.items():
        L.set_module(m, iat)
        try:
            c = L.lift_function(lo, cluster=(hi, es))
        finally:
            L.set_module(None, {})
        got = sorted(int(x, 16) for x in re.findall(r"^void f_([0-9a-f]+)\(void\)\{ fc_", c, re.M))
        for e in got:
            codes[e] = ""
        for e in set(es) - set(got):   # no decodificable como código
            codes[e] = "void f_%x(void){ js_missing(0x%xu); }\n" % (e, e)
        if got:
            codes[got[0]] = c
            out.append(c)
    return out


def lift_all(lk, iat):
    codes = {}
    for m in lk.mods:   # plantillas de código del runtime (thunks): ver lift.TEMPLATES
        L.TEMPLATES.update(a for a, _ in m.code_templates())
    cl_ents = {}   # grupo → entradas
    for mod in lk.mods:
        L.set_module(mod, iat)
        ok = n = 0
        for a in sorted(mod.funcs):
            n += 1
            cl = cluster_of(lk, a)
            if cl:
                cl_ents.setdefault(cl, set()).add(a)
                continue
            try:
                codes[a] = L.lift_function(a)
                ok += 1
            except L.Unsupported:
                pass
        print(f"  {mod.alias:12s} {ok}/{n} funciones")
    L.set_module(None, {})

    # destinos de código referenciados desde datos (vtables) e inmediatos: entran por `icall`
    extra = set(lk.data_code_refs())
    for m in lk.mods:
        extra |= m.catch_continuations() | set(m.seh_targets()) | {a for a, _ in m.code_templates()}
    # direcciones que solo se ven llamar en ejecución (PCF7_UNRESOLVED): se guardan con el juego
    unr = os.path.join(ROOT, "juegos", J.ID, "unresolved.txt" if TARGET == J.PRINCIPAL else f"unresolved-{STEM}.txt")
    if os.path.exists(unr):
        extra |= {int(x, 16) for x in open(unr).read().split() if x}
    # MS-DOS: los punteros a código de los datos del LE (tools/le2pe.py → <programa>.le.txt)
    if J.CFG.get("dos"):
        # tabla de trocitos «int XX; ret» de int86 (Watcom): se entra en ellos con «push dirección; ret»
        m0 = lk.mods[0]
        lo, hi = m0.text
        b, i, stubs = m0.image, lo - m0.base, 0
        while i < hi - m0.base - 3:
            if b[i] == 0xCD and b[i + 2] == 0xC3:
                j = i
                while j < hi - m0.base - 3 and b[j] == 0xCD and b[j + 2] == 0xC3:
                    j += 3
                if (j - i) // 3 >= 8:
                    for k in range(i, j, 3):
                        extra.add(m0.base + k)
                        stubs += 1
                i = j
            else:
                i += 1
        if stubs:
            print(f"  trocitos int XX; ret de int86: {stubs}")
        le = os.path.join(J.TRABAJO, "bin", TARGET + ".le.txt")
        if os.path.exists(le):
            extra |= {int(l.split()[1], 16) for l in open(le) if l.startswith("puntero ")}
            # punteros dentro del código: solo los que no caen dentro de una función conocida (los demás son
            # destinos de tablas de salto)
            m0 = lk.mods[0]
            fin = sorted((a, a + max(n, 1)) for a, n in m0.funcs.items())
            inicios = [a for a, _ in fin]
            cod = {int(l.split()[1], 16) for l in open(le) if l.startswith("puntero_cod ")}
            nuevos = set()
            # los que caen dentro de una función conocida valen si empiezan tras un final de bloque (ret, jmp, int3,
            # nop): bloques de una función grande que también se llaman por puntero (Base de datos del 4.0)
            img, base = m0.image, m0.base
            def tras_final(v):
                o = v - base
                return o >= 5 and (img[o - 1] in (0xC3, 0xCC, 0x90) or img[o - 3] == 0xC2 or img[o - 5] == 0xE9 or img[o - 2] == 0xEB)
            for v in cod:
                k = bisect.bisect_right(inicios, v) - 1
                dentro = k >= 0 and fin[k][0] < v < fin[k][1]
                if not dentro or tras_final(v):
                    nuevos.add(v)
            print(f"  punteros a código dentro del código: {len(cod)}, {len(nuevos)} nuevas entradas")
            extra |= nuevos
    # los programas del CD usan las mismas DLL (MFC42/MSVCRT, desde 0x2000000) que el principal: heredan las de su lista
    principal = os.path.join(ROOT, "juegos", J.ID, "unresolved.txt")
    if TARGET != J.PRINCIPAL and os.path.exists(principal):
        extra |= {v for v in (int(x, 16) for x in open(principal).read().split() if x) if v >= 0x2000000}
    for a in list(extra):
        cl = cluster_of(lk, a)
        if cl:
            cl_ents.setdefault(cl, set()).add(a)
            extra.discard(a)
    extra = sorted(extra)
    # en los módulos sin funciones de Ghidra, las entradas descubiertas también delimitan funciones: si no,
    # cada entrada seguía saltos hasta la siguiente función de la semilla y absorbía cuerpos ajenos (miles
    # de copias del mismo código)
    # (solo las que empiezan tras un final de función: una entrada falsa a mitad de instrucción cortaría
    # la función que la contiene)
    def tras_final(m, a):
        o, b = a - m.base, m.image
        return o >= 5 and (b[o - 1] in (0xC3, 0xCC, 0x90) or b[o - 3] == 0xC2 or b[o - 5] == 0xE9 or b[o - 2] == 0xEB)
    for m in lk.mods:
        if not any(m.funcs.values()):
            m.stops = set(m.funcs)
            for a in extra:
                if m.text[0] <= a < m.text[1]:
                    m.stops.add(a)
                    if tras_final(m, a):
                        m.funcs.setdefault(a, 0)
    nuevas = 0
    starts_of = {}
    for a in extra:
        if a in codes:
            continue
        mod = next((m for m in lk.mods if m.text[0] <= a < m.text[1]), None)
        if not mod:
            continue
        size = None
        if a not in mod.funcs:
            if mod not in starts_of:
                starts_of[mod] = sorted(mod.funcs)
            starts = starts_of[mod]
            j = bisect.bisect_right(starts, a)
            nxt = starts[j] if j < len(starts) else a + 512
            k = bisect.bisect_right(extra, a)
            if k < len(extra) and extra[k] < nxt:
                nxt = extra[k]
            size = max(1, min(orphan_end(mod, a, nxt), 8192))
        try:
            L.set_module(mod, iat)
            codes[a] = L.lift_function(a, size=size)
            nuevas += 1
        except Exception:  # noqa: BLE001
            pass
        finally:
            L.set_module(None, {})
    print(f"  referencias de código desde datos: {len(extra)} ({nuevas} nuevas)")

    # cierre transitivo: toda función llamada debe existir (solo se escanea lo recién traducido)
    rx_call = re.compile(r"\bf_([0-9a-f]+)\(")
    nuevo = list(codes.values()) + lift_clusters(lk, iat, codes, cl_ents)
    for _ in range(40):
        refs = set()
        for c in nuevo:
            refs.update(rx_call.findall(c))
        missing = {int(r, 16) for r in refs} - set(codes)
        if not missing:
            break
        nuevo = []
        cambio = {}
        for a in list(missing):
            cl = cluster_of(lk, a)
            if cl:
                cl_ents.setdefault(cl, set()).add(a)
                cambio[cl] = cl_ents[cl]
                missing.discard(a)
        if cambio:
            for (m, lo, hi) in cambio:   # el código del grupo cambia de clave si cambia su primera entrada
                for e in [k for k, v in codes.items() if lo <= k < hi]:
                    del codes[e]
            nuevo += lift_clusters(lk, iat, codes, cambio)
        for a in missing:
            mod = next((m for m in lk.mods if m.text[0] <= a < m.text[1]), None)
            if mod and not any(mod.funcs.values()):
                mod.stops.add(a)
                if tras_final(mod, a):
                    mod.funcs.setdefault(a, 0)
        for a in missing:
            mod = next((m for m in lk.mods if m.text[0] <= a < m.text[1]), None)
            try:
                if not mod:
                    raise L.Unsupported("fuera de módulo")
                L.set_module(mod, iat)
                codes[a] = L.lift_function(a)
            except Exception:  # noqa: BLE001
                codes[a] = "void f_%x(void){ js_missing(0x%xu); }\n" % (a, a)
            finally:
                L.set_module(None, {})
            nuevo.append(codes[a])
    return codes


def patch_eh_prolog(codes, lk):
    """`__EH_prolog` (MSVCRT) fija el ebp del LLAMANTE (prólogo SEH con `push retaddr; ret`): la
    frontera de llamada no debe restaurar ebp en las llamadas a él ni a sus thunks."""
    msv = lk.by_alias.get("msvcrt.dll")
    eh = {msv.exports["_EH_prolog"]} if msv and "_EH_prolog" in msv.exports else set()
    if not eh:
        return
    for a, c in codes.items():
        if any(re.search(rf"f_{x:x}\(\); (?:ESPCHK_T\(\w+\); )?return;", c) for x in eh):
            eh.add(a)
    rx = re.compile(r"\{u32 _ss=esi,_sp=ebp,_sb=ebx,_sd=edi,_c0=esp; esp -= 4; wr32\(esp, (0x[0-9a-f]+u)\); ("
                    + "|".join("f_%x" % x for x in sorted(eh)) + r")\(\); esi=_ss;ebp=_sp;ebx=_sb;edi=_sd; CALLCHK\(\w+\);\}")
    for a in codes:
        codes[a] = rx.sub(lambda m: "{u32 _ss=esi,_sb=ebx,_sd=edi; esp -= 4; wr32(esp, %s); %s(); esi=_ss;ebx=_sb;edi=_sd;}"
                          % (m.group(1), m.group(2)), codes[a])
    print(f"  __EH_prolog: {len(eh)} funciones sin restaurar ebp")


def _dos_layout():
    """MS-DOS: la pila inicial que DOS4GW da al programa (la del LE, ver tools/le2pe.py → <programa>.le.txt)."""
    if not J.CFG.get("dos"):
        return ""
    le = os.path.join(J.TRABAJO, "bin", TARGET + ".le.txt")
    if not os.path.exists(le):
        raise SystemExit(f"falta {le}: convierte antes el LE con tools/le2pe.py")
    vals = dict(l.split()[:2] for l in open(le) if l.startswith(("pila", "entrada")))
    return "#define PCF_DOS_PILA %su\n#define PCF_DOS_ENTRADA %su\n" % (vals["pila"], vals["entrada"])


_ASM = re.compile(r"L_[0-9a-f]+: /\* (.*?) \*/")


DOS_SALIDA = bool(J.CFG.get("dos"))


def patch_regs_salida(codes):
    """Subrutinas de ensamblador escritas a mano que devuelven un valor en edi/esi/ebx: tras restaurarlo con
    `pop` lo vuelven a escribir antes del `ret` (el pintor de letras con recorte de todos los PC Fútbol devuelve
    así en edi dónde va la letra siguiente). La frontera de llamada no debe restaurar ese registro: si lo
    restaura, todas las letras se pintan encima de la primera. El código compilado nunca hace esto."""
    salida = {}
    for a, c in codes.items():
        ins = [t.strip() for t in _ASM.findall(c)]
        for i, t in enumerate(ins):
            if not re.fullmatch(r"ret(?: \w+)?", t):
                continue
            pops, escritos = set(), set()
            for t2 in reversed(ins[max(0, i - 16):i]):
                m, _, resto = t2.partition(" ")
                if m in ("call", "ret", "retf", "loop") or m.startswith("j"):
                    break
                dest = resto.split(",")[0].strip()
                if m == "pop":
                    if dest in escritos:
                        salida.setdefault(a, set()).add(dest)
                    pops.add(dest)
                elif m not in ("push", "cmp", "test", "bt") and dest in ("edi", "esi", "ebx") and dest not in pops:
                    escritos.add(dest)
            # MS-DOS (ensamblador de Watcom): subrutinas que calculan un valor en edi/esi/ebx y vuelven sin guardar el
            # registro (p. ej. `mov edi, eax; ret`: la dirección de vídeo de una fila). En Windows este patrón solo
            # salía en datos decodificados como código, por eso solo aquí.
            if DOS_SALIDA and escritos - pops:
                salida.setdefault(a, set()).update(escritos - pops)
    if not salida:
        return
    rest = {"esi": "esi=_ss;", "ebx": "ebx=_sb;", "edi": "edi=_sd;"}
    for x, regs in salida.items():
        rx = re.compile(r"(f_%x\(\); esi=_ss;ebp=_sp;ebx=_sb;edi=_sd;)" % x)
        for a in codes:
            if rx.search(codes[a]):
                codes[a] = rx.sub(lambda m: _sin(m.group(1), [rest[r] for r in regs]), codes[a])
    print("  registros de salida: " + ", ".join(f"f_{x:x} ({'/'.join(sorted(r))})" for x, r in sorted(salida.items())))


def _sin(texto, quitar):
    for q in quitar:
        texto = texto.replace(q, "")
    return texto


def trace_entry(codes):
    """cur_fn = función en curso y anillo de las últimas 256 (reconstruye la ruta tras un fallo)."""
    for a in codes:
        codes[a] = codes[a].replace("{", "{cur_fn=0x%xu;fnring[fnring_i++&255]=0x%xu;if(g_watch)fn_watch(0x%xu);" % (a, a, a), 1)


def landing_pads(codes, lk):
    """Las funciones que contienen continuaciones de `catch` o cuerpos `__except` se envuelven con una pista de aterrizaje
    para ijmp() (ver rt.h): f_X pasa a ser fb_X y f_X la llama tras registrar la pista."""
    n = 0
    for m in lk.mods:
        seh = m.seh_targets()
        catches = set(m.catches)
        conts = {c: c for c in m.catch_continuations()}
        conts.update(seh)                       # destino → dirección por la que buscar la función dueña
        starts = sorted(a for a in m.funcs if a in codes and a not in conts)
        owners = {}
        for c, at in conts.items():
            j = bisect.bisect_right(starts, at) - 1
            while j >= 0 and starts[j] in catches:   # Ghidra separa los funclets `catch` como funciones
                j -= 1
            if j < 0:
                continue
            f = starts[j]
            hi = f + m.funcs[f] if m.funcs.get(f) else (starts[j + 1] if j + 1 < len(starts) else c + 1)
            hi = max(hi, c + 1)                 # la continuación puede quedar tras el final que da Ghidra
            if f < c < hi:
                o = owners.setdefault(f, [hi, set()])
                o[0] = max(o[0], hi)
                o[1].add(c)
        for f, (hi, cs) in owners.items():
            code = codes[f]
            head = "void f_%x(void) {" % f
            if not code.startswith(head):
                continue
            cases = " ".join("case 0x%xu: %sf_%x(); return;" % (c, "g_land_now=0x%xu; " % c if c in seh else "", c)
                             for c in sorted(cs) if c in codes)
            # el `__except` de VC5 se entra con `call` desde _except_handler3, no con `jmp`: su entrada
            # propia también salta a la pista (salvo cuando la llama la propia pista)
            for c in cs & set(seh):
                hc = "void f_%x(void) {" % c
                if c in codes and codes[c].startswith(hc):
                    codes[c] = ("static void fx_%x(void) {" % c + codes[c][len(hc):]
                                + "void f_%x(void){ land_to(0x%xu); fx_%x(); }\n" % (c, c, c))
            codes[f] = ("static void fb_%x(void) {" % f + code[len(head):]
                        + "void f_%x(void){ pcf7_land L_; L_.lo=0x%xu; L_.hi=0x%xu; L_.esp0=esp; L_.prev=g_land; g_land=&L_;"
                          " if(_setjmp(L_.jb)){ g_land=L_.prev; switch(L_.target){ %s default: icall(L_.target); } return; }"
                          " fb_%x(); g_land=L_.prev; }\n" % (f, f, hi, cases, f))
            n += 1
    print(f"  pistas de aterrizaje (catch/__except): {n} funciones")


def implemented_stubs():
    names = set()
    for p in glob.glob(os.path.join(PLATFORM_SRC, "*.cpp")):
        names |= set(re.findall(r"\bSTUB\((ext_\w+)\)", open(p, encoding="utf-8").read()))
    return names


def write_imagen(lk, mem, end):
    """imagen.c: cómo se reconstruye `mem` a partir de los ficheros del juego. Por módulo: el fichero (nombre, tamaño,
    SHA-256), las copias (desplazamiento en el fichero → desplazamiento en el módulo, bytes) tal como las hace pefile al
    mapear el PE (cabecera y secciones; en los LE del 4.0, las páginas del fichero original, tools/le2pe.py →
    .le.map.json) y, encima, las «diferencias»: los bytes de la imagen final que no son los copiados (relocaciones, IAT
    resuelta, parches, cabecera PE sintética de le2pe). Se comprueba aquí mismo que copias+diferencias dan la imagen."""
    import hashlib
    import json
    out = ['#include "rt.h"',
           "/* Generado por recomp/build.py: receta de la imagen inicial de la memoria (ver platform/src/imagen.cpp). */",
           "typedef struct { const char *fichero, *sha256; u32 tam, base, size; const u32 *copias; u32 ncopias;"
           " const unsigned char *dif; u32 ndif; } ImgModulo;"]
    entradas = []
    total_dif = 0
    for i, m in enumerate(lk.mods):
        mapa = m.path + ".le.map.json"
        if J.CFG.get("dos") and os.path.exists(mapa):
            mp = json.load(open(mapa))
            fichero, datos = mp["origen"], open(mp["ruta"], "rb").read()
            if hashlib.sha256(datos).hexdigest() != mp["sha256"]:
                raise SystemExit(f"{mp['ruta']} ha cambiado desde que se convirtió con le2pe.py")
            copias = [tuple(c) for c in mp["copias"]]
        else:
            if J.CFG.get("dos"):
                print(f"  AVISO: falta {mapa} (tools/le2pe.py): la receta de {m.alias} referirá al PE convertido, no al LE original")
            fichero, datos = m.alias, open(m.path, "rb").read()
            pe = m.pe
            copias = [(0, 0, len(pe.header))]
            for s in pe.sections:
                if s.Misc_VirtualSize == 0 and s.SizeOfRawData == 0:
                    continue
                srd = s.SizeOfRawData
                prd = pe.adjust_PointerToRawData(s.PointerToRawData)
                va = pe.adjust_SectionAlignment(s.VirtualAddress, pe.OPTIONAL_HEADER.SectionAlignment, pe.OPTIONAL_HEADER.FileAlignment)
                if srd > len(datos) or prd > len(datos) or srd + prd > len(datos) or va >= 0x10000000:
                    continue
                copias.append((prd, va, srd))
        assert len(m.image) <= m.size, f"{m.alias}: la imagen ({len(m.image)}) excede SizeOfImage ({m.size})"
        raw = bytearray(m.size)
        for src, dst, n in copias:
            n = min(n, len(datos) - src, m.size - dst)
            if n > 0:
                raw[dst:dst + n] = datos[src:src + n]
        img = bytes(m.image) + bytes(m.size - len(m.image))
        # diferencias: por trozos de 4 KB iguales se salta rápido; dentro, byte a byte, uniendo huecos de < 8 bytes
        runs = []
        pos = 0
        while pos < m.size:
            fin = min(pos + 4096, m.size)
            if raw[pos:fin] != img[pos:fin]:
                j = pos
                while j < fin:
                    if raw[j] == img[j]:
                        j += 1
                        continue
                    k = j
                    while k < fin and (raw[k] != img[k] or (k + 8 <= fin and raw[k:k + 8] != img[k:k + 8])):
                        k += 1
                    if runs and j - (runs[-1][0] + runs[-1][1]) < 8:
                        a = runs[-1][0]
                        runs[-1] = (a, k - a)
                    else:
                        runs.append((j, k - j))
                    j = k
            pos = fin
        dif = bytearray()
        for a, n in runs:
            while n > 0:
                t = min(n, 65535)
                dif += struct.pack("<IH", a, t) + img[a:a + t]
                a += t
                n -= t
        # comprobación: copias + diferencias = imagen
        chk = bytearray(raw)
        o = 0
        while o < len(dif):
            a, t = struct.unpack_from("<IH", dif, o)
            chk[a:a + t] = dif[o + 6:o + 6 + t]
            o += 6 + t
        assert bytes(chk) == img, f"{m.alias}: la receta no reproduce la imagen"
        total_dif += len(dif)
        print(f"  imagen: {fichero:14s} {len(datos):9d} B, {len(copias)} copias, {len(runs)} diferencias ({len(dif)} B)")
        out.append("static const u32 C%d[] = {%s};" % (i, ",".join("0x%xu,0x%xu,0x%xu" % c for c in copias) or "0"))
        lines = []
        for k in range(0, len(dif), 64):
            lines.append(",".join(str(b) for b in dif[k:k + 64]))
        out.append("static const unsigned char D%d[] = {%s};" % (i, ",\n".join(lines) or "0"))
        esc = lambda s: s.replace("\\", "\\\\").replace('"', '\\"')  # noqa: E731
        entradas.append('  {"%s", "%s", 0x%xu, 0x%xu, 0x%xu, C%d, %d, D%d, %d}' % (
            esc(fichero), hashlib.sha256(datos).hexdigest(), len(datos), m.base, m.size, i, len(copias), i, len(dif)))
    out.append("const ImgModulo IMAGEN_MODS[] = {\n" + ",\n".join(entradas) + "\n};")
    out.append("const unsigned IMAGEN_NMODS = %d;" % len(lk.mods))
    out.append("const u32 IMAGEN_FIN = 0x%xu;" % end)
    out.append('const char IMAGEN_SHA256[] = "%s";' % hashlib.sha256(bytes(mem)).hexdigest())
    write("imagen.c", "\n".join(out) + "\n")
    print(f"imagen.c: {len(lk.mods)} módulos, {total_dif} B de diferencias")


def write(name, text):
    p = os.path.join(OUT, name)
    if os.path.exists(p) and open(p, encoding="utf-8").read() == text:
        return                       # sin cambios: no fuerza recompilar
    open(p, "w", encoding="utf-8").write(text)


# Firmas (dwords que limpia, dwords de argumentos) de APIs que winapi_sigs.h no recoge (las importan
# ProQuinielas y sus controles). Sin ellas el stub no limpiaría la pila al volver.
EXTRA_SIGS = {
    **{f"ext_comctl32_dll_{n}": (k, k) for n, k in {
        "ImageList_Add": 3, "ImageList_AddMasked": 3, "ImageList_Create": 5, "ImageList_Destroy": 1,
        "ImageList_Draw": 6, "ImageList_DrawEx": 10, "ImageList_GetIcon": 3, "ImageList_GetIconSize": 3,
        "ImageList_Remove": 2, "ImageList_ReplaceIcon": 3, "ImageList_SetBkColor": 2,
        "ImageList_SetOverlayImage": 3, "ord16": 12, "ord17": 0}.items()},
    **{f"ext_comdlg32_dll_{n}": (1, 1) for n in ("ChooseColorA", "ChooseFontA", "GetOpenFileNameA",
                                                 "GetSaveFileNameA", "PrintDlgA")},
    **{f"ext_rpcrt4_dll_{n}": (k, k) for n, k in {
        "CStdStubBuffer_AddRef": 1, "CStdStubBuffer_Connect": 2, "CStdStubBuffer_CountRefs": 1,
        "CStdStubBuffer_DebugServerQueryInterface": 2, "CStdStubBuffer_DebugServerRelease": 2,
        "CStdStubBuffer_Disconnect": 1, "CStdStubBuffer_Invoke": 3, "CStdStubBuffer_IsIIDSupported": 2,
        "CStdStubBuffer_QueryInterface": 3, "IUnknown_AddRef_Proxy": 1, "IUnknown_QueryInterface_Proxy": 3,
        "IUnknown_Release_Proxy": 1, "NdrCStdStubBuffer2_Release": 2, "NdrCStdStubBuffer_Release": 2,
        "NdrDllGetClassObject": 6, "NdrOleAllocate": 1, "NdrOleFree": 1, "NdrStubCall": 4,
        "NdrStubForwardingFunction": 4, "RpcRaiseException": 1}.items()},
    "ext_rpcrt4_dll_NdrClientCall": (0, 2),   # cdecl con argumentos variables
    **{f"ext_user32_dll_{n}": (k, k) for n, k in {
        "ChildWindowFromPoint": 3, "CopyImage": 5, "CreateAcceleratorTableA": 2, "CreateCursor": 7,
        "CreateDialogParamA": 5, "CreateMenu": 0, "CreatePopupMenu": 0, "DestroyAcceleratorTable": 1,
        "DialogBoxIndirectParamA": 5, "DialogBoxParamA": 5, "FindWindowW": 2, "GetCursor": 0,
        "GetIconInfo": 2, "GetKeyboardState": 1, "GetMenuCheckMarkDimensions": 0, "GetMenuItemInfoA": 4,
        "GetQueueStatus": 1, "GetWindowRgn": 2, "PeekMessageW": 5, "PostMessageW": 4, "SetKeyboardState": 1,
        "SetMenuDefaultItem": 3, "SetMenuItemInfoA": 4, "SetWindowRgn": 3, "SetWindowsHookExW": 4,
        "keybd_event": 4}.items()},
}


def write_default_stubs(stubs):
    """stubs del sistema que platform/src aún no implementa: avisan y limpian la pila según su firma."""
    sig = {m.group(1): (int(m.group(2)), int(m.group(3)))
           for m in re.finditer(r"WINAPI_SIG\((\w+), (\d+), (\d+)", open(SIGS).read())}
    sig.update(EXTRA_SIGS)
    impl = implemented_stubs()
    body = ['#include "funcs.h"', "void plat_unimplemented(const char *name, u32 nargs);"]
    for s in stubs:
        if s in impl:
            continue
        clean, nargs = sig.get(s, (0, 0))
        body.append('void %s(void){ plat_unimplemented("%s", %d); RET(0, %d); }' % (s, s, nargs, clean))
    write("stubs_default.c", "\n".join(body) + "\n")
    print(f"stubs implementados: {len(impl & set(stubs))}   por defecto (avisan): {len(stubs) - len(impl & set(stubs))}")



def main():
    if "--stubs" in sys.argv:   # solo regenerar stubs_default.c (tras implementar APIs nuevas)
        lk = Linker()
        lk.resolve_iats()
        os.makedirs(OUT, exist_ok=True)
        write_default_stubs(sorted(lk.os_funcs))
        return

    os.makedirs(OUT, exist_ok=True)
    lk = Linker()
    iat = {va: lk.resolve(*imp) for va, imp in lk.iat.items()}
    tramps = lk.resolve_iats()
    global HOSTFN_BASE
    HOSTFN_BASE = lk.TRAMP_BASE + 0x80000
    # la pila del hilo principal va en 0x1000000-0x1F00000 salvo que el juego diga otra cosa (juego.json «pila»:
    # p. ej. el MANAGER.EXE de PCF7 Plus ocupa hasta 0x11FB000)
    pila = int(J.CFG["pila"], 16) if J.CFG.get("pila") else None
    write("layout.h", "/* Generado por recomp/build.py: mapa de memoria de este programa. */\n#pragma once\n"
          "#define PCF7_TRAMP_BASE 0x%xu\n#define PCF7_HOSTFN_BASE 0x%xu\n#define PCF7_HEAP_LO 0x%xu\n"
          % (lk.TRAMP_BASE, HOSTFN_BASE, lk.TRAMP_BASE + 0x100000)
          + ("#define PCF7_STACK_LO 0x%xu\n" % pila if pila else "")
          + _dos_layout())
    hi = max(m.base + m.pe.OPTIONAL_HEADER.SizeOfImage for m in lk.mods if m.base < 0x2000000)
    if hi > (pila or 0x1000000):
        raise SystemExit(f"el programa llega hasta 0x{hi:x} y pisa la pila (0x{pila or 0x1000000:x}): pon «pila» en juego.json")
    print(f"módulos: {[m.alias for m in lk.mods]}  trampolines de IAT: {len(tramps)}")

    codes = lift_all(lk, iat)
    patch_eh_prolog(codes, lk)
    patch_regs_salida(codes)
    trace_entry(codes)
    landing_pads(codes, lk)
    allfn = sorted(codes)
    stubs = sorted(lk.os_funcs)
    print(f"funciones traducidas: {len(allfn)}   APIs del sistema: {len(stubs)}")

    # prototipos
    funcs_h = ("/* Generado por recomp/build.py. No editar. */\n#pragma once\n#include \"rt.h\"\n"
               + "".join("void f_%x(void);\n" % a for a in allfn)
               + "".join("void %s(void);\n" % s for s in stubs))
    write("funcs.h", funcs_h)

    write("stubs.h", "/* Generado por recomp/build.py. No editar. */\n#pragma once\n"
          + "".join("void %s(void);\n" % s for s in stubs))

    # trozos: agrupados por dirección (64 KB) y con solo los prototipos que usan, para que traducir una
    # función nueva no obligue a recompilar todos
    buckets = {}
    for a in allfn:
        buckets.setdefault(a >> 16, []).append(a)
    rx_ref = re.compile(r"\bf_([0-9a-f]+)\(")
    names = set()
    for k, part in buckets.items():
        body = "".join(codes[a] for a in part)
        refs = sorted({int(x, 16) for x in rx_ref.findall(body)})
        name = "chunk_%04x.c" % k
        names.add(name)
        write(name, '#include "rt.h"\n#include "stubs.h"\n' + "".join("void f_%x(void);\n" % r for r in refs) + body)
    for f in glob.glob(os.path.join(OUT, "chunk*.c")):
        if os.path.basename(f) not in names:
            os.remove(f)

    # despacho de llamadas indirectas (y plantillas de código copiadas al montículo)
    templates = [t for m in lk.mods for t in m.code_templates()]
    ftab = sorted([(a, "f_%x" % a) for a in allfn] + [(va, "t_%x" % va) for va, _ in tramps])
    write("ftab.c", '#include "funcs.h"\nvoid icall_ret(u32 x);\n'
          + "".join("static void t_%x(void){ %s(); }\n" % (va, s) for va, s in tramps)
          + "typedef struct { u32 a; void (*f)(void); } FENT;\n"
          + "static const FENT FTAB[] = {\n" + ",\n".join("{0x%xu,%s}" % e for e in ftab) + "\n};\n"
          + "u32 ftab_count(void){ return (u32)(sizeof(FTAB)/sizeof(FTAB[0])); }\n"
          + "void (*HOSTFN[%d])(void);\n" % HOSTFN_N
          + "".join("static const unsigned char TPLM_%x[] = {%s};\n" % (a, ",".join(str(b) for b in mk)) for a, mk in templates)
          + "typedef struct { u32 a, n; const unsigned char *m; } TPL;\n"
          + "static const TPL TPLS[] = {" + "".join("{0x%xu,%du,TPLM_%x}," % (a, len(mk), a) for a, mk in templates) + "{0,0,0}};\n"
          + "u32 g_thunk_base;\n"
          + "static u32 thunk_match(u32 x){ for(const TPL *t=TPLS; t->a; t++){ u32 i; if(x+t->n>=MEMSZ) continue;"
            " for(i=0;i<t->n;i++) if(t->m[i] && MEM[x+i]!=MEM[t->a+i]) break; if(i==t->n) return t->a; } return 0u; }\n"
          + "u32 icall_try(u32 x){\n"
          + "  if(x-0x%xu < 0x%xu && !(x&15u)){ void (*h)(void)=HOSTFN[(x-0x%xu)>>4]; if(!h) return 0u;"
            " u32 b=ebx,s=esi,d=edi,p=ebp; h(); ebx=b; esi=s; edi=d; ebp=p; return 1u; }\n"
            % (HOSTFN_BASE, HOSTFN_N * 16, HOSTFN_BASE)
          + "  int lo=0, hi=(int)(sizeof(FTAB)/sizeof(FTAB[0]))-1;\n"
          + "  while(lo<=hi){ int m=(lo+hi)>>1; if(FTAB[m].a==x){ " + ("u32 p=ebp; FTAB[m].f(); ebp=p;" if J.CFG.get("dos") else "u32 b=ebx,s=esi,d=edi,p=ebp; FTAB[m].f(); ebx=b; esi=s; edi=d; ebp=p;") + " if(g_watch) icall_ret(x); return 1u; } if(FTAB[m].a<x) lo=m+1; else hi=m-1; }\n"
          + "  { u32 t = thunk_match(x); if (t && t != x) { g_thunk_base = x; return icall_try(t); } }\n"
          + "  return 0u; }\n"
          + "void icall(u32 x){ if(!icall_try(x)) js_missing(x); }\n"
          + "_Thread_local pcf7_land *g_land;\n"
          + "_Thread_local u32 g_land_now;\n"
          + "void land_to(u32 x){ if(g_land_now==x){ g_land_now=0; return; } for(pcf7_land *l=g_land; l; l=l->prev)"
            " if(x>l->lo && x<l->hi && esp<l->esp0){ l->target=x; g_land=l; _longjmp(l->jb, 1); } }\n"
          + "void ijmp(u32 x){ land_to(x); icall(x); }\n")

    # GetProcAddress
    gp = sorted((addr, name, mod.base) for mod in lk.mods for name, addr in mod.exports.items()
                if isinstance(name, str))
    esc = lambda s: s.replace("\\", "\\\\").replace('"', '\\"')  # noqa: E731
    def sysent(stub, va):
        m = re.match(r"ext_([a-z0-9]+)_(dll|drv)_(.+)", stub)   # WINSPOOL.DRV
        return (m.group(1) + "." + m.group(2), m.group(3), va)
    write("gp.c", '#include "rt.h"\n#include <string.h>\n'
          "typedef struct { const char *n; u32 a; u32 m; } GPENT;\n"
          "static const GPENT GPALL[] = {\n" + ",\n".join('{"%s",0x%xu,0x%xu}' % (esc(n), a, b) for a, n, b in gp) + "\n};\n"
          "static int neq(u32 p, const char *s){ for(u32 i=0;;i++){ u8 a=MEM[p+i], b=(u8)s[i]; if(a!=b) return 0; if(!a) return 1; } }\n"
          "typedef struct { const char *dll, *n; u32 a; } SYSENT;\n"
          "static const SYSENT SYSGP[] = {\n" + ",\n".join('{"%s","%s",0x%xu}' % sysent(s, va) for va, s in tramps) + "\n};\n"
          "u32 gp_lookup_sys(const char *dll, u32 namep){ for(unsigned i=0;i<sizeof(SYSGP)/sizeof(SYSGP[0]);i++)"
          " if(!strcmp(SYSGP[i].dll, dll) && neq(namep, SYSGP[i].n)) return SYSGP[i].a; return 0; }\n"
          "u32 gp_lookup(u32 namep, u32 modbase){ for(unsigned i=0;i<sizeof(GPALL)/sizeof(GPALL[0]);i++)"
          " if((!modbase||GPALL[i].m==modbase) && neq(namep, GPALL[i].n)) return GPALL[i].a; return 0; }\n")

    write_default_stubs(stubs)

    # información de módulos para el cargador
    # dependencias entre módulos traducidos (sus importaciones) y cuáles carga Windows al arrancar (las
    # que importa el EXE, transitivamente); el resto se inicializan al hacer LoadLibrary (los OCX)
    idx = {m.alias.lower(): i for i, m in enumerate(lk.mods)}
    deps = []
    for m in lk.mods:
        ds = []
        for e in getattr(m.pe, "DIRECTORY_ENTRY_IMPORT", []):
            j = idx.get(e.dll.decode().lower())
            if j is not None and j not in ds:
                ds.append(j)
        deps.append(ds)
    static = {0}
    todo = [0]
    while todo:
        for j in deps[todo.pop()]:
            if j not in static:
                static.add(j)
                todo.append(j)
    mi = ['#include "rt.h"', "typedef struct { const char *name; u32 base, size, entry, tls_dir; int is_dll, is_static; int deps[16]; } MODINFO;",
          "const MODINFO MODS[] = {"]
    for i, m in enumerate(lk.mods):
        oh = m.pe.OPTIONAL_HEADER
        tls = oh.DATA_DIRECTORY[9].VirtualAddress
        assert len(deps[i]) < 16
        mi.append('  {"%s", 0x%xu, 0x%xu, 0x%xu, 0x%xu, %d, %d, {%s}},' % (
            m.alias, m.base, m.size, m.base + oh.AddressOfEntryPoint if oh.AddressOfEntryPoint else 0,
            m.base + tls if tls else 0, 1 if m.pe.is_dll() else 0, 1 if i in static else 0,
            ", ".join(str(j) for j in deps[i] + [-1])))
    mi += ["};", "const unsigned NMODS = %d;" % len(lk.mods), 'const char TARGET_EXE[] = "%s";' % TARGET]
    write("modinfo.c", "\n".join(mi) + "\n")
    write("juego.c", J.juego_c(TARGET))

    # imagen de memoria
    end = max(m.base + m.size for m in lk.mods)
    mem = bytearray(end)
    for m in lk.mods:
        mem[m.base:m.base + len(m.image)] = bytes(m.image)
    open(os.path.join(OUT, "mem.bin"), "wb").write(mem)
    print(f"mem.bin: {end} B (hasta 0x{end:x})")
    write_imagen(lk, mem, end)
    assert end <= lk.TRAMP_BASE and len(tramps) * 16 <= 0x80000


if __name__ == "__main__":
    main()
