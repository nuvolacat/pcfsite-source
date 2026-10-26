"""El juego que se traduce (juegos/<id>/juego.json), elegido con PCF_JUEGO (por defecto pcf7).

Da las rutas de trabajo (binarios, descompilado de Ghidra) y dónde va el C generado: build/gen* para pcf7 (como
siempre) y build/<id>/gen* para los demás."""
from __future__ import annotations

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ID = os.environ.get("PCF_JUEGO", "pcf7")
_P = os.path.join(ROOT, "juegos", ID, "juego.json")
if not os.path.exists(_P):
    raise SystemExit(f"no existe el juego «{ID}» ({_P})")
with open(_P, encoding="utf-8") as _f:
    CFG = json.load(_f)

NOMBRE = CFG["nombre"]
CARPETA = CFG["carpeta"]                   # C:\<CARPETA>: la instalación
TRABAJO = os.path.expanduser(os.environ.get("PCF7_WORK", CFG["trabajo"]))
PRINCIPAL = CFG["principal"].lower()
SALIDA_MENU = int(CFG.get("salida_menu") or "0", 0)
DLLS = [(n, int(b, 16)) for n, b in CFG["dlls"]]
PROGRAMAS = {k.lower(): v for k, v in CFG.get("programas", {}).items()}
LANZADOR = sorted((int(k), v[0], v[1]) for k, v in CFG.get("lanzador", {}).items())   # apartado → (programa, argumento)


def gen_dir(stem: str) -> str:
    """Carpeta del C generado para un programa (stem = nombre sin .exe)."""
    pre = os.path.join(ROOT, "build") if ID == "pcf7" else os.path.join(ROOT, "build", ID)
    return os.path.join(pre, "gen" if stem == PRINCIPAL.rsplit(".", 1)[0] else "gen-" + stem)


def gen_rel(stem: str) -> str:
    """La misma carpeta, relativa a build/ (la plataforma busca mem.bin en <ejecutable>/../<esto>)."""
    return os.path.relpath(gen_dir(stem), os.path.join(ROOT, "build"))


def juego_c(target: str) -> str:
    """juego.c: lo propio del juego que necesita la plataforma (ver platform/src/plat.h)."""
    esc = lambda s: s.replace("\\", "\\\\").replace('"', '\\"')
    salida = SALIDA_MENU if target == PRINCIPAL else 0
    return ("/* Generado por recomp/build.py desde juegos/%s/juego.json: no editar. */\n"
            'const char JUEGO_ID[] = "%s";\n'
            'const char JUEGO_NOMBRE[] = "%s";\n'
            'const char JUEGO_CARPETA[] = "%s";\n'
            'const char JUEGO_TRABAJO[] = "%s";\n'
            'const char JUEGO_PRINCIPAL[] = "%s";\n'
            "const unsigned JUEGO_SALIDA_MENU = 0x%xu;   /* 0: sin lanzador */\n"
            % (ID, esc(ID), esc(NOMBRE), esc(CARPETA), esc(CFG["trabajo"].replace("~/", "")), esc(PRINCIPAL), salida)
            + "/* lanzador: apartado del menú → programa hermano (<id>-<programa>) y su argumento */\n"
            + "const struct JuegoLanza { unsigned apartado; const char *programa, *arg; } JUEGO_LANZADOR[] = {\n"
            + "".join('    {%d, "%s", %s},\n' % (c, esc(e), '"%s"' % esc(a) if a is not None else "0") for c, e, a in LANZADOR)
            + "    {0, 0, 0}\n};\n"
            + "/* variables de entorno propias del juego (además de las de un Windows 98 recién instalado) */\n"
            + "const char *const JUEGO_ENTORNO[] = {"
            + "".join('"%s=%s", ' % (esc(k), esc(v)) for k, v in CFG.get("entorno", {}).items()) + "0};\n")
