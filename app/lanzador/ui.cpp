// lanzador — interfaz mínima (ver ui.h)
#include <algorithm>
#include "ui.h"

#include "util.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../core/platform/third_party/stb_truetype.h"
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#include <cmath>
#include <vector>

namespace ui {
namespace {

struct Fuente {
    SDL_Texture *tex = nullptr;
    std::vector<stbtt_bakedchar> cdata;   // 32..255
    int lado = 0;      // lado del atlas
    float px = 0;      // tamaño en píxeles
    float puntos = 0;  // tamaño en puntos (px / escala)
    float ascent = 0;
};
Fuente g_f[N_ESTILOS];
SDL_Renderer *g_ren = nullptr;
float g_escala = 1;
float g_rx = 0, g_ry = 0;
bool g_pulsado = false, g_clic = false;
bool g_hay_recorte = false;
SDL_FRect g_recorte;

bool carga(Fuente &f, const std::string &ttf, float puntos) {
    if (ttf.empty()) return false;
    f.puntos = puntos;
    f.px = puntos * g_escala;
    f.cdata.assign(224, {});
    int lado = f.px > 40 ? 2048 : 1024;
    std::vector<unsigned char> bitmap;
    for (;; lado *= 2) {
        bitmap.assign((size_t)lado * lado, 0);
        if (stbtt_BakeFontBitmap((const unsigned char *)ttf.data(), 0, f.px, bitmap.data(), lado, lado, 32, 224, f.cdata.data()) > 0) break;
        if (lado >= 4096) return false;
    }
    f.lado = lado;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, (const unsigned char *)ttf.data(), 0)) return false;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    f.ascent = (float)asc * stbtt_ScaleForPixelHeight(&info, f.px);
    std::vector<unsigned char> rgba((size_t)lado * lado * 4);
    for (size_t i = 0; i < (size_t)lado * lado; i++) { rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = 255; rgba[4 * i + 3] = bitmap[i]; }
    f.tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, lado, lado);
    if (!f.tex) return false;
    SDL_UpdateTexture(f.tex, nullptr, rgba.data(), lado * 4);
    SDL_SetTextureBlendMode(f.tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(f.tex, SDL_SCALEMODE_LINEAR);
    return true;
}

// UTF-8 → puntos de código (los que no caben en la fuente, '?')
std::vector<int> cps(const std::string &s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int cp, n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F); n = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F); n = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) { cp = 0xFFFD; n = 4; }
        else { cp = '?'; n = 1; }
        i += (size_t)n;
        // acentos descompuestos (así llegan los nombres de algunos volúmenes de red): se componen con la letra anterior
        if (cp >= 0x300 && cp <= 0x36F && !out.empty()) {
            static const struct { int marca; const char *bases, *compuestas; } T[] = {
                {0x301, "aeiouAEIOUy", "\xe1\xe9\xed\xf3\xfa\xc1\xc9\xcd\xd3\xda\xfd"}, {0x300, "aeiouAEIOU", "\xe0\xe8\xec\xf2\xf9\xc0\xc8\xcc\xd2\xd9"},
                {0x308, "aeiouAEIOUy", "\xe4\xeb\xef\xf6\xfc\xc4\xcb\xcf\xd6\xdc\xff"}, {0x303, "anoANO", "\xe3\xf1\xf5\xc3\xd1\xd5"},
                {0x302, "aeiouAEIOU", "\xe2\xea\xee\xf4\xfb\xc2\xca\xce\xd4\xdb"}, {0x327, "cC", "\xe7\xc7"}};
            int &b = out.back();
            for (const auto &t : T)
                if (t.marca == cp)
                    for (const char *q = t.bases; *q; q++)
                        if (*q == b) { b = (unsigned char)t.compuestas[q - t.bases]; break; }
            continue;
        }
        if (cp == 0x2013 || cp == 0x2014) cp = '-';
        else if (cp == 0x2018 || cp == 0x2019) cp = '\'';
        else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2026) { out.push_back('.'); out.push_back('.'); cp = '.'; }
        if (cp < 32 || cp > 255) cp = '?';
        out.push_back(cp);
    }
    return out;
}

