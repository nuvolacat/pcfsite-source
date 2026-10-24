"""(Adaptado de PCFWeb, oracle/recomp/linker.py @ 8f237df.)

Enlazador: carga los módulos del juego (relocalizados a bases distintas), resuelve sus
exportaciones y su IAT, para que el lifter convierta `call [IAT]` en una llamada **directa**
a la función real (de otra DLL del juego) o a un stub del sistema (`ext_<dll>_<nombre>`).
"""
from __future__ import annotations

import os
import re
import struct

import pefile

HIGHLOW = pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]

import juego as J  # noqa: E402  (juegos/<id>/juego.json)

W = J.TRABAJO
BIN = os.environ.get("PCF7_BIN", f"{W}/bin")
DEC = os.environ.get("PCF7_DECOMP", f"{W}/decomp")

# (nombre lógico, fichero, base, carpeta del descompilado de Ghidra)
# MANAGER.EXE no tiene relocaciones: va en su base (0x400000-0x8e6000). MFC42/MSVCRT son las del CD
# del juego (SYSTEM/, nov-1997) relocalizadas a bases bajas para que todo quepa en MEM.
# El ejecutable a traducir: MANAGER.EXE o cualquiera de los otros programas del CD (DBASEWIN,
# INFOSURF, ACTWIN, PLAYVID, PCF7): todos van en 0x400000 sin relocaciones y usan las mismas DLL.
TARGET = os.environ.get("PCF7_TARGET", J.PRINCIPAL).lower()


def _image_size(path):
    pe = pefile.PE(path, fast_load=True)
    return pe.OPTIONAL_HEADER.SizeOfImage


def _stack(exe, dlls, folder):
    """EXE en su base (0x400000) y las DLL una tras otra desde 0x2000000 (todas tienen relocaciones)."""
    mods = [(exe, f"{folder}/{exe}", 0x00400000, f"{DEC}/{exe}")]
    base = 0x02000000
    for d in dlls:
        path = f"{folder}/{d}"
        mods.append((d, path, base, f"{DEC}/{d}"))
        base = (base + _image_size(path) + 0xFFFF) & ~0xFFFF
    return mods


if TARGET in J.PROGRAMAS and J.PROGRAMAS[TARGET].get("apiladas"):
    # programas con sus propios módulos (p. ej. ProQuinielas de PCF7: Visual Basic 5 + controles ActiveX, todo lo
    # que el instalador copiaba en su carpeta y en WINDOWS\SYSTEM), apilados desde 0x2000000. gsw32.exe (servidor de
    # gráficos de GRAPH32, que GSWDLL32 arranca con WinExec) va en el mismo proceso, en un hilo.
    _p = J.PROGRAMAS[TARGET]
    MODULES = _stack(TARGET, _p["apiladas"], f"{BIN}/{_p['carpeta']}")
else:
    MODULES = [(TARGET, f"{BIN}/{TARGET}", 0x00400000, f"{DEC}/{TARGET}")] + \
              [(n, f"{BIN}/{n}", b, f"{DEC}/{n}") for n, b in J.DLLS]

# DLLs del sistema que NO se traducen: sus funciones se reimplementan como stubs `ext_<dll>_<fn>`.
SYSTEM = {"kernel32.dll", "user32.dll", "gdi32.dll", "ole32.dll", "oleaut32.dll", "winmm.dll",
          "ddraw.dll", "dsound.dll", "advapi32.dll", "comctl32.dll", "shell32.dll", "version.dll",
          "msvcrt.dll", "mfc42.dll"}


