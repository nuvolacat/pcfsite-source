"""Lifter x86 → C — modelo con registros GLOBALES (CPU compartida), para que las funciones
se llamen entre sí (call / jmp-cola) manteniendo la pila.

Cada función se emite como `static void f_<addr>(void)` que opera sobre los globales
(eax…edi, CF/ZF/SF/OF). `ret N` hace `esp += 4 + N; return;`. `call` empuja la dirección de
retorno y llama. `jmp` a otra función = cola (`f_x(); return;`). Los `call` indirectos (IAT y
vtables) van a `icall(addr)` (despacho en tiempo de ejecución).
"""
from __future__ import annotations

import csv
import re
import functools
import os
import os

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import *
from capstone.x86 import (X86_REG_ES, X86_REG_CS, X86_REG_SS, X86_REG_DS,
                          X86_REG_FS, X86_REG_GS, X86_REG_MM0)

# Rutas configurables (cada versión del juego tiene su binario y su exportación de Ghidra):
#   PCF_PE      = ruta al .EXE/.DLL a traducir
#   PCF_DECOMP  = carpeta con functions.tsv/callgraph.json (salida de Ghidra)
PE = os.environ.get("PCF_PE", "manager.exe")
DEC = os.environ.get("PCF_DECOMP", "decomp")

# Modo multi-módulo (enlazador): si `_MOD` está puesto, el lifter usa la imagen relocalizada
# de ese módulo y resuelve los `call [IAT]` con el mapa `IAT` (iat_va → nombre de función C).
_MOD = None
IAT: dict = {}


def set_module(mod, iat=None):
    global _MOD, IAT, _STARTS_CACHE
    _MOD = mod
    _STARTS_CACHE = None
    if iat is not None:
        IAT = iat

R32 = {X86_REG_EAX: "eax", X86_REG_ECX: "ecx", X86_REG_EDX: "edx", X86_REG_EBX: "ebx",
       X86_REG_ESP: "esp", X86_REG_EBP: "ebp", X86_REG_ESI: "esi", X86_REG_EDI: "edi"}
R16 = {X86_REG_AX: "eax", X86_REG_CX: "ecx", X86_REG_DX: "edx", X86_REG_BX: "ebx",
       X86_REG_SP: "esp", X86_REG_BP: "ebp", X86_REG_SI: "esi", X86_REG_DI: "edi"}
R8 = {X86_REG_AL: ("eax", 0), X86_REG_CL: ("ecx", 0), X86_REG_DL: ("edx", 0),
      X86_REG_BL: ("ebx", 0), X86_REG_AH: ("eax", 8), X86_REG_CH: ("ecx", 8),
      X86_REG_DH: ("edx", 8), X86_REG_BH: ("ebx", 8)}
SEG = {X86_REG_ES, X86_REG_CS, X86_REG_SS, X86_REG_DS, X86_REG_FS, X86_REG_GS}

# MS-DOS (juegos con «dos»: true en juego.json, p. ej. PC Fútbol 4 con DOS4GW): interrupciones, puertos de E/S y
# registros de segmento de verdad (ver _dos_ins). Los juegos de Windows no cambian.
import juego as _J  # noqa: E402
DOS = bool(_J.CFG.get("dos"))
_SREG = {X86_REG_ES: 0, X86_REG_CS: 1, X86_REG_SS: 2, X86_REG_DS: 3, X86_REG_FS: 4, X86_REG_GS: 5}

CC = {"e": "ZF", "z": "ZF", "ne": "!ZF", "nz": "!ZF",
      "b": "CF", "c": "CF", "nae": "CF", "ae": "!CF", "nb": "!CF", "nc": "!CF",
      "be": "CF||ZF", "na": "CF||ZF", "a": "!CF&&!ZF", "nbe": "!CF&&!ZF",
      "s": "SF", "ns": "!SF", "o": "OF", "no": "!OF", "p": "PF", "pe": "PF", "np": "!PF", "po": "!PF",
      "l": "SF!=OF", "nge": "SF!=OF", "ge": "SF==OF", "nl": "SF==OF",
      "le": "ZF||SF!=OF", "ng": "ZF||SF!=OF", "g": "!ZF&&SF==OF", "nle": "!ZF&&SF==OF"}


class Unsupported(Exception):
    pass


_IMAGE = None
_TEXT = None


def image(pe_path=PE):
    global _IMAGE
    if _MOD is not None:
        # copia inmutable de la imagen, una vez por módulo (copiarla en cada función era lentísimo)
        if getattr(_MOD, "_img_bytes", None) is None or len(_MOD._img_bytes) != len(_MOD.image):
            _MOD._img_bytes = bytes(_MOD.image)
        return _MOD.base, _MOD._img_bytes, _MOD.size
    if _IMAGE is None:
        pe = pefile.PE(pe_path)
        _IMAGE = (pe.OPTIONAL_HEADER.ImageBase, bytes(pe.get_memory_mapped_image()),
                  pe.OPTIONAL_HEADER.SizeOfImage)
    return _IMAGE


def text_range(pe_path=PE):
    global _TEXT
    if _MOD is not None:
        return _MOD.text
    if _TEXT is None:
        pe = pefile.PE(pe_path)
        base = pe.OPTIONAL_HEADER.ImageBase
        sec = next(s for s in pe.sections if b".text" in s.Name)
        _TEXT = (base + sec.VirtualAddress, base + sec.VirtualAddress + sec.Misc_VirtualSize)
    return _TEXT


def block_size(va, cap=512):
    """Tamaño de un bloque sin entrada propia: desensambla hasta `ret`/`jmp` (inclusive)."""
    base, img, _ = image()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    n = 0
    for ins in md.disasm(img[va - base: va - base + cap], va):
        n = ins.address - va + ins.size
        if ins.mnemonic == "ret" or ins.mnemonic.startswith("jmp"):
            break
    return n or 1


@functools.lru_cache(None)
def _funcs_file():
    d = {}
    for r in csv.reader(open(f"{DEC}/functions.tsv", encoding="utf-8", errors="replace"), delimiter="\t"):
        if r[0] == "entry":
            continue
        d[int(r[0], 16)] = int(r[1])
    return d


def funcs():
    if _MOD is not None:
        return _MOD.funcs
    return _funcs_file()


ENTRIES = None


def entries():
    global ENTRIES
    if _MOD is not None:
        if getattr(_MOD, "_entries", None) is None or len(_MOD._entries) != len(_MOD.funcs):
            _MOD._entries = set(_MOD.funcs)
        return _MOD._entries
    if ENTRIES is None:
        ENTRIES = set(funcs())
    return ENTRIES


# Plantillas de código que el runtime copia al montículo rellenando huecos 0x66… (thunks de MSVBVM50): sus
# funciones se traducen leyendo cada hueco de la copia en ejecución (_tb). _PH: huecos de la instrucción en curso.
TEMPLATES = set()
_PH = None


def _dos_ins(m, ins, d, s, rep):
    """Instrucciones que en MS-DOS hacen algo (en Windows se ignoran): `int n` → dos_int (platform/src/dos.cpp),
    `in`/`out` → puertos (VGA, temporizador, teclado, Sound Blaster…), cli/sti y el bit IF, y los registros de
    segmento (sreg[]: se guardan y se leen; las direcciones siguen siendo planas). None = la de siempre."""
    if m == "int" and d is not None and d.type == X86_OP_IMM:
        return [f"dos_int(0x{d.imm & 0xFF:x}u);"]
    if m == "in" and d is not None and s is not None:
        port = "(edx & 0xFFFF)" if s.type == X86_OP_REG else f"0x{s.imm & 0xFF:x}u"
        return wr(d, d.size, f"port_in({port}, {d.size})")
    if m == "out" and d is not None and s is not None:
        port = "(edx & 0xFFFF)" if d.type == X86_OP_REG else f"0x{d.imm & 0xFF:x}u"
        return [f"port_out({port}, {rd(s, s.size)}, {s.size});"]
    if m in ("insb", "insw", "insd", "outsb", "outsw", "outsd"):
        step = {"b": 1, "w": 2, "d": 4}[m[-1]]
        adv = f"(DF ? -{step} : {step})"
        body = (f"wr{step * 8}(edi, port_in(edx & 0xFFFF, {step})); edi += {adv};" if m.startswith("ins")
                else f"port_out(edx & 0xFFFF, rd{step * 8}(esi), {step}); esi += {adv};")
        return [f"while (ecx) {{ {body} ecx--; }}"] if rep else [body]
    if m == "cli":
        return ["g_if = 0;"]
    if m == "sti":
        return ["g_if = 1; DOS_POLL();"]
    if m == "pushfd":
        return ["esp -= 4; wr32(esp, CF | (PF<<2) | (ZF<<6) | (SF<<7) | (g_if<<9) | (DF<<10) | (OF<<11) | (EFL_HI & 0x240000u));"]
    if m == "pushf":   # 66 9C: solo la palabra baja (dos bytes de pila; el mezclador del PCF4 cuenta con ello)
        return ["esp -= 2; wr16(esp, CF | (PF<<2) | (ZF<<6) | (SF<<7) | (g_if<<9) | (DF<<10) | (OF<<11));"]
    if m == "popfd":
        return ["{u32 _f=rd32(esp); esp += 4; CF=_f&1; PF=(_f>>2)&1; ZF=(_f>>6)&1; SF=(_f>>7)&1; g_if=(_f>>9)&1; DF=(_f>>10)&1; "
                "OF=(_f>>11)&1; EFL_HI=_f&0x240000u; DOS_POLL();}"]
    if m == "popf":
        return ["{u32 _f=rd16(esp); esp += 2; CF=_f&1; PF=(_f>>2)&1; ZF=(_f>>6)&1; SF=(_f>>7)&1; g_if=(_f>>9)&1; DF=(_f>>10)&1; "
                "OF=(_f>>11)&1; DOS_POLL();}"]
    if m == "mov" and d is not None and s is not None:
        if d.type == X86_OP_REG and d.reg in SEG:
            return [f"seg_load({_SREG[d.reg]}, {rd(s, s.size)});"]
        if s.type == X86_OP_REG and s.reg in SEG:
            return wr(d, d.size, f"sreg[{_SREG[s.reg]}]")
    if m in ("push", "pop") and d is not None and d.type == X86_OP_REG and d.reg in SEG:
        i = _SREG[d.reg]
        return [f"{{esp -= 4; wr32(esp, sreg[{i}]);}}"] if m == "push" else [f"seg_load({i}, rd32(esp)); esp += 4;"]
    if m in ("lar", "lsl") and d is not None:   # derechos y límite de un selector: datos de 32 bits, sin límite
        v = ("0xF200u" if d.size == 2 else "0x00C0F200u") if m == "lar" else "0xFFFFFFFFu"
        return wr(d, d.size, v) + ["ZF = 1;"]
    if m in ("verr", "verw"):
        return ["ZF = 1;"]
    if m in ("lds", "les", "lfs", "lgs", "lss") and d is not None and s is not None:
        i = {"lds": 3, "les": 0, "lfs": 4, "lgs": 5, "lss": 2}[m]
        return [f"{{u32 _a = {mem_addr(s)}; u32 _o = rd{d.size * 8}(_a); seg_load({i}, rd16(_a + {d.size}));"] + wr(d, d.size, "_o") + ["}"]
    return None


