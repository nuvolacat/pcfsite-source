#!/usr/bin/env python3
"""Genera source.pcfsite.com en dist/ a partir de data/juegos.json y site/ (sin dependencias).

  python3 build.py            → dist/ listo para publicar
  python3 -m http.server -d dist 8000   → vista previa en http://localhost:8000/

Páginas: portada (/), ficha de cada juego (/<id>), /como-funciona y /diario. La barra, las secciones y el pie los
pinta site/topbar.js (generado aquí desde topbar.src.js), igual que en las demás zonas de pcfsite.com.
"""
import datetime
import html
import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.join(HERE, "site")
DIST = os.path.join(HERE, "dist")
BASE_URL = "https://source.pcfsite.com"
OG_V = 1   # súbelo si se rehacen las imágenes para redes (tools/og.sh): las redes guardan la imagen en caché

e = html.escape
CORNERS = '<i class="corner tl"></i><i class="corner tr"></i><i class="corner bl"></i><i class="corner br"></i>'


def version():
    """v1.AA.MMDD.HHMM, como el resto de zonas de pcfsite.com."""
    n = datetime.datetime.now()
    return f"1.{n:%y}.{n:%m%d}.{n:%H%M}"


def pagina(ruta, titulo, descripcion, cuerpo, imagen=None, v=""):
    """Documento completo: cabecera con Open Graph, estilos del diseño y la barra común."""
    url = BASE_URL + ruta
    og = BASE_URL + (imagen or "/pcf/img/og.jpg") + f"?v={OG_V}"
    css = "".join(f'<link rel="stylesheet" href="/pcf/{n}?v={v}">' for n in ("fonts.css", "ds.css", "comun.css", "source.css"))
    return f"""<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{e(titulo)}</title>
<meta name="description" content="{e(descripcion)}">
<link rel="canonical" href="{url}">
<meta property="og:type" content="website">
<meta property="og:site_name" content="PCFSITE.COM">
<meta property="og:url" content="{url}">
<meta property="og:title" content="{e(titulo)}">
<meta property="og:description" content="{e(descripcion)}">
<meta property="og:image" content="{og}">
<meta property="og:image:type" content="image/jpeg">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta name="twitter:image" content="{og}">
<meta property="og:locale" content="es_ES">
<meta name="twitter:card" content="summary_large_image">
<meta name="theme-color" content="#5980a6">
<link rel="icon" href="/pcf/img/favicon.svg" type="image/svg+xml">
<link rel="icon" href="/favicon.ico" sizes="48x48">
<link rel="apple-touch-icon" href="/pcf/img/apple-touch-icon.png">
{css}
<script src="/pcf/tema.js?v={v}"></script>
</head>
<body>
<div class="wrap">
<main id="contenido">
{cuerpo}
</main>
</div>
<div class="visor" id="visor"><img alt=""></div>
<script src="/topbar.js?v={v}"></script>
<script src="/pcf/visor.js?v={v}"></script>
<script src="/pcf/juego.js?v={v}"></script>
<script src="/pcf/estado.js?v={v}"></script>
</body>
</html>
"""


def captura(juego, cid):
    return f"/pcf/img/capturas/{juego}/{cid}.jpg"


# estado del juego → etiqueta (los «proximamente» salen atenuados)
ETIQUETAS = {"completado": ("tag-ok", "Completado"), "en-curso": ("tag-curso", "En curso"),
             "proximamente": ("tag-prox", "Próximamente")}


def etiqueta(j):
    clase, texto = ETIQUETAS[j["estado"]]
    return f'<span class="tag {clase}">{texto}</span>'


def jugable(j):
    return any(x.get("jugar") for x in j.get("versiones", []))


def tarjeta(j):
    prox = j["estado"] == "proximamente"
    if j.get("portada"):
        cover = f'<img class="shot" src="{captura(j["id"], j["portada"])}" alt="" loading="lazy">'
    else:
        cover = f'<span class="ver">{e(j["nombre"].split()[-1])}</span>'
    tag = etiqueta(j)
    return (f'<a class="card-g blueprint{" prox" if prox else ""}" href="/{j["id"]}">{CORNERS}'
            f'<div class="cover">{cover}<div class="tags">{tag}</div></div>'
            f'<div class="body"><span class="name">{e(j["nombre"])}</span>'
            f'<span class="meta">Temporada {e(j["temporada"])} · {j["anio"]}</span>'
            f'<p class="meta resumen">{e(j["resumen"])}</p></div></a>')


