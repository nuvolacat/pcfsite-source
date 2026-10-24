#!/usr/bin/env python3
"""Como tools/web_probar.py pero con el guion nativo (coordenadas 800x600, ops click/key/char/ifpx/shot/wait/reload).
   python3 web_guion.py SALIDA SEGUNDOS CADA "guion"  (URL en PCF7_WEB_URL, desplazamiento en ms en PCF7_WEB_DESP)"""
import os, sys, time, zlib, struct
from playwright.sync_api import sync_playwright

URL = os.environ.get("PCF7_WEB_URL", "http://127.0.0.1:8797/")
DESP = int(os.environ.get("PCF7_WEB_DESP", "10000"))
F = 1.6
VK = {13: "Enter", 27: "Escape", 32: "Space", 37: "ArrowLeft", 38: "ArrowUp", 39: "ArrowRight", 40: "ArrowDown", 9: "Tab", 8: "Backspace", 112: "F1", 113: "F2"}

def pixel(pg, x, y):
    png = pg.screenshot(clip={"x": x * F, "y": y * F, "width": 1, "height": 1})
    pos, idat = 8, b""
    while pos < len(png):
        n, typ = struct.unpack(">I4s", png[pos:pos + 8]); d = png[pos + 8:pos + 8 + n]
        if typ == b"IHDR": ct = d[9]
        if typ == b"IDAT": idat += d
        pos += 12 + n
    raw = zlib.decompress(idat)
    return tuple(raw[1:4])

def main():
    out, total, cada = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    guion = [p.split(":") for p in sys.argv[4].split(";") if p] if len(sys.argv) > 4 else []
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "consola.txt"), "w", encoding="utf-8")
    with sync_playwright() as p:
        b = p.chromium.launch(args=["--enable-features=SharedArrayBuffer", "--autoplay-policy=no-user-gesture-required"])
        pg = b.new_page(viewport={"width": 1280, "height": 960})
        pg.on("console", lambda m: (log.write(m.text + "\n"), log.flush()))
        pg.on("pageerror", lambda e: (log.write("ERROR DE PÁGINA: " + str(e) + "\n"), log.flush()))
        t0 = time.time(); pg.goto(URL)
        prox, i, skip = cada, 0, False
        while time.time() - t0 < total:
            t = time.time() - t0
            while i < len(guion) and (float(guion[i][0]) + DESP) / 1000 <= t:
                g = guion[i]; i += 1
                if skip: skip = False; log.write(f"guion: salto {g}\n"); continue
                op = g[1]
                if op == "click": pg.mouse.click(int(g[2]) * F, int(g[3]) * F)
                elif op == "move": pg.mouse.move(int(g[2]) * F, int(g[3]) * F)
                elif op == "key": pg.keyboard.press(VK.get(int(g[2]), chr(int(g[2]))))
                elif op == "char": pg.keyboard.press(chr(int(g[2])))
                elif op == "shot": pg.screenshot(path=os.path.join(out, f"shot_{g[2]}.png"))
                elif op == "reload": pg.reload(); log.write("guion: recarga de la página\n")   # partidas: lo guardado debe sobrevivir
                elif op == "ifpx":
                    want = int(g[4], 16); px = pixel(pg, int(g[2]), int(g[3]))
                    ok = all(abs(px[k] - ((want >> (16 - 8 * k)) & 255)) <= 24 for k in range(3))
                    log.write(f"guion: ifpx {g[2]} {g[3]} {g[4]} -> {px} {'sí' if ok else 'no'}\n"); log.flush()
                    skip = not ok
            if cada and t >= prox:
                pg.screenshot(path=os.path.join(out, f"c_{int(prox)}.png")); prox += cada
            time.sleep(0.2)
        try: pg.evaluate("() => Module.ccall('pcf7_volcado', null, [], [])")
        except Exception as e: log.write("volcado: %s\n" % e)
        time.sleep(1)
        pg.screenshot(path=os.path.join(out, "c_final.png"))
        b.close()
main()