def mem_addr(op, seg=True):
    m = op.mem
    parts = []
    if DOS and seg:   # base del segmento (sbase[]: 0 en el código plano normal; el PSP, el entorno… tienen la suya)
        sr = m.segment if m.segment in _SREG else (X86_REG_SS if m.base in (X86_REG_ESP, X86_REG_EBP) else X86_REG_DS)
        parts.append(f"sbase[{_SREG[sr]}]")
    if m.base:
        parts.append(R32.get(m.base) or R16.get(m.base) or (_ for _ in ()).throw(Unsupported("base")))
    if m.index:
        parts.append(f"({R32.get(m.index) or R16.get(m.index)}*{m.scale})")
    if m.disp and _PH and "disp" in _PH:
        parts.append(f"({_PH['disp']})")
    elif m.disp:
        parts.append(f"({m.disp})" if m.disp < 0 else str(m.disp))
    return " + ".join(parts) if parts else "0"


# Inmediatos vivos: en estas instrucciones el inmediato se lee en tiempo de ejecución de los bytes de la propia
# instrucción en MEM (IMM*(dirección)), en vez de quedar como constante de C. Así un EXE modificado por un mod (los de
# PC Fútbol cambian `mov [tabla], valor`, `push valor`, `mov reg, valor`: fechas, tamaños, identificadores) surte
# efecto: la app aplica sus diferencias sobre la imagen (platform/src/imagen.cpp). Los saltos, llamadas, `ret n`,
# `int`, desplazamientos de bits, etc. siguen siendo constantes (son control de flujo o no cambian en los mods).
_IMM_VIVOS = {"mov", "push", "cmp", "test", "add", "sub", "adc", "sbb", "and", "or", "xor", "imul"}
_CUR_INS = None   # la instrucción que se está traduciendo (la pone _emit)


def _imm_vivo(op, size):
    ins = _CUR_INS
    if ins is None or ins.mnemonic not in _IMM_VIVOS or not ins.imm_size or ins.imm_size > 4:
        return None
    if sum(1 for o in ins.operands if o.type == X86_OP_IMM) != 1:
        return None
    a = ins.address + ins.imm_offset
    n = ins.imm_size
    mask = (1 << (8 * size)) - 1
    if n < size:   # inmediato corto extendido con signo (push imm8, add eax, imm8…)
        return f"(IMMS{8 * n}(0x{a:x}u) & 0x{mask:x}u)"
    return f"(IMM{8 * n}(0x{a:x}u) & 0x{mask:x}u)" if n > size else f"IMM{8 * n}(0x{a:x}u)"


def rd(op, size):
    if op.type == X86_OP_REG:
        if size == 4:
            return R32[op.reg]
        if size == 2:
            return f"({R16[op.reg]} & 0xFFFF)"
        if size == 1:
            b, sh = R8[op.reg]
            return f"(({b} >> {sh}) & 0xFF)"
    if op.type == X86_OP_IMM:
        if _PH and "imm" in _PH:
            return f"(({_PH['imm']}) & 0x{(1 << (8 * size)) - 1:x}u)"
        vivo = _imm_vivo(op, size)
        if vivo is not None:
            return vivo
        # inmediato recortado al ancho del operando (`cmp al, -1` -> 0xff; antes 0xffffffff y CF siempre 1)
        return f"0x{op.imm & ((1 << (8 * size)) - 1):x}u"
    if op.type == X86_OP_MEM:
        a = mem_addr(op)
        return {4: f"rd32({a})", 2: f"rd16({a})", 1: f"rd8({a})"}[size]
    raise Unsupported("operando")


def wr(op, size, expr):
    if op.type == X86_OP_REG:
        if size == 4:
            return [f"{R32[op.reg]} = {expr};"]
        if size == 2:
            return [f"{R16[op.reg]} = ({R16[op.reg]} & 0xFFFF0000) | (({expr}) & 0xFFFF);"]
        if size == 1:
            b, sh = R8[op.reg]
            mask = 0xFFFF00FF if sh == 8 else 0xFFFFFF00
            return [f"{b} = ({b} & 0x{mask:08x}) | ((({expr}) & 0xFF) << {sh});"]
    if op.type == X86_OP_MEM:
        a = mem_addr(op)
        return [{4: f"wr32({a}, {expr});", 2: f"wr16({a}, {expr});", 1: f"wr8({a}, {expr});"}[size]]
    raise Unsupported("destino")


# BUG corregido 14-oct (15ª ronda): las banderas se calculaban SIEMPRE a 32 bits. Con operandos de
# 8/16 bits `inc al` (al=0xFF) daba t=0x100 y ZF=0 (DrawMix 16 bpp de Utils nunca tomaba la rama
# "máscara opaca" y los escudos salían negros), SF/OF miraban el bit 31 en vez del 7/15 (jl/jg de
# bytes), `sar al` no extendía el signo, `shl al` no sacaba el acarreo del bit 8, `rol/ror` rotaban
# en 32 bits e `imul ax, bx` multiplicaba sin signo. Ahora todo se hace en el ancho del operando.
def _szinfo(size):
    bits = 8 * size
    return bits, f"0x{(1 << bits) - 1:x}u", f"0x{1 << (bits - 1):x}u"


def f_flags_zsp(size):
    bits, M, S = _szinfo(size)
    return [f"ZF = ((t & {M}) == 0);", f"SF = (t >> {bits - 1}) & 1;",
            "PF = (__builtin_parity((u32)(t & 0xFF)) ^ 1);"]


def f_sub(a, b, size=4):
    bits, M, S = _szinfo(size)
    return [f"t = ({a} - {b}) & {M};", f"CF = {a} < {b};"] + f_flags_zsp(size) +            [f"OF = ((({a} ^ {b}) & ({a} ^ t) & {S}) != 0);"]


def f_add(a, b, size=4):
    bits, M, S = _szinfo(size)
    if size == 4:
        cf = f"CF = t < {a};"
        return [f"t = {a} + {b};", cf] + f_flags_zsp(size) + [f"OF = ((({a} ^ t) & ({b} ^ t) & {S}) != 0);"]
    return [f"t = {a} + {b};", f"CF = (t >> {bits}) & 1;", f"t &= {M};"] + f_flags_zsp(size) +            [f"OF = ((({a} ^ t) & ({b} ^ t) & {S}) != 0);"]


def f_logic(e, size=4):
    bits, M, S = _szinfo(size)
    return [f"t = ({e}) & {M};", "CF = 0;", "OF = 0;"] + f_flags_zsp(size)


def _is_mm(r):
    return X86_REG_MM0 <= r <= X86_REG_MM0 + 7


def mmx(m, d, s, sz, rd, wr):
    """Emite una operación MMX (registros MM[8] de 64 bits)."""
    dm = d.type == X86_OP_REG and _is_mm(d.reg)
    di = (d.reg - X86_REG_MM0) if dm else None
    sm = s is not None and s.type == X86_OP_REG and _is_mm(s.reg)
    si = (s.reg - X86_REG_MM0) if sm else None

    def src64():
        if sm:
            return f"MM[{si}]"
        if s is not None and s.type == X86_OP_MEM:
            return f"rd64({mem_addr(s)})"
        if s is not None and s.type == X86_OP_IMM:
            # BUG corregido 14-oct: los desplazamientos MMX con cuenta INMEDIATA (psrlw mm0, 8;
            # psllw mm1, 0xb...) se traducian con cuenta 0 (no-op) -> las mezclas 565 de Utils
            # (DrawMask/DrawMix a 16 bpp) salian como ruido de colores en los textos.
            return f"{s.imm & 0xFFFFFFFFFFFFFFFF}ull"
        return "0ull"

    if m == "movd":
        if dm:
            if sm:
                return f"MM[{di}] = MM[{si}] & 0xFFFFFFFFull;"
            if s is not None and s.type == X86_OP_MEM:
                return f"MM[{di}] = (u64)rd32({mem_addr(s)});"
            return f"MM[{di}] = (u64)({rd(s, 4)});"
        return wr(d, 4, f"(u32)MM[{si}]")[0]
    if m == "movq":
        if dm:
            return f"MM[{di}] = {src64()};"
        return f"wr64({mem_addr(d)}, MM[{si}]);"
    if m in ("pxor", "pand", "por"):
        op = {"pxor": "^", "pand": "&", "por": "|"}[m]
        return f"MM[{di}] = MM[{di}] {op} {src64()};"
    if m == "pandn":
        return f"MM[{di}] = (~MM[{di}]) & {src64()};"
    if m in ("paddb", "paddw", "paddd", "psubb", "psubw", "psubd"):
        bits = {"b": 8, "w": 16, "d": 32}[m[-1]]
        n = 64 // bits
        sign = "+" if m.startswith("padd") else "-"
        return (f"{{u64 _a=MM[{di}],_b={src64()},_r=0; for(int _i=0;_i<{n};_i++){{"
                f"u64 _m=((1ull<<{bits})-1)<<({bits}*_i); "
                f"u64 _x=((_a&_m)>>({bits}*_i)){sign}((_b&_m)>>({bits}*_i)); "
                f"_r|=((_x&((1ull<<{bits})-1))<<({bits}*_i));}} MM[{di}]=_r;}}")
    if m in ("psllq", "psrlq"):
        cnt = src64() if s is not None else "0"
        op = "<<" if m == "psllq" else ">>"
        return f"MM[{di}] = MM[{di}] {op} (({cnt}) & 63);"
    if m in ("punpcklbw", "punpckhbw", "punpcklwd", "punpckhwd", "punpckldq", "punpckhdq"):
        hi = 4 if "h" in m else 0
        wd = 4 if "dq" in m else (2 if "wd" in m else 1)
        n = 64 // (8 * wd)
        return (f"{{u64 _a=MM[{di}],_b={src64()},_r=0; for(int _i=0;_i<{n};_i++){{"
                f"u64 _x=(_a>>(8*({hi}+{wd}*_i)))&0x{(1<<(8*wd))-1:x},_y=(_b>>(8*({hi}+{wd}*_i)))&0x{(1<<(8*wd))-1:x}; "
                f"_r|=((_x&0x{(1<<(8*wd))-1:x})<<(8*{wd}*(2*_i)))|((_y&0x{(1<<(8*wd))-1:x})<<(8*{wd}*(2*_i+1)));}} MM[{di}]=_r;}}")
    if m == "packuswb":
        return (f"{{u64 _a=MM[{di}],_b={src64()},_r=0; for(int _i=0;_i<4;_i++){{"
                f"i32 _x=(i16)((_a>>(16*_i))&0xFFFF), _y=(i16)((_b>>(16*_i))&0xFFFF); "
                f"if(_x<0)_x=0; if(_x>255)_x=255; if(_y<0)_y=0; if(_y>255)_y=255; "
                f"_r|=((u64)(u8)_x<<(8*_i))|((u64)(u8)_y<<(8*(4+_i)));}} MM[{di}]=_r;}}")
    if m == "pmullw":
        return (f"{{u64 _a=MM[{di}],_b={src64()},_r=0; for(int _i=0;_i<4;_i++){{"
                f"u16 _x=(u16)((_a>>(16*_i))&0xFFFF), _y=(u16)((_b>>(16*_i))&0xFFFF); "
                f"_r|=((u64)(u16)(_x*_y)<<(16*_i));}} MM[{di}]=_r;}}")
    if m in ("psllw", "psrlw", "psraw", "pslld", "psrld", "psrad"):
        bits = 16 if m[-1] == "w" else 32
        n = 64 // bits
        cnt = src64() if s is not None else "0"
        if m.startswith("psll"):
            e = f"(_x << ((_c)&({bits-1})))"
        elif m.startswith("psrl"):
            e = f"(_x >> ((_c)&({bits-1})))"
        else:
            e = f"(({'i16' if bits==16 else 'i32'})_x >> ((_c)&({bits-1})))"
        return (f"{{u64 _a=MM[{di}],_r=0; u32 _c=({cnt})&({bits-1}); for(int _i=0;_i<{n};_i++){{"
                f"u32 _x=(u32)((_a>>({bits}*_i))&((1ull<<{bits})-1)); "
                f"_r|=(((u64)((1ull<<{bits})-1 & {e}))<<({bits}*_i));}} MM[{di}]=_r;}}")
    if m in ("paddusb", "psubusb", "paddusw", "psubusw"):
        bits = 8 if m[-1] == "b" else 16
        n = 64 // bits
        sign = "+" if m.startswith("padd") else "-"
        mx = (1 << bits) - 1
        return (f"{{u64 _a=MM[{di}],_b={src64()},_r=0; for(int _i=0;_i<{n};_i++){{"
                f"i64 _x=(i64)((_a>>({bits}*_i))&{mx}ull), _y=(i64)((_b>>({bits}*_i))&{mx}ull); "
                f"_x=_x{sign}_y; if(_x<0)_x=0; if(_x>{mx})_x={mx}; "
                f"_r|=((u64)_x<<({bits}*_i));}} MM[{di}]=_r;}}")
    if m in ("pcmpeqb", "pcmpeqw", "pcmpeqd"):
        bits = {"b": 8, "w": 16, "d": 32}[m[-1]]
        n = 64 // bits
        return (f"{{u64 _a=MM[{di}],_b={src64()},_r=0; for(int _i=0;_i<{n};_i++){{"
                f"u64 _m=((1ull<<{bits})-1)<<({bits}*_i); u64 _x=((_a&_m)>>({bits}*_i)),_y=((_b&_m)>>({bits}*_i)); "
                f"if(_x==_y)_r|=_m;}} MM[{di}]=_r;}}")
    raise Unsupported(m)