# los apartados de la portada («grupo» en data/juegos.json; sin él, España)
GRUPOS = [("espana", "España"), ("internacional", "Internacional")]


def grupo(titulo, juegos):
    if not juegos:
        return ""
    return (f'<section class="cat cat-juegos">\n  <div class="cat-head"><h2>{titulo}</h2><span class="cnt">{len(juegos)} en el proyecto</span></div>\n'
            f'  <div class="grid">{"".join(tarjeta(j) for j in juegos)}</div>\n</section>\n')


def collage(juegos):
    """La placa de la portada: todas las capturas de todos los juegos, entrelazadas (una de cada juego por turno) y
    enlazadas a su ficha; cada séptima a doble tamaño para dar ritmo. El pie dice cuántas son."""
    colas = [[(j, c) for c in j.get("capturas", [])] for j in juegos if j.get("capturas")]
    orden = []
    while any(colas):
        for q in colas:
            if q: orden.append(q.pop(0))
    tiles = "".join(f'<a class="c" href="/{j["id"]}" title="{e(j["nombre"])}: {e(c["texto"])}">'
                    f'<img src="{captura(j["id"], c["id"])}" alt="{e(j["nombre"])}: {e(c["texto"])}" loading="lazy"></a>' for j, c in orden)
    return (f'<div class="hero-collage" aria-label="Capturas de los juegos">{tiles}'
            f'<span class="hero-collage-pie"><span>{len(colas)} PC Fútbol · {len(orden)} capturas</span><span>reales, de la versión traducida</span></span></div>')


def portada(datos, v):
    juegos = datos["juegos"]
    destacado = next((j for j in juegos if j.get("destacado")), next(j for j in juegos if j["estado"] != "proximamente"))
    cifras = "".join(f'<div><span class="big num">{e(c["valor"])}</span><span class="label">{e(c["texto"])}</span></div>'
                     for c in datos["cifras"])
    cuerpo = f"""
<section class="hero blueprint">{CORNERS}
  <div class="hero-txt">
    <span class="kicker">El código de los PC Fútbol</span>
    <h1>Los clásicos, traducidos desde su propio código</h1>
    <p>Recuperamos el programa original de cada PC Fútbol, instrucción a instrucción, y lo convertimos en código
       fuente moderno. El resultado es el mismo juego, sin emuladores, funcionando en los ordenadores de hoy.</p>
    <div class="acts">
      <a class="btn btn-primary blueprint" href="/como-funciona">{CORNERS}Cómo funciona</a>
    </div>
  </div>
  {collage(juegos)}
</section>

<div class="cifras">{cifras}</div>

{"".join(grupo(t, [j for j in juegos if j.get("grupo", "espana") == g]) for g, t in GRUPOS)}
<section class="cat">
  <div class="cat-head"><h2>Cómo se hace</h2><a class="cnt lnk" href="/como-funciona">Leer más →</a></div>
  <div class="pasos">
    <div class="paso blueprint">{CORNERS}<span class="n">01</span><h3>Traducir</h3>
      <p>Cada función del programa original se traduce a C: las mismas operaciones, en el mismo orden. Nada se
         reescribe a mano ni se inventa.</p></div>
    <div class="paso blueprint">{CORNERS}<span class="n">02</span><h3>Reconstruir Windows</h3>
      <p>Lo que el juego le pedía a Windows 98 (ventanas, DirectX, sonido, vídeo) se vuelve a hacer desde cero
         sobre SDL, para que funcione en cualquier sistema.</p></div>
    <div class="paso blueprint">{CORNERS}<span class="n">03</span><h3>Comprobar</h3>
      <p>Recorremos el juego con guiones automáticos y capturas, comparando con el original, hasta que todo
         funciona igual.</p></div>
  </div>
</section>

<p class="aviso-cd">Los juegos ya traducidos se pueden jugar aquí mismo, en el navegador: el mismo programa de 1998,
sin emuladores ni instalaciones.</p>
"""
    return pagina("/", "source.pcfsite.com · El código de los PC Fútbol",
                  "Los PC Fútbol clásicos traducidos desde su propio código a aplicaciones modernas, sin emuladores.",
                  cuerpo, None, v)



# partidas de juegos.pcfsite.com («Mis partidas» → <clave>.pcfsave) que Source convierte (servicio/pcfsave.py): la
# versión de Source → el fichero que se descarga allí (el 7 de allí es el 7 Plus; el 5.0 y el 6.0, las Ediciones de Oro)
PCFSAVE = {"pcf7plus": "v86_pcf7", "pcf5oro": "v86_pcf5", "pcf6oro": "v86_pcf6", "pcf6arg": "v86_pcf6arg",
           "pcf5arg": "v86_pcf5arg", "pcf4": "v86_pcf4", "pcf2000": "pcf2000", "pcf2001": "pcf2001"}


