// PCF7 — GDI32 sobre la pantalla emulada de 8 bits.
//
// Todas las superficies (la pantalla y los bitmaps de memoria) son de índices de 8 bits sobre la paleta
// del sistema: el juego trabaja con DIB de 256 colores y una paleta lógica que realiza con
// RealizePalette. Se asume paleta de identidad (índice lógico i → índice de sistema i), que es lo que
// hacen los juegos de la época con SYSPAL_NOSTATIC.
#include "display.h"
#include "handles.h"
#include "text.h"
#include "win.h"

#include <algorithm>
#include <cmath>
#include <strings.h>
#include <map>
#include <unordered_map>

Display &display() { static Display d; return d; }

namespace {

enum class GKind { DC, Bitmap, Palette, Brush, Pen, Font, Region };

struct GObj {
    GKind kind;
    // Bitmap: en memoria del host (bytes por píxel = los de la pantalla al crearlo; los monocromo, un byte
    // 0/1 por píxel) o, si es una sección DIB, en MEM (el programa escribe los píxeles directamente)
    int w = 0, h = 0, bypp = 1, bits = 0;   // bits: 1 = monocromo; 0 = 8 * bypp
    std::vector<u8> px;
    u32 mem = 0;                            // sección DIB: dirección de los píxeles
    int mem_stride = 0;                     // bytes por fila (negativo si la DIB va de abajo arriba)
    std::vector<u32> ctab;                  // sección DIB de 8 bits: tabla de colores (0x00RRGGBB)
    // Palette
    std::vector<u32> pal;     // 0x00RRGGBB
    // Brush / Pen
    u32 color = 0;            // COLORREF (0x00BBGGRR) o PALETTEINDEX (0x01000000 | i)
    u32 style = 0;            // BS_NULL / PS_NULL…
    int width = 1;
    // Font: LOGFONTA tal cual (las de serie: índice de GetStockObject)
    std::vector<u8> logfont;
    int stock_font = -1;
    // Region (solo rectángulos)
    Rect rgn;
    bool stock = false;
};

struct DC {
    u32 hwnd = 0;
    bool screen = false;      // DC de ventana (dibuja en la pantalla) o de memoria (dibuja en su bitmap)
    Rect area;                // DC de ventana: rectángulo de la ventana en pantalla
    u32 bitmap = 0, palette = 0, brush = 0, pen = 0, font = 0;
    u32 text_color = 0, bk_color = 0xFFFFFF, bk_mode = 2, text_align = 0, rop2 = 13, stretch_mode = 1;
    i32 cur_x = 0, cur_y = 0;
    i32 org_x = 0, org_y = 0;             // SetViewportOrg
    bool has_clip = false;
    Rect clip;                            // en coordenadas del DC
    u32 map_mode = 1;
    bool masked = false;                  // DC de ventana: solo pinta donde se ve la ventana
    std::vector<u32> owners;              // la ventana (y sus hijas si no las recorta)
};

std::map<u32, GObj> g_objs;
std::map<u32, DC> g_dcs;
u32 g_next_obj = 0x40000, g_next_dc = 0x60000;
u32 g_stock[18];
u32 g_sysbrush[32];
int g_bpp = 8;
std::unordered_map<u32, u8> g_nearest;

u32 new_obj(GObj o) { u32 h = g_next_obj; g_next_obj += 4; g_objs[h] = std::move(o); return h; }
GObj *obj(u32 h) { auto it = g_objs.find(h); return it == g_objs.end() ? nullptr : &it->second; }
DC *dc(u32 h) { auto it = g_dcs.find(h); return it == g_dcs.end() ? nullptr : &it->second; }

void init_stock() {
    static bool done = false;
    if (done) return;
    done = true;
    auto brush = [](u32 c, u32 style) { GObj o{GKind::Brush}; o.color = c; o.style = style; o.stock = true; return new_obj(o); };
    auto pen = [](u32 c, u32 style) { GObj o{GKind::Pen}; o.color = c; o.style = style; o.stock = true; return new_obj(o); };
    g_stock[0] = brush(0xFFFFFF, 0); g_stock[1] = brush(0xC0C0C0, 0); g_stock[2] = brush(0x808080, 0);
    g_stock[3] = brush(0x404040, 0); g_stock[4] = brush(0x000000, 0); g_stock[5] = brush(0, 1);   // NULL_BRUSH
    g_stock[6] = pen(0xFFFFFF, 0); g_stock[7] = pen(0, 0); g_stock[8] = pen(0, 5);               // NULL_PEN
    for (int i : {10, 11, 12, 13, 14, 16, 17}) { GObj f{GKind::Font}; f.logfont.assign(60, 0); f.stock = true; f.stock_font = i; g_stock[i] = new_obj(f); }
    GObj p{GKind::Palette};
    p.stock = true;
    for (int i = 0; i < 20; i++) p.pal.push_back(0);
    g_stock[15] = new_obj(p);
    for (int i = 0; i < 32; i++) { u32 c = sys_color(i); g_sysbrush[i] = brush(c, 0); }
}

u32 colorref_to_rgb(u32 c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

// color de GDI → valor de píxel (índice de paleta en 8 bits, 565 en 16, XRGB en 32)
u32 pixel_of(int bypp, u32 c) {
    if ((c >> 24) == 1) {                                                        // PALETTEINDEX
        if (bypp == 1) return c & 0xFF;
        c = display().screen.pal[c & 0xFF];
        return bypp == 2 ? ((c >> 8) & 0xF800) | ((c >> 5) & 0x7E0) | ((c >> 3) & 0x1F) : c;
    }
    u32 rgb = colorref_to_rgb(c & 0xFFFFFF);
    if (bypp == 1) return nearest_index(rgb);
    if (bypp == 2) return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x7E0) | ((rgb >> 3) & 0x1F);
    return rgb;
}


// superficie de un DC: píxeles, tamaño, bytes por píxel y origen en esa superficie
struct Surf {
    u8 *px; int w, h, bypp, stride, ox, oy; Rect clip;
    int bits = 0;                 // 1 = monocromo (un byte 0/1 por píxel)
    const u32 *ctab = nullptr;    // 8 bits con tabla propia (sección DIB); nullptr = paleta del sistema
    // pantalla: mapa de qué ventana se ve en cada píxel y las que puede pintar este DC (nullptr = todas)
    const u32 *owner = nullptr;
    int ow = 0;
    const std::vector<u32> *allowed = nullptr;
    mutable u32 last_ok = 0, last_bad = 0xFFFFFFFFu;
    u8 *row(i32 y) const { return px + (ptrdiff_t)y * stride; }
    bool vis(i32 x, i32 y) const {
        if (!owner) return true;
        u32 o = owner[(size_t)y * ow + x];
        if (o == last_ok) return true;
        if (o == last_bad) return false;
        for (u32 a : *allowed) if (a == o) { last_ok = o; return true; }
        last_bad = o;
        return false;
    }
};
bool surf_of(DC *d, Surf &s) {
    if (d->screen) {
        Screen &sc = display().screen;
        if (!sc.fb) return false;
        s = {MEM + sc.fb, sc.w, sc.h, sc.bpp / 8, sc.pitch, d->area.l + d->org_x, d->area.t + d->org_y, d->area};
        if (d->masked) {
            int mw, mh;
            const u32 *m = wm_owner_map(mw, mh);
            if (m && mw == sc.w && mh == sc.h) { s.owner = m; s.ow = mw; s.allowed = &d->owners; }
        }
    } else {
        GObj *b = obj(d->bitmap);
        if (!b || b->kind != GKind::Bitmap) return false;
        if (b->mem) {
            u8 *base = MEM + b->mem;
            if (b->mem_stride < 0) base += (size_t)(-b->mem_stride) * (size_t)(b->h - 1);
            s = {base, b->w, b->h, b->bypp, b->mem_stride, d->org_x, d->org_y, {0, 0, b->w, b->h}};
        } else {
            if (b->px.empty()) return false;
            s = {b->px.data(), b->w, b->h, b->bypp, b->w * b->bypp, d->org_x, d->org_y, {0, 0, b->w, b->h}};
        }
        s.bits = b->bits;
        if (!b->ctab.empty()) s.ctab = b->ctab.data();
    }
    if (d->has_clip) {
        Rect c{d->clip.l + s.ox, d->clip.t + s.oy, d->clip.r + s.ox, d->clip.b + s.oy};
        s.clip = {std::max(s.clip.l, c.l), std::max(s.clip.t, c.t), std::min(s.clip.r, c.r), std::min(s.clip.b, c.b)};
    }
    s.clip = {std::max(s.clip.l, 0), std::max(s.clip.t, 0), std::min(s.clip.r, s.w), std::min(s.clip.b, s.h)};
    return true;
}

inline void store(u8 *p, int bypp, u32 v) {
    if (bypp == 1) *p = (u8)v;
    else if (bypp == 2) { u16 w = (u16)v; std::memcpy(p, &w, 2); }
    else if (bypp == 3) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); }
    else std::memcpy(p, &v, 4);
}
inline u32 load(const u8 *p, int bypp) {
    if (bypp == 1) return *p;
    if (bypp == 2) return (u32)p[0] | (u32)p[1] << 8;
    if (bypp == 3) return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16;
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

// índice del color más cercano de una tabla
u8 nearest_in(const u32 *tab, int n, u32 rgb) {
    int best = 0, bd = 1 << 30;
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    for (int i = 0; i < n; i++) {
        int dr = r - (int)((tab[i] >> 16) & 0xFF), dg = g - (int)((tab[i] >> 8) & 0xFF), db = b - (int)(tab[i] & 0xFF);
        int dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (dd < bd) { bd = dd; best = i; if (!dd) break; }
    }
    return (u8)best;
}

// valor de píxel en el formato de una superficie ↔ 0x00RRGGBB
u32 raw_to_rgb(const Surf &s, u32 v) {
    if (s.bits == 1) return v ? 0xFFFFFF : 0;
    switch (s.bypp) {
    case 1: return s.ctab ? s.ctab[v & 0xFF] : display().screen.pal[v & 0xFF];
    case 2: { u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31; return (r << 19 | (r >> 2) << 16) | (g << 10 | (g >> 4) << 8) | (b << 3 | b >> 2); }
    default: return v & 0xFFFFFF;
    }
}
u32 rgb_to_raw(const Surf &s, u32 rgb) {
    if (s.bits == 1) {   // monocromo: blanco o negro según la luminosidad
        u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        return r * 30 + g * 59 + b * 11 >= 128 * 100 ? 1 : 0;
    }
    switch (s.bypp) {
    case 1: return s.ctab ? nearest_in(s.ctab, 256, rgb) : nearest_index(rgb);
    case 2: return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x7E0) | ((rgb >> 3) & 0x1F);
    default: return rgb;
    }
}

u32 raw_of(const Surf &s, u32 c) {
    if ((c >> 24) == 1) {   // PALETTEINDEX
        if (s.bypp == 1 && !s.ctab && s.bits != 1) return c & 0xFF;
        return rgb_to_raw(s, display().screen.pal[c & 0xFF]);
    }
    return rgb_to_raw(s, colorref_to_rgb(c & 0xFFFFFF));
}

void fill(Surf &s, Rect r, u32 v) {
    r = {std::max(r.l + s.ox, s.clip.l), std::max(r.t + s.oy, s.clip.t), std::min(r.r + s.ox, s.clip.r), std::min(r.b + s.oy, s.clip.b)};
    for (i32 y = r.t; y < r.b; y++) {
        u8 *row = s.row(y);
        if (s.owner) { for (i32 x = r.l; x < r.r; x++) if (s.vis(x, y)) store(row + (size_t)x * s.bypp, s.bypp, v); continue; }
        if (s.bypp == 1) { std::memset(row + r.l, (int)v, (size_t)std::max(0, r.r - r.l)); continue; }
        for (i32 x = r.l; x < r.r; x++) store(row + (size_t)x * s.bypp, s.bypp, v);
    }
}

struct Lock {   // la pantalla la lee el hilo del host al presentar
    DC *d;
    explicit Lock(DC *dd) : d(dd) { if (d && d->screen) display().mx.lock(); }
    ~Lock() { if (d && d->screen) { display().screen.frame++; display().mx.unlock(); } }
};

void put_pixel(Surf &s, i32 x, i32 y, u32 v) {
    x += s.ox; y += s.oy;
    if (x >= s.clip.l && x < s.clip.r && y >= s.clip.t && y < s.clip.b && s.vis(x, y)) store(s.row(y) + (size_t)x * s.bypp, s.bypp, v);
}

void line(Surf &s, i32 x0, i32 y0, i32 x1, i32 y1, u32 v) {
    i32 dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if (x0 == x1 && y0 == y1) break;   // como GDI: el último punto no se pinta
        put_pixel(s, x0, y0, v);
        i32 e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

}  // namespace

// ---------------------------------------------------------------- paleta del sistema

u8 nearest_index(u32 rgb) {
    auto it = g_nearest.find(rgb);
    if (it != g_nearest.end()) return it->second;
    const u32 *pal = display().screen.pal;
    int best = 0, bd = 1 << 30;
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    for (int i = 0; i < 256; i++) {
        int dr = r - (int)((pal[i] >> 16) & 0xFF), dg = g - (int)((pal[i] >> 8) & 0xFF), db = b - (int)(pal[i] & 0xFF);
        int dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (dd < bd) { bd = dd; best = i; if (!dd) break; }
    }
    g_nearest[rgb] = (u8)best;
    return (u8)best;
}
void palette_changed() { g_nearest.clear(); display().screen.frame++; }

