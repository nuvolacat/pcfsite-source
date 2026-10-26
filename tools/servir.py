#!/usr/bin/env python3
"""Vista previa local de dist/ con las mismas rutas que nginx (/pcf7 → pcf7.html). Uso: python3 tools/servir.py [puerto]"""
import functools
import http.server
import os
import sys

DIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dist")


class H(http.server.SimpleHTTPRequestHandler):
    def send_head(self):
        p = self.path.split("?", 1)[0]
        if p != "/" and "." not in os.path.basename(p) and os.path.exists(os.path.join(DIST, p.lstrip("/") + ".html")):
            self.path = p + ".html"
        return super().send_head()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(H, directory=DIST)).serve_forever()