_TERMINAL = ("ret", "retf", "iret", "iretd", "hlt", "ud2", "int3")


def _falls_through(ins):
    m = ins.mnemonic
    return not (m in _TERMINAL or m.startswith("jmp") or m.startswith("ret"))


def _branch_targets(ins, base, img, lo, hi):
    """Destinos (dentro de [lo, hi)) de un salto directo o de una tabla de saltos `jmp [t + r*4]`."""
    out = []
    m = ins.mnemonic
    ops = ins.operands
    if not ops:
        return out
    d = ops[0]
    if (m.startswith("j") or m.startswith("loop")) and d.type == X86_OP_IMM:
        if lo <= d.imm < hi:
            out.append(d.imm)
    elif m == "jmp" and d.type == X86_OP_MEM and d.mem.index and not d.mem.base and d.mem.scale == 4:
        miss = 0
        for k in range(512):
            off = d.mem.disp - base + 4 * k
            if off < 0 or off + 4 > len(img):
                break
            t = int.from_bytes(img[off:off + 4], "little")
            if lo <= t < hi:
                out.append(t)
                miss = 0
            else:
                miss += 1
                if miss > 8:
                    break
    return out


def _chkstk_tail(insns, k, reg):
    """¿`jmp reg` en insns[k] es el final de un __chkstk? Justo antes: `mov reg, dword ptr [eax + 4]`, y poco antes
    `mov esp, ecx` (la pila ya bajada el tamaño del marco) y `mov eax, esp`."""
    if k < 4:
        return False
    prev = insns[k - 1]
    if prev.mnemonic != "mov" or len(prev.operands) != 2:
        return False
    d, s = prev.operands
    if d.type != X86_OP_REG or d.reg != reg or s.type != X86_OP_MEM or s.mem.base != X86_REG_EAX or s.mem.disp != 4:
        return False
    textos = [i.op_str.replace(" ", "") for i in insns[max(0, k - 6):k] if i.mnemonic == "mov"]
    return "esp,ecx" in textos and "eax,esp" in textos


def _follow_flow(insns, va, limit, base, img, md, tlo_thi=None, own_hi=None):
    known = {i.address: i for i in insns}
    if not known:
        return insns
    spans = sorted((i.address, i.address + i.size) for i in insns)

    def overlaps(a, n):
        import bisect as _b
        j = _b.bisect_right(spans, (a, 1 << 40)) - 1
        for k in (j, j + 1):
            if 0 <= k < len(spans) and spans[k][0] < a + n and a < spans[k][1]:
                return spans[k][0] != a or spans[k][1] != a + n
        return False

    def pushed_label(i):   # `push etiqueta` (destino de un `ret` posterior, p. ej. los __finally de OLEAUT32)
        if i.mnemonic == "push" and i.operands and i.operands[0].type == X86_OP_IMM:
            v = i.operands[0].imm & 0xFFFFFFFF
            if va < v < (own_hi or limit):
                return [v]
        return []

    work = []
    for i in insns:
        work += _branch_targets(i, base, img, va, limit) + pushed_label(i)
        # el barrido lineal también se corta tras instrucciones que siguen de largo (p.ej. un `call`
        # cuyo tamaño de función, sin entrada de Ghidra, se estimó corto): seguir la caída
        if _falls_through(i) and i.address + i.size not in known:
            work.append(i.address + i.size)
    added = False
    stops = getattr(_MOD, "stops", None) or ()   # otras entradas: se llaman, no se absorben
    while work:
        a = work.pop()
        while a not in known and va <= a < limit and (a == va or a not in stops):
            chunk = img[a - base: min(limit, a + 16) - base]
            dec = list(md.disasm(chunk, a, 1))
            if not dec:
                break
            ins = dec[0]
            if overlaps(ins.address, ins.size) or ins.mnemonic in ("bound", "arpl", "into") or \
                    any(op.size not in (0, 1, 2, 4, 8, 10, 16) for op in ins.operands):
                break
            known[a] = ins
            added = True
            import bisect as _b
            _b.insort(spans, (a, a + ins.size))
            work += _branch_targets(ins, base, img, va, limit) + pushed_label(ins)
            if not _falls_through(ins):
                break
            a += ins.size
    if not added:
        return insns
    return [known[k] for k in sorted(known)]


def lift_function(va, size=None, pe_path=PE, cluster=None):
    """Devuelve el C de una función (modelo global). Lanza Unsupported si no se puede.

    cluster=(hi, entradas): traduce [va, hi) como UNA función con varias entradas (el intérprete de
    p-code de MSVBVM50: cientos de manejadores que saltan entre sí por tablas). Se emite
    `fc_<va>(entrada)` con un switch inicial y cada entrada `f_x` la llama."""
    base, img, _ = image(pe_path)
    if cluster:
        return _lift_cluster(va, cluster[0], sorted(cluster[1]), base, img, pe_path)
    if size is None:
        size = funcs().get(va) or block_size(va)
    # Ghidra a veces da un tamaño que solapa con la funcion siguiente; lo recortamos.
    global _STARTS_CACHE
    if _STARTS_CACHE is None:
        if _MOD is not None:
            if getattr(_MOD, "_starts", None) is None or len(_MOD._starts) != len(_MOD.funcs):
                _MOD._starts = sorted(_MOD.funcs)
            _STARTS_CACHE = _MOD._starts
        else:
            _STARTS_CACHE = sorted(funcs())
    _starts = _STARTS_CACHE
    import bisect as _bs
    _i = _bs.bisect_right(_starts, va)
    if _i < len(_starts) and va + size > _starts[_i]:
        size = _starts[_i] - va
    # Desensamblamos una VENTANA que llega hasta la funcion siguiente (o +64 B). El tamaño que da
    # Ghidra a veces CORTA a medias la ultima instruccion (p.ej. un `ret 0xc` de 3 B del que solo
    # quedan 2 dentro del tamaño): si decodificamos solo `size` bytes, esa instruccion queda sin
    # decodificar, el `ret` desaparece y la funcion cae en el relleno + prologo de la siguiente
    # (desvia la pila). Decodificamos de mas y recortamos a las instrucciones que EMPIEZAN dentro
    # del tamaño declarado.
    _end = va + size + 64
    if _i < len(_starts) and _starts[_i] < _end:
        _end = _starts[_i]
    code = img[va - base: _end - base]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    insns = [i for i in md.disasm(code, va) if i.address < va + size]
    # Ghidra a veces NO incluye el `ret` final en el tamaño de la funcion. Sin el, la pila
    # emulada no se limpia (no se hace `esp += 4 + N`) y se desvia 4 bytes en CADA llamada.
    # Ampliamos la decodificacion hasta el primer ret/jmp para incluirlo.
    if insns:
        _lm = insns[-1].mnemonic
        if _lm not in ("ret", "retf", "iret", "iretd", "hlt") and not _lm.startswith("jmp"):
            _st = insns[-1].address + insns[-1].size
            for _ins in md.disasm(img[_st - base: _end - base], _st):
                insns.append(_ins)
                if _ins.mnemonic in ("ret", "retf", "iret", "iretd") or _ins.mnemonic.startswith("jmp"):
                    break
    # La desensamblación lineal puede meterse en los bytes de una tabla de saltos (datos). Se
    # corta en la primera instrucción "imposible" (tamaño de operando raro o registro dr/cr/tr);
    # así las etiquetas de los saltos por tabla siempre existen.
    def _is_data(_ins):
        _ops = _ins.operands
        if any(op.size not in (0, 1, 2, 4, 8, 10, 16) for op in _ops):
            return True
        if any(op.type == X86_OP_REG and _ins.reg_name(op.reg)[:2] in ("dr", "cr", "tr") for op in _ops):
            return True
        return False
    for _k, _ins in enumerate(insns):
        if _is_data(_ins):
            insns = insns[:_k]
            break
    # PCF7: las tablas (de saltos `jmp [t+r*4]` y de índices `movzx r, byte [t+r]`) que caen dentro de
    # la propia función son DATOS: el barrido lineal se corta en la primera.
    _tbl = [op.mem.disp for _ins in insns for op in _ins.operands
            if op.type == X86_OP_MEM and op.mem.index and not op.mem.base and va < op.mem.disp < va + size + 4096
            and (_ins.mnemonic == "jmp" or _ins.mnemonic.startswith("mov"))]
    if _tbl:
        _first = min(_tbl)
        insns = [x for x in insns if x.address + x.size <= _first]
    # PCF7: el barrido lineal se para en los datos (tablas de saltos de dos niveles de MSVC, con la
    # tabla de índices de bytes en mitad de la función), pero el código SIGUE después. Se completa
    # siguiendo el flujo: destinos de saltos y entradas de tablas dentro de la función que no se han
    # decodificado, hasta el inicio de la función siguiente.
    _limit = _starts[_i] if _i < len(_starts) else va + size + 4096
    _gsz = funcs().get(va)   # tamaño según Ghidra: un `push etiqueta` fuera de él es otra función (callback)
    insns = _follow_flow(insns, va, _limit, base, img, md, tlo_thi=text_range(pe_path),
                         own_hi=va + _gsz if _gsz else None)
    return _emit(va, insns, pe_path, [f"void f_{va:x}(void) {{", "ESPCHK_B();"])