u32 rgb_to_pixel(u32 rgb) { return pixel_of(display().screen.bpp / 8, colorref_to_rgb(rgb)); }

// El framebuffer vive en MEM (lo comparten GDI y la primaria de DirectDraw). Se llama con mx sin tomar.
void set_display_mode(int w, int h, int bpp) {
    if (bpp != 8 && bpp != 16 && bpp != 32) { plog("modo de %d bpp no soportado: se usa 16", bpp); bpp = 16; }
    u32 pitch = (u32)(w * (bpp / 8) + 3) & ~3u;
    u32 fb = heap_alloc(pitch * (u32)h, true);
    std::lock_guard<std::mutex> lk(display().mx);
    Screen &s = display().screen;
    s.w = w; s.h = h; s.bpp = bpp; s.pitch = (int)pitch; s.fb = fb;
    g_bpp = bpp;
    s.frame++;
    plog("modo de vídeo %dx%dx%d (framebuffer en 0x%x)", w, h, bpp, fb);
}
void set_visible_fb(u32 fb) { display().screen.fb = fb; display().screen.frame++; }

// ---------------------------------------------------------------- para user32

u32 gdi_window_dc(u32 hwnd, bool client) {
    init_stock();
    DC d;
    d.hwnd = hwnd;
    d.screen = true;
    if (!wnd_screen_rect(hwnd, client, d.area)) d.area = {0, 0, display().screen.w, display().screen.h};
    else if (hwnd) { d.masked = true; wm_dc_owners(hwnd, d.owners); }
    d.brush = g_stock[0]; d.pen = g_stock[7]; d.font = g_stock[13]; d.palette = g_stock[15];
    u32 h = g_next_dc;
    g_next_dc += 4;
    g_dcs[h] = d;
    return h;
}
void gdi_release_dc(u32 hdc) { if (DC *d = dc(hdc); d && d->screen) g_dcs.erase(hdc); }
void gdi_fill_color(u32 hdc, const Rect &r, u32 colorref) {
    DC *d = dc(hdc);
    if (!d) return;
    Lock lk(d);
    Surf s;
    if (surf_of(d, s)) fill(s, r, raw_of(s, colorref));
}
void gdi_clip_to(u32 hdc, const Rect &r) {
    DC *d = dc(hdc);
    if (!d) return;
    d->clip = d->has_clip ? Rect{std::max(r.l, d->clip.l), std::max(r.t, d->clip.t), std::min(r.r, d->clip.r), std::min(r.b, d->clip.b)} : r;
    d->has_clip = true;
}
void gdi_hline(u32 hdc, i32 x0, i32 x1, i32 y, u32 c) { gdi_fill_color(hdc, {std::min(x0, x1), y, std::max(x0, x1), y + 1}, c); }
void gdi_vline(u32 hdc, i32 x, i32 y0, i32 y1, u32 c) { gdi_fill_color(hdc, {x, std::min(y0, y1), x + 1, std::max(y0, y1)}, c); }
// DrawEdge: BDR_RAISEDOUTER 1, SUNKENOUTER 2, RAISEDINNER 4, SUNKENINNER 8; BF_LEFT 1, TOP 2, RIGHT 4, BOTTOM 8,
// BF_MIDDLE 0x800, BF_FLAT 0x4000, BF_MONO 0x8000, BF_ADJUST 0x2000
void gdi_draw_edge(u32 hdc, Rect r, u32 edge, u32 flags) {
    const u32 white = sys_color(20), light = sys_color(22), face = sys_color(15), shadow = sys_color(16), dark = sys_color(21);
    auto ring = [&](u32 tl, u32 br) {
        if (flags & 2) gdi_hline(hdc, r.l, r.r, r.t, tl);
        if (flags & 1) gdi_vline(hdc, r.l, r.t, r.b, tl);
        if (flags & 8) gdi_hline(hdc, r.l, r.r, r.b - 1, br);
        if (flags & 4) gdi_vline(hdc, r.r - 1, r.t, r.b, br);
        if (flags & 1) r.l++;
        if (flags & 2) r.t++;
        if (flags & 4) r.r--;
        if (flags & 8) r.b--;
    };
    if (flags & 0x8000) { if (edge & 3) ring(dark, dark); if (edge & 12) ring(face, face); }
    else if (flags & 0x4000) { if (edge & 3) ring(shadow, shadow); if (edge & 12) ring(face, face); }
    else {
        if (edge & 1) ring(light, dark); else if (edge & 2) ring(shadow, white);
        if (edge & 4) ring(white, shadow); else if (edge & 8) ring(dark, light);
    }
    if (flags & 0x800) gdi_fill_color(hdc, r, face);
}
void gdi_text(u32 hdc, i32 x, i32 y, const std::string &str, u32 colorref, bool bold, const Rect *clip) {
    DC *d = dc(hdc);
    if (!d) return;
    FontSpec f = stock_font(17);
    if (bold) f.weight = 700;
    Lock lk(d);
    Surf s;
    if (!surf_of(d, s)) return;
    Rect saved = s.clip;
    if (clip) s.clip = rect_and(s.clip, {clip->l + s.ox, clip->t + s.oy, clip->r + s.ox, clip->b + s.oy});
    u32 ink = raw_of(s, colorref);
    draw_text_run(f, str.data(), str.size(), x, y, nullptr, [&](int px, int py) { put_pixel(s, px, py, ink); });
    s.clip = saved;
}
int gdi_text_width(const std::string &str, bool bold) {
    FontSpec f = stock_font(17);
    if (bold) f.weight = 700;
    return text_width(f, str.data(), str.size());
}
u32 gdi_sys_color_brush(int i) { init_stock(); return i >= 0 && i < 32 ? g_sysbrush[i] : 0; }

void gdi_fill_rect(u32 hdc, const Rect &r, u32 hbrush) {
    DC *d = dc(hdc);
    if (!d) return;
    if (hbrush && hbrush <= 32) hbrush = gdi_sys_color_brush((int)hbrush - 1);   // COLOR_xxx + 1
    GObj *b = obj(hbrush);
    if (!b || b->kind != GKind::Brush || b->style == 1) return;
    Lock lk(d);
    Surf s;
    if (!surf_of(d, s)) return;
    fill(s, r, raw_of(s, b->color));
}

// ---------------------------------------------------------------- objetos

STUB(ext_gdi32_dll_GetStockObject) { init_stock(); u32 i = arg(0); RET(i < 18 ? g_stock[i] : 0, 1); }
STUB(ext_gdi32_dll_CreateSolidBrush) { GObj o{GKind::Brush}; o.color = arg(0); RET(new_obj(o), 1); }
STUB(ext_gdi32_dll_CreateHatchBrush) { GObj o{GKind::Brush}; o.color = arg(1); RET(new_obj(o), 2); }
STUB(ext_gdi32_dll_CreatePatternBrush) { GObj o{GKind::Brush}; RET(new_obj(o), 1); }
STUB(ext_gdi32_dll_CreatePen) { GObj o{GKind::Pen}; o.style = arg(0); o.width = (int)arg(1); o.color = arg(2); RET(new_obj(o), 3); }
STUB(ext_gdi32_dll_CreateFontIndirectA) {
    GObj o{GKind::Font};
    o.logfont.assign(MEM + arg(0), MEM + arg(0) + 60);
    RET(new_obj(o), 1);
}
STUB(ext_gdi32_dll_CreatePalette) {
    u32 lp = arg(0);
    GObj o{GKind::Palette};
    u32 n = rd16(lp + 2);
    for (u32 i = 0; i < n && i < 256; i++) {
        u32 e = rd32(lp + 4 + 4 * i);
        o.pal.push_back(((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF));
    }
    RET(new_obj(o), 1);
}
STUB(ext_gdi32_dll_GetPaletteEntries) {
    GObj *p = obj(arg(0));
    u32 start = arg(1), n = arg(2), out = arg(3);
    if (!p || p->kind != GKind::Palette) RET(0, 4);
    if (!out) RET((u32)p->pal.size(), 4);
    u32 k = 0;
    for (; k < n && start + k < p->pal.size(); k++) {
        u32 c = p->pal[start + k];
        wr32(out + 4 * k, ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16));
    }
    RET(k, 4);
}
// SetPaletteEntries / AnimatePalette (hpal, start, n, PALETTEENTRY*): cambian entradas; si la paleta está en
// pantalla (realizada), se ve al momento (PCF2000 funde la pantalla con AnimatePalette en 256 colores)
static void set_pal_entries(bool animate) {
    GObj *p = obj(arg(0));
    u32 start = arg(1), n = arg(2), in = arg(3);
    if (!p || p->kind != GKind::Palette) RET(0, 4);
    u32 k = 0;
    for (; k < n && start + k < p->pal.size(); k++) {
        u32 e = rd32(in + 4 * k);
        p->pal[start + k] = ((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF);
    }
    if (animate) {
        std::lock_guard<std::mutex> lk(display().mx);
        for (u32 i = start; i < start + k && i < 256; i++) display().screen.pal[i] = p->pal[i];
        palette_changed();
    }
    RET(k, 4);
}
STUB(ext_gdi32_dll_SetPaletteEntries) { set_pal_entries(false); }
STUB(ext_gdi32_dll_AnimatePalette) { set_pal_entries(true); }
static u32 new_bitmap(int w, int h, int bypp, bool mono = false) {
    GObj o{GKind::Bitmap};
    o.w = w; o.h = h; o.bypp = bypp;
    if (mono) { o.bits = 1; o.bypp = 1; }
    o.px.assign((size_t)std::max(0, w) * std::max(0, h) * (size_t)o.bypp, 0);
    return new_obj(o);
}
// bits empaquetados de CreateBitmap/SetBitmapBits (filas alineadas a 16 bits) → bitmap
static void set_bitmap_bits(GObj &b, u32 bits, u32 nbytes) {
    if (b.mem) return;
    if (b.bits == 1) {
        u32 stride = (((u32)b.w + 15) / 16) * 2;
        for (int y = 0; y < b.h; y++)
            for (int x = 0; x < b.w; x++) {
                u32 off = (u32)y * stride + (u32)x / 8;
                if (off >= nbytes) return;
                b.px[(size_t)y * b.w + x] = (MEM[bits + off] >> (7 - (x & 7))) & 1;
            }
        return;
    }
    u32 stride = (((u32)b.w * (u32)b.bypp) + 1) & ~1u;
    for (int y = 0; y < b.h; y++) {
        u32 n = std::min<u32>((u32)b.w * (u32)b.bypp, nbytes > (u32)y * stride ? nbytes - (u32)y * stride : 0);
        if (!n) return;
        std::memcpy(b.px.data() + (size_t)y * b.w * b.bypp, MEM + bits + (u32)y * stride, n);
    }
}
STUB(ext_gdi32_dll_CreateBitmap) {   // (w, h, planes, bpp, bits)
    u32 bpp = arg(3);
    u32 h = bpp <= 1 ? new_bitmap((int)arg(0), (int)arg(1), 1, true) : new_bitmap((int)arg(0), (int)arg(1), bpp >= 32 ? 4 : bpp >= 24 ? 3 : bpp >= 16 ? 2 : 1);
    if (arg(4)) { GObj *b = obj(h); u32 st = bpp <= 1 ? (((u32)b->w + 15) / 16) * 2 : (((u32)b->w * (u32)b->bypp) + 1) & ~1u; set_bitmap_bits(*b, arg(4), st * (u32)b->h); }
    RET(h, 5);
}
STUB(ext_gdi32_dll_CreateBitmapIndirect) {   // BITMAP { type, w, h, widthbytes, planes(16), bpp(16), bits }
    u32 p = arg(0);
    u32 bpp = rd16(p + 18);
    u32 h = bpp <= 1 ? new_bitmap((int)rd32(p + 4), (int)rd32(p + 8), 1, true) : new_bitmap((int)rd32(p + 4), (int)rd32(p + 8), bpp >= 32 ? 4 : bpp >= 24 ? 3 : bpp >= 16 ? 2 : 1);
    if (rd32(p + 20)) { GObj *b = obj(h); set_bitmap_bits(*b, rd32(p + 20), rd32(p + 12) * (u32)b->h); }
    RET(h, 1);
}
// compatible con el DC: el de pantalla o el bitmap que tenga seleccionado (un DC de memoria recién creado
// tiene uno monocromo de 1x1, y su bitmap compatible es monocromo, como en Windows)
STUB(ext_gdi32_dll_CreateCompatibleBitmap) {
    DC *d = dc(arg(0));
    GObj *cur = d && !d->screen ? obj(d->bitmap) : nullptr;
    if (cur && cur->kind == GKind::Bitmap && cur->bits == 1) RET(new_bitmap((int)arg(1), (int)arg(2), 1, true), 3);
    if (cur && cur->kind == GKind::Bitmap && cur->mem) RET(new_bitmap((int)arg(1), (int)arg(2), cur->bypp), 3);
    RET(new_bitmap((int)arg(1), (int)arg(2), display().screen.bpp / 8), 3);
}
STUB(ext_gdi32_dll_CreateDiscardableBitmap) { RET(new_bitmap((int)arg(1), (int)arg(2), display().screen.bpp / 8), 3); }
STUB(ext_gdi32_dll_CreateRectRgn) { GObj o{GKind::Region}; o.rgn = {(i32)arg(0), (i32)arg(1), (i32)arg(2), (i32)arg(3)}; RET(new_obj(o), 4); }
STUB(ext_gdi32_dll_CreateRectRgnIndirect) { GObj o{GKind::Region}; o.rgn = rd_rect(arg(0)); RET(new_obj(o), 1); }
STUB(ext_gdi32_dll_SetRectRgn) { if (GObj *o = obj(arg(0))) o->rgn = {(i32)arg(1), (i32)arg(2), (i32)arg(3), (i32)arg(4)}; RET(1, 5); }
STUB(ext_gdi32_dll_DeleteObject) {
    GObj *o = obj(arg(0));
    if (!o) RET(0, 1);
    if (o->stock) RET(1, 1);
    if (o->kind == GKind::Bitmap && o->mem) heap_free(o->mem);
    g_objs.erase(arg(0));
    RET(1, 1);
}
STUB(ext_gdi32_dll_UnrealizeObject) { RET(1, 1); }
STUB(ext_gdi32_dll_GetObjectType) {
    if (dc(arg(0))) RET(3, 1);   // OBJ_DC
    GObj *o = obj(arg(0));
    if (!o) RET(0, 1);
    switch (o->kind) {
        case GKind::Pen: RET(1, 1); case GKind::Brush: RET(2, 1); case GKind::Palette: RET(5, 1);
        case GKind::Font: RET(6, 1); case GKind::Bitmap: RET(7, 1); case GKind::Region: RET(8, 1); default: RET(0, 1);
    }
}
STUB(ext_gdi32_dll_GetObjectA) {
    GObj *o = obj(arg(0));
    u32 n = arg(1), p = arg(2);
    if (!o) RET(0, 3);
    switch (o->kind) {
        case GKind::Bitmap: {
            u32 bpp = o->bits == 1 ? 1 : (u32)o->bypp * 8;
            u32 wb = o->mem ? (u32)std::abs(o->mem_stride) : bpp == 1 ? (((u32)o->w + 15) / 16) * 2 : (((u32)o->w * (u32)o->bypp) + 1) & ~1u;
            if (p && n >= 24) { std::memset(MEM + p, 0, 24); wr32(p + 4, (u32)o->w); wr32(p + 8, (u32)o->h); wr32(p + 12, wb); wr16(p + 16, 1); wr16(p + 18, bpp); wr32(p + 20, o->mem); }
            if (o->mem && p && n >= 84) {   // DIBSECTION: BITMAP + BITMAPINFOHEADER + máscaras + sección
                std::memset(MEM + p + 24, 0, 60);
                wr32(p + 24, 40); wr32(p + 28, (u32)o->w); wr32(p + 32, o->mem_stride < 0 ? (u32)o->h : (u32)-o->h);
                wr16(p + 36, 1); wr16(p + 38, bpp); wr32(p + 44, wb * (u32)o->h);
                RET(84, 3);
            }
            RET(24, 3);
        }
        case GKind::Palette: if (p && n >= 2) wr16(p, (u32)o->pal.size()); RET(2, 3);
        case GKind::Brush: if (p && n >= 12) { wr32(p, o->style); wr32(p + 4, o->color); wr32(p + 8, 0); } RET(12, 3);
        case GKind::Pen: if (p && n >= 16) { wr32(p, o->style); wr32(p + 4, (u32)o->width); wr32(p + 8, 0); wr32(p + 12, o->color); } RET(16, 3);
        case GKind::Font: if (p) std::memcpy(MEM + p, o->logfont.data(), std::min<size_t>(n, o->logfont.size())); RET(60, 3);
        default: RET(0, 3);
    }
}

// ---------------------------------------------------------------- DC

STUB(ext_gdi32_dll_CreateCompatibleDC) {
    init_stock();
    DC d;
    d.bitmap = new_bitmap(1, 1, 1, true);   // bitmap monocromo de 1x1 por defecto, como en Windows
    obj(d.bitmap)->stock = true;
    d.brush = g_stock[0]; d.pen = g_stock[7]; d.font = g_stock[13]; d.palette = g_stock[15];
    u32 h = g_next_dc;
    g_next_dc += 4;
    g_dcs[h] = d;
    RET(h, 1);
}
STUB(ext_gdi32_dll_CreateDCA) {
    std::string drv = gstr(arg(0));
    plog("CreateDCA(\"%s\", \"%s\")", drv.c_str(), gstr(arg(1)).c_str());
    if (drv == "DISPLAY" || drv == "display") RET(gdi_window_dc(0, true), 4);
    RET(0, 4);   // impresoras: no hay
}
STUB(ext_gdi32_dll_DeleteDC) { g_dcs.erase(arg(0)); RET(1, 1); }
STUB(ext_gdi32_dll_SaveDC) { RET(1, 1); }
STUB(ext_gdi32_dll_RestoreDC) { RET(1, 2); }

STUB(ext_gdi32_dll_SelectObject) {
    DC *d = dc(arg(0));
    GObj *o = obj(arg(1));
    if (!d || !o) RET(0, 2);
    u32 old = 0;
    switch (o->kind) {
        case GKind::Bitmap: old = d->bitmap; d->bitmap = arg(1); break;
        case GKind::Brush: old = d->brush; d->brush = arg(1); break;
        case GKind::Pen: old = d->pen; d->pen = arg(1); break;
        case GKind::Font: old = d->font; d->font = arg(1); break;
        case GKind::Region: d->has_clip = true; d->clip = o->rgn; old = 2; break;   // SIMPLEREGION
        default: break;
    }
    RET(old, 2);
}
STUB(ext_gdi32_dll_SelectPalette) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 3);
    u32 old = d->palette;
    d->palette = arg(1);
    RET(old, 3);
}
STUB(ext_gdi32_dll_RealizePalette) {
    DC *d = dc(arg(0));
    GObj *p = d ? obj(d->palette) : nullptr;
    if (!p || p->kind != GKind::Palette || p->stock) RET(0, 1);
    {
        std::lock_guard<std::mutex> lk(display().mx);
        for (size_t i = 0; i < p->pal.size() && i < 256; i++) display().screen.pal[i] = p->pal[i];
        palette_changed();
    }
    RET((u32)p->pal.size(), 1);
}
STUB(ext_gdi32_dll_SetSystemPaletteUse) { RET(1, 2); }   // SYSPAL_STATIC antes