def _san(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


class Module:
    def __init__(self, alias, path, base, decname):
        self.alias = alias
        self.path = path
        self.pe = pefile.PE(path)
        self.pref = self.pe.OPTIONAL_HEADER.ImageBase
        self.base = base
        self.size = (self.pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
        self.image = bytearray(self.pe.get_memory_mapped_image())
        self._relocate()
        # parches del juego (juego.json «parches»: {"programa": [["0xdirección", "bytes en hex", "motivo"], …]})
        for dir_, hexb, motivo in J.CFG.get("parches", {}).get(alias.lower(), []):
            off = int(dir_, 16) - base
            nuevo = bytes.fromhex(hexb)
            self.image[off:off + len(nuevo)] = nuevo
            print(f"  {alias}: parche en {dir_} ({motivo})")
        self.exports = {}
        self.forwards = {}   # exportaciones reenviadas: nombre u ordinal → "DLL.nombre"
        self.imports = {}   # iat_va → (dll, name)
        self.funcs = {}
        self.catches = []
        self._sin_refs = set()   # entradas de Ghidra sin referencias (FUN_*, nrefs 0)
        self._exports()
        self._imports()
        self._funcs(decname)
        sec = next(s for s in self.pe.sections if b".text" in s.Name)
        lo, hi = sec.VirtualAddress, sec.VirtualAddress + sec.Misc_VirtualSize
        # secciones de código contiguas detrás de .text (MSVBVM50 tiene ENGINE, el intérprete de p-code:
        # se traduce como un solo grupo de entradas, ver lift.lift_function(cluster=…))
        align = self.pe.OPTIONAL_HEADER.SectionAlignment
        self.clusters = []
        for s2 in sorted(self.pe.sections, key=lambda x: x.VirtualAddress):
            if s2.VirtualAddress == (hi + align - 1) // align * align and s2.Characteristics & 0x20000000:
                self.clusters.append((base + s2.VirtualAddress, base + s2.VirtualAddress + s2.Misc_VirtualSize))
                hi = s2.VirtualAddress + s2.Misc_VirtualSize
        # directorios del PE (importaciones, exportaciones…) metidos al final de .text: no son código
        # (si detrás no hay más secciones de código)
        if not self.clusters:
            mid = lo + (hi - lo) // 2
            for d in self.pe.OPTIONAL_HEADER.DATA_DIRECTORY:
                if d.Size and mid < d.VirtualAddress < hi:
                    hi = d.VirtualAddress
        self.text = (base + lo, base + hi)

    def _relocate(self):
        delta = self.base - self.pref
        if not delta:
            return
        for reloc in getattr(self.pe, "DIRECTORY_ENTRY_BASERELOC", []) or []:
            for e in reloc.entries:
                if e.type == HIGHLOW:
                    cur = struct.unpack_from("<I", self.image, e.rva)[0]
                    struct.pack_into("<I", self.image, e.rva, (cur + delta) & 0xFFFFFFFF)

    def _exports(self):
        exp = getattr(self.pe, "DIRECTORY_ENTRY_EXPORT", None)
        if not exp:
            return
        for s in exp.symbols:
            if s.forwarder:   # "DLL.nombre" (MSVCRT40 de Windows 95 reenvía todo a MSVCRT y MSVCIRT)
                fw = s.forwarder.decode()
                if s.name:
                    self.forwards[s.name.decode()] = fw
                self.forwards[("ord", s.ordinal)] = fw
                continue
            va = self.base + s.address
            if s.name:
                self.exports[s.name.decode()] = va
            self.exports[("ord", s.ordinal)] = va

    def _imports(self):
        for entry in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []) or []:
            dll = entry.dll.decode().lower()
            for imp in entry.imports:
                name = imp.name.decode() if imp.name else ("ord", imp.ordinal)
                iat_va = self.base + (imp.address - self.pref)
                self.imports[iat_va] = (dll, name)

    def catch_continuations(self):
        """Direcciones donde sigue la ejecución tras un `catch` de C++: el funclet `Catch@…` devuelve en
        eax la etiqueta de continuación (`mov eax, imm32` … `ret`), a mitad de la función que lo
        contiene; el runtime de MSVCRT salta allí y hay que traducirla como entrada propia."""
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        out = set()
        for a in self.catches:
            sz = self.funcs.get(a) or 64
            last = None
            for ins in md.disasm(bytes(self.image[a - self.base:a - self.base + min(sz, 256)]), a):
                if ins.mnemonic == "mov" and ins.op_str.startswith("eax, 0x"):
                    last = int(ins.op_str[5:], 16)
                if ins.mnemonic == "ret":
                    if last and self.text[0] <= last < self.text[1]:
                        out.add(last)
                    break
        return out

    def code_templates(self):
        """Plantillas de código con huecos 0x66… que el runtime copia al montículo y rellena (thunks de
        MSVBVM50): el bloque empieza en el `push ebp; mov ebp, esp` anterior al primer hueco y sigue hasta la
        primera instrucción imposible; cada plantilla acaba en un `ret`. Devuelve [(inicio, máscara)], con
        máscara[i] = 1 si el byte debe coincidir en la copia (0 en los huecos)."""
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        img = bytes(self.image)
        tlo, thi = self.text[0] - self.base, self.text[1] - self.base
        h = img.find(b"\x66\x66\x66\x66", tlo, thi)
        if h < 0:
            return []
        st = img.rfind(b"\x55\x8b\xec", h - 0x40, h)
        if st < 0:
            return []
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        out, cur, mask = [], None, []
        for ins in md.disasm(img[st:st + 0x400], self.base + st):
            if ins.mnemonic in ("out", "in", "insb", "outsb", "insd", "outsd", "arpl", "bound"):
                break
            if cur is None:
                cur, mask = ins.address, []
            m = [1] * ins.size
            for off, n in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
                if n and all(x == 0x66 for x in ins.bytes[off:off + n]):
                    for k in range(off, off + n):
                        m[k] = 0
            mask += m
            if ins.mnemonic.startswith("ret"):
                if 0 in mask:
                    out.append((cur, mask))
                cur = None
        return out

    def seh_targets(self):
        """Cuerpos `__except` de las tablas de ámbito de _except_handler3: el prólogo SEH es
        `push -1; push tabla; push manejador` y cada entrada es {anterior, filtro, manejador}.
        _except_handler3 salta (`jmp eax`) al manejador, a mitad de la función. Devuelve
        {manejador: dirección del prólogo} (el cuerpo puede figurar como función propia)."""
        import struct
        img = bytes(self.image)
        tlo, thi = self.text[0] - self.base, self.text[1] - self.base
        out = {}
        i = img.find(b"\x6a\xff\x68", tlo, thi)
        while i >= 0:
            if img[i + 7] == 0x68:
                tab = struct.unpack_from("<I", img, i + 3)[0] - self.base
                if 0 <= tab < len(img) - 12 and not (tlo <= tab < thi):
                    for k in range(64):
                        e = tab + 12 * k
                        if e + 12 > len(img):
                            break
                        prev, flt, hnd = struct.unpack_from("<III", img, e)
                        if not (prev == 0xFFFFFFFF or prev < k) or not (self.text[0] <= hnd < self.text[1]):
                            break
                        # solo los `__except` (filtro ≠ 0): a ellos se salta y no se vuelve. Los `__finally`
                        # (filtro 0) se llaman con `call` (desde la propia función o desde _local_unwind) y
                        # vuelven: si fueran pista de aterrizaje, la función madre acabaría sin su epílogo
                        # (PCF7 Plus: el «eh vector constructor iterator» dejaba su registro SEH colgado)
                        if flt:
                            out.setdefault(hnd, self.base + i)
            i = img.find(b"\x6a\xff\x68", i + 1, thi)
        return out

    def _quita_cortes(self):
        """FixGaps a veces crea «funciones» en mitad de una instrucción de la función anterior, cuando Ghidra la
        da más corta de lo que es (PCF7 Plus: un constructor estático de 28 KB partido en trozos desalineados).
        Esas entradas, sin referencias, se traducirían desalineadas y la función real se cortaría. Se quitan
        las que no caen en el límite de una instrucción decodificando de corrido desde la función anterior."""
        import capstone as cs
        md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
        img = bytes(self.image)
        starts = sorted(self.funcs)
        quitar = set()
        for k in range(1, len(starts)):
            s = starts[k]
            if s not in self._sin_refs:
                continue
            p = starts[k - 1]
            while p in quitar and k > 1:   # la anterior también era un corte: se decodifica desde la buena
                k -= 1
                p = starts[k - 1]
            if s - p > 0x20000:
                continue
            bordes = set()
            for ins in md.disasm(img[p - self.base: s - self.base + 16], p):
                bordes.add(ins.address)
                if ins.address >= s:
                    break
            if s not in bordes and max(bordes) >= s:
                quitar.add(s)
        for s in quitar:
            del self.funcs[s]
        if quitar:
            print(f"  {self.alias}: {len(quitar)} entradas de Ghidra a mitad de instrucción, descartadas")

    def _funcs(self, decdir):
        p = os.path.join(decdir, "functions.tsv") if not decdir.endswith(".tsv") else decdir
        if os.path.exists(p):
            import csv
            for r in csv.reader(open(p, encoding="utf-8", errors="replace"), delimiter="\t"):
                if r[0] == "entry":
                    continue
                # las direcciones de functions.tsv son las del ImageBase original → relocalizar
                self.funcs[self.base + (int(r[0], 16) - self.pref)] = int(r[1])
                if r[5:] and r[5].strip() == "0" and r[2].startswith("FUN_"):
                    self._sin_refs.add(self.base + (int(r[0], 16) - self.pref))
                if r[2].startswith("Catch@"):
                    self.catches.append(self.base + (int(r[0], 16) - self.pref))
            self._quita_cortes()
        # Semillas automaticas (independientes de Ghidra): entry point + exportaciones. El resto
        # del grafo de llamadas se descubre al ir traduciendo (los `call` generan referencias), y
        # los metodos virtuales entran por data_code_refs(). Asi un EXE nuevo no necesita Ghidra.
        ep = self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        if ep:
            self.funcs.setdefault(self.base + ep, 0)
        # solo las que caen en una sección ejecutable: gfxwin.dll (PCF2000) exporta 183 símbolos de DATOS
        # (vtables, tablas de 64 KB como hiCol565ToInd/tabBlend) que, sembrados como código, daban
        # «funciones» de 600.000 líneas de ceros
        exec_rvas = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
                     for s in self.pe.sections if s.Characteristics & 0x20000000]
        for name, va in self.exports.items():
            if isinstance(name, str) and any(lo <= va - self.base < hi for lo, hi in exec_rvas):
                self.funcs.setdefault(va, 0)