def _lift_cluster(lo, hi, ents, base, img, pe_path):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    seeds = []
    for e in ents:
        dec = list(md.disasm(img[e - base:e - base + 16], e, 1))
        if dec:
            seeds.append(dec[0])
    insns = _follow_flow(seeds, lo, hi, base, img, md, tlo_thi=text_range(pe_path))
    have = {i.address for i in insns}
    ents = [e for e in ents if e in have]
    head = [f"static void fc_{lo:x}(u32 _ent) {{", "ESPCHK_B();",
            "switch(_ent){ " + " ".join(f"case 0x{e:x}u: goto L_{e:x};" for e in ents) + " default: js_dbg(0xC1u, _ent, 0); return; }"]
    body = _emit(lo, insns, pe_path, head)
    return body + "".join(f"void f_{e:x}(void){{ fc_{lo:x}(0x{e:x}u); }}\n" for e in ents)


def _emit(va, insns, pe_path, head):
    base, img, _ = image(pe_path)
    _stops = getattr(_MOD, "stops", None) or ()
    addrs = {i.address for i in insns}
    # direcciones de la propia función que se empujan como valor: posibles destinos de un `ret`
    _pushed = {i.operands[0].imm & 0xFFFFFFFF for i in insns
               if i.mnemonic == "push" and i.operands and i.operands[0].type == X86_OP_IMM}
    _local_rets = {v for v in _pushed if v in addrs and v != va}
    # …o a una entrada propia cercana (sin Ghidra, el bloque del __finally puede figurar como función)
    _ent_rets = {v for v in _pushed if v not in addrs and va < v < va + 0x1000 and v in _stops}
    ent = entries()
    tlo, thi = text_range(pe_path)
    out = list(head)
    if DOS:
        out.append("u32 _ret0 = g_ret_expected;")
    _cluster = bool(head) and head[0].startswith("static void fc_")
    global _PH, _CUR_INS
    _PH = None
    if va in TEMPLATES:
        out.append("u32 _tb = g_thunk_base;")
    for _k, ins in enumerate(insns):
        _CUR_INS = ins if va not in TEMPLATES else None   # (las plantillas ya leen sus huecos de la copia)
        if va in TEMPLATES:   # huecos de la plantilla: se leen de la copia
            _PH, _b = {}, ins.bytes
            if ins.disp_size and all(x == 0x66 for x in _b[ins.disp_offset:ins.disp_offset + ins.disp_size]):
                _o = ins.address - va + ins.disp_offset
                _PH["disp"] = {4: f"rd32(_tb + {_o})", 1: f"(u32)(i32)(i8)rd8(_tb + {_o})"}.get(ins.disp_size)
            if ins.imm_size and all(x == 0x66 for x in _b[ins.imm_offset:ins.imm_offset + ins.imm_size]):
                _o = ins.address - va + ins.imm_offset
                _PH["imm"] = {4: f"rd32(_tb + {_o})", 2: f"rd16(_tb + {_o})", 1: f"(u32)(i32)(i8)rd8(_tb + {_o})"}[ins.imm_size]
            _PH = {k: v for k, v in _PH.items() if v}
        if _k and _falls_through(insns[_k - 1]) and insns[_k - 1].address + insns[_k - 1].size != ins.address:
            _ft = insns[_k - 1].address + insns[_k - 1].size
            out.append(f"  goto L_{_ft:x};" if _ft in addrs else f"  f_{_ft:x}(); return;" if _ft in _stops
                       else f"  js_dbg(0xFA11u, 0x{_ft:x}u, 0); return;")
        ops = ins.operands
        sz = ops[0].size if ops else 4
        m = ins.mnemonic
        # en un grupo (el intérprete de MSVBVM50) cur_fn no dice dónde se está: los saltos indirectos lo anotan
        _site = f"cur_fn=0x{ins.address:x}u; " if _cluster else ""
        rep = ""
        for _p in ("lock ", "repne ", "repe ", "repnz ", "repz ", "rep "):
            if m.startswith(_p):
                if _p != "lock ":
                    rep = _p.strip()
                m = m[len(_p):]
                break
        if not rep and m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd",
                             "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd") and ins.size > 1:
            # Capstone pierde el prefijo de `f2 a5` (repne movsd, lo emite Watcom: se confunde con el movsd de SSE2)
            _pre = ins.bytes[:ins.size - 1]
            if 0xF2 in _pre:
                rep = "repne"
            elif 0xF3 in _pre:
                rep = "rep"
        d = ops[0] if ops else None
        s = ops[1] if len(ops) > 1 else None
        out.append(f"  L_{ins.address:x}: /* {m} {ins.op_str} */")
        if DOS:
            _r = _dos_ins(m, ins, d, s, rep)
            if _r is not None:
                out += _r
                continue
        try:
            # Si un operando tiene un tamaño imposible (p.ej. estamos leyendo los bytes de una
            # tabla de saltos como si fueran codigo), paramos: el resto es dato. Lo mismo si
            # aparece un registro que no existe en el juego (dr/cr/tr): es dato mal decodificado.
            if any(op.size not in (0, 1, 2, 4, 8, 10, 16) for op in ops):
                break
            if any(op.type == X86_OP_REG and ins.reg_name(op.reg)[:2] in ("dr", "cr", "tr") for op in ops):
                break
            if m == "mov":
                # movimiento desde/hacia un registro de segmento: no modelamos segmentos
                if (d is not None and d.type == X86_OP_REG and d.reg in SEG) or \
                   (s is not None and s.type == X86_OP_REG and s.reg in SEG):
                    out += ["/* mov seg: no-op */"]
                else:
                    out += wr(d, sz, rd(s, s.size))
            elif m == "movzx":
                out += wr(d, sz, f"({rd(s, s.size)} & 0x{(1 << (8 * s.size)) - 1:x})")
            elif m == "movsx":
                out += wr(d, sz, f"(u32)(i32)(i{8*sz})((i{8*s.size}){rd(s, s.size)})")
            elif m == "lea":
                out += wr(d, sz, f"(u32)({mem_addr(s, seg=False)})")
            elif m == "test":
                out += f_logic(f"{rd(d, d.size)} & {rd(s, s.size)}", d.size)
            elif m == "cmp":
                out += f_sub(rd(d, d.size), rd(s, s.size), d.size)
            elif m in ("xor", "and", "or"):
                op = {"xor": "^", "and": "&", "or": "|"}[m]
                out += f_logic(f"{rd(d, d.size)} {op} {rd(s, s.size)}", d.size)
                out += wr(d, sz, "t")
            elif m in ("add", "sub", "adc", "sbb"):
                a, b = rd(d, d.size), rd(s, s.size)
                bits, M, S = _szinfo(d.size)
                if m == "add":
                    out += f_add(a, b, d.size)
                elif m == "sub":
                    out += f_sub(a, b, d.size)
                elif m == "adc":
                    out += [f"{{u64 _s=(u64){a}+(u64){b}+(u64)CF; t=(u32)_s&{M}; CF=((u32)(_s>>{bits}))&1; ZF=(t==0); SF=(t>>{bits-1})&1; OF=((({a}^t)&({b}^t)&{S})!=0);}}"]
                else:
                    out += [f"{{u64 _s=(u64){a}-(u64){b}-(u64)CF; t=(u32)_s&{M}; CF=((u32)(_s>>{bits}))&1; ZF=(t==0); SF=(t>>{bits-1})&1; OF=((({a}^{b})&({a}^t)&{S})!=0);}}"]
                out += wr(d, sz, "t")
            elif m in ("inc", "dec"):
                v = rd(d, d.size)
                bits, M, S = _szinfo(d.size)
                if m == "inc":
                    out += [f"t = ({v} + 1) & {M};"] + f_flags_zsp(d.size) + \
                           [f"OF = (({v} ^ t) & (1u ^ t) & {S}) != 0;"]
                else:
                    out += [f"t = ({v} - 1) & {M};"] + f_flags_zsp(d.size) + \
                           [f"OF = (({v} ^ 1u) & ({v} ^ t) & {S}) != 0;"]
                out += wr(d, sz, "t")
            elif m == "neg":
                out += f_sub("0u", rd(d, d.size), d.size); out += wr(d, sz, "t")
            elif m == "not":
                out += wr(d, sz, f"~{rd(d, d.size)}")
            elif m in ("shl", "sal", "shr", "sar", "rol", "ror"):
                cnt = rd(s, s.size) if s else "1u"
                a = rd(d, d.size)
                bits, M, S = _szinfo(d.size)
                zs = f"ZF=(t==0); SF=(t>>{bits-1})&1;"
                if m in ("shl", "sal"):
                    out += [f"{{u32 c=({cnt}&31), v={a}; t = (c < 32) ? ((v << c) & {M}) : 0u; CF = c ? ((c <= {bits}) ? ((v >> ({bits} - c)) & 1) : 0u) : CF; {zs}}}"]
                elif m == "shr":
                    out += [f"{{u32 c=({cnt}&31), v={a}; t = v >> c; CF = c ? ((v >> (c - 1)) & 1) : CF; {zs}}}"]
                elif m == "sar":
                    sx = f"(i32)(i{bits})({a})" if d.size < 4 else f"(i32){a}"
                    out += [f"{{u32 c=({cnt}&31); i32 v={sx}; t = (u32)(v >> c) & {M}; CF = c ? (((u32)(v >> (c - 1))) & 1) : CF; {zs}}}"]
                elif m == "rol":
                    out += [f"{{u32 c=({cnt}&31)%{bits}, v={a}; t = c ? (((v << c) | (v >> ({bits} - c))) & {M}) : v; CF = t & 1; {zs}}}"]
                else:
                    out += [f"{{u32 c=({cnt}&31)%{bits}, v={a}; t = c ? (((v >> c) | (v << ({bits} - c))) & {M}) : v; CF = (t >> {bits-1}) & 1; {zs}}}"]
                out += wr(d, sz, "t")
            elif m == "imul" and len(ops) == 1:
                # BUG corregido 14-oct: las formas de 8/16 bits (imul bx, imul cl) se traducian como la de 32 bits
                # (escribian EAX y EDX enteros): el bucle de resto de DrawMask 565 (Utils) empaqueta bytes con
                # `mov dx, ax` + `bswap edx` y perdia verde/azul -> columna de 1 px al final de cada texto.
                if d.size == 1:
                    out += ["{i32 p=(i32)(i8)(eax&0xFF)*(i32)(i8)(" + rd(d, 1) + "); eax=(eax&0xFFFF0000u)|((u32)p&0xFFFF); CF=OF=(p!=(i32)(i8)(p&0xFF));}"]
                elif d.size == 2:
                    out += ["{i32 p=(i32)(i16)(eax&0xFFFF)*(i32)(i16)(" + rd(d, 2) + "); eax=(eax&0xFFFF0000u)|((u32)p&0xFFFF); edx=(edx&0xFFFF0000u)|(((u32)p>>16)&0xFFFF); CF=OF=(p!=(i32)(i16)(p&0xFFFF));}"]
                else:
                    out += ["{i64 p=(i64)(i32)eax*(i64)(i32)" + rd(d, d.size) + "; eax=(u32)p; edx=(u32)(p>>32); CF=OF=(edx!=0&&edx!=0xffffffff);}"]
            elif m == "imul" and len(ops) in (2, 3):
                # con signo en el ancho del destino (16 bits: `imul ax, bx`); el inmediato ya viene extendido
                x, y = (d, s) if len(ops) == 2 else (s, ops[2])
                bits, M, S = _szinfo(d.size)
                def _sx(o):
                    if o.type == X86_OP_IMM:
                        return f"(i64)(i32){rd(o, o.size)}"
                    return f"(i64)(i{8*o.size})({rd(o, o.size)})" if o.size < 4 else f"(i64)(i32){rd(o, o.size)}"
                out += [f"{{i64 p={_sx(x)}*{_sx(y)}; t=(u32)p&{M}; CF=OF=(p!=(i64)(i{bits})t);}}"]
                out += wr(d, sz, "t")
            elif m == "mul" and len(ops) == 1:
                if d.size == 1:
                    out += ["{u32 p=(eax&0xFF)*((" + rd(d, 1) + ")&0xFF); eax=(eax&0xFFFF0000u)|(p&0xFFFF); CF=OF=((p>>8)!=0);}"]
                elif d.size == 2:
                    out += ["{u32 p=(eax&0xFFFF)*((" + rd(d, 2) + ")&0xFFFF); eax=(eax&0xFFFF0000u)|(p&0xFFFF); edx=(edx&0xFFFF0000u)|((p>>16)&0xFFFF); CF=OF=((p>>16)!=0);}"]
                else:
                    out += ["{u64 p=(u64)eax*(u64)" + rd(d, d.size) + "; eax=(u32)p; edx=(u32)(p>>32); CF=OF=(edx!=0);}"]
            elif m == "div" and len(ops) == 1:
                if d.size == 1:
                    out += ["{u32 dv=(" + rd(d, 1) + ")&0xFF; if(dv){u32 n=eax&0xFFFF; eax=(eax&0xFFFF0000u)|((n/dv)&0xFF)|(((n%dv)&0xFF)<<8);} }"]
                elif d.size == 2:
                    out += ["{u32 dv=(" + rd(d, 2) + ")&0xFFFF; if(dv){u32 n=((edx&0xFFFF)<<16)|(eax&0xFFFF); eax=(eax&0xFFFF0000u)|((n/dv)&0xFFFF); edx=(edx&0xFFFF0000u)|((n%dv)&0xFFFF);} }"]
                else:
                    out += ["{u32 dv=" + rd(d, d.size) + "; if(dv){u64 n=((u64)edx<<32)|eax; eax=(u32)(n/dv); edx=(u32)(n%dv);} }"]
            elif m == "idiv" and len(ops) == 1:
                if d.size == 1:
                    out += ["{i32 dv=(i32)(i8)(" + rd(d, 1) + "); if(dv){i32 n=(i32)(i16)(eax&0xFFFF); eax=(eax&0xFFFF0000u)|((u32)(n/dv)&0xFF)|(((u32)(n%dv)&0xFF)<<8);} }"]
                elif d.size == 2:
                    out += ["{i32 dv=(i32)(i16)(" + rd(d, 2) + "); if(dv){i32 n=(i32)((((u32)edx&0xFFFF)<<16)|(eax&0xFFFF)); eax=(eax&0xFFFF0000u)|((u32)(n/dv)&0xFFFF); edx=(edx&0xFFFF0000u)|((u32)(n%dv)&0xFFFF);} }"]
                else:
                    out += ["{i32 dv=(i32)" + rd(d, d.size) + "; if(dv){i64 n=((i64)(i32)edx<<32)|(u32)eax; eax=(u32)(i32)(n/dv); edx=(u32)(i32)(n%dv);} }"]
            elif m == "cdq":
                out += ["edx = (eax & 0x80000000) ? 0xFFFFFFFFu : 0u;"]
            elif m == "cwde":
                out += ["eax = (u32)(i32)(i16)(eax & 0xFFFF);"]
            elif m == "cbw":
                out += ["eax = (eax & 0xFFFF0000) | ((u32)(i16)(i8)(eax & 0xFF) & 0xFFFF);"]
            elif m == "xchg":
                out += [f"t = {rd(d, d.size)};"] + wr(d, sz, rd(s, s.size)) + wr(s, s.size, "t")
            elif m in ("push", "pop"):
                seg = d.type == X86_OP_REG and d.reg in (X86_REG_ES, X86_REG_CS, X86_REG_SS, X86_REG_DS, X86_REG_FS, X86_REG_GS)
                if seg:
                    out += ["esp -= 4;"] if m == "push" else ["esp += 4;"]
                elif d.size == 2:
                    # push/pop de 16 bits (prefijo 66): dos bytes de pila (patchw32.dll: `push dx` ×2 y luego `pop eax`)
                    if m == "push":
                        out += [f"{{u32 _pv={rd(d, d.size)}; esp -= 2; wr16(esp, _pv & 0xFFFFu);}}"]
                    else:
                        out += wr(d, sz, "rd16(esp)") + ["esp += 2;"]
                elif m == "push":
                    # x86 evalua el operando ANTES de decrementar esp; para `push [esp+N]`
                    # hay que leer con el esp viejo. Usamos un temporal siempre (seguro).
                    out += [f"{{u32 _pv={rd(d, d.size)}; esp -= 4; wr32(esp, _pv);}}"]
                else:
                    out += wr(d, sz, "rd32(esp)") + ["esp += 4;"]
            elif m == "pushad":
                # pushad guarda el ESP original (antes de restar 32) en la 5a posicion
                out += ["esp -= 32;"] + [f"wr32(esp + {4*i}, {r});" for i, r in enumerate(["eax", "ecx", "edx", "ebx", "esp + 32", "ebp", "esi", "edi"])]
            elif m == "popad":
                out += [f"{r} = rd32(esp + {4*i});" for i, r in enumerate(["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"])] + ["esp += 32;"]
            elif m == "pushfd":
                out += ["esp -= 4; wr32(esp, CF | (PF<<2) | (ZF<<6) | (SF<<7) | (1<<9) | (DF<<10) | (OF<<11) | (EFL_HI & 0x240000u));"]
            elif m == "pushf":   # 66 9C: dos bytes de pila
                out += ["esp -= 2; wr16(esp, CF | (PF<<2) | (ZF<<6) | (SF<<7) | (1<<9) | (DF<<10) | (OF<<11));"]
            elif m == "popfd":
                # AC (18) e ID (21) se conservan: así el juego detecta que hay CPUID
                out += ["{u32 _f=rd32(esp); esp += 4; CF=_f&1; PF=(_f>>2)&1; ZF=(_f>>6)&1; SF=(_f>>7)&1; DF=(_f>>10)&1; OF=(_f>>11)&1; EFL_HI=_f&0x240000u;}"]
            elif m == "popf":
                out += ["{u32 _f=rd16(esp); esp += 2; CF=_f&1; PF=(_f>>2)&1; ZF=(_f>>6)&1; SF=(_f>>7)&1; DF=(_f>>10)&1; OF=(_f>>11)&1;}"]
            elif m == "arpl":
                out += ["/* arpl: no-op */"]
            elif m.startswith("set") and m[3:] in CC:
                out += wr(d, 1, f"({CC[m[3:]]} ? 1u : 0u)")
            elif m.startswith("cmov") and m[4:] in CC:
                out += [f"if ({CC[m[4:]]}) {{"] + wr(d, sz, rd(s, s.size)) + ["}"]
            elif m in ("bt", "bts", "btr", "btc"):
                bits = 8 * d.size
                op = {"bt": None, "bts": "|", "btr": "&~", "btc": "^"}[m]
                if d.type == X86_OP_MEM and s.type == X86_OP_REG:
                    # con el operando en memoria y el desplazamiento en un registro, el bit puede estar fuera del
                    # operando: se direcciona [base + (desp >> log2(bits)) * tam] y el bit es desp & (bits-1)
                    # (el strpbrk de MSVCRT monta así un mapa de 256 bits en la pila: `bts [ebp-0x20], eax`)
                    sh = {1: 3, 2: 4, 4: 5}[d.size]
                    bo = f"(i32)(i{8*s.size})({rd(s, s.size)})" if s.size < 4 else f"(i32){rd(s, s.size)}"
                    cuerpo = f"{{i32 _bo={bo}; u32 _ba=(u32)({mem_addr(d)}) + (u32)((_bo >> {sh}) * {d.size}); u32 _bb=(u32)_bo & {bits-1}u; u32 _bv=rd{bits}(_ba); CF=(_bv >> _bb) & 1;"
                    if op:
                        cuerpo += f" wr{bits}(_ba, (_bv {op} (1u << _bb)) & {(1 << bits) - 1:#x}u);"
                    out += [cuerpo + "}"]
                else:
                    bit = f"({rd(s, s.size)} & {bits-1})"
                    v = rd(d, d.size)
                    out += [f"CF = ({v} >> {bit}) & 1;"]
                    if op:
                        out += wr(d, sz, f"{v} {op} (1u << {bit})")
            elif m in ("bsf", "bsr"):
                out += [f"{{u32 v={rd(s, s.size)},r=0,found=0,i; for(i=0;i<32;i++){{u32 b={'(i)' if m=='bsf' else '(31-i)'}; if((v>>b)&1){{r=b;found=1;break;}}}} ZF=!found; t=r;}}"]
                out += wr(d, sz, "t")
            elif m == "jmp":
                if d.type == X86_OP_IMM and d.imm in addrs:
                    if d.imm < ins.address:
                        out += [f"if((++w_steps&0xFFFFFFu)==0u)js_dbg(0xD7F9u,cur_fn,w_steps); goto L_{d.imm:x};"]
                    else:
                        out += [f"goto L_{d.imm:x};"]
                elif d.type == X86_OP_IMM and tlo <= d.imm < thi:
                    out += [f"f_{d.imm:x}(); return;"]
                elif d.type == X86_OP_MEM and not d.mem.base and not d.mem.index:
                    # thunk de importación: jmp dword ptr [IAT]  → tail call a la función importada
                    if d.mem.disp in IAT:
                        out += [f"{IAT[d.mem.disp]}(); return;"]
                    else:
                        out += [f"icall(rd32({d.mem.disp})); return;"]
                elif d.type == X86_OP_MEM and d.mem.index and not d.mem.base and d.mem.scale == 4:
                    # salto por tabla: jmp [tabla + reg*escala]. Leer las ENTRADAS reales
                    # (punteros absolutos) de la imagen
                    tbl = d.mem.disp
                    cases, seen = [], set()
                    # La tabla puede tener entradas NO validas intercaladas (p.ej. un 0xc3 de
                    # relleno en el indice 0). Antes se CORTABA en la primera invalida -> 0 casos
                    # -> fallback erroneo (todas las etiquetas) -> en runtime el valor real de la
                    # tabla no es `case` -> __builtin_trap(). Ahora saltamos invalidas (con un
                    # limite de 8 seguidas) y seguimos leyendo. Ademas leemos en AMBAS direcciones
                    # porque hay tablas con el indice negado (`neg ecx` antes del jmp).
                    for _d in (1, -1):
                        _miss = 0
                        for _i in range(0, 512):
                            _off = tbl - base + _d * _i * 4
                            if _off < 0 or _off + 4 > len(img):
                                break
                            _t = img[_off] | (img[_off + 1] << 8) | (img[_off + 2] << 16) | (img[_off + 3] << 24)
                            if not (tlo <= _t < thi):
                                _miss += 1
                                if _miss > 8:
                                    break
                                continue
                            _miss = 0
                            if _t in seen:
                                continue
                            seen.add(_t)
                            if _t in addrs:
                                cases.append(f"case 0x{_t:x}u: goto L_{_t:x};")
                            else:
                                cases.append(f"case 0x{_t:x}u: f_{_t:x}(); return;")
                    if not cases:
                        cases = [f"case 0x{x:x}u: goto L_{x:x};" for x in sorted(addrs)]
                    out += [f"{{u32 _t={rd(d, d.size)}; switch(_t){{ {' '.join(cases)} default: js_dbg(0xBAD0u, _t, 0x{ins.address:x}u); }} __builtin_trap();}}"]
                elif d.type == X86_OP_MEM:
                    # jmp [reg + disp] = llamada indirecta (vtable) por puntero, NO tabla de saltos
                    out += [f"{_site}ijmp(rd32({mem_addr(d)})); return;"]
                elif d.type == X86_OP_REG and _chkstk_tail(insns, _k, d.reg):
                    # epílogo de __chkstk (sondeo de pila de MSVC/Borland): `mov eax, esp; mov esp, ecx; mov ecx, [eax];
                    # mov eax, [eax+4]; jmp eax` salta a la dirección de retorno con esp ya bajado: es un `ret`
                    # (si se tradujera como salto indirecto, el resto del llamante correría anidado y luego otra vez)
                    out += ["return; /* __chkstk: jmp a la dirección de retorno */"]
                elif d.type == X86_OP_REG:
                    out += [f"{_site}ijmp({rd(d, d.size)}); return;"]
                else:
                    raise Unsupported("jmp")
            elif m.startswith("j") and m[1:] in CC:
                if d.type == X86_OP_IMM and d.imm in addrs:
                    if d.imm < ins.address:
                        out += [f"if ({CC[m[1:]]}) {{ if((++w_steps&0xFFFFFFu)==0u)js_dbg(0xD7F9u,cur_fn,w_steps); goto L_{d.imm:x}; }}"]
                    else:
                        out += [f"if ({CC[m[1:]]}) goto L_{d.imm:x};"]
                elif d.type == X86_OP_IMM and tlo <= d.imm < thi:
                    out += [f"if ({CC[m[1:]]}) {{ f_{d.imm:x}(); return; }}"]
                else:
                    # destino fuera del binario: son bytes de datos decodificados como código (p.ej. el
                    # final de una función alargado hasta la siguiente). Si se ejecutara, avisa.
                    out += [f"if ({CC[m[1:]]}) {{ js_dbg(0xBADCu, 0x{ins.address:x}u, 0); return; }}"]
            elif m == "call":
                ra = ins.address + ins.size
                # En x86 el callee DEBE preservar esi (callee-saved). Nuestra traduccion de algunos
                # epilogos compartidos lo corrompe (continuaciones), asi que se guarda/restaura en
                # la frontera (solo esi, el caso observado).
                _g = "u32 _ss=esi,_sp=ebp,_sb=ebx,_sd=edi,_c0=esp; "
                _r = f" esi=_ss;ebp=_sp;ebx=_sb;edi=_sd; CALLCHK(0x{ins.address:x}u);"
                if DOS:
                    # MS-DOS (Watcom + ensamblador a mano): el llamante ve los registros tal como los deja la
                    # rutina (muchas devuelven valores en esi/edi/ebx, también cuando se llaman por tabla); no hay
                    # epílogos compartidos de C++ que corregir
                    _r = (f" ebp=_sp; if(g_watch&&(esi!=_ss||ebx!=_sb||edi!=_sd))call_regchg(0x{ins.address:x}u,_ss,_sb,_sd);"
                          f" CALLCHK(0x{ins.address:x}u);")
                _re = f"DOS_CALL_PRE(0x{ra:x}u); " if DOS else ""
                if DOS:   # tras la llamada: si un ret interior «volvió» a esta dirección (pop doble), se sigue aquí;
                    _r = f" ebp=_sp; DOS_CALL_POST(0x{ra:x}u);" + _r[len(" ebp=_sp;"):]   # si a otra más arriba, se sube
                if d.type == X86_OP_IMM and (d.imm in ent or tlo <= d.imm < thi):
                    out += [f"{{{_g}esp -= 4; wr32(esp, 0x{ra:x}u); {_re}f_{d.imm:x}();{_r}}}"]
                elif d.type == X86_OP_MEM and not d.mem.base and not d.mem.index and d.mem.disp in IAT:
                    out += [f"{{{_g}esp -= 4; wr32(esp, 0x{ra:x}u); {IAT[d.mem.disp]}();{_r}}}"]
                elif d.type in (X86_OP_MEM, X86_OP_REG):
                    # `call [mem]`/`call reg`: si el destino NO esta en la FTAB, se salta. Antes se
                    # recuperaba solo la direccion de retorno (esp += 4) EN SILENCIO -> los
                    # argumentos empujados quedaban en la pila (fuga de 4*nargs) y `esp` derivaba
                    # (p.ej. DCSFX 0x1000b710 -> `call [ecx+0x18]` con target 0xffffffff y 5 args =
                    # 20 B de fuga). Ahora se REPORTA con js_missing (destino + sitio de llamada).
                    out += [f"{{u32 _tg={rd(d, d.size)}; {_g}esp -= 4; wr32(esp, 0x{ra:x}u); {_re}if(!icall_try(_tg)){{ js_missing(_tg); esp += 4; }}{_r.replace("CALLCHK(", "CALLCHK_I(_tg, ")}}}"]
                else:
                    raise Unsupported("call")
            elif m == "ret":
                n = d.imm if (ops and d.type == X86_OP_IMM) else 0
                if ops and _PH and "imm" in _PH:
                    n = f"({_PH['imm']})"
                if _local_rets or _ent_rets:
                    # `push etiqueta … ret` (los __finally de OLEAUT32): el ret vuelve dentro de la función
                    out += ["{u32 _rt=rd32(esp); switch(_rt){ "
                            + " ".join(f"case 0x{x:x}u: esp += 4 + {n}; goto L_{x:x};" for x in sorted(_local_rets))
                            + " ".join(f" case 0x{x:x}u: esp += 4 + {n}; f_{x:x}(); return;" for x in sorted(_ent_rets))
                            + " default: break; }}"]
                if DOS:
                    # ret a otra dirección: si es la de retorno de un marco activo (pop de la dirección + ret, como
                    # el «call ayudante; pop edi; ret» de Watcom) se desenrolla hasta él; si no, es «push dirección;
                    # ret» (int86) y se salta a ella
                    out += [f"{{u32 _rt=rd32(esp); ESPCHK_E(0x{va:x}u, 0x{ins.address:x}u); esp += 4 + {n}; "
                            f"if (_rt != _ret0) {{ if (dos_en_sombra(_rt)) {{ g_ret_jump = _rt; return; }} g_ret_expected = _ret0; ijmp(_rt); }} return;}}"]
                else:
                    out += [f"ESPCHK_E(0x{va:x}u, 0x{ins.address:x}u); esp += 4 + {n}; return;"]
            elif m == "leave":
                out += ["esp = ebp;", "ebp = rd32(esp);", "esp += 4;"]
            elif m == "nop":
                pass
            elif m == "cld":
                out += ["DF = 0;"]
            elif m == "std":
                out += ["DF = 1;"]
            elif m.startswith("f") or m in ("wait",):
                sti = None
                if d is not None and d.type == X86_OP_REG and X86_REG_ST0 <= d.reg <= X86_REG_ST0 + 7:
                    sti = d.reg - X86_REG_ST0
                if sti is None:
                    sti = 1  # sin operando explícito → st(1)
                mem = (d is not None and d.type == X86_OP_MEM)
                def rv(conv32="F32", conv64="F64", intc=None):
                    if mem:
                        if intc:
                            return {2: f"(double)(i16){rd(d,2)}", 4: f"(double)(i32){rd(d,4)}", 8: f"(double)(i64)rd64({mem_addr(d)})"}[d.size]
                        return {4: f"F32({rd(d,4)})", 8: f"F64(rd64({mem_addr(d)}))", 10: f"F80({mem_addr(d)})"}[d.size]
                    return f"ST[(TOP + {sti}) & 7]"
                _fr = [o.reg - X86_REG_ST0 for o in ops if o.type == X86_OP_REG and X86_REG_ST0 <= o.reg <= X86_REG_ST0 + 7]
                if m in ("fld", "fild"):
                    # el origen se lee ANTES de mover TOP (`fld st(0)` duplica la cima)
                    out += [f"{{double _v={rv(intc=(m=='fild'))}; TOP=(TOP-1)&7; ST[TOP]=_v;}}"]
                elif m in ("fst", "fstp"):
                    tgt = (f"wr32({mem_addr(d)}, B32(ST[TOP]))" if d.size == 4 else f"wr64({mem_addr(d)}, B64(ST[TOP]))") if mem else f"ST[(TOP + {sti}) & 7] = ST[TOP]"
                    out += [tgt + ";"] + (["TOP=(TOP+1)&7;"] if m == "fstp" else [])
                elif m in ("fist", "fistp", "fisttp"):
                    _v = "ST[TOP]" if m == "fisttp" else "fpu_rnd(ST[TOP])"
                    out += [{2: f"wr16({mem_addr(d)}, fpu_i16({_v}))", 4: f"wr32({mem_addr(d)}, fpu_i32({_v}))", 8: f"wr64({mem_addr(d)}, fpu_i64({_v}))"}[d.size] + ";"]
                    if m != "fist":
                        out += ["TOP=(TOP+1)&7;"]
                elif m in ("fadd", "fsub", "fmul", "fdiv", "fsubr", "fdivr"):
                    op = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/", "fsubr": "-", "fdivr": "/"}[m]
                    if len(_fr) == 2 and _fr[0] != 0:
                        # forma DC: `fsub st(i), st(0)` → st(i) = st(i) - st(0); `fsubr st(i), st(0)` → st(0) - st(i)
                        dst = f"ST[(TOP + {_fr[0]}) & 7]"
                        expr = f"ST[TOP] {op} {dst}" if m in ("fsubr", "fdivr") else f"{dst} {op} ST[TOP]"
                        out += [f"{dst} = {expr};"]
                    else:
                        b = f"ST[(TOP + {_fr[1]}) & 7]" if len(_fr) == 2 else rv()
                        expr = f"({b}) {op} ST[TOP]" if m in ("fsubr", "fdivr") else f"ST[TOP] {op} ({b})"
                        out += [f"ST[TOP] = {expr};"]
                elif m in ("faddp", "fsubp", "fmulp", "fdivp", "fsubrp", "fdivrp"):
                    op = {"faddp": "+", "fsubp": "-", "fmulp": "*", "fdivp": "/", "fsubrp": "-", "fdivrp": "/"}[m]
                    b = f"ST[TOP]"
                    dst = f"ST[(TOP+{_fr[0] if _fr else 1})&7]"
                    expr = f"({b}) {op} {dst}" if m in ("fsubrp", "fdivrp") else f"{dst} {op} ({b})"
                    out += [f"{dst} = {expr};", "TOP=(TOP+1)&7;"]
                elif m in ("fcom", "fcomp", "fcompp", "fcomi", "fcomip", "fucom", "fucomp", "fucompp", "fucomi", "fucomip", "fcompi", "fucompi"):
                    b = f"ST[(TOP + {_fr[-1]}) & 7]" if len(_fr) == 2 else rv()
                    # sin orden (NaN): C3 = C2 = C0 = 1
                    out += [f"{{double _b={b}; if(ST[TOP]<_b){{FPUSW=(FPUSW&~0x4500)|0x0100;}} else if(ST[TOP]>_b){{FPUSW&=~0x4500;}} else if(ST[TOP]==_b){{FPUSW=(FPUSW&~0x4500)|0x4000;}} else {{FPUSW|=0x4500;}} }}"]
                    if m in ("fcomi", "fcomip", "fucomi", "fucomip", "fcompi", "fucompi"):
                        out += ["ZF=(FPUSW&0x4000)!=0; CF=(FPUSW&0x0100)!=0; PF=(FPUSW&0x0400)!=0;"]
                    if m in ("fcomp", "fcompp", "fucomp", "fucompp", "fcomip", "fucomip", "fcompi", "fucompi"):
                        out += ["TOP=(TOP+1)&7;"]
                    if m in ("fcompp", "fucompp"):
                        out += ["TOP=(TOP+1)&7;"]
                elif m == "ftst":   # compara ST0 con 0.0 (C3/C2/C0 como fcom)
                    out += ["{double _b=0.0; if(ST[TOP]<_b){FPUSW=(FPUSW&~0x4500)|0x0100;} else if(ST[TOP]>_b){FPUSW&=~0x4500;} else if(ST[TOP]==_b){FPUSW=(FPUSW&~0x4500)|0x4000;} else {FPUSW|=0x4500;} }"]
                elif m == "fxam":   # clase de ST0: C3 C2 C0 (normal 0 1 0; cero 1 0 0; infinito 0 1 1; NaN 0 0 1; vacío 1 0 1), C1 = signo
                    out += ["{double _v=ST[TOP]; u32 _c=0; if(_v!=_v)_c=0x0100; else if(_v==1.0/0.0||_v==-1.0/0.0)_c=0x0500; else if(_v==0.0)_c=0x4000; else _c=0x0400; if(_v<0||(_v==0.0&&1.0/_v<0))_c|=0x0200; FPUSW=(FPUSW&~0x4700)|_c;}"]
                elif m in ("fld1", "fldz", "fldpi", "fldl2e", "fldl2t", "fldlg2", "fldln2"):
                    c = {"fld1": "1.0", "fldz": "0.0", "fldpi": "3.141592653589793", "fldl2e": "1.4426950408889634",
                         "fldl2t": "3.321928094887362", "fldlg2": "0.3010299956639812", "fldln2": "0.6931471805599453"}[m]
                    out += [f"TOP=(TOP-1)&7; ST[TOP]={c};"]
                elif m == "fxch":
                    # Capstone da `fxch st(i)` como (st(0), st(i)): el registro es el ÚLTIMO operando
                    _i = _fr[-1] if _fr else 1
                    out += [f"{{double _s=ST[TOP]; ST[TOP]=ST[(TOP + {_i}) & 7]; ST[(TOP + {_i}) & 7]=_s;}}"]
                elif m in ("fidiv", "fimul", "fiadd", "fisub", "fisubr", "fidivr"):
                    op = {"fiadd": "+", "fimul": "*", "fidiv": "/", "fisub": "-", "fisubr": "-", "fidivr": "/"}[m]
                    iv = {2: f"(double)(i16){rd(d,2)}", 4: f"(double)(i32){rd(d,4)}"}[d.size]
                    expr = f"({iv}) {op} ST[TOP]" if m in ("fisubr", "fidivr") else f"ST[TOP] {op} ({iv})"
                    out += [f"ST[TOP] = {expr};"]
                elif m == "fsqrt":
                    out += ["ST[TOP] = __builtin_sqrt(ST[TOP]);"]
                elif m == "fabs":
                    out += ["ST[TOP] = __builtin_fabs(ST[TOP]);"]
                elif m == "fcos":
                    out += ["ST[TOP] = __builtin_cos(ST[TOP]);"]
                elif m == "fsin":
                    out += ["ST[TOP] = __builtin_sin(ST[TOP]);"]
                elif m == "fptan":
                    out += ["{double _x=ST[TOP]; ST[TOP]=__builtin_tan(_x); TOP=(TOP-1)&7; ST[TOP]=1.0;}"]
                elif m == "fpatan":
                    # FPATAN: st(1) = arctan(st(1) / st(0)), y se desapila
                    out += ["{double _x=ST[TOP]; TOP=(TOP+1)&7; ST[TOP]=__builtin_atan2(ST[TOP], _x);}"]
                elif m == "fsincos":
                    out += ["{double _x=ST[TOP]; ST[TOP]=__builtin_sin(_x); TOP=(TOP-1)&7; ST[TOP]=__builtin_cos(_x);}"]
                elif m == "f2xm1":
                    out += ["ST[TOP] = __builtin_exp2(ST[TOP]) - 1.0;"]
                elif m == "fyl2x":
                    out += ["{double _x=ST[TOP]; TOP=(TOP+1)&7; ST[TOP]=ST[TOP]*__builtin_log2(_x);}"]
                elif m == "frndint":
                    out += ["ST[TOP] = fpu_rnd(ST[TOP]);"]
                elif m == "fscale":
                    out += ["ST[TOP] = ST[TOP] * __builtin_exp2(__builtin_trunc(ST[(TOP+1)&7]));"]
                elif m == "fprem":
                    # resto truncado (como fmod) y reducción completa: C2 = 0 (los bucles `fprem; fnstsw; sahf; jp` terminan)
                    out += ["ST[TOP] = __builtin_fmod(ST[TOP], ST[(TOP+1)&7]); FPUSW &= ~0x4700;"]
                elif m == "fprem1":
                    out += ["ST[TOP] = __builtin_remainder(ST[TOP], ST[(TOP+1)&7]); FPUSW &= ~0x4700;"]
                elif m == "fdecstp":
                    out += ["TOP=(TOP-1)&7;"]
                elif m == "fincstp":
                    out += ["TOP=(TOP+1)&7;"]
                elif m == "fabs":
                    out += ["ST[TOP] = ST[TOP] < 0 ? -ST[TOP] : ST[TOP];"]
                elif m == "fchs":
                    out += ["ST[TOP] = -ST[TOP];"]
                elif m == "fabs":
                    out += ["ST[TOP] = ST[TOP] < 0 ? -ST[TOP] : ST[TOP];"]
                elif m in ("fnstsw", "fstsw"):
                    if mem:
                        out += [f"wr16({mem_addr(d)}, FPUSW);"]
                    else:
                        out += ["eax = (eax & 0xFFFF0000) | (FPUSW & 0xFFFF);"]
                elif m in ("fnstcw", "fstcw"):
                    out += [f"wr16({mem_addr(d)}, FPUCW);"]
                elif m == "fldcw":
                    out += [f"FPUCW = rd16({mem_addr(d)});"]
                elif m in ("finit", "fninit"):
                    out += ["TOP=0; FPUCW=0x037f; FPUSW=0;"]
                elif m in ("fwait", "wait", "fnop", "fclex", "fnclex"):
                    out += ["/* no-op */"]
                elif m == "ffree":
                    out += ["/* ffree */"]
                elif m in ("fldenv", "fnstenv", "frstor", "fsave", "fnsave", "frstor"):
                    out += ["/* estado de la FPU: no-op */"]
                elif m == "fxam":
                    out += ["{double _v=ST[TOP]; if(_v==0.0) FPUSW=(FPUSW&~0x4500)|0x4000; else FPUSW=(FPUSW&~0x4500)|0x0400;}"]
                else:
                    raise Unsupported(m)
            elif m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
                       "scasb", "scasw", "scasd", "lodsb", "lodsw", "lodsd",
                       "cmpsb", "cmpsw", "cmpsd"):
                step = {"movsb": 1, "stosb": 1, "scasb": 1, "lodsb": 1, "cmpsb": 1,
                        "movsw": 2, "stosw": 2, "scasw": 2, "lodsw": 2, "cmpsw": 2,
                        "movsd": 4, "stosd": 4, "scasd": 4, "lodsd": 4, "cmpsd": 4}[m]
                W = {1: "8", 2: "16", 4: "32"}[step]
                a = {1: "(eax & 0xFF)", 2: "(eax & 0xFFFF)", 4: "eax"}[step]
                adv = f"(DF ? -{step} : {step})"
                if m.startswith("movs"):
                    body = f"wr{W}(edi, rd{W}(esi)); esi += {adv}; edi += {adv};"
                elif m.startswith("stos"):
                    body = f"wr{W}(edi, {a}); edi += {adv};"
                elif m.startswith("lods"):
                    body = f"eax = (eax & ~0x{(1<<(8*step))-1:x}) | (rd{W}(esi) & 0x{(1<<(8*step))-1:x}); esi += {adv};"
                elif m.startswith("cmps"):
                    body = (" ".join(f_sub(f"rd{W}(esi)", f"rd{W}(edi)", step)) + f" esi += {adv}; edi += {adv};")
                else:  # scas
                    body = (" ".join(f_sub(a, f"rd{W}(edi)", step)) + f" edi += {adv};")
                if DOS:   # es:edi siempre; ds:esi salvo prefijo de segmento
                    _src = next((o.mem.segment for o in ops if o.type == X86_OP_MEM and o.mem.base == X86_REG_ESI), 0)
                    _si = _SREG.get(_src, 3)
                    body = (body.replace("(edi,", "(edi + sbase[0],").replace("(edi)", "(edi + sbase[0])")
                            .replace("(esi)", f"(esi + sbase[{_si}])"))
                if rep:
                    cond = "ZF" if rep in ("repne", "repnz") else ("!ZF" if rep in ("repe", "repz") else "0")
                    out += [f"while (ecx) {{ {body} ecx--; if((++w_steps&0xFFFFFFu)==0u)js_dbg(0xD7F9u,cur_fn,w_steps); if ({cond}) break; }}"]
                else:
                    out += [body]
            elif m == "sahf":
                out += ["CF = (eax >> 8) & 1; PF = (eax >> 10) & 1; ZF = (eax >> 14) & 1; SF = (eax >> 15) & 1;"]
            elif m == "lahf":
                out += ["eax = (eax & 0xFFFF00FF) | ((CF | (ZF << 6) | (SF << 7) | 2) << 8);"]
            elif m == "bswap":
                v = rd(d, d.size)
                out += [f"t = (({v} & 0xFF) << 24) | (({v} & 0xFF00) << 8) | (({v} >> 8) & 0xFF00) | (({v} >> 24) & 0xFF);"]
                out += wr(d, sz, "t")
            elif m in ("shrd", "shld"):
                cnt = rd(ops[2], ops[2].size) if len(ops) > 2 else "1u"
                a, b = rd(d, d.size), rd(s, s.size)
                bits, M, S = _szinfo(d.size)
                if m == "shrd":
                    out += [f"{{u32 c=({cnt}&31); t = (c && c < {bits}) ? ((({a} >> c) | ({b} << ({bits} - c))) & {M}) : {a}; CF = c ? (({a} >> (c-1)) & 1) : CF; ZF=(t==0); SF=(t>>{bits-1})&1;}}"]
                else:
                    out += [f"{{u32 c=({cnt}&31); t = (c && c < {bits}) ? ((({a} << c) | ({b} >> ({bits} - c))) & {M}) : {a}; CF = (c && c <= {bits}) ? (({a} >> ({bits}-c)) & 1) : CF; ZF=(t==0); SF=(t>>{bits-1})&1;}}"]
                out += wr(d, sz, "t")
            elif m in ("rcl", "rcr"):
                cnt = rd(s, s.size) if s else "1u"
                a = rd(d, d.size)
                bits, M, S = _szinfo(d.size)
                # rotación de (bits+1) posiciones contando CF; se hace con u64 para no desbordar
                if m == "rcl":
                    out += [f"{{u32 c=({cnt}&31)%{bits+1}; u64 v=((u64){a})|((u64)CF<<{bits}); if(c){{ v=(v<<c)|(v>>({bits+1}-c)); }} t=(u32)v&{M}; CF=(u32)(v>>{bits})&1; ZF=(t==0); SF=(t>>{bits-1})&1;}}"]
                else:
                    out += [f"{{u32 c=({cnt}&31)%{bits+1}; u64 v=((u64){a})|((u64)CF<<{bits}); if(c){{ v=(v>>c)|(v<<({bits+1}-c)); }} t=(u32)v&{M}; CF=(u32)(v>>{bits})&1; ZF=(t==0); SF=(t>>{bits-1})&1;}}"]
                out += wr(d, sz, "t")
            elif m in ("loop", "loope", "loopne", "jecxz", "jcxz"):
                if m.startswith("loop"):
                    out += ["ecx--;"]
                    cond = {"loop": "ecx", "loope": "ecx && ZF", "loopne": "ecx && !ZF"}[m]
                    out += [f"if ({cond}) {{"]
                elif m == "jecxz":
                    out += ["if (ecx == 0) {"]
                else:  # jcxz
                    out += ["if ((ecx & 0xFFFF) == 0) {"]
                # destino local → goto; si no, es una funcion aparte (o dato mal detectado)
                if d.imm in addrs:
                    out += [f"goto L_{d.imm:x};"]
                elif tlo <= d.imm < thi:
                    out += [f"f_{d.imm:x}(); return;"]
                else:
                    out += ["/* salto fuera de rango */"]
                out += ["}"]
            elif m == "cpuid":
                out += ["cpuid_emul();"]
            elif m == "fsincos":
                out += ["{double _x=ST[TOP]; ST[TOP]=__builtin_sin(_x); TOP=(TOP-1)&7; ST[TOP]=__builtin_cos(_x);}"]
            elif m == "emms":
                out += ["/* emms */"]
            elif m in ("pxor", "pand", "pandn", "por", "paddb", "paddw", "paddd", "psubb", "psubw", "psubd",
                       "pmullw", "psllq", "psrlq", "psllw", "psrlw", "psraw", "pslld", "psrld", "psrad",
                       "paddusb", "psubusb", "paddusw", "psubusw",
                       "packuswb", "packsswb", "punpcklbw", "punpckhbw",
                       "punpcklwd", "punpckhwd", "punpckldq", "punpckhdq", "movd", "movq",
                       "pcmpeqb", "pcmpeqw", "pcmpeqd"):
                out += [mmx(m, d, s, sz, rd, wr)]
            elif m == "stc":
                out += ["CF = 1;"]
            elif m == "clc":
                out += ["CF = 0;"]
            elif m == "cmc":
                out += ["CF = !CF;"]
            elif m == "salc":
                # set AL from carry (opcode D6, no documentada): AL = CF ? 0xFF : 0
                out += ["{u32 _al = CF ? 0xFFu : 0u; eax = (eax & 0xFFFFFF00u) | _al;}"]
            elif m in ("pushal", "pusha"):
                out += ["{u32 _sp=esp; esp-=32; wr32(esp, eax); wr32(esp+4, ecx); wr32(esp+8, edx); "
                        "wr32(esp+12, ebx); wr32(esp+16, _sp); wr32(esp+20, ebp); wr32(esp+24, esi); wr32(esp+28, edi);}"]
            elif m in ("popal", "popa"):
                out += ["{eax=rd32(esp); ecx=rd32(esp+4); edx=rd32(esp+8); ebx=rd32(esp+12); "
                        "ebp=rd32(esp+20); esi=rd32(esp+24); edi=rd32(esp+28); esp+=32;}"]
            elif m == "enter":
                sz16 = ops[0].imm & 0xFFFF if ops else 0
                out += ["{u32 _bp=ebp; esp-=4; wr32(esp,_bp); ebp=esp; esp-=%du;}" % sz16]
            elif m in ("sti", "cli"):
                out += ["/* interrupciones: no-op */"]
            elif m == "in":
                # entrada de puerto (no hay puertos): devuelve 0
                if d is not None and d.type == X86_OP_REG:
                    out += wr(d, d.size, "0u")
                else:
                    out += ["/* in: no-op */"]
            elif m == "out":
                out += ["/* out: no-op */"]
            elif m in ("int", "int3", "int1", "into"):
                out += ["/* interrupcion: no-op */"]
            elif m == "retf":
                n = d.imm if (d is not None and d.type == X86_OP_IMM) else 0
                out += [f"esp += 8 + {n}; return;"]
            elif m == "ljmp":
                if d is not None and d.type == X86_OP_IMM and d.imm in addrs:
                    out += [f"goto L_{d.imm:x};"]
                elif d is not None and d.type == X86_OP_IMM and tlo <= d.imm < thi:
                    out += [f"f_{d.imm:x}(); return;"]
                else:
                    out += ["/* ljmp: no-op */"]
            elif m == "lcall":
                # llamada lejana: se ignora el segmento, se trata como una llamada normal
                ra = ins.address + ins.size
                if d is not None and d.type == X86_OP_IMM and (d.imm in ent or tlo <= d.imm < thi):
                    out += [f"esp -= 4; wr32(esp, 0x{ra:x}u); f_{d.imm:x}();"]
                else:
                    out += ["/* lcall: no-op */"]
            elif m == "aaa":
                out += ["{u32 _al=eax&0xFF; if((_al&0x0F)>9){eax+=6;} if(_al>0x99){eax+=0x100; CF=1;} else CF=0; eax=(eax&0xFFFFFFF0)|((eax&0xFF)&0x0F);}"]
            elif m == "aad":
                out += ["{u32 _al=((eax>>8)&0xFF)*10u+(eax&0xFF); eax=(eax&0xFFFF0000)|(_al&0xFF); ZF=(_al==0); SF=0; CF=0; OF=0; PF=(__builtin_parity((u32)(_al&0xFF))^1);}"]
            elif m == "aas":
                out += ["{u32 _al=eax&0xFF; if((_al&0x0F)>9){eax-=6; eax-=0x100; CF=1;} else CF=0; eax=(eax&0xFFFFFF00)|((eax&0xFF)&0x0F);}"]
            elif m == "das":
                out += ["{u32 _al=eax&0xFF; u32 _cf=CF; if((_al&0x0F)>9||CF){_al-=6; _cf=1;} if(((eax&0xFF)>0x99)||CF){_al-=0x60; _cf=1;} eax=(eax&0xFFFFFF00)|(_al&0x0F); CF=_cf;}"]
            elif m == "xlatb":
                out += ["eax = (eax & 0xFFFFFF00u) | rd8(ebx + (eax & 0xFF));"]
            elif m == "iretd":
                out += ["esp += 12; return;"]
            elif m in ("lds", "les", "lfs", "lgs", "lss"):
                # carga de puntero lejano: el segmento se ignora, solo interesa el offset
                a = mem_addr(d) if s is not None and s.type == X86_OP_MEM else mem_addr(s)
                out += wr(d, sz, f"rd32({a})")
            elif m in ("outsb", "outsw", "outsd", "insb", "insw", "insd"):
                step = {"outsb":1,"insb":1,"outsw":2,"insw":2,"outsd":4,"insd":4}[m]
                adv = f"(DF ? -{step} : {step})"
                if m.startswith("outs"):
                    out += [f"esi += {adv}; /* outs: no-op */"]
                else:
                    out += [f"edi += {adv}; /* ins: no-op */"]
            elif m in ("rep", "repne", "repe"):
                raise Unsupported(m)
            elif m.startswith("f") and m not in ("fld", "fst", "fstp", "fild", "fistp", "fadd", "fsub", "fmul", "fdiv", "fcom", "fchs", "fabs", "fxch", "fstcw", "fldcw", "fnstcw", "fldz", "fld1", "fild", "fist", "fistp", "fcompp", "fucompp", "fsubr", "fdivr", "faddp", "fsubp", "fmulp", "fdivp", "fcomp", "fldenv", "fnstsw", "fwait", "finit", "fninit", "fdecstp", "fincstp", "fprem", "frndint", "fsqrt", "fscale", "fsin", "fcos", "fptan", "fpatan", "f2xm1", "fyl2x", "fabs", "ftst", "fxam"):
                raise Unsupported(m)
            else:
                raise Unsupported(m)
        except (Unsupported, KeyError, IndexError):
            # instrucción que no se sabe traducir (casi siempre datos decodificados como código, p. ej.
            # `bndstx` tras el final real de un bloque huérfano): si llega a ejecutarse, avisa
            out += [f"js_dbg(0xBAD1u, 0x{ins.address:x}u, 0); return;"]
    # la última instrucción sigue de largo (la función se cortó en el inicio de otra entrada, p.ej. un
    # tamaño estimado sin Ghidra): continúa en esa función como salto de cola, no vuelve sin epílogo
    if insns and _falls_through(insns[-1]):
        _ft = insns[-1].address + insns[-1].size
        out.append(f"  f_{_ft:x}(); return;" if _ft in ent or _ft in _stops else f"  js_dbg(0xFA11u, 0x{_ft:x}u, 0); return;")
    out += ["  return;", "}", ""]
    # saltos de cola (`f_x(); return;`): al volver, esp debe estar por encima de la entrada (ret + args)
    out = [re.sub(r"\b((?:f|ext)_\w+)\(\); return;", rf"\1(); ESPCHK_T(0x{va:x}u); return;", l) for l in out]
    _PH = None
    _CUR_INS = None
    # etiquetas que no llegaron a emitirse (bytes de datos decodificados como código): aviso en ejecución
    text = "\n".join(out)
    if DOS:   # saltos de cola (f_x(); return;) y saltos indirectos (ijmp): el marco es el mismo
        def _tail(m):
            return m.group(0) if text[max(0, m.start() - 40):m.start()].rstrip().endswith("u);") and "wr32(esp" in text[max(0, m.start() - 40):m.start()] else "g_ret_expected = _ret0; " + m.group(0)
        text = re.sub(r"\bf_[0-9a-f]+\(\); ESPCHK_T", _tail, text)
        text = re.sub(r"(?<!_ret0; )\bijmp\(", "g_ret_expected = _ret0; ijmp(", text)
    if DOS:   # IRQ pendientes (temporizador, teclado…): se atienden en los saltos hacia atrás de los bucles
        text = text.replace("js_dbg(0xD7F9u,cur_fn,w_steps); goto", "js_dbg(0xD7F9u,cur_fn,w_steps); DOS_POLL(); goto")
    labels = set(re.findall(r"^  L_([0-9a-f]+):", text, re.M))
    text = re.sub(r"goto L_([0-9a-f]+);", lambda mm: mm.group(0) if mm.group(1) in labels
                  else "{ js_dbg(0xBADEu, 0x%su, 0); return; }" % mm.group(1), text)
    return text