STUB(ext_gdi32_dll_GetDeviceCaps) {
    Screen &s = display().screen;
    switch (arg(1)) {
        case 0: RET(0x400, 2);        // DRIVERVERSION
        case 2: RET(1, 2);            // TECHNOLOGY: DT_RASDISPLAY
        case 4: RET(320, 2);          // HORZSIZE (mm)
        case 6: RET(240, 2);          // VERTSIZE
        case 8: RET(s.w, 2);          // HORZRES
        case 10: RET(s.h, 2);         // VERTRES
        case 12: RET(g_bpp, 2);       // BITSPIXEL
        case 14: RET(1, 2);           // PLANES
        case 16: case 18: RET(0xFFFFFFFFu, 2);
        case 22: RET(0, 2);           // NUMFONTS
        case 24: RET(g_bpp == 8 ? 20 : 0xFFFFFFFFu, 2);   // NUMCOLORS
        case 38: RET(0x100 | 0x1 | 0x8 | 0x80 | 0x200 | 0x800 | 0x2000, 2);   // RASTERCAPS (RC_PALETTE…)
        case 88: case 90: RET(96, 2);                  // LOGPIXELSX/Y
        case 104: RET(256, 2);        // SIZEPALETTE
        case 106: RET(20, 2);         // NUMRESERVED
        case 108: RET(18, 2);         // COLORRES
        case 116: RET(60, 2);         // VREFRESH
    }
    RET(0, 2);
}

STUB(ext_gdi32_dll_SetTextColor) { DC *d = dc(arg(0)); u32 o = d ? d->text_color : 0; if (d) d->text_color = arg(1); RET(o, 2); }
STUB(ext_gdi32_dll_GetTextColor) { DC *d = dc(arg(0)); RET(d ? d->text_color : 0, 1); }
STUB(ext_gdi32_dll_SetBkColor) { DC *d = dc(arg(0)); u32 o = d ? d->bk_color : 0; if (d) d->bk_color = arg(1); RET(o, 2); }
STUB(ext_gdi32_dll_GetBkColor) { DC *d = dc(arg(0)); RET(d ? d->bk_color : 0, 1); }
STUB(ext_gdi32_dll_SetBkMode) { DC *d = dc(arg(0)); u32 o = d ? d->bk_mode : 0; if (d) d->bk_mode = arg(1); RET(o, 2); }
STUB(ext_gdi32_dll_GetBkMode) { DC *d = dc(arg(0)); RET(d ? d->bk_mode : 0, 1); }
STUB(ext_gdi32_dll_SetTextAlign) { DC *d = dc(arg(0)); u32 o = d ? d->text_align : 0; if (d) d->text_align = arg(1); RET(o, 2); }
STUB(ext_gdi32_dll_GetTextAlign) { DC *d = dc(arg(0)); RET(d ? d->text_align : 0, 1); }
STUB(ext_gdi32_dll_SetROP2) { DC *d = dc(arg(0)); u32 o = d ? d->rop2 : 0; if (d) d->rop2 = arg(1); RET(o, 2); }
STUB(ext_gdi32_dll_GetROP2) { DC *d = dc(arg(0)); RET(d ? d->rop2 : 0, 1); }
STUB(ext_gdi32_dll_SetStretchBltMode) { DC *d = dc(arg(0)); u32 o = d ? d->stretch_mode : 0; if (d) d->stretch_mode = arg(1); RET(o, 2); }
STUB(ext_gdi32_dll_GetStretchBltMode) { DC *d = dc(arg(0)); RET(d ? d->stretch_mode : 0, 1); }
STUB(ext_gdi32_dll_SetMapMode) { DC *d = dc(arg(0)); u32 o = d ? d->map_mode : 0; if (d) d->map_mode = arg(1); RET(o, 2); }
STUB(ext_gdi32_dll_GetMapMode) { DC *d = dc(arg(0)); RET(d ? d->map_mode : 1, 1); }
STUB(ext_gdi32_dll_SetViewportOrgEx) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 4);
    if (arg(3)) { wr32(arg(3), (u32)d->org_x); wr32(arg(3) + 4, (u32)d->org_y); }
    d->org_x = (i32)arg(1); d->org_y = (i32)arg(2);
    RET(1, 4);
}
STUB(ext_gdi32_dll_GetViewportOrgEx) { DC *d = dc(arg(0)); if (d) { wr32(arg(1), (u32)d->org_x); wr32(arg(1) + 4, (u32)d->org_y); } RET(d != nullptr, 2); }
STUB(ext_gdi32_dll_OffsetViewportOrgEx) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 4);
    if (arg(3)) { wr32(arg(3), (u32)d->org_x); wr32(arg(3) + 4, (u32)d->org_y); }
    d->org_x += (i32)arg(1); d->org_y += (i32)arg(2);
    RET(1, 4);
}
STUB(ext_gdi32_dll_SetWindowOrgEx) { if (arg(3)) { wr32(arg(3), 0); wr32(arg(3) + 4, 0); } RET(1, 4); }
STUB(ext_gdi32_dll_GetWindowOrgEx) { wr32(arg(1), 0); wr32(arg(1) + 4, 0); RET(1, 2); }
STUB(ext_gdi32_dll_SetViewportExtEx) { if (arg(3)) { wr32(arg(3), 1); wr32(arg(3) + 4, 1); } RET(1, 4); }
STUB(ext_gdi32_dll_SetWindowExtEx) { if (arg(3)) { wr32(arg(3), 1); wr32(arg(3) + 4, 1); } RET(1, 4); }
STUB(ext_gdi32_dll_GetViewportExtEx) { wr32(arg(1), 1); wr32(arg(1) + 4, 1); RET(1, 2); }
STUB(ext_gdi32_dll_GetWindowExtEx) { wr32(arg(1), 1); wr32(arg(1) + 4, 1); RET(1, 2); }
STUB(ext_gdi32_dll_SetBrushOrgEx) { if (arg(3)) { wr32(arg(3), 0); wr32(arg(3) + 4, 0); } RET(1, 4); }
STUB(ext_gdi32_dll_DPtoLP) { RET(1, 3); }
STUB(ext_gdi32_dll_LPtoDP) { RET(1, 3); }

// ---------------------------------------------------------------- recorte