class Linker:
    def __init__(self, modules=None):
        self.mods = []
        self.by_alias = {}
        self.iat = {}     # iat_va → (dll, name)
        self.os_funcs = set()
        for alias, path, base, decname in (modules or MODULES):
            if not os.path.exists(path):
                continue
            m = Module(alias, path, base, decname)
            self.mods.append(m)
            self.by_alias[alias.lower()] = m
            self.iat.update(m.imports)
        self._link_forwards()

    def _link_forwards(self):
        """Exportaciones reenviadas a otro módulo traducido: pasan a apuntar a la función de destino."""
        for _ in range(4):   # cadenas de reenvíos
            for m in self.mods:
                for key, fw in list(m.forwards.items()):
                    dll, _, nm = fw.partition(".")
                    dep = self.by_alias.get(dll.lower() + ".dll")
                    tgt = ("ord", int(nm[1:])) if nm.startswith("#") else nm
                    if dep is not None and tgt in dep.exports:
                        m.exports[key] = dep.exports[tgt]
                        del m.forwards[key]

    def resolve(self, dll, name):
        """Devuelve el nombre de la función C a llamar para un import (dll, name)."""
        dep = self.by_alias.get(dll.lower())
        if dep is not None and name in dep.forwards:   # reenviada a una DLL del sistema
            fdll, _, fname = dep.forwards[name].partition(".")
            dll, name = fdll.lower() + ".dll", fname
            dep = None
        if dep is not None:
            if name in dep.exports:
                return f"f_{dep.exports[name]:x}"
            if isinstance(name, tuple) and name in dep.exports:
                return f"f_{dep.exports[name]:x}"
        # sistema → stub
        fn = f"ext_{_san(dll)}_{_san(name if isinstance(name, str) else 'ord%d' % name[1])}"
        self.os_funcs.add(fn)
        return fn

    def data_code_refs(self):
        """Direcciones de codigo referenciadas desde DATOS (p.ej. vtables: los metodos virtuales
        se llaman por puntero, no con `call`). El lifter tambien debe traducirlas para que
        `icall` las encuentre en la FTAB.

        Se descartan los valores que caen DENTRO de una funcion conocida: son etiquetas internas
        (destinos de saltos por tabla), no entradas de funcion. Tratarlos como funciones nuevas
        genera funciones mal alineadas que ejecutan basura."""
        texts = [m.text for m in self.mods]

        def is_code(v):
            return any(lo <= v < hi for lo, hi in texts)

        # índice por módulo de funciones ordenadas por inicio (búsqueda binaria; antes era lineal
        # sobre todas las funciones por cada palabra de datos y con muchos módulos no acababa)
        import bisect as _bisect
        _idx = []
        for m in self.mods:
            fs = sorted((s, sz) for s, sz in m.funcs.items() if sz)
            maxsz = max((sz for _, sz in fs), default=0)
            _idx.append((m, [s for s, _ in fs], fs, maxsz))

        def containing_func(v):
            # como antes: la función de inicio más temprano que contiene v (pueden solaparse)
            for m, starts, fs, maxsz in _idx:
                if not (m.base <= v < m.base + len(m.image)):
                    continue
                k = _bisect.bisect_left(starts, v) - 1
                found = None
                while k >= 0 and starts[k] + maxsz > v:
                    s0, sz = fs[k]
                    if s0 < v < s0 + sz:
                        found = (m, s0, sz)
                    k -= 1
                if found:
                    return found
            return None

        def inside_known_func(v):
            return containing_func(v) is not None

        # Un puntero en datos al interior de una funcion conocida solo vale si cae en una frontera de
        # instruccion de esa funcion (p.ej. las tablas de _trandisp de MSVCRT apuntan a la mitad de
        # _CIfmod & co.: sin traducir, fmod() no hacia nada y el simulador 3D se quedaba en negro).
        _bounds = {}

        def at_insn_boundary(m, s, sz, v):
            if s not in _bounds:
                from capstone import Cs, CS_ARCH_X86, CS_MODE_32
                md0 = Cs(CS_ARCH_X86, CS_MODE_32)
                _bounds[s] = {i.address for i in md0.disasm(bytes(m.image[s - m.base:s - m.base + sz]), s)}
            return v in _bounds[s]

        refs = set()
        for m in self.mods:
            img = m.image
            tlo, thi = m.text[0] - m.base, m.text[1] - m.base
            for off in range(0, len(img) - 4, 4):
                if tlo <= off < thi:
                    continue
                v = struct.unpack_from("<I", img, off)[0]
                if not is_code(v):
                    continue
                cf = containing_func(v)
                if not cf or at_insn_boundary(*cf, v):
                    refs.add(v)
        # Punteros a codigo en INSTRUCCIONES: `push imm32` (0x68) y `mov reg,imm32` (0xB8-0xBF).
        for m in self.mods:
            img = m.image
            tlo, thi = m.text[0] - m.base, m.text[1] - m.base
            off = tlo
            while off < thi - 5:
                op = img[off]
                v = None
                if op == 0x68:
                    v = struct.unpack_from("<I", img, off + 1)[0]
                elif 0xB8 <= op <= 0xBF:
                    v = struct.unpack_from("<I", img, off + 1)[0]
                if v is not None and is_code(v) and not inside_known_func(v):
                    refs.add(v)
                off += 1
        # Punteros a codigo en INSTRUCCIONES, decodificadas con Capstone (cubre `mov [mem],imm32`
        # = C7, con el que se montan las vtables/controladores: p.ej. libjpeg del juego guarda
        # `consume_markers` con `mov [eax],0x10033300`). El barrido por bytes de arriba solo veia
        # `push imm32` (0x68) y `mov reg,imm32` (0xB8-0xBF), asi que estos destinos quedaban sin
        # traducir y `icall` los saltaba en silencio -> bucles infinitos (decodificador JPG).
        try:
            from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM
        except Exception:
            return refs
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        md.skipdata = True   # resincroniza tras bytes de datos/relleno (si no, el barrido lineal
                             # se desincroniza y deja de ver instrucciones -> pierde destinos)
        for m in self.mods:
            img = bytes(m.image)
            tlo, thi = m.text[0] - m.base, m.text[1] - m.base
            prev = None
            for ins in md.disasm(img[tlo:thi], m.text[0]):
                # funclet de `catch`: `mov eax, continuación; ret` → la continuación está a mitad de una
                # función conocida, pero el runtime de C++ salta allí
                if ins.mnemonic == "ret" and prev is not None and prev.mnemonic == "mov" and prev.op_str.startswith("eax, 0x"):
                    v = int(prev.op_str[5:], 16)
                    if is_code(v):
                        cf = containing_func(v)
                        if not cf or at_insn_boundary(*cf, v):
                            refs.add(v)
                prev = ins
                if ins.mnemonic not in ("mov", "push") or not ins.operands:
                    continue
                op = ins.operands[-1]
                if op.type != CS_OP_IMM:
                    continue
                v = op.imm & 0xFFFFFFFF
                if is_code(v) and not inside_known_func(v):
                    refs.add(v)
        # En los EXE de Visual Basic (con funciones de Ghidra), un destino fuera de toda función conocida solo se acepta si
        # parece un inicio de función (detrás de un final o con un prólogo típico). El .text de un EXE de
        # Visual Basic está lleno de datos (formularios, cadenas): sin este filtro se traducían miles de
        # "funciones" de basura (horas de traducción y cientos de MB de binario).
        def plausible(m, v):
            o = v - m.base
            b = m.image
            if o < 5 or o + 3 > len(b):
                return False
            if b[o - 1] in (0xC3, 0xCC, 0x90) or b[o - 3] == 0xC2 or b[o - 5] == 0xE9 or b[o - 2] == 0xEB:
                return True
            if bytes(b[o:o + 3]) in (b"\x55\x8b\xec", b"\x55\x89\xe5") or bytes(b[o:o + 2]) == b"\xff\x25":
                return True
            # con funciones de Ghidra basta un primer byte típico (sin ellas, demasiadas cadenas lo cumplen)
            if any(m.funcs.values()):
                return b[o] in (0x53, 0x55, 0x56, 0x57, 0x51, 0x6a, 0x68, 0xe9)
            return looks_like_code(m, v)

        # Decodifica desde v hasta 12 instrucciones (o hasta un ret/jmp): basura si sale algo que el código
        # de un compilador de 32 bits no genera (E/S, BCD, segmentos, instrucciones de 16 bits…)
        from capstone import Cs as _Cs, CS_ARCH_X86 as _A, CS_MODE_32 as _M32
        _md = _Cs(_A, _M32)
        _md.detail = True
        _lo_img = min(x.base for x in self.mods)
        _hi_img = max(x.base + len(x.image) for x in self.mods)
        _bad = {"in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "arpl", "bound", "les", "lds",
                "hlt", "into", "aaa", "aas", "daa", "das", "aam", "aad", "pushal", "popal", "salc", "iretd",
                "ljmp", "lcall", "retf", "sahf", "lahf", "cli", "sti", "wait", "xlatb", "lock", "int1"}

        def looks_like_code(m, v):
            o = v - m.base
            n = 0
            end = 0
            for ins in _md.disasm(bytes(m.image[o:o + 64]), v):
                if ins.mnemonic in _bad or ins.mnemonic.startswith("int") and ins.op_str != "3" \
                        or any(p in ins.op_str for p in ("es:", "fs:", "gs:", "ss:", "cs:", "ds:")):
                    return False
                for op in ins.operands:   # desplazamiento grande que no apunta a ningún módulo
                    if op.type == 3:      # X86_OP_MEM
                        d = op.mem.disp & 0xFFFFFFFF
                        if 0x100000 < d < 0xFFF00000 and not (_lo_img <= d < _hi_img):
                            return False
                n += 1
                end = ins.address + ins.size
                if ins.mnemonic.startswith(("ret", "jmp")) or n >= 12:
                    return True
            return n >= 4 and end - v >= 8
        def is_vb(m):
            return any(e.dll.decode().lower() == "msvbvm50.dll" for e in getattr(m.pe, "DIRECTORY_ENTRY_IMPORT", []) or [])
        # filtrados: los EXE de VB y los módulos sin funciones de Ghidra (runtime de VB, OLE, OCX), cuyo
        # .text también mezcla datos
        ghidra = {id(m) for m in self.mods if (any(m.funcs.values()) and is_vb(m)) or not any(m.funcs.values())}
        # zonas de datos del PE que algunos módulos (OCX) llevan dentro de .text: directorios (importaciones,
        # IAT, exportaciones…) y los nombres de las funciones y DLL importadas
        nocode = []
        for m in self.mods:
            for d in m.pe.OPTIONAL_HEADER.DATA_DIRECTORY:
                if d.VirtualAddress and d.Size:
                    nocode.append((m.base + d.VirtualAddress, m.base + d.VirtualAddress + d.Size))
            for e in getattr(m.pe, "DIRECTORY_ENTRY_IMPORT", []) or []:
                nocode.append((m.base + e.struct.Name, m.base + e.struct.Name + len(e.dll) + 1))
                for imp in e.imports:
                    if imp.name and imp.hint_name_table_rva:
                        r = m.base + imp.hint_name_table_rva
                        nocode.append((r, r + 3 + len(imp.name)))
        nocode.sort()
        starts_nc = [lo for lo, _ in nocode]

        def in_nocode(v):
            k = _bisect.bisect_right(starts_nc, v) - 1
            while k >= 0 and v - nocode[k][0] < 1 << 16:
                if nocode[k][0] <= v < nocode[k][1]:
                    return True
                k -= 1
            return False
        out = set()
        for v in refs:
            if in_nocode(v):
                continue
            m = next((x for x in self.mods if x.text[0] <= v < x.text[1]), None)
            if m is None or id(m) not in ghidra or v in m.funcs or inside_known_func(v) or plausible(m, v):
                out.add(v)
        # Vtables dentro de .text (MSVBVM50 y otros módulos sin funciones de Ghidra): rachas de al menos
        # dos palabras alineadas que apuntan a algo con pinta de inicio de función.
        def looks_like_start(m, v):
            o = v - m.base
            if o < 3 or o >= len(m.image):
                return False
            b = m.image
            return b[o - 1] in (0xC3, 0xCC, 0x90) or b[o - 3] == 0xC2
        for m in self.mods:
            if any(m.funcs.values()):
                continue
            img = m.image
            tlo, thi = m.text[0] - m.base, m.text[1] - m.base
            cand = {}
            for off in range(tlo & ~3, thi - 4, 4):
                v = struct.unpack_from("<I", img, off)[0]
                if m.text[0] <= v < m.text[1] and not in_nocode(v) and (looks_like_start(m, v) or looks_like_code(m, v)):
                    cand[off] = v
            for off, v in cand.items():
                if off - 4 in cand or off + 4 in cand or (looks_like_start(m, v) and looks_like_code(m, v)):
                    out.add(v)
        return out

    def resolve_iat(self, iat_va):
        imp = self.iat.get(iat_va)
        if imp is None:
            return None
        return self.resolve(*imp)

    # region fija donde se colocan los "trampolines" de los stubs (para que los IAT apunten a
    # algo que este en la FTAB). Cada trampolin son 16 bytes, determinista y compartido entre
    # runtime.py (que los emite) y mkmem.py (que escribe los IAT).
    # TRAMP_BASE: el primer MB libre por encima de los módulos (MANAGER: 0x2300000). Detrás van las
    # funciones del host con dirección de invitado (+0x80000) y el montículo (+0x100000): ver layout.h.
    # Solo se usa como puntero a función en la FTAB (nunca se lee/escribe).
    @property
    def TRAMP_BASE(self):
        end = max(m.base + m.size for m in self.mods)
        return (end + 0xFFFFF) & ~0xFFFFF

    def resolve_iats(self):
        """Resuelve los IAT EN LAS IMAGENES de los modulos, como haria el cargador de Windows:
        cada entrada del IAT pasa a apuntar a la funcion real (del juego) o a un trampolin que
        llama al stub del sistema. Sin esto, `mov reg,[IAT]; call reg` cargaria el valor crudo
        del fichero (una RVA basura) y el `icall` no haria nada (y no limpiaria los args).
        Devuelve [(tramp_va, nombre_stub)] ordenado."""
        tramps = {}
        out = []
        for iat_va in sorted(self.iat):
            tgt = self.resolve(*self.iat[iat_va])
            if tgt.startswith("f_"):
                va = int(tgt[2:], 16)
            else:
                if tgt not in tramps:
                    va = self.TRAMP_BASE + len(tramps) * 16
                    tramps[tgt] = va
                    out.append((va, tgt))
                va = tramps[tgt]
            for m in self.mods:
                if m.base <= iat_va < m.base + len(m.image):
                    struct.pack_into("<I", m.image, iat_va - m.base, va)
                    break
        return out