float dibuja(const Fuente &f, float x, float y, const std::vector<int> &c, SDL_Color col, bool pintar, float espaciado) {
    float px = 0, py = 0;
    if (pintar) SDL_SetTextureColorMod(f.tex, col.r, col.g, col.b);
    if (pintar) SDL_SetTextureAlphaMod(f.tex, col.a);
    for (int cp : c) {
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(f.cdata.data(), f.lado, f.lado, cp - 32, &px, &py, &q, 1);
        if (pintar) {
            SDL_FRect src{q.s0 * f.lado, q.t0 * f.lado, (q.s1 - q.s0) * f.lado, (q.t1 - q.t0) * f.lado};
            SDL_FRect dst{x + q.x0 / g_escala, y + (f.ascent + q.y0) / g_escala, (q.x1 - q.x0) / g_escala, (q.y1 - q.y0) / g_escala};
            SDL_RenderTexture(g_ren, f.tex, &src, &dst);
        }
        px += espaciado * g_escala;
    }
    return px / g_escala;
}

void color(SDL_Color c) { SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a); }

void circulo(float cx, float cy, float r, SDL_Color c, int n = 28) {
    std::vector<SDL_FPoint> p;
    for (int i = 0; i <= n; i++) { float a = (float)i / n * 6.2831853f; p.push_back({cx + r * cosf(a), cy + r * sinf(a)}); }
    color(c);
    SDL_RenderLines(g_ren, p.data(), (int)p.size());
}

}  // namespace

bool init(SDL_Renderer *ren, float escala, const std::string &dir) {
    g_ren = ren;
    g_escala = escala;
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    std::string regular = util::leer(dir + "/Barlow-Regular.ttf"), medio = util::leer(dir + "/Barlow-Medium.ttf"),
                cond = util::leer(dir + "/BarlowCondensed-SemiBold.ttf");
    if (regular.empty() || cond.empty()) return false;
    if (medio.empty()) medio = regular;
    return carga(g_f[CUERPO], regular, 14) && carga(g_f[CUERPO_13], regular, 13) && carga(g_f[MEDIO], medio, 14) &&
           carga(g_f[PEQUENO], regular, 11) && carga(g_f[ETIQUETA], regular, 12) && carga(g_f[TITULO], cond, 26) &&
           carga(g_f[DIALOGO], cond, 20) && carga(g_f[TARJETA], cond, 17) && carga(g_f[PORTADA], cond, 44) &&
           carga(g_f[PORTADA_SUB], cond, 18) && carga(g_f[BOTON], cond, 14) && carga(g_f[BARRA], cond, 14) && carga(g_f[ANADIR], cond, 15);
}

void fotograma(float x, float y, bool pulsado, bool clic) { g_rx = x; g_ry = y; g_pulsado = pulsado; g_clic = clic; }

bool sobre(const SDL_FRect &r) {
    if (g_hay_recorte && !(g_rx >= g_recorte.x && g_rx < g_recorte.x + g_recorte.w && g_ry >= g_recorte.y && g_ry < g_recorte.y + g_recorte.h)) return false;
    return g_rx >= r.x && g_rx < r.x + r.w && g_ry >= r.y && g_ry < r.y + r.h;
}
bool pulsado_en(const SDL_FRect &r) { return g_clic && sobre(r); }

void texto(float x, float y, const std::string &s, Estilo e, SDL_Color c, float esp) { dibuja(g_f[e], x, y, cps(s), c, true, esp); }
float ancho(const std::string &s, Estilo e, float esp) { return dibuja(g_f[e], 0, 0, cps(s), {}, false, esp); }
float alto(Estilo e) { return g_f[e].puntos * (e == TITULO || e == PORTADA || e == TARJETA || e == DIALOGO ? 1.15f : 1.5f); }