STUB(ext_gdi32_dll_IntersectClipRect) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 5);
    Rect r{(i32)arg(1), (i32)arg(2), (i32)arg(3), (i32)arg(4)};
    if (d->has_clip) r = {std::max(r.l, d->clip.l), std::max(r.t, d->clip.t), std::min(r.r, d->clip.r), std::min(r.b, d->clip.b)};
    d->clip = r; d->has_clip = true;
    RET(r.l < r.r && r.t < r.b ? 2 : 1, 5);
}
STUB(ext_gdi32_dll_ExcludeClipRect) { RET(2, 5); }
STUB(ext_gdi32_dll_SelectClipRgn) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 2);
    GObj *r = obj(arg(1));
    if (!r) { d->has_clip = false; RET(2, 2); }
    d->clip = r->rgn; d->has_clip = true;
    RET(2, 2);
}
STUB(ext_gdi32_dll_ExtSelectClipRgn) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 3);
    GObj *r = obj(arg(1));
    if (!r) d->has_clip = false; else { d->clip = r->rgn; d->has_clip = true; }
    RET(2, 3);
}
STUB(ext_gdi32_dll_GetClipBox) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 2);
    Rect r;
    if (d->has_clip) r = d->clip;
    else if (d->screen) r = {0, 0, d->area.w(), d->area.h()};
    else if (GObj *b = obj(d->bitmap)) r = {0, 0, b->w, b->h};
    wr_rect(arg(1), r);
    RET(2, 2);
}
STUB(ext_gdi32_dll_GetClipRgn) { RET(0, 2); }
STUB(ext_gdi32_dll_RectVisible) { RET(1, 2); }
STUB(ext_gdi32_dll_PtVisible) { RET(1, 3); }
STUB(ext_gdi32_dll_CombineRgn) { RET(2, 4); }
STUB(ext_gdi32_dll_OffsetRgn) { if (GObj *o = obj(arg(0))) { o->rgn.l += (i32)arg(1); o->rgn.r += (i32)arg(1); o->rgn.t += (i32)arg(2); o->rgn.b += (i32)arg(2); } RET(2, 3); }
STUB(ext_gdi32_dll_OffsetClipRgn) { RET(2, 3); }

// ---------------------------------------------------------------- dibujo

// DIB → superficie. Soporta 8 bits (el formato del juego), 4, 1, 16, 24 y 32 bits.
static void draw_dib(DC *d, i32 xd, i32 yd, i32 wd, i32 hd, i32 xs, i32 ys, i32 ws, i32 hs, u32 bits, u32 bmi, u32 usage) {
    Surf s;
    if (!surf_of(d, s) || !wd || !hd || !ws || !hs) return;
    i32 bw = (i32)rd32(bmi + 4), bh = (i32)rd32(bmi + 8);
    u32 hdr = rd32(bmi), bpp = rd16(bmi + 14), comp = rd32(bmi + 16), used = rd32(bmi + 32);
    if (comp != 0 && comp != 3) { plog_once("dibcomp", "StretchDIBits: compresión %u no soportada", comp); return; }
    bool topdown = bh < 0;
    if (topdown) bh = -bh;
    u32 stride = (((u32)bw * bpp + 31) / 32) * 4;
    u32 ncol = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    u32 map[256] = {};
    GObj *lp = obj(d->palette);
    for (u32 i = 0; i < ncol && i < 256; i++) {
        if (usage == 1) {   // DIB_PAL_COLORS: índices de la paleta lógica
            u32 li = rd16(bmi + hdr + 2 * i);
            if (s.bypp == 1 && lp && !lp->stock && li < lp->pal.size()) map[i] = li;
            else map[i] = raw_of(s, lp && li < lp->pal.size() ? colorref_to_rgb(lp->pal[li]) : 0);
        } else {
            u32 q = rd32(bmi + hdr + 4 * i);   // RGBQUAD: B G R 0 → COLORREF
            map[i] = raw_of(s, ((q & 0xFF) << 16) | (q & 0xFF00) | ((q >> 16) & 0xFF));
        }
    }
    bool flip_x = wd < 0, flip_y = hd < 0;
    wd = std::abs(wd); hd = std::abs(hd);
    for (i32 dy = 0; dy < hd; dy++) {
        i32 py = yd + dy + s.oy;
        if (py < s.clip.t || py >= s.clip.b) continue;
        i32 sy = ys + (i32)((i64)(flip_y ? hd - 1 - dy : dy) * hs / hd);
        if (sy < 0 || sy >= bh) continue;
        u32 row = bits + (u32)(topdown ? sy : bh - 1 - sy) * stride;
        u8 *out = s.px + (size_t)py * s.stride;
        for (i32 dx = 0; dx < wd; dx++) {
            i32 px = xd + dx + s.ox;
            if (px < s.clip.l || px >= s.clip.r || !s.vis(px, py)) continue;
            i32 sx = xs + (i32)((i64)(flip_x ? wd - 1 - dx : dx) * ws / wd);
            if (sx < 0 || sx >= bw) continue;
            u32 v;
            switch (bpp) {
                case 8: v = map[MEM[row + (u32)sx]]; break;
                case 4: v = map[(MEM[row + (u32)sx / 2] >> ((sx & 1) ? 0 : 4)) & 15]; break;
                case 1: v = map[(MEM[row + (u32)sx / 8] >> (7 - (sx & 7))) & 1]; break;
                case 16: { u32 c = rd16(row + 2 * (u32)sx); v = raw_of(s, ((c >> 10) & 31) << 3 | ((c >> 5) & 31) << 11 | (c & 31) << 19); break; }
                case 24: { u32 o = row + 3 * (u32)sx; v = raw_of(s, (u32)MEM[o] << 16 | (u32)MEM[o + 1] << 8 | MEM[o + 2]); break; }
                default: { u32 c = rd32(row + 4 * (u32)sx); v = raw_of(s, ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF)); break; }
            }
            store(out + (size_t)px * s.bypp, s.bypp, v);
        }
    }
}

STUB(ext_gdi32_dll_StretchDIBits) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 13);
    u32 rop = arg(12);
    if (rop != 0xCC0020) plog_once("sdrop" + std::to_string(rop), "StretchDIBits: ROP 0x%x tratado como SRCCOPY", rop);
    {
        Lock lk(d);
        draw_dib(d, (i32)arg(1), (i32)arg(2), (i32)arg(3), (i32)arg(4), (i32)arg(5), (i32)arg(6), (i32)arg(7), (i32)arg(8), arg(9), arg(10), arg(11));
    }
    RET(arg(4), 13);
}

// ROP3: el byte 2 del código es la tabla de verdad de (patrón, origen, destino), bit a bit
static inline u32 rop3(u32 code, u32 p, u32 sv, u32 d) {
    u32 r = 0;
    for (int k = 0; k < 8; k++) {
        if (!((code >> k) & 1)) continue;
        r |= ((k & 4) ? p : ~p) & ((k & 2) ? sv : ~sv) & ((k & 1) ? d : ~d);
    }
    return r;
}

static bool blit(DC *dd, i32 x, i32 y, i32 w, i32 h, DC *sd, i32 xs, i32 ys, i32 ws, i32 hs, u32 rop) {
    Surf dst;
    if (!surf_of(dd, dst)) return false;
    u32 code = (rop >> 16) & 0xFF;
    bool use_s = ((code >> 2) & 0x33) != (code & 0x33), use_p = ((code >> 4) & 0x0F) != (code & 0x0F);
    u32 mask = dst.bits == 1 ? 1u : dst.bypp == 1 ? 0xFFu : dst.bypp == 2 ? 0xFFFFu : 0xFFFFFFu;
    u32 pat = 0;
    if (use_p) { GObj *b = obj(dd->brush); pat = b && b->style != 1 ? raw_of(dst, b->color) : 0; }
    Surf src{};
    if (use_s) {
        if (!sd || !surf_of(sd, src)) return false;
    }
    // misma superficie: copia previa del origen
    std::vector<u8> tmp;
    if (use_s && src.px == dst.px) {
        size_t rows = (size_t)src.h, rb = (size_t)std::abs(src.stride);
        tmp.resize(rows * rb);
        for (i32 yy = 0; yy < src.h; yy++) std::memcpy(tmp.data() + (size_t)yy * rb, src.row(yy), rb);
        src.px = tmp.data();
        src.stride = (int)rb;
    }
    bool same = use_s && src.bypp == dst.bypp && src.bits == dst.bits && src.ctab == dst.ctab;
    // conversión del origen al formato del destino
    u32 mono_fg = 0, mono_bg = 0, src_bk = 0;
    if (use_s && src.bits == 1 && dst.bits != 1) { mono_fg = raw_of(dst, dd->text_color); mono_bg = raw_of(dst, dd->bk_color); }
    if (use_s && src.bits != 1 && dst.bits == 1) src_bk = raw_of(src, sd->bk_color);
    auto convert = [&](u32 v) -> u32 {
        if (same) return v;
        if (src.bits == 1) return dst.bits == 1 ? v : (v ? mono_bg : mono_fg);
        if (dst.bits == 1) return v == src_bk ? 1u : 0u;
        return rgb_to_raw(dst, raw_to_rgb(src, v));
    };
    int bp = dst.bypp;
    bool fast = same && code == 0xCC && ws == w && hs == h && !dst.owner;   // SRCCOPY 1:1 del mismo formato
    for (i32 dy = 0; dy < h; dy++) {
        i32 py = y + dy + dst.oy;
        if (py < dst.clip.t || py >= dst.clip.b) continue;
        i32 sy = use_s ? ys + (i32)((i64)dy * hs / h) + src.oy : 0;
        if (use_s && (sy < 0 || sy >= src.h)) continue;
        u8 *drow = dst.row(py);
        const u8 *srow = use_s ? src.row(sy) : nullptr;
        if (fast) {
            i32 x0 = std::max(x + dst.ox, dst.clip.l), x1 = std::min(x + w + dst.ox, dst.clip.r);
            i32 sx0 = xs + src.ox + (x0 - (x + dst.ox));
            if (sx0 < 0) { x0 -= sx0; sx0 = 0; }
            if (sx0 + (x1 - x0) > src.w) x1 = x0 + (src.w - sx0);
            if (x1 > x0) std::memmove(drow + (size_t)x0 * bp, srow + (size_t)sx0 * bp, (size_t)(x1 - x0) * bp);
            continue;
        }
        for (i32 dx = 0; dx < w; dx++) {
            i32 px = x + dx + dst.ox;
            if (px < dst.clip.l || px >= dst.clip.r || !dst.vis(px, py)) continue;
            u32 sv = 0;
            if (use_s) {
                i32 sx = xs + (i32)((i64)dx * ws / w) + src.ox;
                if (sx < 0 || sx >= src.w) continue;
                sv = convert(load(srow + (size_t)sx * src.bypp, src.bypp));
            }
            u8 *dp = drow + (size_t)px * bp;
            u32 dv = code == 0xCC || code == 0xF0 || code == 0x00 || code == 0xFF ? 0 : load(dp, bp);
            u32 r;
            switch (code) {
            case 0xCC: r = sv; break;                 // SRCCOPY
            case 0xF0: r = pat; break;                // PATCOPY
            case 0x00: r = rgb_to_raw(dst, 0); break;           // BLACKNESS
            case 0xFF: r = rgb_to_raw(dst, 0xFFFFFF); break;    // WHITENESS
            default: r = rop3(code, pat, sv, dv); break;
            }
            store(dp, bp, r & mask);
        }
    }
    return true;
}
// ---------------------------------------------------------------- DIB

// tabla de colores de un BITMAPINFO (DIB_RGB_COLORS) o de la paleta del DC (DIB_PAL_COLORS) → 0x00RRGGBB
static std::vector<u32> dib_colors(DC *d, u32 bmi, u32 usage) {
    u32 hdr = rd32(bmi), bpp = rd16(bmi + 14), used = rd32(bmi + 32);
    u32 n = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    std::vector<u32> t;
    GObj *lp = d ? obj(d->palette) : nullptr;
    for (u32 i = 0; i < n && i < 256; i++) {
        if (usage == 1) {
            u32 li = rd16(bmi + hdr + 2 * i);
            t.push_back(lp && li < lp->pal.size() ? lp->pal[li] : display().screen.pal[li & 0xFF]);
        } else {
            u32 q = rd32(bmi + hdr + 4 * i);
            t.push_back(q & 0xFFFFFF);   // RGBQUAD (B, G, R) = 0x00RRGGBB
        }
    }
    return t;
}

STUB(ext_gdi32_dll_CreateDIBSection) {   // (hdc, bmi, usage, void **bits, section, offset)
    u32 bmi = arg(1);
    i32 w = (i32)rd32(bmi + 4), h = (i32)rd32(bmi + 8);
    u32 bpp = rd16(bmi + 14);
    bool topdown = h < 0;
    if (topdown) h = -h;
    GObj o{GKind::Bitmap};
    o.w = w; o.h = h;
    u32 stride = (((u32)w * bpp + 31) / 32) * 4;
    if (bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) {
        o.bypp = (int)bpp / 8;
        o.mem = heap_alloc(stride * (u32)h + 16, true);
        o.mem_stride = topdown ? (int)stride : -(int)stride;
        if (bpp == 8) { o.ctab = dib_colors(dc(arg(0)), bmi, arg(2)); o.ctab.resize(256, 0); }
        if (bpp == 16 && rd32(bmi + 16) == 3 && rd32(bmi + 40) != 0xF800)   // máscaras distintas de 565: 555
            plog_once("dib555", "CreateDIBSection: 16 bits con máscaras %x/%x/%x (se trata como 565)", rd32(bmi + 40), rd32(bmi + 44), rd32(bmi + 48));
    } else {
        plog_once("dibbpp" + std::to_string(bpp), "CreateDIBSection de %u bits: los píxeles no se comparten con el programa", bpp);
        o.bypp = 1;
        if (bpp == 1) o.bits = 1;
        o.px.assign((size_t)w * h, 0);
        o.mem = 0;
    }
    if (arg(3)) wr32(arg(3), o.mem ? o.mem : heap_alloc(stride * (u32)h + 16, true));
    RET(new_obj(o), 6);
}
STUB(ext_gdi32_dll_SetDIBColorTable) {   // (hdc, start, n, RGBQUAD*)
    DC *d = dc(arg(0));
    GObj *b = d ? obj(d->bitmap) : nullptr;
    if (!b || b->ctab.empty()) RET(0, 4);
    u32 k = 0;
    for (; k < arg(2) && arg(1) + k < 256; k++) b->ctab[arg(1) + k] = rd32(arg(3) + 4 * k) & 0xFFFFFF;
    RET(k, 4);
}

