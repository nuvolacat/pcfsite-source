#!/usr/bin/env python3
"""Iconos y manifiestos para «Añadir a pantalla de inicio» (PWA) de cada juego en el navegador.

El icono es el original del programa principal de cada edición (el MANAGER.EXE, MANAGARG.EXE en las Apertura): su
imagen de 32x32 con más colores, ampliada píxel a píxel (sin suavizar) sobre fondo negro, que es como iOS pinta lo
transparente. El ejecutable se lee de los datos del juego (datos.pcfsite.com/<id>/) o de una carpeta local con --exe.
El PC Fútbol 4.0 es de MS-DOS y su ejecutable no trae icono: lleva el del PC Fútbol 2000.

Sale en site/pcf/pwa/: <id>-180.png (apple-touch-icon), <id>-192.png, <id>-512.png y <id>.webmanifest (nombre corto:
«PC Fútbol» y su versión). Lo enlazan la página del juego (core/web/index.html, deploy.sh pone el id) y la ficha.

    python3 tools/iconos_pwa.py            # necesita pefile y Pillow (pip install pefile pillow)
"""
import argparse, io, json, os, struct, sys, urllib.request

import pefile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SALIDA = os.path.join(HERE, "..", "site", "pcf", "pwa")
DATOS = "https://datos.pcfsite.com"

# id del juego en /jugar/<id>/ → (programa principal dentro de sus datos, nombre para la pantalla de inicio); con
# «otro:ruta», el de los datos de otro juego
JUEGOS = {
    "pcf4": ("pcf2000:game/PCF2000/manager.exe", "PC Fútbol 4.0"),   # MS-DOS, sin icono: el del 2000
    "pcf5": ("game/FUTBOL5/MANAGER.EXE", "PC Fútbol 5.0"),
    "pcf5oro": ("game/FUTBOL5/MANAGER.EXE", "PC Fútbol 5.0 Oro"),
    "pcf5arg": ("game/APERTURA97/MANAGARG.EXE", "PC Fútbol 5.0 Apertura 97"),
    "pcf6": ("game/FUTBOL6/MANAGER.EXE", "PC Fútbol 6.0"),
    "pcf6oro": ("game/FUTBOL6/MANAGER.EXE", "PC Fútbol 6.0 Oro"),
    "pcf6arg": ("game/APERTURA98/MANAGARG.EXE", "PC Fútbol 6.0 Apertura 98"),
    "pcf7": ("game/FUTBOL7/MANAGER.EXE", "PC Fútbol 7"),
    "pcf7plus": ("game/FUTBOL7/MANAGER.EXE", "PC Fútbol 7 Plus"),
    "pcf2000": ("game/PCF2000/manager.exe", "PC Fútbol 2000"),
    "pcf2001": ("game/PCF2001/manager.exe", "PC Fútbol 2001"),
    "pcf2001act": ("game/PCF2001/manager.exe", "PC Fútbol 2001 26/27"),
}
# tamaño del PNG → aumento entero del icono de 32 px (el resto, fondo negro alrededor)
TAMANOS = {180: 4, 192: 5, 512: 12}


def icono(exe):
    """La imagen de 32x32 con más bits por píxel del primer grupo de iconos (RT_GROUP_ICON) del ejecutable."""
    pe = pefile.PE(data=exe)
    rec = {}
    for t in getattr(pe, "DIRECTORY_ENTRY_RESOURCE", type("", (), {"entries": []})).entries:
        for e in t.directory.entries:
            for l in e.directory.entries:
                rec.setdefault(t.id, {})[e.id] = pe.get_data(l.data.struct.OffsetToData, l.data.struct.Size)
    grupos, imagenes = rec.get(14, {}), rec.get(3, {})
    mejor = None
    for g in grupos.values():
        for i in range(struct.unpack_from("<H", g, 4)[0]):
            w, h, colores, _, planos, bpp, _, iid = struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i)
            datos = imagenes.get(iid)
            if datos is None:
                continue
            # un .ico de una sola imagen con su cabecera, para que lo lea Pillow
            ico = struct.pack("<HHHBBBBHHII", 0, 1, 1, w, h, colores, 0, planos, bpp, len(datos), 22) + datos
            im = Image.open(io.BytesIO(ico)).convert("RGBA")
            clave = (im.size[0] == 32, bpp, im.size[0])
            if mejor is None or clave > mejor[0]:
                mejor = (clave, im)
        break
    return mejor[1] if mejor else None


def leer(id_, ruta, carpeta):
    if carpeta:
        with open(os.path.join(carpeta, id_ + ".exe"), "rb") as f:
            return f.read()
    if ":" in ruta:
        id_, ruta = ruta.split(":", 1)
    pide = urllib.request.Request(f"{DATOS}/{id_}/{ruta}", headers={"Origin": "https://source.pcfsite.com", "User-Agent": "Mozilla/5.0 (pcfsite-source iconos_pwa)"})
    with urllib.request.urlopen(pide, timeout=60) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="carpeta con <id>.exe en vez de bajarlos de los datos")
    a = ap.parse_args()
    os.makedirs(SALIDA, exist_ok=True)
    for id_, (ruta, nombre) in JUEGOS.items():
        im = icono(leer(id_, ruta, a.exe)) if ruta else None
        iconos = []
        if im is not None:
            for tam, x in TAMANOS.items():
                lienzo = Image.new("RGBA", (tam, tam), (0, 0, 0, 255))
                grande = im.resize((im.size[0] * x, im.size[1] * x), Image.NEAREST)
                lienzo.alpha_composite(grande, ((tam - grande.size[0]) // 2, (tam - grande.size[1]) // 2))
                lienzo.convert("RGB").save(os.path.join(SALIDA, f"{id_}-{tam}.png"), optimize=True)
                if tam != 180:
                    iconos.append({"src": f"/pcf/pwa/{id_}-{tam}.png", "sizes": f"{tam}x{tam}", "type": "image/png"})
        else:
            iconos.append({"src": "/pcf/img/apple-touch-icon.png", "sizes": "180x180", "type": "image/png"})
        manifiesto = {
            "name": nombre, "short_name": nombre, "lang": "es",
            "start_url": f"/jugar/{id_}/", "scope": f"/jugar/{id_}/", "id": f"/jugar/{id_}/",
            "display": "fullscreen", "display_override": ["fullscreen", "standalone"], "orientation": "landscape",
            "background_color": "#000000", "theme_color": "#000000", "icons": iconos,
        }
        with open(os.path.join(SALIDA, id_ + ".webmanifest"), "w", encoding="utf-8") as f:
            json.dump(manifiesto, f, ensure_ascii=False, indent=1)
            f.write("\n")
        print(f"{id_:11} {nombre:24} {'icono del ' + ruta.rsplit('/', 1)[1] if im is not None else 'sin icono (el de la web)'}")


if __name__ == "__main__":
    sys.exit(main())