float parrafo(float x, float y, float w, const std::string &s, Estilo e, SDL_Color c, bool pintar) {
    float h = alto(e), yy = y;
    size_t i = 0;
    while (i <= s.size()) {
        size_t fin = s.find('\n', i);
        if (fin == std::string::npos) fin = s.size();
        std::string l = s.substr(i, fin - i);
        i = fin + 1;
        std::string linea;
        size_t j = 0;
        while (j < l.size()) {
            size_t k = l.find(' ', j);
            if (k == std::string::npos) k = l.size();
            std::string pal = l.substr(j, k - j);
            j = k + 1;
            // una «palabra» más ancha que el párrafo (una ruta) se parte en las barras, o donde haga falta
            while (ancho(pal, e) > w) {
                if (!linea.empty()) { if (pintar) texto(x, yy, linea, e, c); yy += h; linea.clear(); }
                size_t corte = 0, u = 0;
                for (size_t q = 1; q < pal.size(); q++) {
                    if (((unsigned char)pal[q] & 0xC0) == 0x80) continue;   // no partir un carácter UTF-8
                    if (ancho(pal.substr(0, q), e) > w) break;
                    u = q;
                    if (pal[q - 1] == '/' || pal[q - 1] == '\\') corte = q;
                }
                if (!corte) corte = u ? u : 1;
                if (pintar) texto(x, yy, pal.substr(0, corte), e, c);
                yy += h;
                pal.erase(0, corte);
            }
            std::string prueba = linea.empty() ? pal : linea + " " + pal;
            if (!linea.empty() && ancho(prueba, e) > w) { if (pintar) texto(x, yy, linea, e, c); yy += h; linea = pal; }
            else linea = prueba;
        }
        if (pintar) texto(x, yy, linea, e, c);
        yy += h;
    }
    return yy - y;
}

std::string recorta(const std::string &s, float w, Estilo e) {
    if (ancho(s, e) <= w) return s;
    size_t i = 0;
    while (i < s.size()) {
        i++;
        while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;   // no partir un carácter UTF-8
        std::string p = "…" + s.substr(i);
        if (ancho(p, e) <= w) return p;
    }
    return "…";
}

std::string mayusculas(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') o += (char)(c - 32);
        else if (c == 0xC3 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0xA0 && (unsigned char)s[i + 1] <= 0xBE && (unsigned char)s[i + 1] != 0xB7) {
            o += (char)0xC3; o += (char)((unsigned char)s[i + 1] - 0x20); i++;
        } else o += (char)c;
    }
    return o;
}

void rect(const SDL_FRect &r, SDL_Color c) { color(c); SDL_RenderFillRect(g_ren, &r); }
void borde(const SDL_FRect &r, SDL_Color c) { color(c); SDL_RenderRect(g_ren, &r); }
void linea(float x1, float y1, float x2, float y2, SDL_Color c) { color(c); SDL_RenderLine(g_ren, x1, y1, x2, y2); }

void borde_discontinuo(const SDL_FRect &r, SDL_Color c) {
    color(c);
    const float on = 6, off = 4;
    auto lado = [&](float x1, float y1, float x2, float y2) {
        float L = std::hypot(x2 - x1, y2 - y1), dx = (x2 - x1) / L, dy = (y2 - y1) / L;
        for (float t = 0; t < L; t += on + off) {
            float e = std::min(L, t + on);
            SDL_RenderLine(g_ren, x1 + dx * t, y1 + dy * t, x1 + dx * e, y1 + dy * e);
        }
    };
    lado(r.x, r.y, r.x + r.w, r.y);
    lado(r.x + r.w, r.y, r.x + r.w, r.y + r.h);
    lado(r.x + r.w, r.y + r.h, r.x, r.y + r.h);
    lado(r.x, r.y + r.h, r.x, r.y);
}

void marcas(const SDL_FRect &r) {
    SDL_Color c = alfa(TEXTO, 140);   // texto al 55 %
    const float b = 5;                // brazo de la cruz (11 px en total)
    float xs[2] = {r.x, r.x + r.w}, ys[2] = {r.y, r.y + r.h};
    for (float x : xs)
        for (float y : ys) { linea(x - b, y, x + b, y, c); linea(x, y - b, x, y + b, c); }
}

void plano(const SDL_FRect &r) { borde(r); marcas(r); }

void sombra(const SDL_FRect &r) {
    for (int i = 6; i >= 1; i--) {
        float d = (float)i * 3;
        rect({r.x - d, r.y - d + 8, r.w + 2 * d, r.h + 2 * d}, alfa(NEUTRO_900, 9));
    }
}