// DIB → bitmap: se dibuja con un DC temporal que lo tiene seleccionado
static void dib_to_bitmap(u32 hbm, DC *ref, i32 start, i32 lines, u32 bits, u32 bmi, u32 usage) {
    GObj *b = obj(hbm);
    if (!b || b->kind != GKind::Bitmap) return;
    DC tmp;
    tmp.bitmap = hbm;
    tmp.palette = ref ? ref->palette : g_stock[15];
    i32 bw = (i32)rd32(bmi + 4), bh = std::abs((i32)rd32(bmi + 8));
    bool topdown = (i32)rd32(bmi + 8) < 0;
    // las filas [start, start+lines) cuentan desde abajo en una DIB normal
    i32 y_top = topdown ? start : bh - start - lines;
    draw_dib(&tmp, 0, y_top, bw, lines, 0, y_top, bw, lines, bits, bmi, usage);
}
STUB(ext_gdi32_dll_SetDIBits) {   // (hdc, hbm, start, lines, bits, bmi, usage)
    dib_to_bitmap(arg(1), dc(arg(0)), (i32)arg(2), (i32)arg(3), arg(4), arg(5), arg(6));
    RET(arg(3), 7);
}
STUB(ext_gdi32_dll_CreateDIBitmap) {   // (hdc, BITMAPINFOHEADER*, init, bits, bmi, usage)
    u32 bih = arg(1);
    i32 w = (i32)rd32(bih + 4), h = std::abs((i32)rd32(bih + 8));
    u32 hbm = new_bitmap(w, h, display().screen.bpp / 8);
    if ((arg(2) & 4) && arg(3) && arg(4)) dib_to_bitmap(hbm, dc(arg(0)), 0, h, arg(3), arg(4), arg(5));   // CBM_INIT
    RET(hbm, 6);
}
STUB(ext_gdi32_dll_SetDIBitsToDevice) {   // (hdc, xd, yd, w, h, xs, ys, startscan, nscans, bits, bmi, usage)
    DC *d = dc(arg(0));
    if (!d) RET(0, 12);
    i32 w = (i32)arg(3), h = (i32)arg(4), xs = (i32)arg(5), ys = (i32)arg(6);
    u32 bmi = arg(10);
    i32 bh = std::abs((i32)rd32(bmi + 8));
    bool topdown = (i32)rd32(bmi + 8) < 0;
    i32 sy = topdown ? ys : bh - ys - h;   // ys cuenta desde abajo en una DIB normal
    {
        Lock lk(d);
        draw_dib(d, (i32)arg(1), (i32)arg(2), w, h, xs, sy, w, h, arg(9), bmi, arg(11));
    }
    RET(arg(8), 12);
}
STUB(ext_gdi32_dll_GetDIBits) {   // (hdc, hbm, start, lines, bits, bmi, usage)
    GObj *b = obj(arg(1));
    u32 bmi = arg(5);
    if (!b || b->kind != GKind::Bitmap) RET(0, 7);
    u32 bpp = rd16(bmi + 14);
    if (!arg(4) || !bpp) {   // solo rellenar la cabecera
        u32 obpp = b->bits == 1 ? 1 : (u32)b->bypp * 8;
        wr32(bmi + 4, (u32)b->w); wr32(bmi + 8, (u32)b->h); wr16(bmi + 12, 1); wr16(bmi + 14, obpp);
        wr32(bmi + 16, 0); wr32(bmi + 20, ((((u32)b->w * obpp + 31) / 32) * 4) * (u32)b->h);
        RET((u32)b->h, 7);
    }
    DC tmp;
    tmp.bitmap = arg(1);
    Surf s;
    if (!surf_of(&tmp, s)) RET(0, 7);
    i32 bh = (i32)rd32(bmi + 8);
    bool topdown = bh < 0;
    u32 stride = (((u32)b->w * bpp + 31) / 32) * 4;
    std::vector<u32> ctab;
    if (bpp <= 8) {   // tabla: la de la sección o la paleta del sistema
        u32 n = 1u << bpp;
        for (u32 i = 0; i < n; i++) {
            u32 c = bpp == 1 ? (i ? 0xFFFFFF : 0) : (s.ctab ? s.ctab[i & 0xFF] : display().screen.pal[i & 0xFF]);
            ctab.push_back(c);
            if (rd32(bmi) >= 40) wr32(bmi + rd32(bmi) + 4 * i, c);
        }
    }
    u32 start = arg(2), lines = arg(3), out = arg(4);
    for (u32 l = 0; l < lines; l++) {
        i32 sy = topdown ? (i32)(start + l) : b->h - 1 - (i32)(start + l);
        if (sy < 0 || sy >= b->h) continue;
        u32 row = out + l * stride;
        std::memset(MEM + row, 0, stride);
        for (i32 x = 0; x < b->w; x++) {
            u32 rgb = raw_to_rgb(s, load(s.row(sy) + (size_t)x * s.bypp, s.bypp));
            switch (bpp) {
            case 1: if (nearest_in(ctab.data(), 2, rgb)) MEM[row + (u32)x / 8] |= (u8)(0x80 >> (x & 7)); break;
            case 4: MEM[row + (u32)x / 2] |= (u8)(nearest_in(ctab.data(), 16, rgb) << ((x & 1) ? 0 : 4)); break;
            case 8: MEM[row + (u32)x] = s.bypp == 1 && s.bits != 1 ? (u8)load(s.row(sy) + x, 1) : nearest_in(ctab.data(), 256, rgb); break;
            case 16: wr16(row + 2 * (u32)x, ((rgb >> 9) & 0x7C00) | ((rgb >> 6) & 0x3E0) | ((rgb >> 3) & 0x1F)); break;
            case 24: MEM[row + 3 * (u32)x] = (u8)rgb; MEM[row + 3 * (u32)x + 1] = (u8)(rgb >> 8); MEM[row + 3 * (u32)x + 2] = (u8)(rgb >> 16); break;
            default: wr32(row + 4 * (u32)x, rgb); break;
            }
        }
    }
    RET(lines, 7);
}
STUB(ext_gdi32_dll_GetBitmapBits) {   // (hbm, n, bits): filas alineadas a 16 bits
    GObj *b = obj(arg(0));
    if (!b || b->kind != GKind::Bitmap) RET(0, 3);
    DC tmp;
    tmp.bitmap = arg(0);
    Surf s;
    if (!surf_of(&tmp, s)) RET(0, 3);
    u32 n = arg(1), out = arg(2), done = 0;
    if (b->bits == 1) {
        u32 stride = (((u32)b->w + 15) / 16) * 2;
        for (i32 y = 0; y < b->h && done < n; y++)
            for (u32 k = 0; k < stride && done < n; k++, done++) {
                u8 v = 0;
                for (int bit = 0; bit < 8; bit++) { i32 x = (i32)(k * 8 + (u32)bit); if (x < b->w && s.row(y)[x]) v |= (u8)(0x80 >> bit); }
                wr8(out + done, v);
            }
        RET(done, 3);
    }
    u32 rb = (u32)b->w * (u32)b->bypp, stride = (rb + 1) & ~1u;
    for (i32 y = 0; y < b->h && done < n; y++) {
        u32 k = std::min(rb, n - done);
        std::memcpy(MEM + out + done, s.row(y), k);
        done += std::min(stride, n - done);
    }
    RET(done, 3);
}
STUB(ext_gdi32_dll_SetBitmapBits) { GObj *b = obj(arg(0)); if (b && b->kind == GKind::Bitmap) set_bitmap_bits(*b, arg(2), arg(1)); RET(arg(1), 3); }
STUB(ext_gdi32_dll_GetBitmapDimensionEx) { if (arg(1)) { wr32(arg(1), 0); wr32(arg(1) + 4, 0); } RET(1, 2); }
STUB(ext_gdi32_dll_StretchBlt) {   // (hdc, x, y, w, h, hdcsrc, xs, ys, ws, hs, rop)
    DC *dd = dc(arg(0)), *sd = dc(arg(5));
    if (!dd) RET(0, 11);
    std::lock_guard<std::mutex> lk(display().mx);
    i32 w = (i32)arg(3), h = (i32)arg(4), ws = (i32)arg(8), hs = (i32)arg(9);
    bool ok = w && h && blit(dd, (i32)arg(1), (i32)arg(2), w, h, sd, (i32)arg(6), (i32)arg(7), ws, hs, arg(10));
    display().screen.frame++;
    RET(ok, 11);
}

STUB(ext_gdi32_dll_BitBlt) {
    DC *dd = dc(arg(0)), *sd = dc(arg(5));
    if (!dd) RET(0, 9);
    std::lock_guard<std::mutex> lk(display().mx);
    i32 w = (i32)arg(3), h = (i32)arg(4);
    bool ok = blit(dd, (i32)arg(1), (i32)arg(2), w, h, sd, (i32)arg(6), (i32)arg(7), w, h, arg(8));
    display().screen.frame++;
    RET(ok, 9);
}
STUB(ext_gdi32_dll_PatBlt) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 6);
    std::lock_guard<std::mutex> lk(display().mx);
    bool ok = blit(d, (i32)arg(1), (i32)arg(2), (i32)arg(3), (i32)arg(4), nullptr, 0, 0, 0, 0, arg(5));
    display().screen.frame++;
    RET(ok, 6);
}

STUB(ext_gdi32_dll_MoveToEx) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 4);
    if (arg(3)) { wr32(arg(3), (u32)d->cur_x); wr32(arg(3) + 4, (u32)d->cur_y); }
    d->cur_x = (i32)arg(1); d->cur_y = (i32)arg(2);
    RET(1, 4);
}
STUB(ext_gdi32_dll_GetCurrentPositionEx) { DC *d = dc(arg(0)); if (d) { wr32(arg(1), (u32)d->cur_x); wr32(arg(1) + 4, (u32)d->cur_y); } RET(d != nullptr, 2); }
STUB(ext_gdi32_dll_LineTo) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 3);
    GObj *p = obj(d->pen);
    if (p && p->style != 5) {
        Lock lk(d);
        Surf s;
        if (surf_of(d, s)) line(s, d->cur_x, d->cur_y, (i32)arg(1), (i32)arg(2), raw_of(s, p->color));
    }
    d->cur_x = (i32)arg(1); d->cur_y = (i32)arg(2);
    RET(1, 3);
}
STUB(ext_gdi32_dll_Rectangle) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 5);
    i32 l = (i32)arg(1), t = (i32)arg(2), r = (i32)arg(3), b = (i32)arg(4);
    Lock lk(d);
    Surf s;
    if (!surf_of(d, s)) RET(0, 5);
    GObj *br = obj(d->brush), *pn = obj(d->pen);
    if (br && br->style != 1) fill(s, {l + 1, t + 1, r - 1, b - 1}, raw_of(s, br->color));
    if (pn && pn->style != 5) {
        u32 v = raw_of(s, pn->color);
        line(s, l, t, r - 1, t, v); line(s, r - 1, t, r - 1, b - 1, v); line(s, r - 1, b - 1, l, b - 1, v); line(s, l, b - 1, l, t, v);
    }
    RET(1, 5);
}
STUB(ext_gdi32_dll_GetPixel) {
    DC *d = dc(arg(0));
    Surf s;
    if (!d || !surf_of(d, s)) RET(0xFFFFFFFFu, 3);
    i32 x = (i32)arg(1) + s.ox, y = (i32)arg(2) + s.oy;
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) RET(0xFFFFFFFFu, 3);
    const u8 *p = s.px + (size_t)y * s.stride + (size_t)x * s.bypp;
    u32 c;
    if (s.bypp == 1) c = display().screen.pal[*p];
    else if (s.bypp == 2) { u32 v = (u32)p[0] | (u32)p[1] << 8; c = ((v >> 11) & 31) << 19 | ((v >> 5) & 63) << 10 | (v & 31) << 3; }
    else { std::memcpy(&c, p, 4); c &= 0xFFFFFF; }
    RET(((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16), 3);
}
STUB(ext_gdi32_dll_GetNearestColor) { RET(arg(1), 2); }

// ---------------------------------------------------------------- figuras: polígonos, elipses, arcos

