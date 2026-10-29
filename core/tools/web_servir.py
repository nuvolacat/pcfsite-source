#!/usr/bin/env python3
"""Servidor local para probar la versión web (build/web-site): cabeceras COOP/COEP (los hilos de WebAssembly
necesitan SharedArrayBuffer), peticiones Range (los datos se piden por trozos) y MIME de .wasm.
Uso: python3 tools/web_servir.py [puerto]   (por defecto 8793)"""
import http.server
import io
import json
import os
import time
import re
import sys

_ID = os.environ.get("PCF_JUEGO", "pcf7")   # el juego (juegos/<id>); pcf7 en build/web-site, los demás en build/<id>/
SITE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", *([] if _ID == "pcf7" else [_ID]), "web-site")
# la página con el juego, el título y el programa principal de juegos/<id>/juego.json, como la deja deploy.sh
_CFG = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "juegos", _ID, "juego.json")))
_PRINCIPAL = _CFG.get("principal", "manager.exe").rsplit(".", 1)[0].lower()


class H(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "application/javascript", ".mpg": "video/mpeg"}

    def __init__(self, *a, **k):
        super().__init__(*a, directory=SITE, **k)

    def log_message(self, fmt, *args):
        if os.environ.get("VERBOSE"):
            super().log_message(fmt, *args)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def translate_path(self, path):
        return super().translate_path(re.sub(r"/{2,}", "/", path))   # WasmFS pide «datos//game/…»

    def send_head(self):
        if self.path.split("?")[0] in ("/", "/index.html"):
            html = open(os.path.join(SITE, "index.html"), encoding="utf-8").read()
            html = html.replace('data-juego="pcf7">', f'data-juego="{_ID}" data-principal="{_PRINCIPAL}">', 1)
            html = html.replace("<title>PC Fútbol 7</title>", f"<title>{_CFG['nombre']}</title>", 1).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            return io.BytesIO(html)
        # PCF7_WEB_RETARDO=ms: los datos del juego (/datos/) tardan eso en empezar a llegar, como desde R2
        if "/datos/" in self.path and os.environ.get("PCF7_WEB_RETARDO"):
            time.sleep(float(os.environ["PCF7_WEB_RETARDO"]) / 1000)
        rng = self.headers.get("Range")
        path = self.translate_path(self.path)
        if not rng or not os.path.isfile(path):
            return super().send_head()
        m = re.match(r"bytes=(\d+)-(\d*)", rng)
        size = os.path.getsize(path)
        if not m:
            return super().send_head()
        a = int(m.group(1))
        b = int(m.group(2)) if m.group(2) else size - 1
        b = min(b, size - 1)
        if a >= size:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return None
        f = open(path, "rb")
        f.seek(a)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {a}-{b}/{size}")
        self.send_header("Content-Length", str(b - a + 1))
        self.end_headers()
        self._rest = b - a + 1
        return f

    def copyfile(self, source, outputfile):
        n = getattr(self, "_rest", None)
        if n is None:
            return super().copyfile(source, outputfile)
        while n > 0:
            buf = source.read(min(n, 1 << 16))
            if not buf:
                break
            outputfile.write(buf)
            n -= len(buf)
        self._rest = None


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8793
    print(f"http://127.0.0.1:{port}/")
    http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