void icono(Icono i, float x, float y, float t, SDL_Color c) {
    float cx = x + t / 2, cy = y + t / 2;
    switch (i) {
    case ENGRANAJE: {
        circulo(cx, cy, t * 0.33f, c);
        circulo(cx, cy, t * 0.12f, c, 12);
        for (int k = 0; k < 8; k++) {
            float a = (float)k / 8 * 6.2831853f, r1 = t * 0.33f, r2 = t * 0.47f;
            linea(cx + r1 * cosf(a), cy + r1 * sinf(a), cx + r2 * cosf(a), cy + r2 * sinf(a), c);
        }
        break;
    }
    case MAS:
        linea(cx, y + t * 0.2f, cx, y + t * 0.8f, c);
        linea(x + t * 0.2f, cy, x + t * 0.8f, cy, c);
        break;
    case INFO:
        circulo(cx, cy, t * 0.42f, c);
        linea(cx, cy - t * 0.05f, cx, cy + t * 0.24f, c);
        rect({cx - 0.75f, cy - t * 0.26f, 1.5f, 1.5f}, c);
        break;
    case CARPETA: {
        color(c);
        SDL_FPoint p[] = {{x + t * 0.1f, y + t * 0.25f}, {x + t * 0.4f, y + t * 0.25f}, {x + t * 0.5f, y + t * 0.38f}, {x + t * 0.9f, y + t * 0.38f},
                          {x + t * 0.9f, y + t * 0.8f}, {x + t * 0.1f, y + t * 0.8f}, {x + t * 0.1f, y + t * 0.25f}};
        SDL_RenderLines(g_ren, p, 7);
        break;
    }
    case VISTO: {   // una marca de verificación
        color(c);
        SDL_FPoint p[] = {{x + t * 0.2f, y + t * 0.52f}, {x + t * 0.42f, y + t * 0.74f}, {x + t * 0.82f, y + t * 0.28f}};
        SDL_RenderLines(g_ren, p, 3);
        SDL_FPoint q[] = {{x + t * 0.2f, y + t * 0.52f + 1}, {x + t * 0.42f, y + t * 0.74f + 1}, {x + t * 0.82f, y + t * 0.28f + 1}};
        SDL_RenderLines(g_ren, q, 3);
        break;
    }
    case REJILLA: {
        float s = t * 0.3f, g = t * 0.12f, x0 = x + (t - 2 * s - g) / 2, y0 = y + (t - 2 * s - g) / 2;
        for (int k = 0; k < 4; k++) borde({x0 + (k % 2) * (s + g), y0 + (k / 2) * (s + g), s, s}, c);
        break;
    }
    case LISTA:
        for (int k = 0; k < 3; k++) {
            float yy = y + t * (0.25f + 0.25f * k);
            rect({x + t * 0.15f, yy - 1, 2, 2}, c);
            linea(x + t * 0.35f, yy, x + t * 0.85f, yy, c);
        }
        break;
    default: break;
    }
}

int segmentos(float x, float y, const std::vector<Icono> &opciones, int elegida) {
    const float w = 36, h = 30;
    int pulsado = -1;
    SDL_FRect todo{x, y, w * opciones.size(), h};
    for (size_t k = 0; k < opciones.size(); k++) {
        SDL_FRect r{x + w * k, y, w, h};
        bool act = (int)k == elegida;
        if (act) rect(r, ACENTO);
        else if (sobre(r)) rect(r, alfa(TEXTO, 18));
        if (k) linea(r.x, r.y, r.x, r.y + h, DIVISOR);
        icono(opciones[k], r.x + (w - 16) / 2, r.y + (h - 16) / 2, 16, act ? FONDO : TEXTO);
        if (pulsado_en(r)) pulsado = (int)k;
    }
    borde(todo);
    return pulsado;
}

float ancho_boton(const std::string &etiqueta, Icono ic) { return ancho(etiqueta, BOTON) + 26 + (ic != SIN_ICONO ? 22 : 0); }