namespace {
// contorno con la pluma del DC (grosor > 1: se estampa un cuadrado)
void pen_line(Surf &s, DC *d, i32 x0, i32 y0, i32 x1, i32 y1, bool last) {
    GObj *p = obj(d->pen);
    if (!p || p->style == 5) return;   // PS_NULL
    u32 v = raw_of(s, p->color);
    int wdt = std::max(1, p->width);
    i32 dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        bool end = x0 == x1 && y0 == y1;
        if (end && !last) break;
        if (wdt == 1) put_pixel(s, x0, y0, v);
        else for (int a = -wdt / 2; a < wdt - wdt / 2; a++) for (int b = -wdt / 2; b < wdt - wdt / 2; b++) put_pixel(s, x0 + a, y0 + b, v);
        if (end) break;
        i32 e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
// relleno por líneas de barrido (centros de píxel; ALTERNATE o WINDING)
void fill_polygon(Surf &s, const std::vector<std::pair<double, double>> &pts, u32 v, bool winding) {
    if (pts.size() < 3) return;
    double ymin = 1e9, ymax = -1e9;
    for (auto &p : pts) { ymin = std::min(ymin, p.second); ymax = std::max(ymax, p.second); }
    for (i32 y = (i32)std::floor(ymin); y <= (i32)std::ceil(ymax); y++) {
        double cy = y + 0.5;
        std::vector<std::pair<double, int>> xs;
        for (size_t i = 0; i < pts.size(); i++) {
            auto a = pts[i], b = pts[(i + 1) % pts.size()];
            if ((a.second <= cy && b.second > cy) || (b.second <= cy && a.second > cy)) {
                double x = a.first + (cy - a.second) * (b.first - a.first) / (b.second - a.second);
                xs.push_back({x, a.second < b.second ? 1 : -1});
            }
        }
        std::sort(xs.begin(), xs.end());
        int wn = 0;
        for (size_t i = 0; i + 1 < xs.size(); i++) {
            wn += xs[i].second;
            bool in = winding ? wn != 0 : (i % 2 == 0);
            if (!in) continue;
            for (i32 x = (i32)std::ceil(xs[i].first - 0.5); x < (i32)std::ceil(xs[i + 1].first - 0.5); x++) put_pixel(s, x, y, v);
        }
    }
}
std::vector<std::pair<double, double>> read_points(u32 p, u32 n) {
    std::vector<std::pair<double, double>> v;
    for (u32 i = 0; i < n; i++) v.push_back({(double)(i32)rd32(p + 8 * i), (double)(i32)rd32(p + 8 * i + 4)});
    return v;
}
// puntos de una elipse inscrita en [l,t,r,b) (para rellenos y contornos)
std::vector<std::pair<double, double>> ellipse_points(i32 l, i32 t, i32 r, i32 b, double a0 = 0, double a1 = 2 * M_PI) {
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0, rx = (r - l) / 2.0, ry = (b - t) / 2.0;
    int n = std::max(16, (int)((rx + ry) * 1.5));
    std::vector<std::pair<double, double>> v;
    for (int i = 0; i <= n; i++) {
        double a = a0 + (a1 - a0) * i / n;
        v.push_back({cx + rx * std::cos(a), cy - ry * std::sin(a)});
    }
    return v;
}
double angle_of(i32 l, i32 t, i32 r, i32 b, i32 x, i32 y) {
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0, rx = std::max(1.0, (r - l) / 2.0), ry = std::max(1.0, (b - t) / 2.0);
    double a = std::atan2(-(y - cy) / ry, (x - cx) / rx);
    return a < 0 ? a + 2 * M_PI : a;
}
u32 g_poly_fill_mode = 1;
void shape(DC *d, const std::vector<std::pair<double, double>> &pts, bool closed, bool filled) {
    Lock lk(d);
    Surf s;
    if (!surf_of(d, s)) return;
    GObj *br = obj(d->brush);
    if (filled && br && br->style != 1) fill_polygon(s, pts, raw_of(s, br->color), g_poly_fill_mode == 2);
    for (size_t i = 0; i + 1 < pts.size(); i++)
        pen_line(s, d, (i32)std::lround(pts[i].first), (i32)std::lround(pts[i].second), (i32)std::lround(pts[i + 1].first), (i32)std::lround(pts[i + 1].second), false);
    if (closed && pts.size() > 2)
        pen_line(s, d, (i32)std::lround(pts.back().first), (i32)std::lround(pts.back().second), (i32)std::lround(pts[0].first), (i32)std::lround(pts[0].second), false);
}
}  // namespace

STUB(ext_gdi32_dll_Polygon) { DC *d = dc(arg(0)); if (d) shape(d, read_points(arg(1), arg(2)), true, true); RET(1, 3); }
STUB(ext_gdi32_dll_Polyline) { DC *d = dc(arg(0)); if (d) shape(d, read_points(arg(1), arg(2)), false, false); RET(1, 3); }
STUB(ext_gdi32_dll_PolylineTo) {
    DC *d = dc(arg(0));
    if (!d) RET(0, 3);
    auto pts = read_points(arg(1), arg(2));
    pts.insert(pts.begin(), {(double)d->cur_x, (double)d->cur_y});
    shape(d, pts, false, false);
    if (arg(2)) { d->cur_x = (i32)pts.back().first; d->cur_y = (i32)pts.back().second; }
    RET(1, 3);
}
STUB(ext_gdi32_dll_PolyPolygon) {   // (hdc, pts, counts, n)
    DC *d = dc(arg(0));
    u32 p = arg(1);
    for (u32 i = 0; d && i < arg(3); i++) { u32 c = rd32(arg(2) + 4 * i); shape(d, read_points(p, c), true, true); p += 8 * c; }
    RET(1, 4);
}
STUB(ext_gdi32_dll_PolyBezierTo) {   // segmentos cúbicos desde la posición actual
    DC *d = dc(arg(0));
    if (!d) RET(0, 3);
    auto c = read_points(arg(1), arg(2));
    std::vector<std::pair<double, double>> pts{{(double)d->cur_x, (double)d->cur_y}};
    for (size_t i = 0; i + 2 < c.size(); i += 3) {
        auto p0 = pts.back();
        for (int k = 1; k <= 16; k++) {
            double t = k / 16.0, u = 1 - t;
            pts.push_back({u * u * u * p0.first + 3 * u * u * t * c[i].first + 3 * u * t * t * c[i + 1].first + t * t * t * c[i + 2].first,
                           u * u * u * p0.second + 3 * u * u * t * c[i].second + 3 * u * t * t * c[i + 1].second + t * t * t * c[i + 2].second});
        }
    }
    shape(d, pts, false, false);
    d->cur_x = (i32)pts.back().first; d->cur_y = (i32)pts.back().second;
    RET(1, 3);
}
STUB(ext_gdi32_dll_PolyDraw) { RET(1, 4); }
STUB(ext_gdi32_dll_Ellipse) { DC *d = dc(arg(0)); if (d) shape(d, ellipse_points((i32)arg(1), (i32)arg(2), (i32)arg(3) - 1, (i32)arg(4) - 1), true, true); RET(1, 5); }
STUB(ext_gdi32_dll_RoundRect) {   // (hdc, l, t, r, b, ancho y alto de la elipse de las esquinas)
    DC *d = dc(arg(0));
    if (!d) RET(0, 7);
    i32 l = (i32)arg(1), t = (i32)arg(2), r = (i32)arg(3) - 1, b = (i32)arg(4) - 1;
    double ex = std::min<double>((i32)arg(5), r - l) / 2.0, ey = std::min<double>((i32)arg(6), b - t) / 2.0;
    std::vector<std::pair<double, double>> pts;
    auto corner = [&](double cx, double cy, double a0) {
        for (int k = 0; k <= 8; k++) { double a = a0 + M_PI / 2 * k / 8; pts.push_back({cx + ex * std::cos(a), cy - ey * std::sin(a)}); }
    };
    corner(r - ex, t + ey, 0); corner(l + ex, t + ey, M_PI / 2); corner(l + ex, b - ey, M_PI); corner(r - ex, b - ey, 3 * M_PI / 2);
    shape(d, pts, true, true);
    RET(1, 7);
}
static void arc_like(int kind, bool to) {   // 0 Arc, 1 Pie, 2 Chord: (hdc, l, t, r, b, x1, y1, x2, y2)
    DC *d = dc(arg(0));
    if (!d) return;
    i32 l = (i32)arg(1), t = (i32)arg(2), r = (i32)arg(3), b = (i32)arg(4);
    double a0 = angle_of(l, t, r, b, (i32)arg(5), (i32)arg(6)), a1 = angle_of(l, t, r, b, (i32)arg(7), (i32)arg(8));
    if (a1 <= a0) a1 += 2 * M_PI;   // en sentido contrario a las agujas del reloj
    auto pts = ellipse_points(l, t, r - 1, b - 1, a0, a1);
    if (kind == 1) pts.push_back({(l + r - 1) / 2.0, (t + b - 1) / 2.0});
    if (to && !pts.empty()) pts.insert(pts.begin(), {(double)d->cur_x, (double)d->cur_y});
    shape(d, pts, kind != 0, kind != 0);
    if (to && !pts.empty()) { d->cur_x = (i32)pts.back().first; d->cur_y = (i32)pts.back().second; }
}
STUB(ext_gdi32_dll_Arc) { arc_like(0, false); RET(1, 9); }
STUB(ext_gdi32_dll_ArcTo) { arc_like(0, true); RET(1, 9); }
STUB(ext_gdi32_dll_Pie) { arc_like(1, false); RET(1, 9); }
STUB(ext_gdi32_dll_Chord) { arc_like(2, false); RET(1, 9); }
STUB(ext_gdi32_dll_SetArcDirection) { RET(1, 2); }
STUB(ext_gdi32_dll_SetPolyFillMode) { u32 o = g_poly_fill_mode; g_poly_fill_mode = arg(1); RET(o, 2); }
STUB(ext_gdi32_dll_GetPolyFillMode) { RET(g_poly_fill_mode, 1); }
static u32 set_pixel(u32 hdc, i32 x, i32 y, u32 c) {
    DC *d = dc(hdc);
    if (!d) return 0xFFFFFFFFu;
    Lock lk(d);
    Surf s;
    if (!surf_of(d, s)) return 0xFFFFFFFFu;
    u32 v = raw_of(s, c);
    put_pixel(s, x, y, v);
    u32 rgb = raw_to_rgb(s, v);
    return ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16);
}
STUB(ext_gdi32_dll_SetPixel) { RET(set_pixel(arg(0), (i32)arg(1), (i32)arg(2), arg(3)), 4); }
STUB(ext_gdi32_dll_SetPixelV) { set_pixel(arg(0), (i32)arg(1), (i32)arg(2), arg(3)); RET(1, 4); }

// ---------------------------------------------------------------- regiones (rectángulo envolvente)

STUB(ext_gdi32_dll_CreateEllipticRgnIndirect) { GObj o{GKind::Region}; o.rgn = rd_rect(arg(0)); RET(new_obj(o), 1); }
STUB(ext_gdi32_dll_CreateEllipticRgn) { GObj o{GKind::Region}; o.rgn = {(i32)arg(0), (i32)arg(1), (i32)arg(2), (i32)arg(3)}; RET(new_obj(o), 4); }
STUB(ext_gdi32_dll_CreateRoundRectRgn) { GObj o{GKind::Region}; o.rgn = {(i32)arg(0), (i32)arg(1), (i32)arg(2), (i32)arg(3)}; RET(new_obj(o), 6); }
STUB(ext_gdi32_dll_CreatePolygonRgn) {
    GObj o{GKind::Region};
    auto pts = read_points(arg(0), arg(1));
    if (!pts.empty()) {
        Rect r{(i32)pts[0].first, (i32)pts[0].second, (i32)pts[0].first, (i32)pts[0].second};
        for (auto &p : pts) { r.l = std::min(r.l, (i32)p.first); r.t = std::min(r.t, (i32)p.second); r.r = std::max(r.r, (i32)p.first); r.b = std::max(r.b, (i32)p.second); }
        o.rgn = r;
    }
    RET(new_obj(o), 3);
}
STUB(ext_gdi32_dll_ExtCreateRegion) {   // (xform, n, RGNDATA*): rcBound de la cabecera
    GObj o{GKind::Region};
    if (arg(2)) o.rgn = rd_rect(arg(2) + 16);
    RET(new_obj(o), 3);
}
STUB(ext_gdi32_dll_GetRgnBox) { GObj *o = obj(arg(0)); if (!o) RET(0, 2); wr_rect(arg(1), o->rgn); RET(o->rgn.w() > 0 && o->rgn.h() > 0 ? 2 : 1, 2); }
STUB(ext_gdi32_dll_PtInRegion) { GObj *o = obj(arg(0)); i32 x = (i32)arg(1), y = (i32)arg(2); RET(o && x >= o->rgn.l && x < o->rgn.r && y >= o->rgn.t && y < o->rgn.b, 3); }
STUB(ext_gdi32_dll_RectInRegion) {
    GObj *o = obj(arg(0));
    if (!o) RET(0, 2);
    Rect r = rd_rect(arg(1));
    RET(r.l < o->rgn.r && r.r > o->rgn.l && r.t < o->rgn.b && r.b > o->rgn.t, 2);
}
STUB(ext_gdi32_dll_EqualRgn) { GObj *a = obj(arg(0)), *b = obj(arg(1)); RET(a && b && a->rgn.l == b->rgn.l && a->rgn.t == b->rgn.t && a->rgn.r == b->rgn.r && a->rgn.b == b->rgn.b, 2); }
STUB(ext_gdi32_dll_FillRgn) { GObj *o = obj(arg(1)); if (o) gdi_fill_rect(arg(0), o->rgn, arg(2)); RET(1, 3); }
STUB(ext_gdi32_dll_PaintRgn) { GObj *o = obj(arg(1)); DC *d = dc(arg(0)); if (o && d) gdi_fill_rect(arg(0), o->rgn, d->brush); RET(1, 2); }
STUB(ext_gdi32_dll_FrameRgn) {   // (hdc, rgn, brush, w, h)
    GObj *o = obj(arg(1));
    if (o) {
        Rect r = o->rgn;
        i32 w = (i32)arg(3), h = (i32)arg(4);
        gdi_fill_rect(arg(0), {r.l, r.t, r.r, r.t + h}, arg(2)); gdi_fill_rect(arg(0), {r.l, r.b - h, r.r, r.b}, arg(2));
        gdi_fill_rect(arg(0), {r.l, r.t, r.l + w, r.b}, arg(2)); gdi_fill_rect(arg(0), {r.r - w, r.t, r.r, r.b}, arg(2));
    }
    RET(1, 5);
}

