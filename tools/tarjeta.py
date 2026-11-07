"""Tarjetas de 1200×630 para compartir enlaces: sobre la imagen general de cada web se pinta el contenido de la página.

La misma pieza está en source (tools/), descargas (raíz) y online (tools/): cada web tiene su repositorio. La del foro,
que las pinta al pedirlas, es foro/app/og.py.
"""
import hashlib
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

FONDO, TEXTO, GRIS, AZUL, LINEA = (242, 242, 242), (30, 30, 30), (72, 72, 72), (62, 96, 132), (214, 217, 222)
IZQ, ANCHO = 98, 510
DERECHA = (640, 132, 1135, 566)


def _limpio(s):
    return re.sub(r"\s+", " ", "".join(c for c in (s or "") if ord(c) < 0x180 or 0x2010 <= ord(c) <= 0x2026)).strip()


def _lineas(d, texto, fuente, ancho, maximo):
    """Reparte en líneas; lo que no cabe acaba en «…»."""
    out, cur, resto = [], "", False
    for w in texto.split():
        prueba = f"{cur} {w}".strip()
        if d.textlength(prueba, font=fuente) <= ancho:
            cur = prueba
            continue
        if cur:
            out.append(cur)
        cur = w
        if len(out) == maximo:
            resto = True
            break
    if not resto:
        out.append(cur)
        if d.textlength(out[-1], font=fuente) <= ancho and len(out) <= maximo:
            return out
    ultima = out[maximo - 1]
    while ultima and d.textlength(ultima + "…", font=fuente) > ancho:
        ultima = ultima.rsplit(" ", 1)[0] if " " in ultima else ultima[:-1]
    return out[:maximo - 1] + [ultima + "…"]


def pinta(plantilla, fuentes, kicker, titulo, texto, pie="", foto=None, color=None, filas=None, tapa=582):
    """`foto`: fichero para la derecha (`color`: fondo de esa zona, con la foto centrada). `filas`: lista de nombres.
    `tapa`: hasta dónde se borra la plantilla (la de juegos pasa del marco); el marco se vuelve a dibujar."""
    f = lambda n, t: ImageFont.truetype(str(Path(fuentes) / f"{n}-latin.woff2"), t)
    im = Image.open(plantilla).convert("RGB")
    d = ImageDraw.Draw(im)
    d.rectangle((47, 114, 1152, tapa), fill=FONDO)
    if tapa > 582:
        d.rectangle((44, 112, 1155, 584), outline=LINEA)
    x0, y0, x1, y1 = DERECHA
    if foto:
        foto = ImageOps.exif_transpose(Image.open(foto)).convert("RGBA")
        if color:
            d.rectangle((x0, y0, x1, y1), fill=color)
            foto = ImageOps.contain(foto, (int((x1 - x0) * .8), int((y1 - y0) * .66)))
        else:
            foto = ImageOps.contain(foto, (x1 - x0, y1 - y0))
        x, y = x0 + (x1 - x0 - foto.width) // 2, y0 + (y1 - y0 - foto.height) // 2
        im.paste(foto, (x, y), foto)
        if not color:
            d.rectangle((x - 1, y - 1, x + foto.width, y + foto.height), outline=(200, 205, 212))
    if filas:
        fr, alto = f("BarlowCondensed-600", 30), (y1 - y0) // 6
        d.line((x0, y0, x0, y1), fill=LINEA)
        for i, fila in enumerate(filas[:6]):
            y = y0 + i * alto
            d.text((x0 + 24, y + alto // 2), _lineas(d, _limpio(fila), fr, x1 - x0 - 48, 1)[0], font=fr, fill=TEXTO, anchor="lm")
            if i < min(len(filas), 6) - 1:
                d.line((x0, y + alto, x1, y + alto), fill=LINEA)
    fk = f("Barlow-600", 19)
    y = 146
    d.text((IZQ, y), _lineas(d, _limpio(kicker).upper(), fk, ANCHO, 1)[0], font=fk, fill=AZUL)
    y += 36
    titulo = _limpio(titulo).upper()
    for tam in (66, 60, 54, 48, 44):
        ft = f("BarlowCondensed-600", tam)
        if len(_lineas(d, titulo, ft, ANCHO, 99)) <= 4:
            break
    for ln in _lineas(d, titulo, ft, ANCHO, 4):
        d.text((IZQ, y), ln, font=ft, fill=TEXTO)
        y += int(ft.size * 1.06)
    y += 12
    fx = f("Barlow-400", 23)
    hueco = max(0, (505 - y) // 31)
    if texto and hueco:
        for ln in _lineas(d, _limpio(texto), fx, ANCHO, min(hueco, 4)):
            d.text((IZQ, y), ln, font=fx, fill=GRIS)
            y += 31
    if pie:
        fp = f("Barlow-500", 21)
        d.text((IZQ, 542), _lineas(d, _limpio(pie), fp, ANCHO, 1)[0], font=fp, fill=TEXTO, anchor="lm")
    return im


def guarda(im, destino):
    """JPEG en `destino`; devuelve la huella del fichero, para la URL (?v=): las redes guardan cada imagen por su URL."""
    Path(destino).parent.mkdir(parents=True, exist_ok=True)
    im.save(destino, "JPEG", quality=86, optimize=True, progressive=True)
    return hashlib.sha256(Path(destino).read_bytes()).hexdigest()[:10]
