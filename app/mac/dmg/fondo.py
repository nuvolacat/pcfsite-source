#!/usr/bin/env python3
"""Fondo de la ventana del .dmg (logo de nuvola: nuvola.png, el de pcfsite.com) (app/mac/empaquetar.sh --dmg): genera fondo.png (660×400) y fondo@2x.png, que
empaquetar.sh junta en fondo.tiff (Retina). Estilo de la app: claro, acento #5980a6, marcas «+», Barlow.
Necesita Pillow (core/.venv): core/.venv/bin/python app/mac/dmg/fondo.py
Los iconos los dibuja Finder encima: la app centrada en (170, 205) y Aplicaciones en (490, 205), a 128 px."""
import os
from PIL import Image, ImageDraw, ImageFont

D = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(D, "..", "..")
FONDO, TEXTO, ACENTO, LINEA, SUAVE = (242, 242, 243), (29, 31, 32), (89, 128, 166), (210, 211, 214), (122, 122, 125)
W, H = 660, 400
APP_X, AP_X, ICO_Y = 170, 490, 205


def fuente(n, t, k):
    return ImageFont.truetype(os.path.join(APP, "fonts", n), int(t * k))


def pinta(k):
    im = Image.new("RGB", (W * k, H * k), FONDO)
    d = ImageDraw.Draw(im)
    s = lambda v: int(round(v * k))
    # marco «blueprint» con marcas + en las esquinas
    x0, y0, x1, y1 = 18, 18, W - 18, H - 18
    d.rectangle([s(x0), s(y0), s(x1), s(y1)], outline=LINEA, width=max(1, k))
    for (x, y) in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
        d.line([s(x - 7), s(y), s(x + 7), s(y)], fill=TEXTO, width=max(1, k))
        d.line([s(x), s(y - 7), s(x), s(y + 7)], fill=TEXTO, width=max(1, k))
    # cabecera: logo y «instalar en el Mac»
    logo = Image.open(os.path.join(APP, "assets", "logo.png")).convert("RGBA")
    lw = s(170); logo = logo.resize((lw, round(logo.height * lw / logo.width)), Image.LANCZOS)
    im.paste(logo, (s(40), s(42)), logo)
    f = fuente("BarlowCondensed-SemiBold.ttf", 13, k)
    t = "INSTALAR EN EL MAC"
    esp = s(1.6)
    ancho = sum(d.textlength(c, font=f) + esp for c in t) - esp
    x = s(W - 40) - ancho
    for c in t:
        d.text((x, s(48)), c, font=f, fill=ACENTO); x += d.textlength(c, font=f) + esp
    d.line([s(40), s(92), s(W - 40), s(92)], fill=LINEA, width=max(1, k))
    # flecha discontinua entre los dos iconos, con el texto encima
    ya = ICO_Y - 6
    xa, xb = APP_X + 82, AP_X - 82
    x = xa
    while x < xb - 16:
        d.line([s(x), s(ya), s(min(x + 8, xb - 16)), s(ya)], fill=ACENTO, width=s(2.2)); x += 14
    d.polygon([(s(xb), s(ya)), (s(xb - 14), s(ya - 8)), (s(xb - 14), s(ya + 8))], fill=ACENTO)
    f = fuente("BarlowCondensed-SemiBold.ttf", 17, k)
    t = "Arrástrala a Aplicaciones"
    d.text((s((xa + xb) / 2) - d.textlength(t, font=f) / 2, s(ya - 34)), t, font=f, fill=TEXTO)
    # pie: qué hacer después y, en la esquina, nuvola
    d.line([s(40), s(H - 74), s(W - 40), s(H - 74)], fill=LINEA, width=max(1, k))
    f = fuente("Barlow-Regular.ttf", 12, k)
    d.text((s(40), s(H - 62)), "Luego ábrela desde Aplicaciones. Los juegos se preparan", font=f, fill=TEXTO)
    d.text((s(40), s(H - 46)), "desde la ISO de tu CD original.  ·  source.pcfsite.com", font=f, fill=SUAVE)
    nuvola = Image.open(os.path.join(D, "nuvola.png")).convert("RGBA")
    nh = s(18); nuvola = nuvola.resize((round(nuvola.width * nh / nuvola.height), nh), Image.LANCZOS)
    im.paste(nuvola, (s(W - 40) - nuvola.width, s(H - 58)), nuvola)
    return im


pinta(1).save(os.path.join(D, "fondo.png"))
pinta(2).save(os.path.join(D, "fondo@2x.png"))
print("fondo.png y fondo@2x.png")