// ---------------------------------------------------------------- objetos y DC: más

STUB(ext_gdi32_dll_CreateBrushIndirect) {   // LOGBRUSH { style, color, hatch }
    GObj o{GKind::Brush};
    o.style = rd32(arg(0)) == 1 ? 1 : 0;   // BS_NULL; el resto como sólida
    o.color = rd32(arg(0) + 4);
    RET(new_obj(o), 1);
}
STUB(ext_gdi32_dll_CreateDIBPatternBrushPt) {   // color medio del patrón: el primero de su tabla
    GObj o{GKind::Brush};
    u32 bmi = arg(0);
    u32 q = rd16(bmi + 14) <= 8 ? rd32(bmi + rd32(bmi)) : 0xC0C0C0;
    o.color = ((q >> 16) & 0xFF) | (q & 0xFF00) | ((q & 0xFF) << 16);
    RET(new_obj(o), 2);
}
STUB(ext_gdi32_dll_CreatePenIndirect) {   // LOGPEN { style, POINT width, color }
    GObj o{GKind::Pen};
    o.style = rd32(arg(0)); o.width = (int)rd32(arg(0) + 4); o.color = rd32(arg(0) + 12);
    RET(new_obj(o), 1);
}
STUB(ext_gdi32_dll_ExtCreatePen) {   // (style, width, LOGBRUSH*, n, styles)
    GObj o{GKind::Pen};
    o.style = arg(0) & 0xF; o.width = (int)arg(1); o.color = arg(2) ? rd32(arg(2) + 4) : 0;
    RET(new_obj(o), 5);
}
STUB(ext_gdi32_dll_GetCurrentObject) {   // OBJ_PEN 1, BRUSH 2, PAL 5, FONT 6, BITMAP 7
    DC *d = dc(arg(0));
    if (!d) RET(0, 2);
    switch (arg(1)) { case 1: RET(d->pen, 2); case 2: RET(d->brush, 2); case 5: RET(d->palette, 2); case 6: RET(d->font, 2); case 7: RET(d->bitmap, 2); }
    RET(0, 2);
}
STUB(ext_gdi32_dll_GetBrushOrgEx) { wr32(arg(1), 0); wr32(arg(1) + 4, 0); RET(1, 2); }
STUB(ext_gdi32_dll_OffsetWindowOrgEx) { if (arg(3)) { wr32(arg(3), 0); wr32(arg(3) + 4, 0); } RET(1, 4); }
STUB(ext_gdi32_dll_ScaleViewportExtEx) { if (arg(5)) { wr32(arg(5), 1); wr32(arg(5) + 4, 1); } RET(1, 6); }
STUB(ext_gdi32_dll_ScaleWindowExtEx) { if (arg(5)) { wr32(arg(5), 1); wr32(arg(5) + 4, 1); } RET(1, 6); }
STUB(ext_gdi32_dll_SetColorAdjustment) { RET(1, 2); }
STUB(ext_gdi32_dll_CreateICA) { RET(gdi_window_dc(0, true), 4); }   // contexto de información de la pantalla
STUB(ext_gdi32_dll_ResetDCA) { RET(arg(0), 2); }
STUB(ext_gdi32_dll_GetSystemPaletteEntries) {   // (hdc, start, n, PALETTEENTRY*)
    for (u32 i = 0; i < arg(2) && arg(1) + i < 256; i++) {
        u32 c = display().screen.pal[arg(1) + i];
        if (arg(3)) wr32(arg(3) + 4 * i, ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16));
    }
    RET(g_bpp == 8 ? std::min<u32>(arg(2), 256) : 0, 4);
}
STUB(ext_gdi32_dll_GetNearestPaletteIndex) {
    GObj *p = obj(arg(0));
    if (!p || p->pal.empty()) RET(0, 2);
    std::vector<u32> rgb;
    for (u32 c : p->pal) rgb.push_back(c);
    RET(nearest_in(rgb.data(), (int)rgb.size(), colorref_to_rgb(arg(1) & 0xFFFFFF)), 2);
}
STUB(ext_gdi32_dll_CreateHalftonePalette) {
    GObj o{GKind::Palette};
    for (int r = 0; r < 6; r++) for (int g = 0; g < 6; g++) for (int b = 0; b < 6; b++) o.pal.push_back((u32)(r * 51) << 16 | (u32)(g * 51) << 8 | (u32)(b * 51));
    RET(new_obj(o), 1);
}
STUB(ext_gdi32_dll_TranslateCharsetInfo) {   // (src, CHARSETINFO*, flags): solo occidental (1252)
    u32 p = arg(1);
    if (p) { wr32(p, 0); wr32(p + 4, 1252); std::memset(MEM + p + 8, 0, 24); wr32(p + 8 + 16, 1); }
    RET(1, 3);
}
// las tres fuentes que hay (las Liberation hacen de Arial, Times New Roman y Courier New)
static void enum_fonts(u32 cb, u32 lp, const std::string &only) {
    static const char *faces[] = {"Arial", "MS Sans Serif", "Times New Roman", "Courier New"};
    u32 lf = heap_alloc(60 + 148 + 64, true), tm = lf + 60 + 64;
    for (const char *f : faces) {
        if (!only.empty() && strcasecmp(only.c_str(), f)) continue;
        std::memset(MEM + lf, 0, 60 + 148 + 64);
        wr32(lf, (u32)-13);
        wr32(lf + 16, 400);
        wr8(lf + 27, !strcmp(f, "Courier New") ? 0x31 : !strcmp(f, "Times New Roman") ? 0x12 : 0x22);
        gstrcpy(lf + 28, f, 32);
        wr32(tm, 16); wr32(tm + 4, 13); wr32(tm + 8, 3); wr32(tm + 20, 7); wr32(tm + 24, 14); wr32(tm + 28, 400);
        if (!guest_call(cb, {lf, tm, 4 /* TRUETYPE_FONTTYPE */, lp})) break;
    }
    heap_free(lf);
}
STUB(ext_gdi32_dll_EnumFontsA) { enum_fonts(arg(2), arg(3), arg(1) ? gstr(arg(1)) : ""); RET(1, 4); }
STUB(ext_gdi32_dll_EnumFontFamiliesExA) { enum_fonts(arg(2), arg(3), arg(1) ? gstr(arg(1) + 28) : ""); RET(1, 5); }
// texto Unicode: se convierte a Windows-1252
static u32 narrow_tmp(u32 w, u32 n) {
    u32 p = heap_alloc(n + 1);
    for (u32 i = 0; i < n; i++) { u32 c = rd16(w + 2 * i); wr8(p + i, c < 256 ? c : '?'); }
    wr8(p + n, 0);
    return p;
}
extern "C" void ext_gdi32_dll_TextOutA(void);
extern "C" void ext_gdi32_dll_GetTextExtentPoint32A(void);
extern "C" void ext_gdi32_dll_GetTextMetricsA(void);
STUB(ext_gdi32_dll_TextOutW) { u32 p = narrow_tmp(arg(3), arg(4)); wr32(esp + 16, p); ext_gdi32_dll_TextOutA(); heap_free(p); }
STUB(ext_gdi32_dll_GetTextExtentPoint32W) { u32 p = narrow_tmp(arg(1), arg(2)); wr32(esp + 8, p); ext_gdi32_dll_GetTextExtentPoint32A(); heap_free(p); }
STUB(ext_gdi32_dll_GetTextMetricsW) {   // TEXTMETRICW: los caracteres primero/último… son WCHAR
    u32 out = arg(1), tmp = heap_alloc(64, true);
    wr32(esp + 8, tmp);
    ext_gdi32_dll_GetTextMetricsA();
    std::memcpy(MEM + out, MEM + tmp, 44);
    wr16(out + 44, 0x20); wr16(out + 46, 0xFFFF); wr16(out + 48, 0x1F); wr16(out + 50, 0x20);
    std::memcpy(MEM + out + 52, MEM + tmp + 48, 5);
    heap_free(tmp);
}
// caminos (paths): no hay; las figuras se dibujan directamente
STUB(ext_gdi32_dll_BeginPath) { RET(1, 1); }
STUB(ext_gdi32_dll_EndPath) { RET(1, 1); }
STUB(ext_gdi32_dll_WidenPath) { RET(1, 1); }
STUB(ext_gdi32_dll_PathToRegion) { GObj o{GKind::Region}; RET(new_obj(o), 1); }
STUB(ext_gdi32_dll_SelectClipPath) { RET(1, 2); }
// metarchivos: no hay (los controles los ofrecen como formato alternativo)
STUB(ext_gdi32_dll_CreateMetaFileA) { RET(0, 1); }
STUB(ext_gdi32_dll_CloseMetaFile) { RET(0, 1); }
STUB(ext_gdi32_dll_DeleteMetaFile) { RET(1, 1); }
STUB(ext_gdi32_dll_CopyMetaFileA) { RET(0, 2); }
STUB(ext_gdi32_dll_GetMetaFileA) { RET(0, 1); }
STUB(ext_gdi32_dll_PlayMetaFile) { RET(1, 2); }
STUB(ext_gdi32_dll_PlayMetaFileRecord) { RET(1, 4); }
STUB(ext_gdi32_dll_EnumMetaFile) { RET(1, 4); }
STUB(ext_gdi32_dll_GetMetaFileBitsEx) { RET(0, 3); }
STUB(ext_gdi32_dll_SetMetaFileBitsEx) { RET(0, 2); }
STUB(ext_gdi32_dll_CreateEnhMetaFileA) { RET(0, 4); }
STUB(ext_gdi32_dll_CloseEnhMetaFile) { RET(0, 1); }
STUB(ext_gdi32_dll_DeleteEnhMetaFile) { RET(1, 1); }
STUB(ext_gdi32_dll_CopyEnhMetaFileA) { RET(0, 2); }
STUB(ext_gdi32_dll_PlayEnhMetaFile) { RET(1, 3); }
STUB(ext_gdi32_dll_GetEnhMetaFileBits) { RET(0, 3); }
STUB(ext_gdi32_dll_SetEnhMetaFileBits) { RET(0, 2); }
STUB(ext_gdi32_dll_GetEnhMetaFileHeader) { RET(0, 3); }
STUB(ext_gdi32_dll_GetWinMetaFileBits) { RET(0, 5); }

// ---------------------------------------------------------------- texto (los juegos usan sus propias fuentes; esto es para MFC y VB)

namespace {
FontSpec dc_font(DC *d) {
    GObj *f = d ? obj(d->font) : nullptr;
    if (!f || f->kind != GKind::Font) return stock_font(13);
    if (f->stock_font >= 0) return stock_font(f->stock_font);
    return font_from_logfont(f->logfont.data());
}

// una línea de texto en (x, y) según la alineación del DC; devuelve el ancho dibujado
int text_out(DC *d, i32 x, i32 y, const char *str, size_t n, u32 options, const Rect *rc, const std::vector<int> *dx) {
    FontSpec f = dc_font(d);
    TextMetrics tm = text_metrics(f);
    int w = 0;
    if (dx) for (size_t i = 0; i < n && i < dx->size(); i++) w += (*dx)[i];
    else w = text_width(f, str, n);
    u32 al = d->text_align;
    if (al & 1) { x = d->cur_x; y = d->cur_y; }                      // TA_UPDATECP
    if ((al & 6) == 6) x -= w / 2;                                   // TA_CENTER
    else if (al & 2) x -= w;                                         // TA_RIGHT
    if ((al & 24) == 24) y -= tm.ascent;                             // TA_BASELINE
    else if (al & 8) y -= tm.height;                                 // TA_BOTTOM
    if (al & 1) d->cur_x = x + ((al & 6) ? 0 : w);
    Lock lk(d);
    Surf s;
    if (!surf_of(d, s)) return w;
    Rect saved = s.clip;
    if (rc && (options & 4)) {                                       // ETO_CLIPPED
        Rect c{rc->l + s.ox, rc->t + s.oy, rc->r + s.ox, rc->b + s.oy};
        s.clip = {std::max(s.clip.l, c.l), std::max(s.clip.t, c.t), std::min(s.clip.r, c.r), std::min(s.clip.b, c.b)};
    }
    if (rc && (options & 2)) fill(s, *rc, raw_of(s, d->bk_color));          // ETO_OPAQUE
    else if (d->bk_mode == 2) fill(s, {x, y, x + w, y + tm.height}, raw_of(s, d->bk_color));   // OPAQUE
    u32 ink = raw_of(s, d->text_color);
    draw_text_run(f, str, n, x, y, dx, [&](int px, int py) { put_pixel(s, px, py, ink); });
    s.clip = saved;
    return w;
}
}  // namespace