bool boton(SDL_FRect r, const std::string &etiqueta, TipoBoton tipo, bool activo, Icono ic) {
    bool enc = activo && sobre(r);
    bool apretado = enc && g_pulsado;
    SDL_Color col = TEXTO;
    if (tipo == PRIMARIO) {
        rect(r, apretado ? ACENTO_700 : enc ? ACENTO_600 : ACENTO);
        marcas(r);
        col = FONDO;
    } else if (tipo == SECUNDARIO) {
        if (enc) rect(r, alfa(TEXTO, apretado ? 36 : 18));
        borde(r);
    } else {
        if (enc) rect(r, alfa(ACENTO, apretado ? 46 : 26));
        col = ACENTO_700;
    }
    if (!activo) col = alfa(col, 115);
    float w = ancho(etiqueta, BOTON) + (ic != SIN_ICONO ? 22 : 0), h = alto(BOTON);
    float x = r.x + (r.w - w) / 2;
    if (ic != SIN_ICONO) { icono(ic, x, r.y + (r.h - 16) / 2, 16, col); x += 22; }
    texto(x, r.y + (r.h - h) / 2 + 1, etiqueta, BOTON, col);
    return enc && g_clic;
}

void barra(const SDL_FRect &r, float f) {
    rect(r, SUPERFICIE);
    borde(r);
    f = std::min(1.f, std::max(0.f, f));
    rect({r.x + 2, r.y + 2, (r.w - 4) * f, r.h - 4}, ACENTO);
}

bool casilla(float x, float y, float w, const std::string &etiqueta, bool &valor, bool activo) {
    SDL_FRect r{x, y, w, 24}, caja{x, y + 4, 16, 16};
    bool enc = activo && sobre(r);
    rect(caja, valor ? (enc ? ACENTO_600 : ACENTO) : (enc ? ACENTO_100 : SUPERFICIE));
    borde(caja, valor ? ACENTO_700 : NEUTRO_600);
    if (valor) icono(VISTO, caja.x, caja.y, 16, {255, 255, 255, 255});
    texto(x + 24, y + (24 - alto(CUERPO_13)) / 2, recorta(etiqueta, w - 24, CUERPO_13), CUERPO_13, activo ? TEXTO : NEUTRO_600);
    if (enc && g_clic) { valor = !valor; return true; }
    return false;
}

void campo(const SDL_FRect &r, const std::string &valor, Estilo e) {
    rect(r, SUPERFICIE);
    borde(r, sobre(r) ? alfa(TEXTO, 115) : DIVISOR);
    texto(r.x + 10, r.y + (r.h - alto(e)) / 2 + 1, recorta(valor, r.w - 20, e), e, TEXTO);
}

SDL_Texture *cargar_png(const std::string &path, int &w, int &h) {
    std::string datos = util::leer(path);
    if (datos.empty()) return nullptr;
    int n = 0;
    unsigned char *px = stbi_load_from_memory((const unsigned char *)datos.data(), (int)datos.size(), &w, &h, &n, 4);
    if (!px) return nullptr;
    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(px);
    return t;
}

void imagen(SDL_Texture *t, const SDL_FRect &r) { if (t) SDL_RenderTexture(g_ren, t, nullptr, &r); }

void imagen_cubrir(SDL_Texture *t, int tw, int th, const SDL_FRect &r) {
    if (!t || tw <= 0 || th <= 0) return;
    float e = std::max(r.w / tw, r.h / th);   // escala para cubrir
    SDL_FRect src{0, 0, r.w / e, r.h / e};
    src.x = (tw - src.w) / 2;
    src.y = (th - src.h) * 0.15f;   // de la portada se conserva más la parte de arriba (el título)
    SDL_RenderTexture(g_ren, t, &src, &r);
}

void etiqueta(float x, float y, const std::string &s) {
    std::string t = mayusculas(s);
    float w = ancho(t, BOTON, 0.5f) + 16, h = 22;
    rect({x, y, w, h}, ACENTO);
    texto(x + 8, y + (h - alto(BOTON)) / 2 + 1, t, BOTON, FONDO, 0.5f);
}

void recorte(const SDL_FRect &r) {
    g_hay_recorte = true;
    g_recorte = r;
    SDL_Rect c{(int)std::floor(r.x), (int)std::floor(r.y), (int)std::ceil(r.w), (int)std::ceil(r.h)};   // en coordenadas ya escaladas
    SDL_SetRenderClipRect(g_ren, &c);
}
void fin_recorte() { g_hay_recorte = false; SDL_SetRenderClipRect(g_ren, nullptr); }

}  // namespace ui
