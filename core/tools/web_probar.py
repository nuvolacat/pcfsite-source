#!/usr/bin/env python3
"""Prueba automática de la versión web: abre la página en Chromium (Playwright), guarda la consola y hace capturas.

  python3 tools/web_probar.py SALIDA SEGUNDOS [CAPTURA_CADA_S] [GUION]
  GUION: "ms:click:x:y;ms:key:Enter;…" en coordenadas de la pantalla del juego (640×480)
Necesita el servidor (tools/web_servir.py) en marcha. La consola va a SALIDA/consola.txt."""
import os
import sys
import time

from playwright.sync_api import sync_playwright

URL = os.environ.get("PCF7_WEB_URL", "http://127.0.0.1:8793/?log")


def main():
    out, total = sys.argv[1], float(sys.argv[2])
    cada = float(sys.argv[3]) if len(sys.argv) > 3 else 0
    guion = [p.split(":") for p in sys.argv[4].split(";")] if len(sys.argv) > 4 and sys.argv[4] else []
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "consola.txt"), "w", encoding="utf-8")
    with sync_playwright() as p:
        b = p.chromium.launch(args=["--enable-features=SharedArrayBuffer", "--autoplay-policy=no-user-gesture-required"])
        pg = b.new_page(viewport={"width": 1280, "height": 960})
        pg.on("console", lambda m: (log.write(m.text + "\n"), log.flush()))
        pg.on("pageerror", lambda e: (log.write("ERROR DE PÁGINA: " + str(e) + "\n"), log.flush()))
        if os.environ.get("PCF7_WEB_MBPS"):   # conexión lenta simulada (Mbit/s de bajada), como al jugar desde casa
            bps = float(os.environ["PCF7_WEB_MBPS"]) * 1e6 / 8
            cdp = pg.context.new_cdp_session(pg)
            cdp.send("Network.enable")
            cdp.send("Network.emulateNetworkConditions", {"offline": False, "latency": float(os.environ.get("PCF7_WEB_LAT", "30")), "downloadThroughput": bps, "uploadThroughput": bps})
        t0 = time.time()
        pg.goto(URL)
        prox = cada
        i = 0
        while time.time() - t0 < total:
            t = time.time() - t0
            while i < len(guion) and float(guion[i][0]) / 1000 <= t:
                g = guion[i]
                if g[1] == "click":   # la pantalla del juego ocupa el lienzo con bandas: 640×480 → 1280×960
                    pg.mouse.click(int(g[2]) * 2, int(g[3]) * 2)
                elif g[1] == "move":
                    pg.mouse.move(int(g[2]) * 2, int(g[3]) * 2)
                elif g[1] == "key":
                    pg.keyboard.press(g[2])
                i += 1
            if cada and t >= prox:
                pg.screenshot(path=os.path.join(out, f"c_{int(prox)}.png"))
                prox += cada
            time.sleep(0.2)
        pg.evaluate("() => Module.ccall('pcf7_volcado', null, [], [])")   # estado de los hilos al final
        if os.environ.get("DUMPMEM"):
            a, n = os.environ["DUMPMEM"].split(":")
            pg.evaluate(f"() => Module.ccall('pcf7_mem', null, ['number','number'], [0x{a}, 0x{n}])")
        if os.environ.get("VOLCADO2"):
            time.sleep(5)
            pg.evaluate("() => Module.ccall('pcf7_volcado', null, [], [])")
        est = pg.evaluate("""() => { const r = [];
            for (const k of Object.keys(Module)) { const v = Module[k]; if (v && v.audioContext) r.push(k + ': ' + v.audioContext.state); }
            if (globalThis.AudioContext) r.push('AudioContext disponible');
            return r.join(' | ') || 'sin AudioContext en Module'; }""")
        log.write("AUDIO: " + str(est) + "\n")
        time.sleep(1)
        pg.screenshot(path=os.path.join(out, "c_final.png"))
        b.close()


if __name__ == "__main__":
    main()
