// PCF7 — DirectDraw (DirectX 6) para el juego: ddraw.dll!DirectDrawCreate / DirectDrawEnumerateA.
//
// Los objetos COM viven en la memoria del juego ([vtable][refcount][…]) y sus vtables apuntan a
// funciones del host (host_fn). Las superficies tienen su memoria en MEM: el juego hace Lock y escribe
// píxeles directamente. La superficie primaria usa el framebuffer de la pantalla emulada (display.h);
// Flip intercambia la memoria de la primaria y del búfer trasero y cambia el framebuffer visible.
#include "ddraw.h"
#include "display.h"
#include "win.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

void register_dynamic_export(const char *dll, const char *name, void (*f)(void));
void user32_fullscreen_window(u32 hwnd);   // user32.cpp
u32 dd_get_dd7();                          // IDirectDraw7 (abajo)

namespace {

constexpr u32 DD_OK = 0, DDERR_GENERIC = 0x80004005u, E_NOINTERFACE = 0x80004002u,
              DDERR_INVALIDPARAMS = 0x80070057u, DDERR_NOTFOUND = 0x887600FFu, DDERR_UNSUPPORTED = 0x80004001u,
              DDERR_NOPALETTEATTACHED = 0x8876023Cu, DDERR_NOCOLORKEY = 0x887600D7u;

// ---------------------------------------------------------------- GUID de las interfaces

struct Guid { u32 a; u16 b, c; u8 d[8]; };
const Guid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const Guid IID_IDirectDraw = {0x6C14DB80, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectDraw2 = {0xB3A6F3E0, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
const Guid IID_IDirectDraw4 = {0x9C59509A, 0x39BD, 0x11D1, {0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5}};
const Guid IID_IDirectDrawSurface = {0x6C14DB81, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectDrawSurface2 = {0x57805885, 0x6EEC, 0x11CF, {0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27}};
const Guid IID_IDirectDrawSurface3 = {0xDA044E00, 0x69B2, 0x11D0, {0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB}};
const Guid IID_IDirectDrawSurface4 = {0x0B2B8630, 0xAD35, 0x11D0, {0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B}};
// DirectX 7 (PC Fútbol 2000, dcsfx.dll): IDirectDraw7 = IDirectDraw4 + StartModeTest/EvaluateMode; IDirectDrawSurface7 =
// IDirectDrawSurface4 + SetPriority/GetPriority/SetLOD/GetLOD. Mismos objetos, vtables más largas.
const Guid IID_IDirectDraw7 = {0x15E65EC0, 0x3B9C, 0x11D2, {0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B}};
const Guid IID_IDirectDrawSurface7 = {0x06675A80, 0x3B9B, 0x11D2, {0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B}};

bool guid_eq(u32 p, const Guid &g) {
    return rd32(p) == g.a && rd16(p + 4) == g.b && rd16(p + 6) == g.c && std::memcmp(MEM + p + 8, g.d, 8) == 0;
}
std::string guid_str(u32 p) {
    char b[64];
    snprintf(b, sizeof b, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(p), rd16(p + 4), rd16(p + 6),
             MEM[p + 8], MEM[p + 9], MEM[p + 10], MEM[p + 11], MEM[p + 12], MEM[p + 13], MEM[p + 14], MEM[p + 15]);
    return b;
}

// ---------------------------------------------------------------- objetos COM en MEM

u32 make_vtbl(std::initializer_list<void (*)(void)> fns) {
    u32 v = heap_alloc(4 * (u32)fns.size(), true);
    u32 i = 0;
    for (auto f : fns) wr32(v + 4 * i++, host_fn(f));
    return v;
}
u32 com_new(u32 vtbl) {
    u32 o = heap_alloc(16, true);
    wr32(o, vtbl);
    wr32(o + 4, 1);
    return o;
}
u32 addref(u32 o) { u32 n = rd32(o + 4) + 1; wr32(o + 4, n); return n; }
u32 release(u32 o) { u32 n = rd32(o + 4); if (n) n--; wr32(o + 4, n); return n; }

// ---------------------------------------------------------------- estado

struct Palette { u32 obj = 0; u32 rgb[256] = {}; u32 caps = 0; };

std::map<u32, Surface> g_surf;
std::map<u32, Palette> g_pal;
u32 g_vt_dd1 = 0, g_vt_dd2 = 0, g_vt_dd4 = 0, g_vt_surf = 0, g_vt_pal = 0, g_vt_clip = 0;
u32 g_vt_dd7 = 0, g_dd7 = 0;   // DirectX 7
u32 g_dd = 0;           // objeto IDirectDraw (v1)
u32 g_dd4 = 0;          // objeto IDirectDraw4
u32 g_primary = 0;
u32 g_coop_hwnd = 0;
bool g_coop_full = false;

Surface *surf(u32 o) { auto it = g_surf.find(o); return it == g_surf.end() ? nullptr : &it->second; }

void fill_pixel_format(u32 p, int bpp) {
    std::memset(MEM + p, 0, 32);
    wr32(p, 32);
    if (bpp == 8) { wr32(p + 4, 0x60); wr32(p + 12, 8); }   // DDPF_PALETTEINDEXED8 | DDPF_RGB
    else if (bpp == 16) { wr32(p + 4, 0x40); wr32(p + 12, 16); wr32(p + 16, 0xF800); wr32(p + 20, 0x07E0); wr32(p + 24, 0x001F); }
    else { wr32(p + 4, 0x40); wr32(p + 12, 32); wr32(p + 16, 0xFF0000); wr32(p + 20, 0x00FF00); wr32(p + 24, 0x0000FF); }
}
void fill_pixel_format(u32 p, const Surface &s) {
    if (!s.pf_flags) { fill_pixel_format(p, s.bpp); return; }
    std::memset(MEM + p, 0, 32);
    wr32(p, 32);
    wr32(p + 4, s.pf_flags);
    wr32(p + 12, (u32)s.bpp);
    if (s.pf_flags & 0x400) { wr32(p + 20, 0xFFFF); return; }   // DDPF_ZBUFFER
    wr32(p + 16, s.rmask); wr32(p + 20, s.gmask); wr32(p + 24, s.bmask); wr32(p + 28, s.amask);
}

// DDSURFACEDESC (0x6C) o DDSURFACEDESC2 (0x7C): mismo diseño hasta ddsCaps
void fill_desc(u32 p, const Surface &s) {
    u32 size = rd32(p);
    if (size != 0x6C && size != 0x7C) size = 0x7C;
    std::memset(MEM + p, 0, size);
    wr32(p, size);
    wr32(p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (s.mem ? 0x800 : 0));   // CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT|LPSURFACE
    wr32(p + 8, (u32)s.h); wr32(p + 12, (u32)s.w); wr32(p + 16, (u32)s.pitch);
    if (s.back) { wr32(p + 4, rd32(p + 4) | 0x20); wr32(p + 20, 1); }
    wr32(p + 36, s.mem);
    if (s.has_ck) { wr32(p + 4, rd32(p + 4) | 0x10000); wr32(p + 64, s.ck_lo); wr32(p + 68, s.ck_hi); }
    fill_pixel_format(p + 72, s);
    wr32(p + 104, s.caps);
}

u32 new_surface(int w, int h, int bpp, u32 caps, u32 mem = 0) {
    Surface s;
    s.obj = com_new(g_vt_surf);
    s.w = w; s.h = h; s.bpp = bpp; s.caps = caps;
    s.pitch = (int)(((u32)w * (u32)(bpp / 8) + 3) & ~3u);
    s.mem = mem ? mem : heap_alloc((u32)s.pitch * (u32)std::max(h, 1), true);
    u32 o = s.obj;
    g_surf[o] = s;
    return o;
}

void present_if_primary(const Surface *s) {
    if (s && (s->primary || s->obj == g_primary)) {
        std::lock_guard<std::mutex> lk(display().mx);
        display().screen.frame++;
    }
}

// ---------------------------------------------------------------- IUnknown

void m_AddRef() { RET(addref(arg(0)), 1); }
// Al soltar la última referencia de una superficie se libera su memoria (el simulador crea y destruye
// texturas en cada fotograma: sin esto el montículo se agota en un par de minutos de partido). El objeto
// COM (16 B) no se reutiliza, para que un puntero colgante del juego no apunte a otra superficie.
u32 surface_release(u32 o) {
    u32 n = release(o);
    auto it = g_surf.find(o);
    if (n || it == g_surf.end()) return n;
    Surface &s = it->second;
    bool chain = s.primary || o == g_primary;
    for (auto &[k, v] : g_surf) if (v.back == o) chain = true;
    if (chain) return n;
    static const bool no_free = std::getenv("PCF7_NO_SURF_FREE") != nullptr;   // diagnóstico
    if (s.mem && !no_free) heap_free(s.mem);
    u32 z = s.zbuf;
    d3d_surface_gone(o);
    g_surf.erase(it);
    if (z) surface_release(z);
    return n;
}
void m_Release() { RET(surface_release(arg(0)), 1); }
void m_Unsupported1() { RET(DDERR_UNSUPPORTED, 1); }
void m_Ok1() { RET(DD_OK, 1); }
void m_Ok2() { RET(DD_OK, 2); }
void m_Ok3() { RET(DD_OK, 3); }
void m_Ok4() { RET(DD_OK, 4); }
void m_Ok5() { RET(DD_OK, 5); }
void m_Ok6() { RET(DD_OK, 6); }
void m_Unsup2() { RET(DDERR_UNSUPPORTED, 2); }
void m_Unsup3() { RET(DDERR_UNSUPPORTED, 3); }
void m_Unsup4() { RET(DDERR_UNSUPPORTED, 4); }
void m_Unsup5() { RET(DDERR_UNSUPPORTED, 5); }
void m_Unsup6() { RET(DDERR_UNSUPPORTED, 6); }

// ---------------------------------------------------------------- IDirectDraw / IDirectDraw4

void dd_QueryInterface() {
    u32 iid = arg(1), out = arg(2);
    if (guid_eq(iid, IID_IDirectDraw4)) {
        if (!g_dd4) g_dd4 = com_new(g_vt_dd4); else addref(g_dd4);
        wr32(out, g_dd4);
        RET(DD_OK, 3);
    }
    if (guid_eq(iid, IID_IDirectDraw7)) { wr32(out, dd_get_dd7()); RET(DD_OK, 3); }
    if (guid_eq(iid, IID_IDirectDraw2)) {
        static u32 dd2 = 0;
        if (!dd2) dd2 = com_new(g_vt_dd2); else addref(dd2);
        wr32(out, dd2);
        RET(DD_OK, 3);
    }
    if (guid_eq(iid, IID_IDirectDraw) || guid_eq(iid, IID_IUnknown)) { addref(g_dd); wr32(out, g_dd); RET(DD_OK, 3); }
    if (u32 d3d = dd_create_d3d(iid)) { wr32(out, d3d); RET(DD_OK, 3); }
    plog("IDirectDraw::QueryInterface(%s): no soportada", guid_str(iid).c_str());
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}

void dd_CreateClipper() { u32 o = com_new(g_vt_clip); wr32(arg(2), o); RET(DD_OK, 4); }

void dd_CreatePalette() {
    u32 flags = arg(1), entries = arg(2), out = arg(3);
    Palette p;
    p.obj = com_new(g_vt_pal);
    p.caps = flags;
    u32 n = (flags & 4) ? 256 : (flags & 2) ? 16 : (flags & 1) ? 4 : 256;   // DDPCAPS_8BIT / 4BIT / 2BIT
    for (u32 i = 0; i < n && entries; i++) {
        u32 e = rd32(entries + 4 * i);   // PALETTEENTRY: R G B flags
        p.rgb[i] = (e & 0xFF) << 16 | (e & 0xFF00) | ((e >> 16) & 0xFF);
    }
    wr32(out, p.obj);
    g_pal[p.obj] = p;
    RET(DD_OK, 5);
}

void dd_CreateSurface() {
    u32 desc = arg(1), out = arg(2);
    u32 flags = rd32(desc + 4);
    u32 caps = (flags & 1) ? rd32(desc + 104) : 0;
    Screen &sc = display().screen;
    int w = (flags & 4) ? (int)rd32(desc + 12) : sc.w;
    int h = (flags & 2) ? (int)rd32(desc + 8) : sc.h;
    int bpp = (flags & 0x1000) ? (int)rd32(desc + 72 + 12) : sc.bpp;
    if (bpp != 8 && bpp != 16 && bpp != 32) bpp = sc.bpp;
    u32 o;
    if (caps & 0x200) {   // DDSCAPS_PRIMARYSURFACE
        o = new_surface(sc.w, sc.h, sc.bpp, caps | 0x4000 | 0x8000, sc.fb);   // VIDEOMEMORY | VISIBLE
        Surface *s = surf(o);
        s->primary = true;
        s->pitch = sc.pitch;
        g_primary = o;
        if ((flags & 0x20) && rd32(desc + 20) >= 1) {   // DDSD_BACKBUFFERCOUNT: cadena de flip
            u32 b = new_surface(sc.w, sc.h, sc.bpp, 0x4 | 0x4000 | (caps & 0x2000));   // BACKBUFFER | VIDEOMEMORY | 3DDEVICE
            surf(o)->back = b;
            surf(o)->caps |= 0x10 | 0x8;                // FLIP | COMPLEX
            surf(b)->caps |= 0x10 | 0x8;
        }
        plog("CreateSurface: primaria %dx%dx%d%s", sc.w, sc.h, sc.bpp, surf(o)->back ? " con búfer trasero" : "");
    } else {
        if (caps & 0x20000) bpp = 16;   // DDSCAPS_ZBUFFER: profundidad en los bits del formato
        o = new_surface(w, h, bpp, caps ? caps : 0x40 | 0x800);
        Surface *s = surf(o);
        if (flags & 0x10000) { s->has_ck = true; s->ck_lo = rd32(desc + 64); s->ck_hi = rd32(desc + 68); }
        if (caps & 0x20000) s->pf_flags = 0x400;
        else if ((flags & 0x1000) && bpp == 16) {   // formato propio (texturas 555, 1555, 4444…)
            u32 pf = desc + 72;
            s->pf_flags = rd32(pf + 4);
            s->rmask = rd32(pf + 16); s->gmask = rd32(pf + 20); s->bmask = rd32(pf + 24);
            s->amask = (s->pf_flags & 1) ? rd32(pf + 28) : 0;
            if (!s->rmask) { s->pf_flags = 0x40; s->rmask = 0xF800; s->gmask = 0x07E0; s->bmask = 0x001F; s->amask = 0; }
        }
    }
    wr32(out, o);
    RET(DD_OK, 4);
}

void dd_EnumDisplayModes() {
    u32 ctx = arg(3), cb = arg(4);
    static const int modes[][3] = {{640, 480, 8}, {640, 480, 16}, {640, 480, 32}, {800, 600, 8}, {800, 600, 16},
                                   {800, 600, 32}, {1024, 768, 8}, {1024, 768, 16}, {1024, 768, 32}};
    u32 d = heap_alloc(0x7C, true);
    for (auto &m : modes) {
        Surface s;
        s.w = m[0]; s.h = m[1]; s.bpp = m[2]; s.pitch = m[0] * m[2] / 8;
        wr32(d, 0x7C);
        fill_desc(d, s);
        wr32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x40000);   // + DDSD_REFRESHRATE
        wr32(d + 24, 60);
        wr32(d + 36, 0);
        wr32(d + 104, 0);
        if (!guest_call(cb, {d, ctx})) break;   // DDENUMRET_CANCEL
    }
    heap_free(d);
    RET(DD_OK, 5);
}

void dd_GetCaps() {
    for (u32 p : {arg(1), arg(2)}) {
        if (!p) continue;
        u32 size = rd32(p);
        if (size < 8 || size > 0x200) size = 0x17C;
        std::memset(MEM + p, 0, size);
        wr32(p, size);
        wr32(p + 4, 0x40 | 0x200 | 0x400000 | 0x04000000 | 0x8000);   // BLT | BLTSTRETCH | COLORKEY | BLTCOLORFILL | PALETTE
        wr32(p + 8, 0x1);                                              // DDCAPS2_CERTIFIED
        wr32(p + 12, 0x200);                                           // DDCKEYCAPS_SRCBLT
        wr32(p + 20, 0x4 | 0x400 | 0x40);                              // DDPCAPS_8BIT|ALLOW256|PRIMARYSURFACE
        wr32(p + 60, 32u << 20); wr32(p + 64, 32u << 20);              // memoria de vídeo total / libre
        if (size >= 0x68) wr32(p + 0x68, 0x200 | 0x4 | 0x10 | 0x40 | 0x800 | 0x4000 | 0x8 | 0x2000 | 0x1000 | 0x20000);  // ddsCaps
    }
    RET(DD_OK, 3);
}

void dd_GetDisplayMode() {
    Surface s;
    Screen &sc = display().screen;
    s.w = sc.w; s.h = sc.h; s.bpp = sc.bpp; s.pitch = sc.pitch;
    fill_desc(arg(1), s);
    wr32(arg(1) + 36, 0);
    wr32(arg(1) + 104, 0);
    RET(DD_OK, 2);
}

void dd_SetCooperativeLevel() {
    g_coop_hwnd = arg(1);
    g_coop_full = (arg(2) & 0x11) == 0x11;   // DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN
    if (g_coop_full) user32_fullscreen_window(arg(1));
    plog("SetCooperativeLevel(0x%x, 0x%x)", arg(1), arg(2));
    RET(DD_OK, 3);
}

void set_mode(int w, int h, int bpp) {
    plog("SetDisplayMode(%d, %d, %d)", w, h, bpp);
    set_display_mode(w, h, bpp);
    if (g_coop_full) user32_fullscreen_window(g_coop_hwnd);
    if (Surface *p = surf(g_primary)) { p->w = w; p->h = h; p->bpp = bpp; p->mem = display().screen.fb; p->pitch = display().screen.pitch; }
}
void dd1_SetDisplayMode() { set_mode((int)arg(1), (int)arg(2), (int)arg(3)); RET(DD_OK, 4); }
void dd4_SetDisplayMode() { set_mode((int)arg(1), (int)arg(2), (int)arg(3)); RET(DD_OK, 6); }
void dd_RestoreDisplayMode() { RET(DD_OK, 1); }
void dd_WaitForVerticalBlank() { RET(DD_OK, 3); }
void dd_GetAvailableVidMem() { if (arg(2)) wr32(arg(2), 32u << 20); if (arg(3)) wr32(arg(3), 32u << 20); RET(DD_OK, 4); }
void dd_GetMonitorFrequency() { if (arg(1)) wr32(arg(1), 60); RET(DD_OK, 2); }
void dd_GetVerticalBlankStatus() { if (arg(1)) wr32(arg(1), 1); RET(DD_OK, 2); }
void dd_GetFourCCCodes() { if (arg(1)) wr32(arg(1), 0); RET(DD_OK, 3); }
void dd_TestCooperativeLevel() { RET(DD_OK, 1); }
void dd_GetDeviceIdentifier() {
    u32 p = arg(1);
    std::memset(MEM + p, 0, 0x428);
    gstrcpy(p, "display");
    gstrcpy(p + 512, "PCF7 software DirectDraw");
    RET(DD_OK, 3);
}
void dd_GetGDISurface() { if (g_primary) addref(g_primary); wr32(arg(1), g_primary); RET(g_primary ? DD_OK : DDERR_NOTFOUND, 2); }

// ---------------------------------------------------------------- IDirectDrawSurface (1..4: el mismo diseño de vtable)

void s_QueryInterface() {
    u32 iid = arg(1), out = arg(2);
    if (guid_eq(iid, IID_IDirectDrawSurface) || guid_eq(iid, IID_IDirectDrawSurface2) || guid_eq(iid, IID_IDirectDrawSurface3) ||
        guid_eq(iid, IID_IDirectDrawSurface4) || guid_eq(iid, IID_IDirectDrawSurface7) || guid_eq(iid, IID_IUnknown)) {
        addref(arg(0));
        wr32(out, arg(0));
        RET(DD_OK, 3);
    }
    if (u32 t = d3d_surface_qi(arg(0), iid)) { wr32(out, t); RET(DD_OK, 3); }
    plog("IDirectDrawSurface::QueryInterface(%s): no soportada", guid_str(iid).c_str());
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}

// valor de píxel de una superficie en (x, y)
inline u32 px_get(const Surface &s, int x, int y) {
    u32 a = s.mem + (u32)y * (u32)s.pitch + (u32)x * (u32)(s.bpp / 8);
    return s.bpp == 8 ? MEM[a] : s.bpp == 16 ? (u32)MEM[a] | (u32)MEM[a + 1] << 8 : rd32(a);
}
inline void px_set(const Surface &s, int x, int y, u32 v) {
    u32 a = s.mem + (u32)y * (u32)s.pitch + (u32)x * (u32)(s.bpp / 8);
    if (s.bpp == 8) MEM[a] = (u8)v; else if (s.bpp == 16) { MEM[a] = (u8)v; MEM[a + 1] = (u8)(v >> 8); } else wr32(a, v);
}

// copia (con estirado y clave de color de origen opcional), recortada a ambas superficies
void blit(Surface &d, Rect dr, Surface *s, Rect sr, bool use_ck, u32 ck_lo, u32 ck_hi) {
    if (dr.w() <= 0 || dr.h() <= 0) return;
    if (s && (sr.w() <= 0 || sr.h() <= 0)) return;
    bool same_size = s && sr.w() == dr.w() && sr.h() == dr.h();
    std::vector<u8> tmp;
    Surface src = s ? *s : Surface{};
    if (s && s->mem == d.mem) {   // misma memoria: copia previa
        tmp.assign(MEM + s->mem, MEM + s->mem + (size_t)s->pitch * s->h);
        u32 t = heap_alloc((u32)tmp.size());
        std::memcpy(MEM + t, tmp.data(), tmp.size());
        src.mem = t;
    }
    for (int y = dr.t; y < dr.b; y++) {
        if (y < 0 || y >= d.h) continue;
        int sy = same_size ? sr.t + (y - dr.t) : sr.t + (int)((i64)(y - dr.t) * sr.h() / dr.h());
        if (sy < 0 || sy >= src.h) continue;
        if (same_size && !use_ck && src.bpp == d.bpp) {
            int x0 = std::max(dr.l, 0), x1 = std::min(dr.r, d.w);
            int sx0 = sr.l + (x0 - dr.l);
            if (sx0 < 0) { x0 -= sx0; sx0 = 0; }
            if (sx0 + (x1 - x0) > src.w) x1 = x0 + (src.w - sx0);
            if (x1 > x0) {
                int bp = d.bpp / 8;
                std::memmove(MEM + d.mem + (u32)y * (u32)d.pitch + (u32)x0 * (u32)bp,
                             MEM + src.mem + (u32)sy * (u32)src.pitch + (u32)sx0 * (u32)bp, (size_t)(x1 - x0) * (size_t)bp);
            }
            continue;
        }
        for (int x = dr.l; x < dr.r; x++) {
            if (x < 0 || x >= d.w) continue;
            int sx = same_size ? sr.l + (x - dr.l) : sr.l + (int)((i64)(x - dr.l) * sr.w() / dr.w());
            if (sx < 0 || sx >= src.w) continue;
            u32 v = px_get(src, sx, sy);
            if (use_ck && v >= ck_lo && v <= ck_hi) continue;
            px_set(d, x, y, v);
        }
    }
    if (!tmp.empty()) heap_free(src.mem);
}

void fill_rect(Surface &d, Rect r, u32 v) {
    r = {std::max(r.l, 0), std::max(r.t, 0), std::min(r.r, d.w), std::min(r.b, d.h)};
    for (int y = r.t; y < r.b; y++) {
        if (d.bpp == 8) { std::memset(MEM + d.mem + (u32)y * (u32)d.pitch + (u32)r.l, (int)v, (size_t)std::max(0, r.w())); continue; }
        for (int x = r.l; x < r.r; x++) px_set(d, x, y, v);
    }
}

Rect rect_or_full(u32 p, const Surface &s) { return p ? rd_rect(p) : Rect{0, 0, s.w, s.h}; }

void s_Blt() {
    Surface *d = surf(arg(0));
    u32 dr = arg(1), so = arg(2), sr = arg(3), flags = arg(4), fx = arg(5);
    if (!d) RET(DDERR_INVALIDPARAMS, 6);
    Rect r = rect_or_full(dr, *d);
    if (flags & 0x400) {   // DDBLT_COLORFILL
        fill_rect(*d, r, rd32(fx + 0x50));   // DDBLTFX.dwFillColor
    } else if (Surface *s = surf(so)) {
        bool ck = false;
        u32 lo = 0, hi = 0;
        if (flags & 0x8000) { ck = s->has_ck; lo = s->ck_lo; hi = s->ck_hi; }              // DDBLT_KEYSRC
        else if (flags & 0x10000) { ck = true; lo = rd32(fx + 0x5C); hi = rd32(fx + 0x60); }   // DDBLT_KEYSRCOVERRIDE
        blit(*d, r, s, rect_or_full(sr, *s), ck, lo, hi);
    } else if (flags & 0x2000000) {   // DDBLT_DEPTHFILL: dwFillDepth comparte sitio con dwFillColor
        fill_rect(*d, r, rd32(fx + 0x50));
    } else if (flags & 0x20000) {     // DDBLT_ROP (BLACKNESS)
        fill_rect(*d, r, 0);
    }
    present_if_primary(d);
    RET(DD_OK, 6);
}

void s_BltFast() {
    Surface *d = surf(arg(0)), *s = surf(arg(3));
    i32 x = (i32)arg(1), y = (i32)arg(2);
    u32 sr = arg(4), flags = arg(5);
    if (!d || !s) RET(DDERR_INVALIDPARAMS, 6);
    Rect r = rect_or_full(sr, *s);
    bool ck = (flags & 1) && s->has_ck;   // DDBLTFAST_SRCCOLORKEY
    blit(*d, {x, y, x + r.w(), y + r.h()}, s, r, ck, s->ck_lo, s->ck_hi);
    present_if_primary(d);
    RET(DD_OK, 6);
}

void s_Flip() {
    Surface *p = surf(arg(0));
    if (!p || !p->back) { present_if_primary(p); RET(DD_OK, 3); }
    Surface *b = surf(p->back);
    std::swap(p->mem, b->mem);
    std::lock_guard<std::mutex> lk(display().mx);
    if (p->primary) set_visible_fb(p->mem);
    RET(DD_OK, 3);
}

void s_GetAttachedSurface() {
    Surface *s = surf(arg(0));
    u32 caps = rd32(arg(1)), out = arg(2);
    if (s && s->back && (caps & 0x4)) { addref(s->back); wr32(out, s->back); RET(DD_OK, 3); }   // DDSCAPS_BACKBUFFER
    if (s && s->back && (caps & 0x2000)) { addref(s->back); wr32(out, s->back); RET(DD_OK, 3); }
    if (s && s->zbuf && (caps & 0x20000)) { addref(s->zbuf); wr32(out, s->zbuf); RET(DD_OK, 3); }
    wr32(out, 0);
    RET(DDERR_NOTFOUND, 3);
}
void s_AddAttachedSurface() {
    Surface *s = surf(arg(0)), *a = surf(arg(1));
    if (s && a && (a->caps & 0x20000)) { s->zbuf = arg(1); addref(arg(1)); }   // z-buffer del destino 3D
    RET(DD_OK, 2);
}
void s_DeleteAttachedSurface() { RET(DD_OK, 3); }
void s_EnumAttachedSurfaces() {
    Surface *s = surf(arg(0));
    if (s && s->back) {
        u32 d = heap_alloc(0x7C, true);
        wr32(d, 0x7C);
        fill_desc(d, *surf(s->back));
        addref(s->back);
        guest_call(arg(2), {s->back, d, arg(1)});
        heap_free(d);
    }
    RET(DD_OK, 3);
}
void s_GetCaps() { Surface *s = surf(arg(0)); wr32(arg(1), s ? s->caps : 0); RET(DD_OK, 2); }
void s_GetSurfaceDesc() { Surface *s = surf(arg(0)); if (!s) RET(DDERR_INVALIDPARAMS, 2); fill_desc(arg(1), *s); RET(DD_OK, 2); }
void s_GetPixelFormat() { Surface *s = surf(arg(0)); if (s) fill_pixel_format(arg(1), *s); else fill_pixel_format(arg(1), 8); RET(DD_OK, 2); }

void s_Lock() {
    Surface *s = surf(arg(0));
    u32 rp = arg(1), desc = arg(2);
    if (!s) RET(DDERR_INVALIDPARAMS, 5);
    fill_desc(desc, *s);
    if (rp) {
        Rect r = rd_rect(rp);
        wr32(desc + 36, s->mem + (u32)r.t * (u32)s->pitch + (u32)r.l * (u32)(s->bpp / 8));
    }
    RET(DD_OK, 5);
}
void s_Unlock() { present_if_primary(surf(arg(0))); RET(DD_OK, 2); }
void s_IsLost() { RET(DD_OK, 1); }
void s_Restore() { RET(DD_OK, 1); }
void s_SetColorKey() {
    Surface *s = surf(arg(0));
    u32 flags = arg(1), ck = arg(2);
    if (s && (flags & 8)) {   // DDCKEY_SRCBLT
        if (ck) { s->has_ck = true; s->ck_lo = rd32(ck); s->ck_hi = rd32(ck + 4); } else s->has_ck = false;
    }
    RET(DD_OK, 3);
}
void s_GetColorKey() {
    Surface *s = surf(arg(0));
    if (!s || !s->has_ck) RET(DDERR_NOCOLORKEY, 3);
    wr32(arg(2), s->ck_lo); wr32(arg(2) + 4, s->ck_hi);
    RET(DD_OK, 3);
}

void apply_palette(const Palette &p) {
    std::lock_guard<std::mutex> lk(display().mx);
    std::memcpy(display().screen.pal, p.rgb, sizeof p.rgb);
    palette_changed();
}
void s_SetPalette() {
    Surface *s = surf(arg(0));
    if (!s) RET(DDERR_INVALIDPARAMS, 2);
    s->palette = arg(1);
    if (arg(1)) addref(arg(1));
    if (s->primary) { auto it = g_pal.find(arg(1)); if (it != g_pal.end()) apply_palette(it->second); }
    RET(DD_OK, 2);
}
void s_GetPalette() {
    Surface *s = surf(arg(0));
    if (!s || !s->palette) { wr32(arg(1), 0); RET(DDERR_NOPALETTEATTACHED, 2); }
    addref(s->palette);
    wr32(arg(1), s->palette);
    RET(DD_OK, 2);
}
void s_SetClipper() { Surface *s = surf(arg(0)); if (s) s->clipper = arg(1); RET(DD_OK, 2); }
void s_GetClipper() { Surface *s = surf(arg(0)); wr32(arg(1), s ? s->clipper : 0); RET(s && s->clipper ? DD_OK : DDERR_NOTFOUND, 2); }
void s_GetDC() { plog_once("sgetdc", "IDirectDrawSurface::GetDC: no soportado"); wr32(arg(1), 0); RET(DDERR_UNSUPPORTED, 2); }
void s_GetDDInterface() { wr32(arg(1), g_dd4 ? g_dd4 : g_dd); RET(DD_OK, 2); }
void s_GetBltStatus() { RET(DD_OK, 2); }
void s_GetFlipStatus() { RET(DD_OK, 2); }
void s_GetUniquenessValue() { wr32(arg(1), 1); RET(DD_OK, 2); }

// ---------------------------------------------------------------- IDirectDrawPalette

void p_GetCaps() { auto it = g_pal.find(arg(0)); wr32(arg(1), it == g_pal.end() ? 0 : it->second.caps); RET(DD_OK, 2); }
void p_GetEntries() {
    auto it = g_pal.find(arg(0));
    u32 start = arg(2), n = arg(3), out = arg(4);
    if (it == g_pal.end()) RET(DDERR_INVALIDPARAMS, 5);
    for (u32 i = 0; i < n && start + i < 256; i++) {
        u32 c = it->second.rgb[start + i];
        wr32(out + 4 * i, ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16));
    }
    RET(DD_OK, 5);
}
void p_SetEntries() {
    auto it = g_pal.find(arg(0));
    u32 start = arg(2), n = arg(3), in = arg(4);
    if (it == g_pal.end()) RET(DDERR_INVALIDPARAMS, 5);
    for (u32 i = 0; i < n && start + i < 256; i++) {
        u32 e = rd32(in + 4 * i);
        it->second.rgb[start + i] = (e & 0xFF) << 16 | (e & 0xFF00) | ((e >> 16) & 0xFF);
    }
    if (Surface *p = surf(g_primary); p && p->palette == arg(0)) apply_palette(it->second);
    RET(DD_OK, 5);
}

// ---------------------------------------------------------------- IDirectDrawClipper

void c_GetHWnd() { wr32(arg(1), g_coop_hwnd); RET(DD_OK, 2); }

void init_vtables() {
    if (g_vt_dd1) return;
    // IDirectDraw: 23 métodos; IDirectDraw4 añade GetAvailableVidMem, GetSurfaceFromDC, RestoreAllSurfaces,
    // TestCooperativeLevel, GetDeviceIdentifier, y su SetDisplayMode tiene dos argumentos más
    g_vt_dd1 = make_vtbl({dd_QueryInterface, m_AddRef, m_Release, m_Ok1, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
                          m_Unsup3, dd_EnumDisplayModes, m_Unsup5, m_Ok1, dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes,
                          dd_GetGDISurface, dd_GetMonitorFrequency, m_Unsup2, dd_GetVerticalBlankStatus, m_Ok2,
                          dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd1_SetDisplayMode, dd_WaitForVerticalBlank});
    // IDirectDraw2: el de la versión 1 con SetDisplayMode de 5 argumentos y GetAvailableVidMem
    g_vt_dd2 = make_vtbl({dd_QueryInterface, m_AddRef, m_Release, m_Ok1, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
                          m_Unsup3, dd_EnumDisplayModes, m_Unsup5, m_Ok1, dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes,
                          dd_GetGDISurface, dd_GetMonitorFrequency, m_Unsup2, dd_GetVerticalBlankStatus, m_Ok2,
                          dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd4_SetDisplayMode, dd_WaitForVerticalBlank,
                          dd_GetAvailableVidMem});
    g_vt_dd4 = make_vtbl({dd_QueryInterface, m_AddRef, m_Release, m_Ok1, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
                          m_Unsup3, dd_EnumDisplayModes, m_Unsup5, m_Ok1, dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes,
                          dd_GetGDISurface, dd_GetMonitorFrequency, m_Unsup2, dd_GetVerticalBlankStatus, m_Ok2,
                          dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd4_SetDisplayMode, dd_WaitForVerticalBlank,
                          dd_GetAvailableVidMem, m_Unsup3, m_Ok1, dd_TestCooperativeLevel, dd_GetDeviceIdentifier});
    // IDirectDraw7: la 4 más StartModeTest(3) y EvaluateMode(2)
    g_vt_dd7 = make_vtbl({dd_QueryInterface, m_AddRef, m_Release, m_Ok1, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
                          m_Unsup3, dd_EnumDisplayModes, m_Unsup5, m_Ok1, dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes,
                          dd_GetGDISurface, dd_GetMonitorFrequency, m_Unsup2, dd_GetVerticalBlankStatus, m_Ok2,
                          dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd4_SetDisplayMode, dd_WaitForVerticalBlank,
                          dd_GetAvailableVidMem, m_Unsup3, m_Ok1, dd_TestCooperativeLevel, dd_GetDeviceIdentifier,
                          m_Unsup3, m_Ok2});
    g_vt_surf = make_vtbl({s_QueryInterface, m_AddRef, m_Release, s_AddAttachedSurface, m_Unsup2, s_Blt, m_Unsup4, s_BltFast,
                           s_DeleteAttachedSurface, s_EnumAttachedSurfaces, m_Unsup4, s_Flip, s_GetAttachedSurface,
                           s_GetBltStatus, s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus, m_Unsup3,
                           s_GetPalette, s_GetPixelFormat, s_GetSurfaceDesc, m_Ok3, s_IsLost, s_Lock, m_Ok2, s_Restore,
                           s_SetClipper, s_SetColorKey, m_Unsup3, s_SetPalette, s_Unlock, m_Unsup6, m_Unsup2, m_Unsup3,
                           s_GetDDInterface, m_Ok2, m_Ok2, m_Unsup3, m_Unsup5, m_Unsup4, m_Unsup2, s_GetUniquenessValue,
                           m_Ok1,
                           m_Ok2, m_Ok2, m_Ok2, m_Ok2});   // IDirectDrawSurface7: SetPriority, GetPriority, SetLOD, GetLOD
    g_vt_pal = make_vtbl({dd_QueryInterface, m_AddRef, m_Release, p_GetCaps, p_GetEntries, m_Ok4, p_SetEntries});
    g_vt_clip = make_vtbl({dd_QueryInterface, m_AddRef, m_Release, m_Unsup4, c_GetHWnd, m_Ok3, m_Ok2, m_Ok3, m_Ok3});
}

// ---------------------------------------------------------------- exportaciones de ddraw.dll

void DirectDrawCreate() {
    init_vtables();
    u32 out = arg(1);
    if (!g_dd) g_dd = com_new(g_vt_dd1); else addref(g_dd);
    wr32(out, g_dd);
    plog("DirectDrawCreate -> 0x%x", g_dd);
    RET(DD_OK, 3);
}
void DirectDrawEnumerateA() {
    u32 cb = arg(0), ctx = arg(1);
    u32 desc = heap_alloc(64, true), name = desc + 32;
    gstrcpy(desc, "Primary Display Driver");
    gstrcpy(name, "display");
    guest_call(cb, {0, desc, name, ctx});
    heap_free(desc);
    RET(DD_OK, 2);
}

struct Registrar {
    Registrar() {
        register_dynamic_export("ddraw.dll", "DirectDrawCreate", DirectDrawCreate);
        register_dynamic_export("ddraw.dll", "DirectDrawEnumerateA", DirectDrawEnumerateA);
    }
} g_registrar;

}  // namespace

Surface *dd_surf(u32 obj) { return surf(obj); }
// paleta con la que se ve una superficie de 8 bits: la suya o, si no tiene (búfer trasero), la de la primaria
const u32 *dd_surface_palette_rgb(Surface *s) {
    if (s && s->palette) if (const u32 *p = dd_palette_rgb(s->palette)) return p;
    if (Surface *p = surf(g_primary); p && p->palette) return dd_palette_rgb(p->palette);
    return nullptr;
}
const u32 *dd_palette_rgb(u32 pal) { auto it = g_pal.find(pal); return it == g_pal.end() ? nullptr : it->second.rgb; }
void dd_present_if_primary(u32 obj) { present_if_primary(surf(obj)); }
u32 dd_get_dd4() {
    init_vtables();
    if (!g_dd4) g_dd4 = com_new(g_vt_dd4); else addref(g_dd4);
    return g_dd4;
}
u32 dd_get_dd7() {
    init_vtables();
    if (!g_dd7) g_dd7 = com_new(g_vt_dd7); else addref(g_dd7);
    return g_dd7;
}
u32 dd_surface_addref(u32 obj) { return addref(obj); }
u32 dd_surface_release(u32 obj) { return surface_release(obj); }

// ---------------------------------------------------------------- importaciones estáticas de ddraw.dll
// (PCF2000: sus motores de dibujo dcsf/dcsfx/dcd3d importan ddraw.dll en vez de pedirlo con GetProcAddress)
STUB(ext_ddraw_dll_DirectDrawCreate) { DirectDrawCreate(); }
STUB(ext_ddraw_dll_DirectDrawEnumerateA) { DirectDrawEnumerateA(); }
STUB(ext_ddraw_dll_DirectDrawEnumerateExA) {   // (callback, ctx, flags): el callback lleva además el monitor
    u32 cb = arg(0), ctx = arg(1);
    u32 desc = heap_alloc(64, true), name = desc + 32;
    gstrcpy(desc, "Primary Display Driver");
    gstrcpy(name, "display");
    guest_call(cb, {0, desc, name, ctx, 0});
    heap_free(desc);
    RET(DD_OK, 3);
}
STUB(ext_ddraw_dll_DirectDrawCreateEx) {   // (guid, out, iid, unk)
    u32 out = arg(1), iid = arg(2);
    if (guid_eq(iid, IID_IDirectDraw7)) { wr32(out, dd_get_dd7()); plog("DirectDrawCreateEx -> IDirectDraw7 0x%x", g_dd7); RET(DD_OK, 4); }
    if (guid_eq(iid, IID_IDirectDraw4)) { wr32(out, dd_get_dd4()); RET(DD_OK, 4); }
    plog_once("ddcreateex", "DirectDrawCreateEx(%s): interfaz no soportada", guid_str(iid).c_str());
    wr32(out, 0);
    RET(0x80004002u, 4);   // E_NOINTERFACE
}