def de_juegos(j):
    vs = [x for x in j.get("versiones", []) if x.get("jugar") and x["id"] in PCFSAVE]
    if not vs:
        return ""
    x = vs[0]
    cual = ", elige arriba «" + e(x["nombre"]) + "»" if len([y for y in j["versiones"] if y.get("jugar")]) > 1 else ""
    return (" ¿Vienes de juegos.pcfsite.com? Descarga allí tu partida en «Mis partidas» (el fichero " + PCFSAVE[x["id"]]
            + ".pcfsave)" + cual + " y cárgala aquí con «Cargar partidas».")


def ficha(j, v):
    prox = j["estado"] == "proximamente"
    tag = etiqueta(j)
    fondo = f'<div class="ficha-fondo" style="background-image:url(\'{captura(j["id"], j["fondo"])}?v={v}\')" aria-hidden="true"></div>' if j.get("fondo") else ""
    partes = [f"""{fondo}
<div class="ficha-head"><h1>{e(j["nombre"])}</h1>{tag}<span class="meta">Temporada {e(j["temporada"])} · {j["anio"]} · Dinamic Multimedia</span></div>
<p class="ficha-intro">{e(j["resumen"])}</p>"""]
    jugables = [x for x in j.get("versiones", []) if x.get("jugar")]
    if jugables:
        # el juego, incrustado (se carga al pulsar: son 9 MB de programa y 2 GB de memoria); encima, un botón por
        # versión solo si hay más de una (las que aún no están, desactivadas); juego.js cambia de versión
        ini = jugables[0]
        vers = "".join(
            f'<button class="ver{" on" if x is ini else ""}" type="button" data-id="{x["id"]}" data-jugar="{x["jugar"]}" '
            f'data-nombre="{e(x["nombre"])}"{" aria-pressed=\"true\"" if x is ini else ""}>{e(x["nombre"])}</button>' if x.get("jugar") else
            f'<button class="ver" type="button" disabled>{e(x["nombre"])}<small>Próximamente</small></button>'
            for x in j["versiones"])
        partes.append(f"""
<section class="juego" id="jugar">
  {f'<div class="juego-versiones" role="group" aria-label="Versión">{vers}</div>' if len(j["versiones"]) > 1 else ""}
  <div class="juego-marco blueprint" data-juego="{ini["jugar"]}" data-partidas="{ini["jugar"]}partidas.js" data-nombre="{ini["id"]}">{CORNERS}
    <img class="juego-poster" src="{captura(j["id"], j["portada"])}" alt="">
    <button class="btn btn-primary blueprint juego-boton" type="button">{CORNERS}Jugar a <span class="juego-nombre">{e(ini["nombre"])}</span></button>
  </div>
  <div class="juego-pie"><span class="meta">Hace falta la cuenta de pcfsite · Ordenador con Chrome, Edge, Firefox o Safari recientes · 2 GB de memoria libre</span>
    <span class="juego-botones">
      <button class="btn btn-secondary juego-exportar" type="button">Descargar mis partidas</button>
      <button class="btn btn-secondary juego-importar" type="button">Cargar partidas</button>
      <input class="juego-fichero" type="file" accept=".json,.pcfsave,application/json" hidden>
      <button class="btn btn-secondary juego-completa" type="button" hidden>Pantalla completa</button>
    </span></div>
  <p class="juego-partidas meta">Las partidas se guardan en este navegador y, si entras con tu cuenta de pcfsite, también
    en el servidor: así las recuperas desde cualquier ordenador. Puedes descargar una copia cuando quieras.{
    de_juegos(j)}</p>
</section>""")
    if j.get("capturas"):
        figs = "".join(f'<figure><img src="{captura(j["id"], c["id"])}" alt="{e(c["texto"])}" loading="lazy" data-visor>'
                       f'<figcaption>{e(c["texto"])}</figcaption></figure>' for c in j["capturas"])
        partes.append(f'<section class="cat"><div class="cat-head"><h2>Capturas</h2><span class="cnt">reales, de la versión traducida</span></div><div class="galeria">{figs}</div></section>')
    if j.get("funciona") or j.get("pendiente"):
        ok = "".join(f"<li>{e(x)}</li>" for x in j.get("funciona", []))
        pe = "".join(f"<li>{e(x)}</li>" for x in j.get("pendiente", []))
        cajas = f'<div class="lista ok blueprint">{CORNERS}<h2>Funciona</h2><ul>{ok}</ul></div>'
        if pe:
            cajas += f'<div class="lista pend blueprint">{CORNERS}<h2>Pendiente</h2><ul>{pe}</ul></div>'
        partes.append(f'<section class="cat"><div class="cat-head"><h2>Estado</h2></div><div class="{"dos" if pe else "uno"}">{cajas}</div></section>')
    if j.get("plataformas"):
        ps = "".join(f'<span class="p"><b>{e(p["nombre"])}</b>'
                     + ('<span class="tag tag-ok">Funciona</span>' if p["estado"] == "funciona" else '<span class="tag tag-prox">Previsto</span>')
                     + "</span>" for p in j["plataformas"])
        partes.append(f'<section class="cat"><div class="cat-head"><h2>Plataformas</h2></div><div class="plat">{ps}</div></section>')
    if j.get("notas"):
        partes.append('<div class="notas">' + "".join(f"<p>{e(n)}</p>" for n in j["notas"]) + "</div>")
    if j.get("hitos"):
        hs = "".join(f'<li><span class="f">{fecha(h["fecha"])}</span><p>{e(h["texto"])}</p></li>' for h in reversed(j["hitos"]))
        partes.append(f'<section class="cat"><div class="cat-head"><h2>Diario</h2><a class="cnt lnk" href="/diario">Todos los juegos →</a></div><ol class="diario">{hs}</ol></section>')
    if prox:
        partes.append('<p class="aviso-cd">Todavía no hay trabajo publicado de este juego. Sigue el avance en el '
                      '<a href="https://foro.pcfsite.com/">foro</a>.</p>')
    img = f"/pcf/img/og-{j['id']}.jpg" if os.path.exists(os.path.join(SITE, "pcf", "img", f"og-{j['id']}.jpg")) else None
    return pagina("/" + j["id"], f'{j["nombre"]} · source.pcfsite.com', j["resumen"], "".join(partes), img, v)