// DrawText (user32): líneas, alineación y saltos de palabra en un rectángulo
int gdi_draw_text(u32 hdc, const std::string &text, Rect &rc, u32 fmt) {
    {   // PCF7_TEXTWATCH: quién pinta este texto, en qué rectángulo y con qué formato (diagnóstico)
        static const char *tw = std::getenv("PCF7_TEXTWATCH");
        if (tw && text.find(tw) != std::string::npos) plog("gdi_draw_text «%s» rect %d,%d-%d,%d fmt 0x%x (f_%x)", text.c_str(), rc.l, rc.t, rc.r, rc.b, fmt, cur_fn);
    }
    DC *d = dc(hdc);
    if (!d) return 0;
    FontSpec f = dc_font(d);
    TextMetrics tm = text_metrics(f);
    // prefijos & (DT_NOPREFIX = 0x800): se quitan y se subraya la letra siguiente
    std::vector<std::string> lines;
    std::vector<int> under;   // posición subrayada por línea (-1 ninguna)
    auto width = [&](const std::string &x) { return text_width(f, x.data(), x.size()); };
    std::string cur;
    int ul = -1;
    auto flush = [&]() { lines.push_back(cur); under.push_back(ul); cur.clear(); ul = -1; };
    bool single = fmt & 0x20, wrap = (fmt & 0x10) && !single;   // DT_SINGLELINE, DT_WORDBREAK
    int maxw = rc.w();
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (!(fmt & 0x800) && c == '&') {
            if (i + 1 < text.size() && text[i + 1] == '&') { cur.push_back('&'); i++; continue; }
            ul = (int)cur.size();
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n' && !single) { flush(); continue; }
        if (c == '\t' && (fmt & 0x40)) { cur += "        "; continue; }   // DT_EXPANDTABS
        cur.push_back(c == '\n' ? ' ' : c);
        if (wrap && width(cur) > maxw && cur.size() > 1) {
            size_t sp = cur.find_last_of(' ');
            if (sp != std::string::npos && sp > 0) {
                std::string rest = cur.substr(sp + 1);
                cur.resize(sp);
                flush();
                cur = rest;
            } else {
                char last = cur.back();
                cur.pop_back();
                flush();
                cur.push_back(last);
            }
        }
    }
    if (!cur.empty() || lines.empty()) flush();
    int lh = tm.height + ((fmt & 0x80) ? tm.external_leading : 0);   // DT_EXTERNALLEADING
    int total = lh * (int)lines.size();
    int maxlw = 0;
    for (auto &l : lines) maxlw = std::max(maxlw, width(l));
    if (fmt & 0x400) {   // DT_CALCRECT
        rc.r = rc.l + maxlw;
        rc.b = rc.t + total;
        return total;
    }
    int y = rc.t;
    if (single && (fmt & 4)) y = rc.t + (rc.h() - tm.height) / 2;   // DT_VCENTER
    else if (single && (fmt & 8)) y = rc.b - tm.height;              // DT_BOTTOM
    u32 saved_align = d->text_align;
    d->text_align = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string l = lines[i];
        if ((fmt & 0x8000) && width(l) > rc.w()) {   // DT_END_ELLIPSIS
            while (!l.empty() && width(l + "...") > rc.w()) l.pop_back();
            l += "...";
        }
        int lw = width(l);
        int x = rc.l;
        if (fmt & 1) x = rc.l + (rc.w() - lw) / 2;        // DT_CENTER
        else if (fmt & 2) x = rc.r - lw;                  // DT_RIGHT
        Rect clip = rc;
        text_out(d, x, y, l.data(), l.size(), (fmt & 0x100) ? 0 : 4, &clip, nullptr);   // DT_NOCLIP
        if (under[i] >= 0 && under[i] < (int)l.size()) {
            int ux = x + text_width(f, l.data(), (size_t)under[i]);
            int uw = char_advance(f, (u8)l[(size_t)under[i]]);
            Lock lk(d);
            Surf s;
            if (surf_of(d, s)) { u32 ink = raw_of(s, d->text_color); for (int k = 0; k < uw; k++) put_pixel(s, ux + k, y + tm.ascent + 1, ink); }
        }
        y += lh;
    }
    d->text_align = saved_align;
    return total;
}

STUB(ext_gdi32_dll_CreateFontA) {   // (h, w, esc, orient, weight, italic, underline, strike, charset, out, clip, quality, pitch, face)
    GObj o{GKind::Font};
    o.logfont.assign(60, 0);
    u8 *lf = o.logfont.data();
    for (int i = 0; i < 5; i++) { u32 v = arg((u32)i); std::memcpy(lf + 4 * i, &v, 4); }
    for (int i = 0; i < 8; i++) lf[20 + i] = (u8)arg(5 + (u32)i);
    if (arg(13)) { std::string f = gstr(arg(13), 31); std::memcpy(lf + 28, f.c_str(), f.size()); }
    RET(new_obj(o), 14);
}
STUB(ext_gdi32_dll_GetTextMetricsA) {
    u32 p = arg(1);
    FontSpec f = dc_font(dc(arg(0)));
    TextMetrics t = text_metrics(f);
    std::memset(MEM + p, 0, 56);
    wr32(p, (u32)t.height); wr32(p + 4, (u32)t.ascent); wr32(p + 8, (u32)t.descent); wr32(p + 12, (u32)t.internal_leading);
    wr32(p + 16, (u32)t.external_leading); wr32(p + 20, (u32)t.ave_width); wr32(p + 24, (u32)t.max_width); wr32(p + 28, (u32)t.weight);
    wr32(p + 36, 96); wr32(p + 40, 96);
    wr8(p + 44, 0x20); wr8(p + 45, 0xFF); wr8(p + 46, 0x1F); wr8(p + 47, 0x20);
    wr8(p + 48, f.italic); wr8(p + 49, f.underline); wr8(p + 50, f.strikeout); wr8(p + 51, t.pitch_family);
    RET(1, 2);
}
static void text_extent(u32 hdc, u32 str, u32 n, u32 size) {
    FontSpec f = dc_font(dc(hdc));
    std::string s = gstr(str, n);
    s.resize(n, ' ');
    wr32(size, (u32)text_width(f, s.data(), n));
    wr32(size + 4, (u32)text_metrics(f).height);
}
STUB(ext_gdi32_dll_GetTextExtentPointA) { text_extent(arg(0), arg(1), arg(2), arg(3)); RET(1, 4); }
STUB(ext_gdi32_dll_GetTextExtentPoint32A) { text_extent(arg(0), arg(1), arg(2), arg(3)); RET(1, 4); }
STUB(ext_gdi32_dll_GetCharWidthA) {
    FontSpec f = dc_font(dc(arg(0)));
    for (u32 c = arg(1); c <= arg(2) && c < 256; c++) wr32(arg(3) + 4 * (c - arg(1)), (u32)char_advance(f, (u8)c));
    RET(1, 4);
}
STUB(ext_gdi32_dll_GetCharWidth32A) {
    FontSpec f = dc_font(dc(arg(0)));
    for (u32 c = arg(1); c <= arg(2) && c < 256; c++) wr32(arg(3) + 4 * (c - arg(1)), (u32)char_advance(f, (u8)c));
    RET(1, 4);
}
STUB(ext_gdi32_dll_GetTextFaceW) {
    std::string f = dc_font(dc(arg(0))).face;
    u32 n = std::min<u32>((u32)f.size(), arg(1) ? arg(1) - 1 : 0);
    for (u32 i = 0; i < n; i++) wr16(arg(2) + 2 * i, (u8)f[i]);
    if (arg(1)) wr16(arg(2) + 2 * n, 0);
    RET(n, 3);
}
STUB(ext_gdi32_dll_GetTextFaceA) { RET(gstrcpy(arg(2), dc_font(dc(arg(0))).face, arg(1)), 3); }
void text_watch(const std::string &s);   // core.cpp (PCF7_TEXTWATCH)
STUB(ext_gdi32_dll_TextOutA) {
    text_watch(std::string((const char *)MEM + arg(3), arg(4)));
    static const bool tl = std::getenv("PCF7_TEXTLOG") != nullptr;   // diagnóstico: cada TextOutA
    if (tl) plog("TextOutA(%u, %d, %d, «%s», %u)", arg(0), (int)arg(1), (int)arg(2), std::string((const char *)MEM + arg(3), arg(4)).c_str(), arg(4));
    DC *d = dc(arg(0));
    if (!d) RET(0, 5);
    std::string s = gstr(arg(3), arg(4));
    s.resize(arg(4), ' ');
    text_out(d, (i32)arg(1), (i32)arg(2), s.data(), s.size(), 0, nullptr, nullptr);
    RET(1, 5);
}
STUB(ext_gdi32_dll_ExtTextOutA) {   // (hdc, x, y, options, rect, str, n, dx)
    if (arg(5)) text_watch(std::string((const char *)MEM + arg(5), arg(6)));
    DC *d = dc(arg(0));
    if (!d) RET(0, 8);
    u32 n = arg(6);
    std::string s = arg(5) ? gstr(arg(5), n) : std::string();
    s.resize(arg(5) ? n : 0, ' ');
    Rect r;
    if (arg(4)) r = rd_rect(arg(4));
    std::vector<int> dx;
    if (arg(7)) for (u32 i = 0; i < n; i++) dx.push_back((i32)rd32(arg(7) + 4 * i));
    if (s.empty() && arg(4) && (arg(3) & 2)) {   // solo rellenar el rectángulo
        Lock lk(d);
        Surf sf;
        if (surf_of(d, sf)) fill(sf, r, raw_of(sf, d->bk_color));
        RET(1, 8);
    }
    text_out(d, (i32)arg(1), (i32)arg(2), s.data(), s.size(), arg(3), arg(4) ? &r : nullptr, arg(7) ? &dx : nullptr);
    RET(1, 8);
}
STUB(ext_gdi32_dll_SetTextJustification) { RET(1, 3); }
STUB(ext_gdi32_dll_SetTextCharacterExtra) { RET(0, 2); }
STUB(ext_gdi32_dll_SetMapperFlags) { RET(0, 2); }
STUB(ext_gdi32_dll_EnumFontFamiliesA) { RET(1, 4); }

// ---------------------------------------------------------------- impresión: no hay impresora

STUB(ext_gdi32_dll_StartDocA) { RET(0, 2); }
STUB(ext_gdi32_dll_EndDoc) { RET(0, 1); }
STUB(ext_gdi32_dll_StartPage) { RET(0, 1); }
STUB(ext_gdi32_dll_EndPage) { RET(0, 1); }
STUB(ext_gdi32_dll_AbortDoc) { RET(0, 1); }
STUB(ext_gdi32_dll_SetAbortProc) { RET(0, 2); }
STUB(ext_gdi32_dll_Escape) { RET(0, 5); }

// ---------------------------------------------------------------- para los controles (controls.cpp)

extern "C" void ext_gdi32_dll_SelectObject(void);

u32 gdi_select(u32 hdc, u32 hobj) {
    return call_stub(ext_gdi32_dll_SelectObject, {hdc, hobj});
}
void gdi_set_text_color(u32 hdc, u32 c) { if (DC *d = dc(hdc)) d->text_color = c; }
void gdi_set_bk(u32 hdc, u32 c, int mode) { if (DC *d = dc(hdc)) { d->bk_color = c; d->bk_mode = (u32)mode; } }
int gdi_dc_text_width(u32 hdc, const std::string &s) { return text_width(dc_font(dc(hdc)), s.data(), s.size()); }
int gdi_dc_text_height(u32 hdc) { return text_metrics(dc_font(dc(hdc))).height; }
void gdi_invert(u32 hdc, const Rect &r) {
    DC *d = dc(hdc);
    if (!d) return;
    Lock lk(d);
    Surf s;
    if (!surf_of(d, s)) return;
    Rect a = rect_and({r.l + s.ox, r.t + s.oy, r.r + s.ox, r.b + s.oy}, s.clip);
    for (i32 y = a.t; y < a.b; y++)
        for (i32 x = a.l; x < a.r; x++) {
            if (!s.vis(x, y)) continue;
            u8 *p = s.row(y) + (size_t)x * s.bypp;
            store(p, s.bypp, rgb_to_raw(s, ~raw_to_rgb(s, load(p, s.bypp)) & 0xFFFFFF));
        }
}
void gdi_focus_rect(u32 hdc, const Rect &r) {
    DC *d = dc(hdc);
    if (!d) return;
    Lock lk(d);
    Surf s;
    if (!surf_of(d, s)) return;
    auto inv = [&](i32 x, i32 y) {
        x += s.ox; y += s.oy;
        if (x < s.clip.l || x >= s.clip.r || y < s.clip.t || y >= s.clip.b || !s.vis(x, y)) return;
        u8 *p = s.row(y) + (size_t)x * s.bypp;
        store(p, s.bypp, rgb_to_raw(s, ~raw_to_rgb(s, load(p, s.bypp)) & 0xFFFFFF));
    };
    for (i32 x = r.l; x < r.r; x += 2) { inv(x, r.t); inv(x, r.b - 1); }
    for (i32 y = r.t + 1; y < r.b - 1; y += 2) { inv(r.l, y); inv(r.r - 1, y); }
}

// bitmap centrado en un rectángulo (botones y estáticos con imagen)
void draw_bitmap_center(u32 hdc, u32 hbm, const Rect &r) {
    GObj *b = obj(hbm);
    DC *dd = dc(hdc);
    if (!b || b->kind != GKind::Bitmap || !dd) return;
    DC src;
    src.bitmap = hbm;
    src.text_color = 0; src.bk_color = 0xFFFFFF;
    i32 x = r.l + (r.w() - b->w) / 2, y = r.t + (r.h() - b->h) / 2;
    std::lock_guard<std::mutex> lk(display().mx);
    blit(dd, x, y, b->w, b->h, &src, 0, 0, b->w, b->h, 0xCC0020);
    display().screen.frame++;
}