MESES = ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"]


def fecha(iso):
    a, m, d = (int(x) for x in iso.split("-"))
    return f"{d} de {MESES[m - 1]} de {a}"


def diario(datos, v):
    todos = [(h["fecha"], j, h) for j in datos["juegos"] for h in j.get("hitos", [])]
    todos.sort(key=lambda t: t[0], reverse=True)
    hs = "".join(f'<li><span class="f">{fecha(f)}</span><span class="j">{e(j["nombre"])}</span><p>{e(h["texto"])}</p></li>'
                 for f, j, h in todos)
    cuerpo = f'<div class="texto"><h1>Diario</h1><p>Los avances de cada juego, del más reciente al más antiguo.</p></div><ol class="diario">{hs}</ol>'
    return pagina("/diario", "Diario · source.pcfsite.com", "Los avances de la traducción de cada PC Fútbol.", cuerpo, None, v)


def como_funciona(v):
    with open(os.path.join(SITE, "como-funciona.frag"), encoding="utf-8") as f:
        cuerpo = f.read()
    return pagina("/como-funciona", "Cómo funciona · source.pcfsite.com",
                  "Cómo se traducen los PC Fútbol desde su código original a aplicaciones modernas.", cuerpo, None, v)


def main():
    with open(os.path.join(HERE, "data", "juegos.json"), encoding="utf-8") as f:
        datos = json.load(f)
    v = version()
    if os.path.isdir(DIST):
        shutil.rmtree(DIST)
    shutil.copytree(os.path.join(SITE, "pcf"), os.path.join(DIST, "pcf"))
    shutil.copy(os.path.join(SITE, "favicon.ico"), DIST)
    with open(os.path.join(SITE, "topbar.src.js"), encoding="utf-8") as f:
        tb = f.read().replace("__VERSION__", v)
    with open(os.path.join(DIST, "topbar.js"), "w", encoding="utf-8") as f:
        f.write(tb)

    paginas = {"index.html": portada(datos, v), "diario.html": diario(datos, v), "como-funciona.html": como_funciona(v)}
    for j in datos["juegos"]:
        paginas[j["id"] + ".html"] = ficha(j, v)
    for nombre, texto in paginas.items():
        with open(os.path.join(DIST, nombre), "w", encoding="utf-8") as f:
            f.write(texto)
    with open(os.path.join(DIST, "_version.txt"), "w") as f:
        f.write(v + "\n")
    print(f"source.pcfsite.com v{v}: {len(paginas)} páginas en dist/")


if __name__ == "__main__":
    main()
