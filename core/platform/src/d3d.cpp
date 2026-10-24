// PCF7 — Direct3D (DirectX 6) por software: IDirect3D3, IDirect3DDevice3, IDirect3DViewport3, luces,
// materiales, texturas (IDirect3DTexture2) y búferes de vértices.
//
// El simulador de partidos dibuja con DrawPrimitive y vértices sin transformar (D3DFVF_LVERTEX y con
// normales), así que aquí está la tubería entera de D3D6: matrices mundo/vista/proyección, luces de
// material, recorte en espacio homogéneo, niebla, z-buffer de 16 bits (el de la superficie adjunta al
// destino, en MEM), texturas con clave de color y mezcla alfa. El destino es el búfer trasero de
// DirectDraw (16 bits 565): se escribe directamente en su memoria y el Flip normal lo muestra.
#include "com.h"
#include "ddraw.h"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace {

constexpr u32 D3D_OK = 0, DDERR_INVALIDPARAMS = 0x80070057u, E_NOINTERFACE = 0x80004002u,
              DDERR_UNSUPPORTED = 0x80004001u;

const ComGuid IID_IDirect3D = {0x3BBA0080, 0x2421, 0x11CF, {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
const ComGuid IID_IDirect3D2 = {0x6AAE1EC1, 0x662A, 0x11D0, {0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A}};   // DirectX 5 (PCF6)
const ComGuid IID_IDirect3D3 = {0xBB223240, 0xE72B, 0x11D0, {0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E}};
const ComGuid IID_IDirectDraw4 = {0x9C59509A, 0x39BD, 0x11D1, {0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5}};
const ComGuid IID_IDirect3DHALDevice = {0x84E63DE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};
const ComGuid IID_IDirect3DTexture = {0x2CDCD9E0, 0x25A0, 0x11CF, {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
const ComGuid IID_IDirect3DTexture2 = {0x93281502, 0x8CF8, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};

inline float rdf(u32 a) { u32 v = rd32(a); float f; std::memcpy(&f, &v, 4); return f; }
inline void wrf(u32 a, float f) { u32 v; std::memcpy(&v, &f, 4); wr32(a, v); }
inline float u2f(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }

bool tracing() { static bool on = std::getenv("PCF7_TRACE_D3D") != nullptr; return on; }
std::map<std::string, u64> g_calls;   // PCF7_TRACE_D3D: llamadas por método (d3d_stats)
void trace(const char *what) {
    if (!tracing()) return;
    if (!g_calls[what]++) plog("D3D: %s (desde f_%x, ret 0x%x)", what, cur_fn, rd32(esp));
}

// ---------------------------------------------------------------- matrices (filas, como D3D: v' = v·M)

struct Mat {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};
Mat mat_load(u32 p) { Mat r; for (int i = 0; i < 16; i++) r.m[i / 4][i % 4] = rdf(p + 4 * (u32)i); return r; }
void mat_store(u32 p, const Mat &a) { for (int i = 0; i < 16; i++) wrf(p + 4 * (u32)i, a.m[i / 4][i % 4]); }
Mat operator*(const Mat &a, const Mat &b) {
    Mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// ---------------------------------------------------------------- objetos

struct Light {
    u32 type = 3;                       // D3DLIGHT_POINT 1, SPOT 2, DIRECTIONAL 3, PARALLELPOINT 4
    float col[4] = {1, 1, 1, 1};
    float pos[3] = {}, dir[3] = {0, 0, 1};
    float range = 1e30f, att[3] = {1, 0, 0};
    bool active = true;
};
struct Material {
    float diffuse[4] = {1, 1, 1, 1}, ambient[4] = {0, 0, 0, 0}, specular[4] = {}, emissive[4] = {};
};
struct Viewport {
    u32 device = 0;
    i32 x = 0, y = 0, w = 640, h = 480;
    float cx = -1, cy = 1, cw = 2, ch = 2, minz = 0, maxz = 1;
    u32 background = 0;                 // handle de material
    std::vector<u32> lights;
};
struct VBuffer { u32 mem = 0, fvf = 0, n = 0, caps = 0; };

struct Device {
    u32 obj = 0;
    u32 target = 0;                     // superficie destino
    u32 viewport = 0;                   // actual
    std::vector<u32> viewports;
    u32 rs[256] = {};                   // render states
    u32 ls[16] = {};                    // light states
    u32 tss[8][32] = {};                // texture stage states
    u32 tex[8] = {};                    // superficie de textura por etapa (0 = ninguna)
    Mat world, view, proj;
    // Begin/Vertex/End
    u32 b_pt = 0, b_fvf = 0, b_flags = 0;
    std::vector<u8> b_verts;
    std::vector<u16> b_idx;
    bool b_indexed = false;
};

std::map<u32, Device> g_dev;
std::map<u32, Viewport> g_vp;
std::map<u32, Light> g_light;
std::map<u32, Material> g_mat;
std::map<u32, VBuffer> g_vb;
std::map<u32, u32> g_tex_iface;         // interfaz IDirect3DTexture(2) → superficie

u32 g_vt_d3d1 = 0, g_vt_d3d2 = 0, g_vt_d3d3 = 0, g_vt_dev = 0, g_vt_dev2 = 0, g_vt_vp = 0, g_vt_light = 0, g_vt_mat = 0, g_vt_tex1 = 0,
    g_vt_tex2 = 0, g_vt_vb = 0;
u32 g_d3d1 = 0, g_d3d2 = 0, g_d3d3 = 0;

Device *dev(u32 o) { auto it = g_dev.find(o); return it == g_dev.end() ? nullptr : &it->second; }
Viewport *vp(u32 o) { auto it = g_vp.find(o); return it == g_vp.end() ? nullptr : &it->second; }

void m_AddRef() { RET(com_addref(arg(0)), 1); }
void m_Release() { RET(com_release(arg(0)), 1); }
void m_Ok1() { RET(D3D_OK, 1); }
void m_Ok2() { RET(D3D_OK, 2); }
void m_Ok3() { RET(D3D_OK, 3); }
void m_Unsup2() { RET(DDERR_UNSUPPORTED, 2); }
void m_Unsup3() { RET(DDERR_UNSUPPORTED, 3); }
void m_QINone() { wr32(arg(2), 0); RET(E_NOINTERFACE, 3); }

// ---------------------------------------------------------------- texturas: formatos de 16 bits → ARGB

const std::vector<u32> &lut16(u32 rm, u32 gm, u32 bm, u32 am) {
    static std::map<std::array<u32, 4>, std::vector<u32>> cache;
    auto &v = cache[{rm, gm, bm, am}];
    if (!v.empty()) return v;
    v.resize(65536);
    auto ch = [](u32 px, u32 mask) -> u32 {
        if (!mask) return 255;
        int sh = __builtin_ctz(mask);
        u32 mx = mask >> sh, c = (px & mask) >> sh;
        return (c * 255 + mx / 2) / mx;
    };
    for (u32 px = 0; px < 65536; px++)
        v[px] = ch(px, am) << 24 | ch(px, rm) << 16 | ch(px, gm) << 8 | ch(px, bm);
    if (!am) for (auto &c : v) c |= 0xFF000000u;
    return v;
}

struct TexView {
    const u8 *mem = nullptr;
    int w = 0, h = 0, pitch = 0, bpp = 16;
    const u32 *lut = nullptr;           // 16 bits
    const u32 *pal = nullptr;           // 8 bits
    bool has_alpha = false;
    bool ck = false;
    u32 ck_lo = 0, ck_hi = 0;
    u32 raw(int x, int y) const {
        const u8 *p = mem + (size_t)y * (size_t)pitch;
        return bpp == 16 ? (u32)p[2 * x] | (u32)p[2 * x + 1] << 8 : bpp == 8 ? p[x] : (u32)p[4 * x] | (u32)p[4 * x + 1] << 8 | (u32)p[4 * x + 2] << 16 | (u32)p[4 * x + 3] << 24;
    }
    u32 argb(u32 r) const {
        if (bpp == 16) return lut[r];
        if (bpp == 8) return 0xFF000000u | (pal ? pal[r] : r * 0x010101u);
        return has_alpha ? r : r | 0xFF000000u;
    }
};
bool tex_view(u32 surface, TexView &t) {
    Surface *s = dd_surf(surface);
    if (!s || !s->mem || s->w <= 0 || s->h <= 0) return false;
    t.mem = MEM + s->mem;
    t.w = s->w; t.h = s->h; t.pitch = s->pitch; t.bpp = s->bpp;
    if (s->bpp == 16) {
        u32 rm = s->rmask, gm = s->gmask, bm = s->bmask, am = s->amask;
        if (!rm) { rm = 0xF800; gm = 0x07E0; bm = 0x001F; am = 0; }
        t.lut = lut16(rm, gm, bm, am).data();
        t.has_alpha = am != 0;
    } else if (s->bpp == 8) {
        t.pal = dd_surface_palette_rgb(s);   // textura sin paleta propia: la de la pantalla
    } else {
        t.has_alpha = s->amask != 0;
    }
    t.ck = s->has_ck; t.ck_lo = s->ck_lo; t.ck_hi = s->ck_hi;
    return true;
}

// ---------------------------------------------------------------- tubería de vértices

constexpr u32 FVF_XYZ = 0x2, FVF_XYZRHW = 0x4, FVF_POSMASK = 0xE, FVF_NORMAL = 0x10, FVF_RESERVED1 = 0x20,
              FVF_DIFFUSE = 0x40, FVF_SPECULAR = 0x80;

// fuente de vértices: dirección + paso de cada componente (DrawPrimitive normal o "strided")
struct VSrc {
    u32 fvf = 0;
    u32 pos = 0, pos_st = 0, nrm = 0, nrm_st = 0, dif = 0, dif_st = 0, spc = 0, spc_st = 0, tex = 0, tex_st = 0;
};
u32 fvf_size(u32 fvf) {
    u32 n = 0;
    switch (fvf & FVF_POSMASK) {
    case FVF_XYZ: n = 12; break;
    case FVF_XYZRHW: n = 16; break;
    case 0x6: n = 16; break; case 0x8: n = 20; break; case 0xA: n = 24; break; case 0xC: n = 28; break; case 0xE: n = 32; break;
    }
    if (fvf & FVF_NORMAL) n += 12;
    if (fvf & FVF_RESERVED1) n += 4;
    if (fvf & FVF_DIFFUSE) n += 4;
    if (fvf & FVF_SPECULAR) n += 4;
    n += 8 * ((fvf >> 8) & 0xF);
    return n;
}
VSrc vsrc_flat(u32 fvf, u32 base) {
    VSrc s;
    s.fvf = fvf;
    u32 st = fvf_size(fvf), o = 0;
    s.pos = base; s.pos_st = st;
    switch (fvf & FVF_POSMASK) {
    case FVF_XYZ: o = 12; break;
    case FVF_XYZRHW: o = 16; break;
    default: o = 12 + 4 * (((fvf & FVF_POSMASK) - 4) / 2); break;
    }
    if (fvf & FVF_NORMAL) { s.nrm = base + o; s.nrm_st = st; o += 12; }
    if (fvf & FVF_RESERVED1) o += 4;
    if (fvf & FVF_DIFFUSE) { s.dif = base + o; s.dif_st = st; o += 4; }
    if (fvf & FVF_SPECULAR) { s.spc = base + o; s.spc_st = st; o += 4; }
    if ((fvf >> 8) & 0xF) { s.tex = base + o; s.tex_st = st; }
    return s;
}
VSrc vsrc_strided(u32 fvf, u32 d) {   // D3DDRAWPRIMITIVESTRIDEDDATA: {ptr, stride} × (pos, normal, diffuse, specular, tex[8])
    VSrc s;
    s.fvf = fvf;
    s.pos = rd32(d); s.pos_st = rd32(d + 4);
    if (fvf & FVF_NORMAL) { s.nrm = rd32(d + 8); s.nrm_st = rd32(d + 12); }
    if (fvf & FVF_DIFFUSE) { s.dif = rd32(d + 16); s.dif_st = rd32(d + 20); }
    if (fvf & FVF_SPECULAR) { s.spc = rd32(d + 24); s.spc_st = rd32(d + 28); }
    if ((fvf >> 8) & 0xF) { s.tex = rd32(d + 32); s.tex_st = rd32(d + 36); }
    return s;
}

// vértice en espacio de recorte (antes de dividir por w) con sus atributos
struct CV {
    float x, y, z, w;
    float c[4];      // difuso r g b a (0..1)
    float s[4];      // especular r g b + niebla (a)
    float u, v;
};
// vértice en pantalla
struct SV {
    float x, y, z, q;   // q = 1/w
    float c[4], s[4];
    float u, v;
};

inline void unpack_color(u32 c, float *o) {
    o[0] = (float)((c >> 16) & 0xFF) / 255.f; o[1] = (float)((c >> 8) & 0xFF) / 255.f;
    o[2] = (float)(c & 0xFF) / 255.f; o[3] = (float)(c >> 24) / 255.f;
}
inline u32 pack_color(const float *c) {
    auto q = [](float v) { return (u32)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
    return q(c[3]) << 24 | q(c[0]) << 16 | q(c[1]) << 8 | q(c[2]);
}

// ---------------------------------------------------------------- rasterizador

enum : u32 {
    RS_TEXTUREHANDLE = 1, RS_TEXTUREADDRESS = 3, RS_TEXTUREPERSPECTIVE = 4, RS_WRAPU = 5, RS_WRAPV = 6, RS_ZENABLE = 7,
    RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_TEXTUREMAG = 17,
    RS_TEXTUREMIN = 18, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_TEXTUREMAPBLEND = 21, RS_CULLMODE = 22, RS_ZFUNC = 23,
    RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28,
    RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37,
    RS_FOGDENSITY = 38, RS_COLORKEYENABLE = 41, RS_TEXTUREFACTOR = 60,
    LS_MATERIAL = 1, LS_AMBIENT = 2, LS_COLORMODEL = 3, LS_FOGMODE = 4, LS_FOGSTART = 5, LS_FOGEND = 6, LS_FOGDENSITY = 7,
    TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
    TSS_ADDRESS = 12, TSS_ADDRESSU = 13, TSS_ADDRESSV = 14, TSS_MAGFILTER = 16, TSS_MINFILTER = 17,
};
enum : u32 { TOP_DISABLE = 1, TOP_SELECTARG1 = 2, TOP_SELECTARG2 = 3, TOP_MODULATE = 4, TOP_MODULATE2X = 5,
             TOP_MODULATE4X = 6, TOP_ADD = 7, TOP_ADDSIGNED = 8, TOP_ADDSIGNED2X = 9, TOP_SUBTRACT = 10, TOP_ADDSMOOTH = 11,
             TOP_BLENDDIFFUSEALPHA = 12, TOP_BLENDTEXTUREALPHA = 13, TOP_BLENDFACTORALPHA = 14, TOP_BLENDTEXTUREALPHAPM = 15,
             TOP_BLENDCURRENTALPHA = 16 };
enum : u32 { TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3, TA_SPECULAR = 4, TA_COMPLEMENT = 0x10,
             TA_ALPHAREPLICATE = 0x20 };

// estado fijo de un dibujo (se calcula una vez por llamada)
struct Raster {
    Surface *rt = nullptr;
    u8 *fb = nullptr;
    u16 *zb = nullptr;
    int zpitch = 0;                      // en u16
    int clip_l = 0, clip_t = 0, clip_r = 0, clip_b = 0;
    bool ztest = false, zwrite = false;
    u32 zfunc = 4;
    bool textured = false, bilinear = false, colorkey = false, persp = true;
    TexView tv;
    u32 addr_u = 1, addr_v = 1;
    u32 cop = TOP_MODULATE, carg1 = TA_TEXTURE, carg2 = TA_CURRENT, aop = TOP_SELECTARG1, aarg1 = TA_TEXTURE, aarg2 = TA_CURRENT;
    float tfactor[4] = {1, 1, 1, 1};
    bool flat = false;
    bool blend = false;
    u32 src = 2, dst = 1;
    bool atest = false;
    u32 aref = 0, afunc = 8;
    bool specular = false;
    int fog = 0;                         // 0 nada, 1 por vértice (especular.a), 2 tabla lineal, 3 exp, 4 exp2
    float fog_start = 0, fog_end = 1, fog_density = 1;
    float fogc[3] = {};
    u32 cull = 3;
    u32 fill = 3;
    const u32 *pal = nullptr;            // destino de 8 bits: su paleta (0x00RRGGBB) y la tabla RGB555 → índice
    const u8 *inv = nullptr;
};

// tabla inversa de una paleta (color RGB555 → índice más parecido), se rehace solo si la paleta cambia
const u8 *pal_inverse(const u32 *pal) {
    static std::vector<u8> inv(32768);
    static u32 last[256];
    static bool hecha = false;
    if (hecha && std::memcmp(last, pal, sizeof last) == 0) return inv.data();
    std::memcpy(last, pal, sizeof last);
    hecha = true;
    for (u32 c = 0; c < 32768; c++) {
        int r = (int)((c >> 10) & 31) * 255 / 31, g = (int)((c >> 5) & 31) * 255 / 31, b = (int)(c & 31) * 255 / 31;
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 256; i++) {
            int dr = r - (int)((pal[i] >> 16) & 0xFF), dg = g - (int)((pal[i] >> 8) & 0xFF), db = b - (int)(pal[i] & 0xFF);
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd) { bd = d; best = i; if (!d) break; }
        }
        inv[c] = (u8)best;
    }
    return inv.data();
}

inline bool cmp(u32 f, float a, float b) {
    switch (f) {
    case 1: return false;
    case 2: return a < b;
    case 3: return a == b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a != b;
    case 7: return a >= b;
    default: return true;
    }
}

inline int wrap_coord(int i, int n, u32 mode) {
    switch (mode) {
    case 2: {   // MIRROR
        int p = 2 * n, m = ((i % p) + p) % p;
        return m < n ? m : p - 1 - m;
    }
    case 3: case 4: return std::clamp(i, 0, n - 1);   // CLAMP / BORDER
    default:
        return (n & (n - 1)) == 0 ? (i & (n - 1)) : ((i % n) + n) % n;
    }
}

// devuelve false si el téxel es la clave de color (píxel descartado)
inline bool sample(const Raster &R, float u, float v, float *out) {
    const TexView &t = R.tv;
    if (!R.bilinear || R.colorkey) {
        int x = wrap_coord((int)std::floor(u * (float)t.w), t.w, R.addr_u);
        int y = wrap_coord((int)std::floor(v * (float)t.h), t.h, R.addr_v);
        u32 raw = t.raw(x, y);
        if (R.colorkey && raw >= t.ck_lo && raw <= t.ck_hi) return false;
        unpack_color(t.argb(raw), out);
        return true;
    }
    float fx = u * (float)t.w - 0.5f, fy = v * (float)t.h - 0.5f;
    float x0f = std::floor(fx), y0f = std::floor(fy);
    float ax = fx - x0f, ay = fy - y0f;
    int x0 = (int)x0f, y0 = (int)y0f;
    int xa = wrap_coord(x0, t.w, R.addr_u), xb = wrap_coord(x0 + 1, t.w, R.addr_u);
    int ya = wrap_coord(y0, t.h, R.addr_v), yb = wrap_coord(y0 + 1, t.h, R.addr_v);
    float c00[4], c10[4], c01[4], c11[4];
    unpack_color(t.argb(t.raw(xa, ya)), c00); unpack_color(t.argb(t.raw(xb, ya)), c10);
    unpack_color(t.argb(t.raw(xa, yb)), c01); unpack_color(t.argb(t.raw(xb, yb)), c11);
    for (int i = 0; i < 4; i++)
        out[i] = (c00[i] * (1 - ax) + c10[i] * ax) * (1 - ay) + (c01[i] * (1 - ax) + c11[i] * ax) * ay;
    return true;
}

inline void targ(u32 a, const float *dif, const float *cur, const float *tex, const float *spc, const float *tf, float *o) {
    const float *src;
    switch (a & 0xF) {
    case TA_DIFFUSE: src = dif; break;
    case TA_TEXTURE: src = tex; break;
    case TA_TFACTOR: src = tf; break;
    case TA_SPECULAR: src = spc; break;
    default: src = cur; break;
    }
    for (int i = 0; i < 4; i++) o[i] = src[i];
    if (a & TA_ALPHAREPLICATE) o[0] = o[1] = o[2] = o[3];
    if (a & TA_COMPLEMENT) for (int i = 0; i < 4; i++) o[i] = 1 - o[i];
}
inline float top(u32 op, float a1, float a2, float aa1, float adif, float atex, float afac, float acur) {
    switch (op) {
    case TOP_SELECTARG1: return a1;
    case TOP_SELECTARG2: return a2;
    case TOP_MODULATE: return a1 * a2;
    case TOP_MODULATE2X: return std::min(1.f, 2 * a1 * a2);
    case TOP_MODULATE4X: return std::min(1.f, 4 * a1 * a2);
    case TOP_ADD: return std::min(1.f, a1 + a2);
    case TOP_ADDSIGNED: return std::clamp(a1 + a2 - 0.5f, 0.f, 1.f);
    case TOP_ADDSIGNED2X: return std::clamp(2 * (a1 + a2 - 0.5f), 0.f, 1.f);
    case TOP_SUBTRACT: return std::max(0.f, a1 - a2);
    case TOP_ADDSMOOTH: return std::min(1.f, a1 + a2 - a1 * a2);
    case TOP_BLENDDIFFUSEALPHA: return a1 * adif + a2 * (1 - adif);
    case TOP_BLENDTEXTUREALPHA: return a1 * atex + a2 * (1 - atex);
    case TOP_BLENDFACTORALPHA: return a1 * afac + a2 * (1 - afac);
    case TOP_BLENDTEXTUREALPHAPM: return std::min(1.f, a1 + a2 * (1 - atex));
    case TOP_BLENDCURRENTALPHA: return a1 * acur + a2 * (1 - acur);
    default: (void)aa1; return a2;
    }
}

inline void blend_factor(u32 f, const float *s, const float *d, float *o) {
    switch (f) {
    case 1: o[0] = o[1] = o[2] = o[3] = 0; break;
    case 3: for (int i = 0; i < 4; i++) o[i] = s[i]; break;
    case 4: for (int i = 0; i < 4; i++) o[i] = 1 - s[i]; break;
    case 5: case 12: o[0] = o[1] = o[2] = o[3] = s[3]; break;
    case 6: case 13: o[0] = o[1] = o[2] = o[3] = 1 - s[3]; break;
    case 7: o[0] = o[1] = o[2] = o[3] = d[3]; break;
    case 8: o[0] = o[1] = o[2] = o[3] = 1 - d[3]; break;
    case 9: for (int i = 0; i < 4; i++) o[i] = d[i]; break;
    case 10: for (int i = 0; i < 4; i++) o[i] = 1 - d[i]; break;
    case 11: { float f2 = std::min(s[3], 1 - d[3]); o[0] = o[1] = o[2] = f2; o[3] = 1; break; }
    default: o[0] = o[1] = o[2] = o[3] = 1; break;
    }
}

inline u32 rd_px(const Raster &R, int x, int y) {
    const u8 *p = R.fb + (size_t)y * (size_t)R.rt->pitch + (size_t)x * (size_t)(R.rt->bpp / 8);
    if (R.rt->bpp == 8) return R.pal ? R.pal[p[0]] : p[0] * 0x010101u;   // 8 bits: el color de la paleta
    return R.rt->bpp == 16 ? (u32)p[0] | (u32)p[1] << 8 : (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
inline void px_to_rgb(const Raster &R, u32 px, float *o) {
    if (R.rt->bpp == 16) {
        o[0] = (float)((px >> 11) & 31) / 31.f; o[1] = (float)((px >> 5) & 63) / 63.f; o[2] = (float)(px & 31) / 31.f;
    } else {
        o[0] = (float)((px >> 16) & 0xFF) / 255.f; o[1] = (float)((px >> 8) & 0xFF) / 255.f; o[2] = (float)(px & 0xFF) / 255.f;
    }
    o[3] = 1;
}
inline void wr_px(const Raster &R, int x, int y, const float *c) {
    u8 *p = R.fb + (size_t)y * (size_t)R.rt->pitch + (size_t)x * (size_t)(R.rt->bpp / 8);
    auto q = [](float v, float m) { return (u32)(std::clamp(v, 0.f, 1.f) * m + 0.5f); };
    if (R.rt->bpp == 8) {
        p[0] = R.inv ? R.inv[q(c[0], 31) << 10 | q(c[1], 31) << 5 | q(c[2], 31)] : (u8)q(c[1], 255);
    } else if (R.rt->bpp == 16) {
        u32 v = q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31);
        p[0] = (u8)v; p[1] = (u8)(v >> 8);
    } else {
        u32 v = q(c[0], 255) << 16 | q(c[1], 255) << 8 | q(c[2], 255);
        std::memcpy(p, &v, 4);
    }
}

// sombrea y escribe un píxel; atributos ya divididos por q
inline void shade(const Raster &R, int x, int y, float z, float w, const float *dif, const float *spc, float u, float v) {
    u16 *zp = nullptr;
    if (R.zb) {
        zp = R.zb + (size_t)y * (size_t)R.zpitch + (size_t)x;
        if (R.ztest) {
            float zz = std::clamp(z, 0.f, 1.f) * 65535.f;
            if (!cmp(R.zfunc, zz, (float)*zp)) return;
        }
    }
    float col[4];
    if (R.textured) {
        float tex[4];
        if (!sample(R, u, v, tex)) return;
        float a1[4], a2[4];
        const float *cur = dif;
        targ(R.carg1, dif, cur, tex, spc, R.tfactor, a1);
        targ(R.carg2, dif, cur, tex, spc, R.tfactor, a2);
        if (R.cop == TOP_DISABLE) { for (int i = 0; i < 3; i++) col[i] = dif[i]; }
        else for (int i = 0; i < 3; i++) col[i] = top(R.cop, a1[i], a2[i], a1[3], dif[3], tex[3], R.tfactor[3], cur[3]);
        targ(R.aarg1, dif, cur, tex, spc, R.tfactor, a1);
        targ(R.aarg2, dif, cur, tex, spc, R.tfactor, a2);
        col[3] = R.aop == TOP_DISABLE ? dif[3] : top(R.aop, a1[3], a2[3], a1[3], dif[3], tex[3], R.tfactor[3], cur[3]);
    } else {
        for (int i = 0; i < 4; i++) col[i] = dif[i];
    }
    if (R.atest && !cmp(R.afunc, std::round(col[3] * 255.f), (float)R.aref)) return;
    if (R.specular) for (int i = 0; i < 3; i++) col[i] = std::min(1.f, col[i] + spc[i]);
    if (R.fog) {
        float f;
        if (R.fog == 1) f = spc[3];
        else {
            float d = w;
            if (R.fog == 2) f = R.fog_end == R.fog_start ? 1.f : (R.fog_end - d) / (R.fog_end - R.fog_start);
            else if (R.fog == 3) f = std::exp(-R.fog_density * d);
            else f = std::exp(-(R.fog_density * d) * (R.fog_density * d));
        }
        f = std::clamp(f, 0.f, 1.f);
        for (int i = 0; i < 3; i++) col[i] = col[i] * f + R.fogc[i] * (1 - f);
    }
    if (R.blend) {
        float d[4], fs[4], fd[4];
        px_to_rgb(R, rd_px(R, x, y), d);
        if (R.src == 12) { fs[0] = fs[1] = fs[2] = fs[3] = col[3]; fd[0] = fd[1] = fd[2] = fd[3] = 1 - col[3]; }
        else if (R.src == 13) { fs[0] = fs[1] = fs[2] = fs[3] = 1 - col[3]; fd[0] = fd[1] = fd[2] = fd[3] = col[3]; }
        else { blend_factor(R.src, col, d, fs); blend_factor(R.dst, col, d, fd); }
        for (int i = 0; i < 3; i++) col[i] = col[i] * fs[i] + d[i] * fd[i];
    }
    wr_px(R, x, y, col);
    if (zp && R.zwrite) *zp = (u16)(std::clamp(z, 0.f, 1.f) * 65535.f + 0.5f);
}

// triángulo en pantalla: función de arista con regla arriba-izquierda, centros de píxel enteros (D3D6)
void raster_tri(const Raster &R, const SV &a0, const SV &b0, const SV &c0) {
    const SV *a = &a0, *b = &b0, *c = &c0;
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (!(std::fabs(area) > 1e-8f)) return;
    // pantalla con y hacia abajo: área > 0 = sentido horario
    if (R.cull == 2 && area > 0) return;
    if (R.cull == 3 && area < 0) return;
    if (area < 0) { std::swap(b, c); area = -area; }
    float minx = std::min({a->x, b->x, c->x}), maxx = std::max({a->x, b->x, c->x});
    float miny = std::min({a->y, b->y, c->y}), maxy = std::max({a->y, b->y, c->y});
    int x0 = std::max(R.clip_l, (int)std::ceil(minx)), x1 = std::min(R.clip_r - 1, (int)std::floor(maxx));
    int y0 = std::max(R.clip_t, (int)std::ceil(miny)), y1 = std::min(R.clip_b - 1, (int)std::floor(maxy));
    if (x0 > x1 || y0 > y1) return;

    // atributos por vértice (multiplicados por q para la corrección de perspectiva)
    constexpr int NA = 12;
    float A[3][NA];
    const SV *v[3] = {a, b, c};
    for (int k = 0; k < 3; k++) {
        const SV *s = R.flat ? a : v[k];
        float q = R.persp ? v[k]->q : 1.f;
        for (int i = 0; i < 4; i++) { A[k][i] = s->c[i] * q; A[k][4 + i] = s->s[i] * q; }
        A[k][8] = v[k]->u * q; A[k][9] = v[k]->v * q;
        A[k][10] = q;                       // para recuperar los atributos
        A[k][11] = v[k]->q;                 // 1/w (niebla por profundidad)
    }
    // ecuaciones de plano: attr = A0 + dx*(x - ax) + dy*(y - ay)
    float inv = 1.f / area;
    float e1x = b->x - a->x, e1y = b->y - a->y, e2x = c->x - a->x, e2y = c->y - a->y;
    auto grad = [&](float fa, float fb, float fc, float &dx, float &dy) {
        float d1 = fb - fa, d2 = fc - fa;
        dx = (d1 * e2y - d2 * e1y) * inv;
        dy = (d2 * e1x - d1 * e2x) * inv;
    };
    float gdx[NA], gdy[NA], zdx, zdy;
    for (int i = 0; i < NA; i++) grad(A[0][i], A[1][i], A[2][i], gdx[i], gdy[i]);
    grad(a->z, b->z, c->z, zdx, zdy);

    // aristas: E(x,y) = (x - xa)(yb - ya) - (y - ya)(xb - xa) ≥ 0 dentro (área positiva)
    struct Edge { float A, B, C; bool tl; };
    auto mk = [](const SV *p, const SV *q) {
        Edge e;
        e.A = -(q->y - p->y);
        e.B = q->x - p->x;
        e.C = -(e.A * p->x + e.B * p->y);
        // regla arriba-izquierda: arista superior (horizontal con el interior debajo) o izquierda
        e.tl = (p->y == q->y && q->x > p->x) || (q->y < p->y);
        return e;
    };
    Edge E[3] = {mk(a, b), mk(b, c), mk(c, a)};   // con área > 0 el interior cumple A·x + B·y + C ≥ 0
    for (int y = y0; y <= y1; y++) {
        float fy = (float)y;
        // intervalo [xl, xr] de la fila a partir de las tres aristas
        float xl = (float)x0, xr = (float)x1;
        bool empty = false;
        for (auto &e : E) {
            float r = e.B * fy + e.C;   // A*x + r ≥ 0
            if (e.A > 0) xl = std::max(xl, std::ceil(-r / e.A));
            else if (e.A < 0) xr = std::min(xr, std::floor(-r / e.A));
            else if (r < 0 || (r == 0 && !e.tl)) { empty = true; break; }
        }
        if (empty) continue;
        int ix0 = (int)xl, ix1 = (int)xr;
        // ajuste exacto de los extremos (regla arriba-izquierda y redondeo)
        auto inside = [&](int x) {
            for (auto &e : E) {
                float v2 = e.A * (float)x + e.B * fy + e.C;
                if (v2 < 0 || (v2 == 0 && !e.tl)) return false;
            }
            return true;
        };
        while (ix0 <= ix1 && !inside(ix0)) ix0++;
        while (ix1 >= ix0 && !inside(ix1)) ix1--;
        if (ix0 > ix1) continue;
        float dx0 = (float)ix0 - a->x, dy0 = fy - a->y;
        float at[NA];
        for (int i = 0; i < NA; i++) at[i] = A[0][i] + gdx[i] * dx0 + gdy[i] * dy0;
        float z = a->z + zdx * dx0 + zdy * dy0;
        for (int x = ix0; x <= ix1; x++) {
            float q = at[10], iq = q != 0 ? 1.f / q : 0.f;
            float dif[4], spc[4];
            for (int i = 0; i < 4; i++) { dif[i] = at[i] * iq; spc[i] = at[4 + i] * iq; }
            float w = at[11] > 0 ? 1.f / at[11] : 0.f;
            shade(R, x, y, z, w, dif, spc, at[8] * iq, at[9] * iq);
            for (int i = 0; i < NA; i++) at[i] += gdx[i];
            z += zdx;
        }
    }
}

void raster_line(const Raster &R, const SV &a, const SV &b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    int n = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)));
    if (n <= 0) n = 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        int x = (int)std::lround(a.x + dx * t), y = (int)std::lround(a.y + dy * t);
        if (x < R.clip_l || x >= R.clip_r || y < R.clip_t || y >= R.clip_b) continue;
        float dif[4], spc[4];
        const SV &s = R.flat ? a : (t < 0.5f ? a : b);
        for (int k = 0; k < 4; k++) { dif[k] = R.flat ? a.c[k] : a.c[k] + (b.c[k] - a.c[k]) * t; spc[k] = s.s[k]; }
        float w = 1.f / std::max(1e-6f, a.q + (b.q - a.q) * t);
        shade(R, x, y, a.z + (b.z - a.z) * t, w, dif, spc, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t);
    }
}

// ---------------------------------------------------------------- dibujo

bool setup_raster(Device &d, Raster &R) {
    R.rt = dd_surf(d.target);
    if (!R.rt || !R.rt->mem || (R.rt->bpp != 8 && R.rt->bpp != 16 && R.rt->bpp != 32)) return false;
    R.fb = MEM + R.rt->mem;
    if (R.rt->bpp == 8) {   // destino con paleta (PC Fútbol 6.0 juega en 640x480x8)
        R.pal = dd_surface_palette_rgb(R.rt);
        R.inv = R.pal ? pal_inverse(R.pal) : nullptr;
    }
    Viewport *v = vp(d.viewport);
    R.clip_l = 0; R.clip_t = 0; R.clip_r = R.rt->w; R.clip_b = R.rt->h;
    if (v) {
        R.clip_l = std::max(0, v->x); R.clip_t = std::max(0, v->y);
        R.clip_r = std::min(R.rt->w, v->x + v->w); R.clip_b = std::min(R.rt->h, v->y + v->h);
    }
    if (Surface *z = dd_surf(R.rt->zbuf); z && z->mem && z->w >= R.rt->w && z->h >= R.rt->h) {
        R.zb = (u16 *)(MEM + z->mem);
        R.zpitch = z->pitch / 2;
    }
    const u32 *rs = d.rs;
    R.ztest = R.zb && rs[RS_ZENABLE];
    R.zwrite = R.zb && rs[RS_ZENABLE] && rs[RS_ZWRITEENABLE];
    R.zfunc = rs[RS_ZFUNC];
    R.textured = d.tex[0] && d.tss[0][TSS_COLOROP] != TOP_DISABLE && tex_view(d.tex[0], R.tv);
    R.colorkey = R.textured && rs[RS_COLORKEYENABLE] && R.tv.ck;
    R.bilinear = d.tss[0][TSS_MAGFILTER] == 2 || d.tss[0][TSS_MINFILTER] == 2;
    R.persp = rs[RS_TEXTUREPERSPECTIVE] != 0;
    R.addr_u = d.tss[0][TSS_ADDRESSU]; R.addr_v = d.tss[0][TSS_ADDRESSV];
    R.cop = d.tss[0][TSS_COLOROP]; R.carg1 = d.tss[0][TSS_COLORARG1]; R.carg2 = d.tss[0][TSS_COLORARG2];
    R.aop = d.tss[0][TSS_ALPHAOP]; R.aarg1 = d.tss[0][TSS_ALPHAARG1]; R.aarg2 = d.tss[0][TSS_ALPHAARG2];
    // D3DTBLEND_MODULATE: alfa de la textura si la tiene, si no el del vértice
    if (rs[RS_TEXTUREMAPBLEND] == 2 && R.aop == TOP_SELECTARG1 && R.aarg1 == TA_TEXTURE && R.textured && !R.tv.has_alpha && !R.colorkey)
        R.aop = TOP_SELECTARG2;
    unpack_color(rs[RS_TEXTUREFACTOR], R.tfactor);
    R.flat = rs[RS_SHADEMODE] == 1;
    R.blend = rs[RS_ALPHABLENDENABLE] != 0;
    R.src = rs[RS_SRCBLEND]; R.dst = rs[RS_DESTBLEND];
    R.atest = rs[RS_ALPHATESTENABLE] != 0;
    R.aref = rs[RS_ALPHAREF] & 0xFF; R.afunc = rs[RS_ALPHAFUNC];
    R.specular = rs[RS_SPECULARENABLE] != 0;
    R.fog = 0;
    if (rs[RS_FOGENABLE]) {
        u32 tm = rs[RS_FOGTABLEMODE], lm = d.ls[LS_FOGMODE];
        auto mode = [](u32 m) { return m == 3 ? 2 : m == 1 ? 3 : 4; };   // D3DFOG_LINEAR 3, EXP 1, EXP2 2
        if (tm) { R.fog = mode(tm); R.fog_start = u2f(rs[RS_FOGSTART]); R.fog_end = u2f(rs[RS_FOGEND]); R.fog_density = u2f(rs[RS_FOGDENSITY]); }
        else if (lm) { R.fog = mode(lm); R.fog_start = u2f(d.ls[LS_FOGSTART]); R.fog_end = u2f(d.ls[LS_FOGEND]); R.fog_density = u2f(d.ls[LS_FOGDENSITY]); }
        else R.fog = 1;
        float fc[4];
        unpack_color(rs[RS_FOGCOLOR], fc);
        if (tracing()) { static int n = 0; if (n++ < 3) plog("D3D niebla: modo %d inicio %g fin %g densidad %g color %06x", R.fog, R.fog_start, R.fog_end, R.fog_density, rs[RS_FOGCOLOR] & 0xFFFFFF); }
        for (int i = 0; i < 3; i++) R.fogc[i] = fc[i];
    }
    R.cull = rs[RS_CULLMODE];
    R.fill = rs[RS_FILLMODE];
    return true;
}

// iluminación de un vértice (espacio mundo)
void light_vertex(const Device &d, const float *p, const float *n, float *dif, float *spc) {
    Material m;
    if (auto it = g_mat.find(d.ls[LS_MATERIAL]); it != g_mat.end()) m = it->second;
    float amb[4];
    unpack_color(d.ls[LS_AMBIENT], amb);
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = m.emissive[i] + amb[i] * m.ambient[i];
    if (const Viewport *v = [&]() -> const Viewport * { auto it = g_vp.find(d.viewport); return it == g_vp.end() ? nullptr : &it->second; }()) {
        for (u32 lo : v->lights) {
            auto it = g_light.find(lo);
            if (it == g_light.end() || !it->second.active) continue;
            const Light &l = it->second;
            float L[3], att = 1;
            if (l.type == 3) { L[0] = -l.dir[0]; L[1] = -l.dir[1]; L[2] = -l.dir[2]; }
            else {
                for (int i = 0; i < 3; i++) L[i] = l.pos[i] - p[i];
                float dist = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
                if (dist > l.range) continue;
                float den = l.att[0] + l.att[1] * dist + l.att[2] * dist * dist;
                att = den > 0 ? 1.f / den : 1.f;
                if (dist > 0) for (float &x : L) x /= dist;
            }
            float ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (ln > 0) for (float &x : L) x /= ln;
            float nl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
            if (nl <= 0) continue;
            for (int i = 0; i < 3; i++) c[i] += l.col[i] * m.diffuse[i] * nl * att;
        }
    }
    for (int i = 0; i < 3; i++) dif[i] = std::min(1.f, c[i]);
    dif[3] = m.diffuse[3];
    spc[0] = spc[1] = spc[2] = 0;
}

constexpr float CLIP_EPS = 1e-5f;

// Sutherland–Hodgman contra los 6 planos del volumen de recorte del viewport
int clip_poly(std::vector<CV> &poly, const float *planes /* 6×(a,b,c,d) */) {
    std::vector<CV> out;
    for (int p = 0; p < 6 && !poly.empty(); p++) {
        const float *P = planes + 4 * p;
        auto dist = [&](const CV &v) { return P[0] * v.x + P[1] * v.y + P[2] * v.z + P[3] * v.w; };
        out.clear();
        for (size_t i = 0; i < poly.size(); i++) {
            const CV &A = poly[i], &B = poly[(i + 1) % poly.size()];
            float da = dist(A), db = dist(B);
            if (da >= 0) out.push_back(A);
            if ((da >= 0) != (db >= 0)) {
                float t = da / (da - db);
                CV r;
                r.x = A.x + (B.x - A.x) * t; r.y = A.y + (B.y - A.y) * t; r.z = A.z + (B.z - A.z) * t; r.w = A.w + (B.w - A.w) * t;
                for (int k = 0; k < 4; k++) { r.c[k] = A.c[k] + (B.c[k] - A.c[k]) * t; r.s[k] = A.s[k] + (B.s[k] - A.s[k]) * t; }
                r.u = A.u + (B.u - A.u) * t; r.v = A.v + (B.v - A.v) * t;
                out.push_back(r);
            }
        }
        poly.swap(out);
    }
    return (int)poly.size();
}

struct Prepared {
    bool tl = false;                    // pretransformados (XYZRHW): sin recorte
    std::vector<CV> cv;
    std::vector<SV> sv;
    std::vector<u8> code;               // códigos de recorte
    float planes[24];
    // viewport → pantalla
    float vx, vy, vw, vh, cx, cy, cw, ch, minz, maxz;
};

SV project(const Prepared &P, const CV &c) {
    SV s;
    float q = 1.f / (std::fabs(c.w) < CLIP_EPS ? CLIP_EPS : c.w);
    float xn = c.x * q, yn = c.y * q, zn = c.z * q;
    s.x = P.vx + (xn - P.cx) / P.cw * P.vw;
    s.y = P.vy + (P.cy - yn) / P.ch * P.vh;
    s.z = P.minz + zn * (P.maxz - P.minz);
    s.q = q;
    for (int k = 0; k < 4; k++) { s.c[k] = c.c[k]; s.s[k] = c.s[k]; }
    s.u = c.u; s.v = c.v;
    return s;
}

void prepare(Device &d, const VSrc &src, u32 first, u32 n, u32 flags, Prepared &P) {
    Viewport def;
    const Viewport *v = vp(d.viewport);
    if (!v) {
        Surface *rt = dd_surf(d.target);
        if (rt) { def.w = rt->w; def.h = rt->h; }
        v = &def;
    }
    P.vx = (float)v->x; P.vy = (float)v->y; P.vw = (float)v->w; P.vh = (float)v->h;
    P.cx = v->cx; P.cy = v->cy; P.cw = v->cw; P.ch = v->ch; P.minz = v->minz; P.maxz = v->maxz;
    float x0 = P.cx, x1 = P.cx + P.cw, y0 = P.cy - P.ch, y1 = P.cy;
    float pl[24] = {1, 0, 0, -x0, -1, 0, 0, x1, 0, 1, 0, -y0, 0, -1, 0, y1, 0, 0, 1, 0, 0, 0, -1, 1};
    std::memcpy(P.planes, pl, sizeof pl);

    u32 fvf = src.fvf;
    P.tl = (fvf & FVF_POSMASK) == FVF_XYZRHW;
    bool light = !P.tl && (fvf & FVF_NORMAL) && !(flags & 0x10);   // D3DDP_DONOTLIGHT
    Mat wvp = d.world * d.view * d.proj;
    const Mat &W = d.world;
    P.cv.resize(n);
    P.sv.resize(n);
    P.code.assign(n, 0);
    for (u32 i = 0; i < n; i++) {
        u32 k = first + i;
        u32 pa = src.pos + k * src.pos_st;
        float x = rdf(pa), y = rdf(pa + 4), z = rdf(pa + 8);
        float dif[4] = {1, 1, 1, 1}, spc[4] = {0, 0, 0, 0};
        if (fvf & FVF_DIFFUSE) unpack_color(rd32(src.dif + k * src.dif_st), dif);
        if (fvf & FVF_SPECULAR) unpack_color(rd32(src.spc + k * src.spc_st), spc);
        else spc[3] = 1;   // sin especular: sin niebla por vértice
        float tu = 0, tv = 0;
        if (src.tex) { tu = rdf(src.tex + k * src.tex_st); tv = rdf(src.tex + k * src.tex_st + 4); }
        if (P.tl) {
            SV &s = P.sv[i];
            s.x = x; s.y = y; s.z = z;
            float rhw = rdf(pa + 12);
            s.q = rhw > 0 ? rhw : 1.f;
            for (int c = 0; c < 4; c++) { s.c[c] = dif[c]; s.s[c] = spc[c]; }
            s.u = tu; s.v = tv;
            continue;
        }
        if (light) {
            u32 na = src.nrm + k * src.nrm_st;
            float nx = rdf(na), ny = rdf(na + 4), nz = rdf(na + 8);
            float wp[3], wn[3];
            for (int c = 0; c < 3; c++) {
                wp[c] = x * W.m[0][c] + y * W.m[1][c] + z * W.m[2][c] + W.m[3][c];
                wn[c] = nx * W.m[0][c] + ny * W.m[1][c] + nz * W.m[2][c];
            }
            float l = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
            if (l > 0) for (float &c : wn) c /= l;
            float sa = spc[3];
            light_vertex(d, wp, wn, dif, spc);
            spc[3] = sa;
        }
        CV &c = P.cv[i];
        c.x = x * wvp.m[0][0] + y * wvp.m[1][0] + z * wvp.m[2][0] + wvp.m[3][0];
        c.y = x * wvp.m[0][1] + y * wvp.m[1][1] + z * wvp.m[2][1] + wvp.m[3][1];
        c.z = x * wvp.m[0][2] + y * wvp.m[1][2] + z * wvp.m[2][2] + wvp.m[3][2];
        c.w = x * wvp.m[0][3] + y * wvp.m[1][3] + z * wvp.m[2][3] + wvp.m[3][3];
        for (int q = 0; q < 4; q++) { c.c[q] = dif[q]; c.s[q] = spc[q]; }
        c.u = tu; c.v = tv;
        u8 code = 0;
        for (int p = 0; p < 6; p++) {
            const float *L = P.planes + 4 * p;
            if (L[0] * c.x + L[1] * c.y + L[2] * c.z + L[3] * c.w < 0) code |= (u8)(1 << p);
        }
        P.code[i] = code;
        if (!code) P.sv[i] = project(P, c);
    }
}

void draw_tri(const Raster &R, Prepared &P, u32 i0, u32 i1, u32 i2) {
    if (i0 >= P.sv.size() || i1 >= P.sv.size() || i2 >= P.sv.size()) return;
    if (R.fill == 2) { raster_line(R, P.sv[i0], P.sv[i1]); raster_line(R, P.sv[i1], P.sv[i2]); raster_line(R, P.sv[i2], P.sv[i0]); return; }
    if (P.tl || !(P.code[i0] | P.code[i1] | P.code[i2])) { raster_tri(R, P.sv[i0], P.sv[i1], P.sv[i2]); return; }
    if (P.code[i0] & P.code[i1] & P.code[i2]) return;   // fuera del todo
    std::vector<CV> poly = {P.cv[i0], P.cv[i1], P.cv[i2]};
    int n = clip_poly(poly, P.planes);
    if (n < 3) return;
    std::vector<SV> s(n);
    for (int k = 0; k < n; k++) s[k] = project(P, poly[k]);
    for (int k = 1; k + 1 < n; k++) raster_tri(R, s[0], s[k], s[k + 1]);
}

void draw_line(const Raster &R, Prepared &P, u32 i0, u32 i1) {
    if (i0 >= P.sv.size() || i1 >= P.sv.size()) return;
    if (!P.tl && (P.code[i0] | P.code[i1])) {
        if (P.code[i0] & P.code[i1]) return;
        std::vector<CV> poly = {P.cv[i0], P.cv[i1]};
        // recorte de segmento: como polígono degenerado de 2 vértices
        if (clip_poly(poly, P.planes) < 2) return;
        raster_line(R, project(P, poly[0]), project(P, poly[1]));
        return;
    }
    raster_line(R, P.sv[i0], P.sv[i1]);
}

// pt: POINTLIST 1, LINELIST 2, LINESTRIP 3, TRIANGLELIST 4, TRIANGLESTRIP 5, TRIANGLEFAN 6
void draw(Device &d, u32 pt, const VSrc &src, u32 first, u32 nv, const u16 *idx, u32 ni, u32 flags) {
    if (tracing()) {
        static int n = 0;
        if (n++ < 40)
            plog("D3D draw: pt %u fvf 0x%x nv %u ni %u flags 0x%x tex 0x%x z %u blend %u/%u%s cull %u ret 0x%x", pt, src.fvf, nv, ni,
                 flags, d.tex[0], d.rs[RS_ZENABLE], d.rs[RS_SRCBLEND], d.rs[RS_DESTBLEND], d.rs[RS_ALPHABLENDENABLE] ? " on" : "",
                 d.rs[RS_CULLMODE], rd32(esp));
        if (n == 30 || n == 2) {
            u32 *t = d.tss[0];
            plog("  tss0: cop %u %u %u aop %u %u %u addr %u/%u filt %u/%u  tblend %u shade %u atest %u ck %u fog %u spec %u",
                 t[TSS_COLOROP], t[TSS_COLORARG1], t[TSS_COLORARG2], t[TSS_ALPHAOP], t[TSS_ALPHAARG1], t[TSS_ALPHAARG2],
                 t[TSS_ADDRESSU], t[TSS_ADDRESSV], t[TSS_MAGFILTER], t[TSS_MINFILTER], d.rs[RS_TEXTUREMAPBLEND], d.rs[RS_SHADEMODE],
                 d.rs[RS_ALPHATESTENABLE], d.rs[RS_COLORKEYENABLE], d.rs[RS_FOGENABLE], d.rs[RS_SPECULARENABLE]);
            if (Surface *ts = dd_surf(d.tex[0])) plog("  tex %dx%dx%d masks %x %x %x %x ck %d", ts->w, ts->h, ts->bpp, ts->rmask, ts->gmask, ts->bmask, ts->amask, ts->has_ck);
            if (Surface *rt = dd_surf(d.target)) plog("  target %dx%d bpp %d mem 0x%x primary %d", rt->w, rt->h, rt->bpp, rt->mem, rt->primary);
            if (Viewport *v = vp(d.viewport)) plog("  vp %d,%d %dx%d clip %g %g %g %g z %g..%g", v->x, v->y, v->w, v->h, v->cx, v->cy, v->cw, v->ch, v->minz, v->maxz);
            for (u32 i = 0; i < std::min(nv, 6u); i++) {
                u32 sz = fvf_size(src.fvf), p = src.pos + i * sz;
                std::string line;
                char b[64];
                for (u32 k = 0; k < sz; k += 4) {
                    if (k >= 16 && k < 24 && (src.fvf & FVF_POSMASK) == FVF_XYZRHW) snprintf(b, sizeof b, " %08x", rd32(p + k));
                    else snprintf(b, sizeof b, " %g", rdf(p + k));
                    line += b;
                }
                plog("   v%u:%s", i, line.c_str());
            }
            Mat m = d.world * d.view * d.proj;
            plog("  wvp %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g", m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3], m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3], m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
        }
    }
    Raster R;
    if (!setup_raster(d, R)) return;
    Prepared P;
    prepare(d, src, first, nv, flags, P);
    u32 n = idx ? ni : nv;
    auto I = [&](u32 k) -> u32 { return idx ? idx[k] : k; };
    switch (pt) {
    case 1:
        for (u32 k = 0; k < n; k++) { u32 i = I(k); if (i < nv && (P.tl || !P.code[i])) raster_line(R, P.sv[i], P.sv[i]); }
        break;
    case 2: for (u32 k = 0; k + 1 < n; k += 2) draw_line(R, P, I(k), I(k + 1)); break;
    case 3: for (u32 k = 0; k + 1 < n; k++) draw_line(R, P, I(k), I(k + 1)); break;
    case 4: for (u32 k = 0; k + 2 < n; k += 3) draw_tri(R, P, I(k), I(k + 1), I(k + 2)); break;
    case 5:
        for (u32 k = 0; k + 2 < n; k++) {
            if (k & 1) draw_tri(R, P, I(k + 1), I(k), I(k + 2));
            else draw_tri(R, P, I(k), I(k + 1), I(k + 2));
        }
        break;
    case 6: for (u32 k = 1; k + 1 < n; k++) draw_tri(R, P, I(0), I(k), I(k + 1)); break;
    }
    if (R.rt->primary) dd_present_if_primary(d.target);
}

std::vector<u16> read_idx(u32 p, u32 n) {
    std::vector<u16> v(n);
    if (n) std::memcpy(v.data(), MEM + p, 2 * (size_t)n);
    return v;
}

// ---------------------------------------------------------------- D3DDEVICEDESC (DX6, 0xFC bytes)

void fill_d3d_desc(u32 p) {
    u32 size = rd32(p);
    if (size < 0x10 || size > 0xFC) size = 0xFC;
    u8 tmp[0xFC] = {};
    auto w = [&](u32 o, u32 v) { std::memcpy(tmp + o, &v, 4); };
    auto w16 = [&](u32 o, u16 v) { std::memcpy(tmp + o, &v, 2); };
    w(0x00, size);
    w(0x04, 0x7FF);                         // todos los campos válidos
    w(0x08, 2);                             // dcmColorModel = D3DCOLOR_RGB
    w(0x0C, 0x1 | 0x2 | 0x10 | 0x40 | 0x80 | 0x200 | 0x400 | 0x800 | 0x4000);   // dwDevCaps
    w(0x10, 8); w(0x14, 1);                 // dtcTransformCaps
    w(0x18, 1);                             // bClipping
    w(0x1C, 16); w(0x20, 7); w(0x24, 1); w(0x28, 8);   // dlcLightingCaps
    for (u32 pc : {0x2Cu, 0x64u}) {         // dpcLineCaps, dpcTriCaps
        w(pc, 56);
        w(pc + 0x04, 0x7F);
        w(pc + 0x08, 0x1 | 0x2 | 0x10 | 0x80 | 0x100 | 0x8000);
        w(pc + 0x0C, 0xFF); w(pc + 0x10, 0x1FFF); w(pc + 0x14, 0x1FFF); w(pc + 0x18, 0xFF);
        w(pc + 0x1C, 0x7FFFF);
        w(pc + 0x20, 0x1 | 0x2 | 0x4 | 0x20 | 0x40);
        w(pc + 0x24, 0x3F); w(pc + 0x28, 0xFF); w(pc + 0x2C, 0x1F);
    }
    w(0x9C, 0x400 | 0x100);                 // dwDeviceRenderBitDepth: DDBD_16 | DDBD_32
    w(0xA0, 0x400);                         // dwDeviceZBufferBitDepth: DDBD_16
    w(0xA8, 0xFFFF);
    w(0xAC, 1); w(0xB0, 1); w(0xB4, 1024); w(0xB8, 1024);
    w(0xCC, 256); w(0xD0, 8);
    w(0xF0, 8);
    w(0xF4, 0xFFFFFF);
    w16(0xF8, 2); w16(0xFA, 2);
    std::memcpy(MEM + p, tmp, size);
}

// ---------------------------------------------------------------- IDirect3DDevice3

void dev_defaults(Device &d) {
    u32 *rs = d.rs;
    rs[RS_TEXTUREPERSPECTIVE] = 1; rs[RS_ZENABLE] = 0; rs[RS_FILLMODE] = 3; rs[RS_SHADEMODE] = 2;
    rs[RS_ZWRITEENABLE] = 1; rs[RS_TEXTUREMAG] = 1; rs[RS_TEXTUREMIN] = 1; rs[RS_SRCBLEND] = 2; rs[RS_DESTBLEND] = 1;
    rs[RS_TEXTUREMAPBLEND] = 2; rs[RS_CULLMODE] = 3; rs[RS_ZFUNC] = 4; rs[RS_ALPHAFUNC] = 8; rs[RS_DITHERENABLE] = 0;
    rs[RS_TEXTUREADDRESS] = 1; rs[RS_FOGDENSITY] = 0x3F800000; rs[RS_FOGEND] = 0x3F800000; rs[RS_TEXTUREFACTOR] = 0xFFFFFFFF;
    for (int s = 0; s < 8; s++) {
        u32 *t = d.tss[s];
        t[TSS_COLOROP] = s == 0 ? TOP_MODULATE : TOP_DISABLE; t[TSS_COLORARG1] = TA_TEXTURE; t[TSS_COLORARG2] = TA_CURRENT;
        t[TSS_ALPHAOP] = s == 0 ? TOP_SELECTARG1 : TOP_DISABLE; t[TSS_ALPHAARG1] = TA_TEXTURE; t[TSS_ALPHAARG2] = TA_CURRENT;
        t[TSS_ADDRESS] = t[TSS_ADDRESSU] = t[TSS_ADDRESSV] = 1; t[TSS_MAGFILTER] = 1; t[TSS_MINFILTER] = 1;
    }
    d.ls[LS_COLORMODEL] = 2;
}

// D3DRENDERSTATE_TEXTUREMAPBLEND → etapa 0 (lo que hace el runtime de DX6)
void apply_tblend(Device &d, u32 v) {
    u32 *t = d.tss[0];
    t[TSS_COLORARG1] = TA_TEXTURE; t[TSS_COLORARG2] = TA_DIFFUSE; t[TSS_ALPHAARG1] = TA_TEXTURE; t[TSS_ALPHAARG2] = TA_DIFFUSE;
    switch (v) {
    case 1: case 5: case 7: t[TSS_COLOROP] = TOP_SELECTARG1; t[TSS_ALPHAOP] = TOP_SELECTARG1; break;                    // DECAL, DECALMASK, COPY
    case 2: case 6: t[TSS_COLOROP] = TOP_MODULATE; t[TSS_ALPHAOP] = TOP_SELECTARG1; break;                             // MODULATE(MASK)
    case 3: t[TSS_COLOROP] = TOP_BLENDTEXTUREALPHA; t[TSS_ALPHAOP] = TOP_SELECTARG2; break;                            // DECALALPHA
    case 4: t[TSS_COLOROP] = TOP_MODULATE; t[TSS_ALPHAOP] = TOP_MODULATE; break;                                      // MODULATEALPHA
    case 8: t[TSS_COLOROP] = TOP_ADD; t[TSS_ALPHAOP] = TOP_SELECTARG2; break;                                         // ADD
    }
}

u32 tex_surface(u32 iface) {
    if (!iface) return 0;
    auto it = g_tex_iface.find(iface);
    if (it != g_tex_iface.end()) return it->second;
    return dd_surf(iface) ? iface : 0;   // un handle es la propia superficie
}

void set_render_state(Device &d, u32 s, u32 v) {
    if (s >= 256) return;
    d.rs[s] = v;
    switch (s) {
    case RS_TEXTUREHANDLE: d.tex[0] = tex_surface(v); break;
    case RS_TEXTUREMAPBLEND: apply_tblend(d, v); break;
    case RS_TEXTUREADDRESS: d.tss[0][TSS_ADDRESS] = d.tss[0][TSS_ADDRESSU] = d.tss[0][TSS_ADDRESSV] = v; break;
    case 44: d.tss[0][TSS_ADDRESSU] = v; break;
    case 45: d.tss[0][TSS_ADDRESSV] = v; break;
    case RS_TEXTUREMAG: d.tss[0][TSS_MAGFILTER] = v == 2 ? 2 : 1; break;
    case RS_TEXTUREMIN: d.tss[0][TSS_MINFILTER] = (v == 2 || v == 4 || v == 6) ? 2 : 1; break;
    }
}

void dev_QueryInterface() { trace(__func__); trace("Device::QueryInterface"); m_QINone(); }
void dev_GetCaps() { trace(__func__); if (arg(1)) fill_d3d_desc(arg(1)); if (arg(2)) fill_d3d_desc(arg(2)); RET(D3D_OK, 3); }
void dev_GetStats() { trace(__func__); u32 p = arg(1); if (p) { u32 n = rd32(p); std::memset(MEM + p + 4, 0, n > 4 && n < 64 ? n - 4 : 16); } RET(D3D_OK, 2); }
void dev_AddViewport() { trace(__func__);
    Device *d = dev(arg(0));
    if (Viewport *v = vp(arg(1)); d && v) { v->device = arg(0); d->viewports.push_back(arg(1)); com_addref(arg(1)); }
    RET(D3D_OK, 2);
}
void dev_DeleteViewport() { trace(__func__);
    Device *d = dev(arg(0));
    if (d) {
        auto &l = d->viewports;
        l.erase(std::remove(l.begin(), l.end(), arg(1)), l.end());
        if (d->viewport == arg(1)) d->viewport = 0;
    }
    RET(D3D_OK, 2);
}
void dev_NextViewport() { trace(__func__);
    Device *d = dev(arg(0));
    u32 ref = arg(1), out = arg(2), flags = arg(3);
    u32 r = 0;
    if (d && !d->viewports.empty()) {
        if (flags == 2) r = d->viewports.front();          // D3DNEXT_HEAD
        else if (flags == 4) r = d->viewports.back();      // D3DNEXT_TAIL
        else {
            auto it = std::find(d->viewports.begin(), d->viewports.end(), ref);
            if (it != d->viewports.end() && ++it != d->viewports.end()) r = *it;
        }
    }
    wr32(out, r);
    RET(r ? D3D_OK : DDERR_INVALIDPARAMS, 4);
}
void dev_EnumTextureFormats() { trace(__func__);
    u32 cb = arg(1), ctx = arg(2);
    u32 pf = heap_alloc(32, true);
    struct F { u32 flags, bits, r, g, b, a; };
    static const F fmts[] = {
        {0x40, 16, 0xF800, 0x07E0, 0x001F, 0},          // 565
        {0x41, 16, 0x7C00, 0x03E0, 0x001F, 0x8000},     // 1555
        {0x41, 16, 0x0F00, 0x00F0, 0x000F, 0xF000},     // 4444
        {0x40, 16, 0x7C00, 0x03E0, 0x001F, 0},          // 555
        {0x60, 8, 0, 0, 0, 0},                          // 8 bits con paleta
    };
    for (auto &f : fmts) {
        std::memset(MEM + pf, 0, 32);
        wr32(pf, 32); wr32(pf + 4, f.flags); wr32(pf + 12, f.bits);
        wr32(pf + 16, f.r); wr32(pf + 20, f.g); wr32(pf + 24, f.b); wr32(pf + 28, f.a);
        if (!guest_call(cb, {pf, ctx})) break;
    }
    heap_free(pf);
    RET(D3D_OK, 3);
}
void dev_BeginScene() { trace(__func__); RET(D3D_OK, 1); }
void dev_EndScene() { trace(__func__);
    Device *d = dev(arg(0));
    if (d) dd_present_if_primary(d->target);
    RET(D3D_OK, 1);
}
void dev_GetDirect3D() { trace(__func__); if (g_d3d3) com_addref(g_d3d3); wr32(arg(1), g_d3d3); RET(D3D_OK, 2); }
void dev_SetCurrentViewport() { trace(__func__); if (Device *d = dev(arg(0))) d->viewport = arg(1); RET(D3D_OK, 2); }
void dev_GetCurrentViewport() { trace(__func__); Device *d = dev(arg(0)); u32 v = d ? d->viewport : 0; if (v) com_addref(v); wr32(arg(1), v); RET(D3D_OK, 2); }
void dev_SetRenderTarget() { trace(__func__); if (Device *d = dev(arg(0))) d->target = arg(1); RET(D3D_OK, 3); }
void dev_GetRenderTarget() { trace(__func__); Device *d = dev(arg(0)); u32 t = d ? d->target : 0; if (t) com_addref(t); wr32(arg(1), t); RET(D3D_OK, 2); }
void dev_Begin() { trace(__func__);
    Device *d = dev(arg(0));
    if (d) { d->b_pt = arg(1); d->b_fvf = arg(2); d->b_flags = arg(3); d->b_verts.clear(); d->b_idx.clear(); d->b_indexed = false; }
    RET(D3D_OK, 4);
}
void dev_BeginIndexed() { trace(__func__);
    Device *d = dev(arg(0));
    if (d) {
        d->b_pt = arg(1); d->b_fvf = arg(2); d->b_flags = arg(5); d->b_indexed = true; d->b_idx.clear();
        u32 sz = fvf_size(arg(2)) * arg(4);
        d->b_verts.assign(MEM + arg(3), MEM + arg(3) + sz);
    }
    RET(D3D_OK, 6);
}
void dev_Vertex() { trace(__func__);
    Device *d = dev(arg(0));
    if (d && !d->b_indexed) { u32 sz = fvf_size(d->b_fvf); d->b_verts.insert(d->b_verts.end(), MEM + arg(1), MEM + arg(1) + sz); }
    RET(D3D_OK, 2);
}
void dev_Index() { trace(__func__); if (Device *d = dev(arg(0))) d->b_idx.push_back((u16)arg(1)); RET(D3D_OK, 2); }
void dev_End() { trace(__func__);
    Device *d = dev(arg(0));
    if (d && !d->b_verts.empty()) {
        u32 tmp = heap_alloc((u32)d->b_verts.size());
        std::memcpy(MEM + tmp, d->b_verts.data(), d->b_verts.size());
        u32 nv = (u32)d->b_verts.size() / std::max(1u, fvf_size(d->b_fvf));
        if (d->b_indexed) draw(*d, d->b_pt, vsrc_flat(d->b_fvf, tmp), 0, nv, d->b_idx.data(), (u32)d->b_idx.size(), d->b_flags);
        else draw(*d, d->b_pt, vsrc_flat(d->b_fvf, tmp), 0, nv, nullptr, 0, d->b_flags);
        heap_free(tmp);
    }
    RET(D3D_OK, 2);
}
void dev_GetRenderState() { trace(__func__); Device *d = dev(arg(0)); wr32(arg(2), d && arg(1) < 256 ? d->rs[arg(1)] : 0); RET(D3D_OK, 3); }
void dev_SetRenderState() { trace(__func__); if (Device *d = dev(arg(0))) set_render_state(*d, arg(1), arg(2)); RET(D3D_OK, 3); }
void dev_GetLightState() { trace(__func__); Device *d = dev(arg(0)); wr32(arg(2), d && arg(1) < 16 ? d->ls[arg(1)] : 0); RET(D3D_OK, 3); }
void dev_SetLightState() { trace(__func__); Device *d = dev(arg(0)); if (d && arg(1) < 16) d->ls[arg(1)] = arg(2); RET(D3D_OK, 3); }
Mat *dev_mat(Device &d, u32 t) { return t == 1 ? &d.world : t == 2 ? &d.view : t == 3 ? &d.proj : nullptr; }
void dev_SetTransform() {
    trace(__func__);
    Device *d = dev(arg(0));
    if (d) if (Mat *m = dev_mat(*d, arg(1))) *m = mat_load(arg(2));
    if (tracing()) {
        static int n = 0;
        if (n++ < 6) {
            Mat m = mat_load(arg(2));
            plog("D3D SetTransform(%u) ret 0x%x: %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g", arg(1), rd32(esp), m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3], m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3], m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
        }
    }
    RET(D3D_OK, 3);
}
void dev_GetTransform() { trace(__func__); Device *d = dev(arg(0)); if (d) if (Mat *m = dev_mat(*d, arg(1))) mat_store(arg(2), *m); RET(D3D_OK, 3); }
void dev_MultiplyTransform() { trace(__func__); Device *d = dev(arg(0)); if (d) if (Mat *m = dev_mat(*d, arg(1))) *m = mat_load(arg(2)) * *m; RET(D3D_OK, 3); }
void dev_DrawPrimitive() { trace(__func__);
    if (Device *d = dev(arg(0))) draw(*d, arg(1), vsrc_flat(arg(2), arg(3)), 0, arg(4), nullptr, 0, arg(5));
    RET(D3D_OK, 6);
}
void dev_DrawIndexedPrimitive() { trace(__func__);
    if (Device *d = dev(arg(0))) { auto ix = read_idx(arg(5), arg(6)); draw(*d, arg(1), vsrc_flat(arg(2), arg(3)), 0, arg(4), ix.data(), arg(6), arg(7)); }
    RET(D3D_OK, 8);
}
void dev_SetClipStatus() { trace(__func__); RET(D3D_OK, 2); }
void dev_GetClipStatus() { trace(__func__); u32 p = arg(1); if (p) std::memset(MEM + p, 0, 24); RET(D3D_OK, 2); }
void dev_DrawPrimitiveStrided() { trace(__func__);
    if (Device *d = dev(arg(0))) draw(*d, arg(1), vsrc_strided(arg(2), arg(3)), 0, arg(4), nullptr, 0, arg(5));
    RET(D3D_OK, 6);
}
void dev_DrawIndexedPrimitiveStrided() { trace(__func__);
    if (Device *d = dev(arg(0))) { auto ix = read_idx(arg(5), arg(6)); draw(*d, arg(1), vsrc_strided(arg(2), arg(3)), 0, arg(4), ix.data(), arg(6), arg(7)); }
    RET(D3D_OK, 8);
}
void dev_DrawPrimitiveVB() { trace(__func__);
    Device *d = dev(arg(0));
    auto it = g_vb.find(arg(2));
    if (d && it != g_vb.end()) draw(*d, arg(1), vsrc_flat(it->second.fvf, it->second.mem), arg(3), arg(4), nullptr, 0, arg(5));
    RET(D3D_OK, 6);
}
void dev_DrawIndexedPrimitiveVB() { trace(__func__);
    Device *d = dev(arg(0));
    auto it = g_vb.find(arg(2));
    if (d && it != g_vb.end()) { auto ix = read_idx(arg(3), arg(4)); draw(*d, arg(1), vsrc_flat(it->second.fvf, it->second.mem), 0, it->second.n, ix.data(), arg(4), arg(5)); }
    RET(D3D_OK, 6);
}
void dev_ComputeSphereVisibility() { trace(__func__); for (u32 i = 0; i < arg(3); i++) wr32(arg(5) + 4 * i, 0); RET(D3D_OK, 6); }
void dev_GetTexture() { trace(__func__); Device *d = dev(arg(0)); u32 t = d && arg(1) < 8 ? d->tex[arg(1)] : 0; wr32(arg(2), t ? dd_surf(t)->tex : 0); RET(D3D_OK, 3); }
void dev_SetTexture() { trace(__func__);
    Device *d = dev(arg(0));
    if (d && arg(1) < 8) { d->tex[arg(1)] = tex_surface(arg(2)); if (arg(1) == 0) d->rs[RS_TEXTUREHANDLE] = d->tex[0]; }
    RET(D3D_OK, 3);
}
void dev_GetTextureStageState() { trace(__func__); Device *d = dev(arg(0)); wr32(arg(3), d && arg(1) < 8 && arg(2) < 32 ? d->tss[arg(1)][arg(2)] : 0); RET(D3D_OK, 4); }
void dev_SetTextureStageState() { trace(__func__);
    Device *d = dev(arg(0));
    u32 s = arg(1), t = arg(2), v = arg(3);
    if (d && s < 8 && t < 32) {
        d->tss[s][t] = v;
        if (t == TSS_ADDRESS) d->tss[s][TSS_ADDRESSU] = d->tss[s][TSS_ADDRESSV] = v;
    }
    RET(D3D_OK, 4);
}
void dev_ValidateDevice() { trace(__func__); if (arg(1)) wr32(arg(1), 1); RET(D3D_OK, 2); }

// ---------------------------------------------------------------- IDirect3DViewport3

void rd_viewport1(Viewport &v, u32 p) {   // D3DVIEWPORT
    v.x = (i32)rd32(p + 4); v.y = (i32)rd32(p + 8); v.w = (i32)rd32(p + 12); v.h = (i32)rd32(p + 16);
    float mx = rdf(p + 28), my = rdf(p + 32);
    if (mx <= 0) mx = 1;
    if (my <= 0) my = 1;
    v.cx = -mx; v.cw = 2 * mx; v.cy = my; v.ch = 2 * my;
    v.minz = rdf(p + 36); v.maxz = rdf(p + 40);
    if (v.maxz <= v.minz) { v.minz = 0; v.maxz = 1; }
}
void rd_viewport2(Viewport &v, u32 p) {   // D3DVIEWPORT2
    v.x = (i32)rd32(p + 4); v.y = (i32)rd32(p + 8); v.w = (i32)rd32(p + 12); v.h = (i32)rd32(p + 16);
    v.cx = rdf(p + 20); v.cy = rdf(p + 24); v.cw = rdf(p + 28); v.ch = rdf(p + 32);
    v.minz = rdf(p + 36); v.maxz = rdf(p + 40);
    if (v.cw == 0) v.cw = 2;
    if (v.ch == 0) v.ch = 2;
}
void wr_viewport2(const Viewport &v, u32 p) {
    wr32(p, 44); wr32(p + 4, (u32)v.x); wr32(p + 8, (u32)v.y); wr32(p + 12, (u32)v.w); wr32(p + 16, (u32)v.h);
    wrf(p + 20, v.cx); wrf(p + 24, v.cy); wrf(p + 28, v.cw); wrf(p + 32, v.ch); wrf(p + 36, v.minz); wrf(p + 40, v.maxz);
}
void vp_GetViewport() { trace(__func__);
    Viewport *v = vp(arg(0));
    u32 p = arg(1);
    if (v) {
        wr32(p, 44); wr32(p + 4, (u32)v->x); wr32(p + 8, (u32)v->y); wr32(p + 12, (u32)v->w); wr32(p + 16, (u32)v->h);
        wrf(p + 20, (float)v->w / v->cw); wrf(p + 24, (float)v->h / v->ch); wrf(p + 28, v->cw / 2); wrf(p + 32, v->ch / 2);
        wrf(p + 36, v->minz); wrf(p + 40, v->maxz);
    }
    RET(D3D_OK, 2);
}
void vp_SetViewport() { trace(__func__); if (Viewport *v = vp(arg(0))) rd_viewport1(*v, arg(1)); RET(D3D_OK, 2); }
void vp_GetViewport2() { trace(__func__); if (Viewport *v = vp(arg(0))) wr_viewport2(*v, arg(1)); RET(D3D_OK, 2); }
void vp_SetViewport2() { trace(__func__); if (Viewport *v = vp(arg(0))) rd_viewport2(*v, arg(1)); RET(D3D_OK, 2); }

void vp_transform(Viewport &v, u32 n, u32 data, u32 flags, u32 offscreen) {
    // D3DTRANSFORMDATA: dwSize, lpIn, dwInSize, lpOut, dwOutSize, lpHOut, dwClip, dwClipIntersection, dwClipUnion, drExtent
    Device *d = dev(v.device);
    Mat wvp = d ? d->world * d->view * d->proj : Mat{};
    u32 in = rd32(data + 4), insz = rd32(data + 8), out = rd32(data + 12), outsz = rd32(data + 16);
    float x0 = v.cx, x1 = v.cx + v.cw, y0 = v.cy - v.ch, y1 = v.cy;
    u32 all = 0xFFFFFFFFu, any = 0;
    float ex0 = 1e30f, ey0 = 1e30f, ex1 = -1e30f, ey1 = -1e30f;
    for (u32 i = 0; i < n; i++) {
        u32 pi = in + i * insz, po = out + i * outsz;
        float x = rdf(pi), y = rdf(pi + 4), z = rdf(pi + 8);
        float cx = x * wvp.m[0][0] + y * wvp.m[1][0] + z * wvp.m[2][0] + wvp.m[3][0];
        float cy = x * wvp.m[0][1] + y * wvp.m[1][1] + z * wvp.m[2][1] + wvp.m[3][1];
        float cz = x * wvp.m[0][2] + y * wvp.m[1][2] + z * wvp.m[2][2] + wvp.m[3][2];
        float cw = x * wvp.m[0][3] + y * wvp.m[1][3] + z * wvp.m[2][3] + wvp.m[3][3];
        u32 code = 0;
        if (cx < x0 * cw) code |= 1;        // D3DCLIP_LEFT
        if (cx > x1 * cw) code |= 2;        // RIGHT
        if (cy > y1 * cw) code |= 4;        // TOP
        if (cy < y0 * cw) code |= 8;        // BOTTOM
        if (cz < 0) code |= 0x10;           // FRONT
        if (cz > cw) code |= 0x20;          // BACK
        all &= code; any |= code;
        float q = 1.f / (std::fabs(cw) < CLIP_EPS ? CLIP_EPS : cw);
        float sx = (float)v.x + (cx * q - v.cx) / v.cw * (float)v.w;
        float sy = (float)v.y + (v.cy - cy * q) / v.ch * (float)v.h;
        float sz = v.minz + cz * q * (v.maxz - v.minz);
        if (outsz >= 16) { wrf(po, sx); wrf(po + 4, sy); wrf(po + 8, sz); wrf(po + 12, q); }
        if (outsz >= 32 && insz >= 32 && out != in) for (u32 k = 16; k < 32; k += 4) wr32(po + k, rd32(pi + k));
        ex0 = std::min(ex0, sx); ey0 = std::min(ey0, sy); ex1 = std::max(ex1, sx); ey1 = std::max(ey1, sy);
        if (u32 h = rd32(data + 20)) {   // D3DHVERTEX: dwFlags, hx, hy, hz
            wr32(h + 16 * i, code); wrf(h + 16 * i + 4, cx); wrf(h + 16 * i + 8, cy); wrf(h + 16 * i + 12, cz);
        }
    }
    (void)flags;
    if (!n) all = 0;
    wr32(data + 28, all); wr32(data + 32, any);
    wr32(data + 36, (u32)(i32)ex0); wr32(data + 40, (u32)(i32)ey0); wr32(data + 44, (u32)(i32)ex1); wr32(data + 48, (u32)(i32)ey1);
    if (offscreen) wr32(offscreen, all ? 1 : 0);
}
void vp_TransformVertices() { trace(__func__);
    if (Viewport *v = vp(arg(0))) vp_transform(*v, arg(1), arg(2), arg(3), arg(4));
    RET(D3D_OK, 5);
}
void vp_SetBackground() { trace(__func__); if (Viewport *v = vp(arg(0))) v->background = arg(1); RET(D3D_OK, 2); }
void vp_GetBackground() { trace(__func__); Viewport *v = vp(arg(0)); wr32(arg(1), v ? v->background : 0); if (arg(2)) wr32(arg(2), v && v->background); RET(D3D_OK, 3); }
void vp_GetBackgroundDepth() { trace(__func__); wr32(arg(1), 0); if (arg(2)) wr32(arg(2), 0); RET(D3D_OK, 3); }

void vp_clear(Viewport &v, u32 n, u32 rects, u32 flags, u32 color, float z) {
    Device *d = dev(v.device);
    Surface *rt = d ? dd_surf(d->target) : nullptr;
    if (!rt || !rt->mem) return;
    Surface *zs = dd_surf(rt->zbuf);
    u32 px = 0;
    if (flags & 1) {
        float c[4];
        unpack_color(color, c);
        if (rt->bpp == 16) px = (u32)(c[0] * 31 + .5f) << 11 | (u32)(c[1] * 63 + .5f) << 5 | (u32)(c[2] * 31 + .5f);
        else if (rt->bpp == 8) {
            const u32 *pal = dd_surface_palette_rgb(rt);
            px = pal ? pal_inverse(pal)[(u32)(c[0] * 31 + .5f) << 10 | (u32)(c[1] * 31 + .5f) << 5 | (u32)(c[2] * 31 + .5f)] : 0;
        } else px = color & 0xFFFFFF;
    }
    u16 zv = (u16)(std::clamp(z, 0.f, 1.f) * 65535.f + .5f);
    for (u32 i = 0; i < std::max(n, 1u); i++) {
        i32 l = v.x, t = v.y, r = v.x + v.w, b = v.y + v.h;
        if (n && rects) { u32 p = rects + 16 * i; l = (i32)rd32(p); t = (i32)rd32(p + 4); r = (i32)rd32(p + 8); b = (i32)rd32(p + 12); }
        l = std::max(l, 0); t = std::max(t, 0); r = std::min(r, rt->w); b = std::min(b, rt->h);
        for (i32 y = t; y < b; y++) {
            if (flags & 1) {
                u8 *row = MEM + rt->mem + (size_t)y * (size_t)rt->pitch;
                for (i32 x = l; x < r; x++) {
                    if (rt->bpp == 16) { row[2 * x] = (u8)px; row[2 * x + 1] = (u8)(px >> 8); }
                    else if (rt->bpp == 8) row[x] = (u8)px;
                    else std::memcpy(row + 4 * x, &px, 4);
                }
            }
            if ((flags & 2) && zs && zs->mem && y < zs->h) {
                u16 *zr = (u16 *)(MEM + zs->mem + (size_t)y * (size_t)zs->pitch);
                for (i32 x = l; x < std::min(r, zs->w); x++) zr[x] = zv;
            }
        }
    }
    if (rt->primary) dd_present_if_primary(d->target);
}
void vp_Clear() { trace(__func__);
    Viewport *v = vp(arg(0));
    if (v) {
        u32 color = 0;
        if (auto it = g_mat.find(v->background); it != g_mat.end()) color = pack_color(it->second.diffuse);
        vp_clear(*v, arg(1), arg(2), arg(3), color, 1.f);
    }
    RET(D3D_OK, 4);
}
void vp_Clear2() { trace(__func__); if (Viewport *v = vp(arg(0))) vp_clear(*v, arg(1), arg(2), arg(3), arg(4), u2f(arg(5))); RET(D3D_OK, 7); }
void vp_AddLight() { trace(__func__); if (Viewport *v = vp(arg(0))) { v->lights.push_back(arg(1)); com_addref(arg(1)); } RET(D3D_OK, 2); }
void vp_DeleteLight() { trace(__func__);
    if (Viewport *v = vp(arg(0))) v->lights.erase(std::remove(v->lights.begin(), v->lights.end(), arg(1)), v->lights.end());
    RET(D3D_OK, 2);
}
void vp_NextLight() { trace(__func__);
    Viewport *v = vp(arg(0));
    u32 r = 0;
    if (v && !v->lights.empty()) {
        if (arg(3) == 2) r = v->lights.front();
        else if (arg(3) == 4) r = v->lights.back();
        else { auto it = std::find(v->lights.begin(), v->lights.end(), arg(1)); if (it != v->lights.end() && ++it != v->lights.end()) r = *it; }
    }
    wr32(arg(2), r);
    RET(r ? D3D_OK : DDERR_INVALIDPARAMS, 4);
}

// ---------------------------------------------------------------- luces y materiales

void light_SetLight() { trace(__func__);
    auto it = g_light.find(arg(0));
    u32 p = arg(1);
    if (it != g_light.end() && p) {
        Light &l = it->second;
        u32 size = rd32(p);
        l.type = rd32(p + 4);
        for (int i = 0; i < 4; i++) l.col[i] = rdf(p + 8 + 4 * (u32)i);
        for (int i = 0; i < 3; i++) { l.pos[i] = rdf(p + 24 + 4 * (u32)i); l.dir[i] = rdf(p + 36 + 4 * (u32)i); }
        l.range = rdf(p + 48);
        if (!(l.range > 0)) l.range = 1e30f;
        for (int i = 0; i < 3; i++) l.att[i] = rdf(p + 56 + 4 * (u32)i);
        if (l.att[0] == 0 && l.att[1] == 0 && l.att[2] == 0) l.att[0] = 1;
        l.active = size < 0x50 || (rd32(p + 0x4C) & 1);   // D3DLIGHT2.dwFlags: D3DLIGHT_ACTIVE
    }
    RET(D3D_OK, 2);
}
void light_GetLight() { trace(__func__);
    auto it = g_light.find(arg(0));
    u32 p = arg(1);
    if (it != g_light.end() && p) {
        const Light &l = it->second;
        wr32(p + 4, l.type);
        for (int i = 0; i < 4; i++) wrf(p + 8 + 4 * (u32)i, l.col[i]);
        for (int i = 0; i < 3; i++) { wrf(p + 24 + 4 * (u32)i, l.pos[i]); wrf(p + 36 + 4 * (u32)i, l.dir[i]); }
    }
    RET(D3D_OK, 2);
}
void mat_SetMaterial() { trace(__func__);
    auto it = g_mat.find(arg(0));
    u32 p = arg(1);   // D3DMATERIAL: dwSize, diffuse, ambient, specular, emissive (D3DCOLORVALUE), power, hTexture, dwRampSize
    if (it != g_mat.end() && p) {
        Material &m = it->second;
        for (int i = 0; i < 4; i++) {
            m.diffuse[i] = rdf(p + 4 + 4 * (u32)i); m.ambient[i] = rdf(p + 20 + 4 * (u32)i);
            m.specular[i] = rdf(p + 36 + 4 * (u32)i); m.emissive[i] = rdf(p + 52 + 4 * (u32)i);
        }
    }
    RET(D3D_OK, 2);
}
void mat_GetMaterial() { trace(__func__);
    auto it = g_mat.find(arg(0));
    u32 p = arg(1);
    if (it != g_mat.end() && p) {
        const Material &m = it->second;
        for (int i = 0; i < 4; i++) {
            wrf(p + 4 + 4 * (u32)i, m.diffuse[i]); wrf(p + 20 + 4 * (u32)i, m.ambient[i]);
            wrf(p + 36 + 4 * (u32)i, m.specular[i]); wrf(p + 52 + 4 * (u32)i, m.emissive[i]);
        }
    }
    RET(D3D_OK, 2);
}
void mat_GetHandle() { trace(__func__); wr32(arg(2), arg(0)); RET(D3D_OK, 3); }   // el handle es el propio objeto

// ---------------------------------------------------------------- texturas

u32 tex_surf_of(u32 iface) { auto it = g_tex_iface.find(iface); return it == g_tex_iface.end() ? 0 : it->second; }
void tex2_GetHandle() { trace(__func__); wr32(arg(2), tex_surf_of(arg(0))); RET(D3D_OK, 3); }
// la interfaz de textura es la misma superficie COM: comparte su contador de referencias
void tex_AddRef() { u32 sf = tex_surf_of(arg(0)); RET(sf ? dd_surface_addref(sf) : 1, 1); }
void tex_Release() { u32 sf = tex_surf_of(arg(0)); RET(sf ? dd_surface_release(sf) : 0, 1); }
void tex_PaletteChanged() { trace(__func__); RET(D3D_OK, 3); }
void tex_load(u32 dst_iface, u32 src_iface) {
    Surface *d = dd_surf(tex_surf_of(dst_iface)), *s = dd_surf(tex_surf_of(src_iface));
    if (!d || !s || !d->mem || !s->mem) return;
    int rows = std::min(d->h, s->h);
    size_t bytes = (size_t)std::min(d->w, s->w) * (size_t)(std::min(d->bpp, s->bpp) / 8);
    for (int y = 0; y < rows; y++)
        std::memmove(MEM + d->mem + (size_t)y * (size_t)d->pitch, MEM + s->mem + (size_t)y * (size_t)s->pitch, bytes);
    if (s->palette && !d->palette) { d->palette = s->palette; com_addref(s->palette); }
    else if (s->palette && d->palette && s->palette != d->palette) {
        if (const u32 *sp = dd_palette_rgb(s->palette)) if (u32 *dp = const_cast<u32 *>(dd_palette_rgb(d->palette))) std::memcpy(dp, sp, 1024);
    }
    d->has_ck = s->has_ck; d->ck_lo = s->ck_lo; d->ck_hi = s->ck_hi;
}
void tex2_Load() { trace(__func__); tex_load(arg(0), arg(1)); RET(D3D_OK, 2); }
void tex1_Initialize() { trace(__func__); RET(D3D_OK, 3); }
void tex1_GetHandle() { trace(__func__); wr32(arg(2), tex_surf_of(arg(0))); RET(D3D_OK, 3); }
void tex1_Load() { trace(__func__); tex_load(arg(0), arg(1)); RET(D3D_OK, 2); }
void tex_QueryInterface() { trace(__func__);
    u32 s = tex_surf_of(arg(0)), iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IDirect3DTexture) || com_guid_eq(iid, IID_IDirect3DTexture2)) { u32 t = d3d_surface_qi(s, iid); wr32(out, t); RET(t ? D3D_OK : E_NOINTERFACE, 3); }
    dd_surface_addref(s);   // las demás interfaces son de la superficie
    wr32(out, s);
    RET(D3D_OK, 3);
}

// ---------------------------------------------------------------- búferes de vértices

void vb_Lock() { trace(__func__);
    auto it = g_vb.find(arg(0));
    if (it == g_vb.end()) RET(DDERR_INVALIDPARAMS, 4);
    wr32(arg(2), it->second.mem);
    if (arg(3)) wr32(arg(3), it->second.n * fvf_size(it->second.fvf));
    RET(D3D_OK, 4);
}
void vb_GetDesc() { trace(__func__);
    auto it = g_vb.find(arg(0));
    u32 p = arg(1);
    if (it != g_vb.end() && p) { wr32(p + 4, it->second.caps); wr32(p + 8, it->second.fvf); wr32(p + 12, it->second.n); }
    RET(D3D_OK, 2);
}
void vb_ProcessVertices() { trace(__func__);   // (op, dstIndex, count, srcVB, srcIndex, device, flags) → TLVERTEX
    auto di = g_vb.find(arg(0)), si = g_vb.find(arg(4));
    Device *d = dev(arg(6));
    if (di == g_vb.end() || si == g_vb.end() || !d) RET(DDERR_INVALIDPARAMS, 8);
    u32 dst0 = arg(2), n = arg(3), src0 = arg(5), op = arg(1);
    Prepared P;
    prepare(*d, vsrc_flat(si->second.fvf, si->second.mem), src0, n, (op & 0x400) ? 0 : 0x10, P);
    u32 dfvf = di->second.fvf, dsz = fvf_size(dfvf);
    VSrc dl = vsrc_flat(dfvf, di->second.mem);
    for (u32 i = 0; i < n; i++) {
        u32 k = dst0 + i;
        const CV &c = P.cv[i];
        SV s = project(P, c);
        u32 base = di->second.mem + k * dsz;
        wrf(base, s.x); wrf(base + 4, s.y); wrf(base + 8, s.z);
        if ((dfvf & FVF_POSMASK) == FVF_XYZRHW) wrf(base + 12, s.q);
        if (dfvf & FVF_DIFFUSE) wr32(dl.dif + k * dsz, pack_color(c.c));
        if (dfvf & FVF_SPECULAR) wr32(dl.spc + k * dsz, pack_color(c.s));
        if (dl.tex) { wrf(dl.tex + k * dsz, c.u); wrf(dl.tex + k * dsz + 4, c.v); }
    }
    RET(D3D_OK, 8);
}

// ---------------------------------------------------------------- IDirect3DDevice2 (DirectX 5, PCF6)
// Igual que IDirect3DDevice3 salvo: SwapTextureHandles tras GetCaps, EnumTextureFormats con DDSURFACEDESC, y los
// vértices se dan por tipo (D3DVERTEXTYPE) en vez de por FVF. Las texturas van por manejador (D3DRENDERSTATE_TEXTUREHANDLE).

// D3DVT_VERTEX / LVERTEX / TLVERTEX → el FVF equivalente (D3DFVF_VERTEX / LVERTEX / TLVERTEX)
u32 vt_to_fvf(u32 vt) {
    switch (vt) {
    case 1: return 0x112;   // XYZ | NORMAL | TEX1
    case 2: return 0x1E2;   // XYZ | RESERVED1 | DIFFUSE | SPECULAR | TEX1
    case 3: return 0x1C4;   // XYZRHW | DIFFUSE | SPECULAR | TEX1
    default: plog_once("d3dvt" + std::to_string(vt), "Direct3D 5: tipo de vértice %u desconocido", vt); return 0x1C4;
    }
}
// el tipo de vértice está en el argumento 2 de Begin, BeginIndexed, DrawPrimitive y DrawIndexedPrimitive: se cambia
// en la pila por su FVF y se sigue con la versión 3
void vt_arg2() { wr32(esp + 4 + 4 * 2, vt_to_fvf(arg(2))); }
void dev2_Begin() { vt_arg2(); dev_Begin(); }
void dev2_BeginIndexed() { vt_arg2(); dev_BeginIndexed(); }
void dev2_DrawPrimitive() { vt_arg2(); dev_DrawPrimitive(); }
void dev2_DrawIndexedPrimitive() { vt_arg2(); dev_DrawIndexedPrimitive(); }
void dev2_SwapTextureHandles() { trace(__func__); RET(D3D_OK, 3); }
void dev2_GetDirect3D() { trace(__func__); if (g_d3d2) com_addref(g_d3d2); wr32(arg(1), g_d3d2); RET(D3D_OK, 2); }
void dev2_EnumTextureFormats() { trace(__func__);   // callback(LPDDSURFACEDESC, ctx)
    u32 cb = arg(1), ctx = arg(2);
    u32 sd = heap_alloc(0x6C, true);
    struct F { u32 flags, bits, r, g, b, a; };
    static const F fmts[] = {
        {0x40, 16, 0xF800, 0x07E0, 0x001F, 0},          // 565
        {0x41, 16, 0x7C00, 0x03E0, 0x001F, 0x8000},     // 1555
        {0x41, 16, 0x0F00, 0x00F0, 0x000F, 0xF000},     // 4444
        {0x40, 16, 0x7C00, 0x03E0, 0x001F, 0},          // 555
        {0x60, 8, 0, 0, 0, 0},                          // 8 bits con paleta
    };
    for (auto &f : fmts) {
        std::memset(MEM + sd, 0, 0x6C);
        wr32(sd, 0x6C); wr32(sd + 4, 0x1001);           // DDSD_CAPS | DDSD_PIXELFORMAT
        u32 pf = sd + 0x48;
        wr32(pf, 32); wr32(pf + 4, f.flags); wr32(pf + 12, f.bits);
        wr32(pf + 16, f.r); wr32(pf + 20, f.g); wr32(pf + 24, f.b); wr32(pf + 28, f.a);
        wr32(sd + 0x68, 0x1000);                         // DDSCAPS_TEXTURE
        if (!guest_call(cb, {sd, ctx})) break;
    }
    heap_free(sd);
    RET(D3D_OK, 3);
}

// ---------------------------------------------------------------- IDirect3D / IDirect3D2 / IDirect3D3

void d3d_EnumDevices() { trace(__func__);
    u32 cb = arg(1), ctx = arg(2);
    u32 buf = heap_alloc(0x300, true);
    u32 guid = buf, desc = buf + 0x10, name = buf + 0x50, hw = buf + 0x60, hel = buf + 0x160;
    wr32(guid, IID_IDirect3DHALDevice.a); wr16(guid + 4, IID_IDirect3DHALDevice.b); wr16(guid + 6, IID_IDirect3DHALDevice.c);
    std::memcpy(MEM + guid + 8, IID_IDirect3DHALDevice.d, 8);
    gstrcpy(desc, "PCF7 Direct3D (software)");
    gstrcpy(name, "Direct3D HAL");
    wr32(hw, 0xFC); fill_d3d_desc(hw);
    wr32(hel, 0xFC); fill_d3d_desc(hel);
    guest_call(cb, {guid, desc, name, hw, hel, ctx});
    heap_free(buf);
    RET(D3D_OK, 3);
}
void d3d_EnumZBufferFormats() { trace(__func__);
    u32 cb = arg(2), ctx = arg(3);
    u32 pf = heap_alloc(32, true);
    wr32(pf, 32); wr32(pf + 4, 0x400);         // DDPF_ZBUFFER
    wr32(pf + 12, 16); wr32(pf + 20, 0xFFFF);  // dwZBufferBitDepth, dwZBitMask
    guest_call(cb, {pf, ctx});
    heap_free(pf);
    RET(D3D_OK, 4);
}
void d3d_CreateLight() { trace(__func__); u32 o = com_object(g_vt_light); g_light[o] = Light{}; wr32(arg(1), o); RET(D3D_OK, 3); }
void d3d_CreateMaterial() { trace(__func__); u32 o = com_object(g_vt_mat); g_mat[o] = Material{}; wr32(arg(1), o); RET(D3D_OK, 3); }
void d3d_CreateViewport() { trace(__func__); u32 o = com_object(g_vt_vp); g_vp[o] = Viewport{}; wr32(arg(1), o); RET(D3D_OK, 3); }
void d3d_FindDevice() { trace(__func__);   // D3DFINDDEVICERESULT: dwSize, guid, ddHwDesc, ddSwDesc
    u32 r = arg(2);
    if (r) {
        u32 g = r + 4;
        wr32(g, IID_IDirect3DHALDevice.a); wr16(g + 4, IID_IDirect3DHALDevice.b); wr16(g + 6, IID_IDirect3DHALDevice.c);
        std::memcpy(MEM + g + 8, IID_IDirect3DHALDevice.d, 8);
        u32 sz = rd32(r);
        if (sz >= 0x14 + 2 * 0xFC) { wr32(r + 0x14, 0xFC); fill_d3d_desc(r + 0x14); wr32(r + 0x14 + 0xFC, 0xFC); fill_d3d_desc(r + 0x14 + 0xFC); }
    }
    RET(D3D_OK, 3);
}
void d3d_CreateDevice() { trace(__func__);
    u32 target = arg(2), out = arg(3);
    Surface *s = dd_surf(target);
    if (!s) { wr32(out, 0); RET(DDERR_INVALIDPARAMS, 5); }
    u32 o = com_object(g_vt_dev);
    Device &d = g_dev[o];
    d.obj = o;
    d.target = target;
    dd_surface_addref(target);
    dev_defaults(d);
    wr32(out, o);
    plog("Direct3D: CreateDevice sobre %dx%dx%d%s -> 0x%x", s->w, s->h, s->bpp, s->zbuf ? " con z-buffer" : "", o);
    RET(D3D_OK, 5);
}
// IDirect3D2::CreateDevice(rclsid, superficie, &dispositivo): como la de IDirect3D3 sin pUnkOuter
void d3d2_CreateDevice() { trace(__func__);
    u32 target = arg(2), out = arg(3);
    Surface *s = dd_surf(target);
    if (!s) { wr32(out, 0); RET(DDERR_INVALIDPARAMS, 4); }
    u32 o = com_object(g_vt_dev2);
    Device &d = g_dev[o];
    d.obj = o;
    d.target = target;
    dd_surface_addref(target);
    dev_defaults(d);
    wr32(out, o);
    plog("Direct3D 5: CreateDevice sobre %dx%dx%d%s -> 0x%x", s->w, s->h, s->bpp, s->zbuf ? " con z-buffer" : "", o);
    RET(D3D_OK, 4);
}
void d3d_CreateVertexBuffer() { trace(__func__);   // D3DVERTEXBUFFERDESC: dwSize, dwCaps, dwFVF, dwNumVertices
    u32 desc = arg(1), out = arg(2);
    VBuffer vb;
    vb.caps = rd32(desc + 4); vb.fvf = rd32(desc + 8); vb.n = rd32(desc + 12);
    vb.mem = heap_alloc(std::max(1u, vb.n * fvf_size(vb.fvf)), true);
    u32 o = com_object(g_vt_vb);
    g_vb[o] = vb;
    wr32(out, o);
    RET(D3D_OK, 5);
}
void d3d_QueryInterface() { trace(__func__);
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IDirect3D3) || com_guid_eq(iid, IID_IDirect3D2) || com_guid_eq(iid, IID_IDirect3D)) {
        u32 o = dd_create_d3d(iid); wr32(out, o); RET(D3D_OK, 3);
    }
    if (com_guid_eq(iid, IID_IDirectDraw4)) { u32 o = dd_get_dd4(); wr32(out, o); RET(D3D_OK, 3); }
    plog_once("d3dqi" + std::to_string(rd32(iid)), "IDirect3D::QueryInterface: interfaz no soportada {%08x-%04x-%04x-%02x%02x}",
              rd32(iid), rd16(iid + 4), rd16(iid + 6), MEM[iid + 8], MEM[iid + 9]);
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}

void init_vtables() {
    if (g_vt_d3d1) return;
    g_vt_d3d1 = com_vtbl({d3d_QueryInterface, m_AddRef, m_Release, m_Ok2, d3d_EnumDevices, d3d_CreateLight, d3d_CreateMaterial,
                          d3d_CreateViewport, d3d_FindDevice});
    g_vt_d3d2 = com_vtbl({d3d_QueryInterface, m_AddRef, m_Release, d3d_EnumDevices, d3d_CreateLight, d3d_CreateMaterial,
                          d3d_CreateViewport, d3d_FindDevice, d3d2_CreateDevice});
    g_vt_dev2 = com_vtbl({dev_QueryInterface, m_AddRef, m_Release, dev_GetCaps, dev2_SwapTextureHandles, dev_GetStats,
                          dev_AddViewport, dev_DeleteViewport, dev_NextViewport, dev2_EnumTextureFormats, dev_BeginScene,
                          dev_EndScene, dev2_GetDirect3D, dev_SetCurrentViewport, dev_GetCurrentViewport, dev_SetRenderTarget,
                          dev_GetRenderTarget, dev2_Begin, dev2_BeginIndexed, dev_Vertex, dev_Index, dev_End, dev_GetRenderState,
                          dev_SetRenderState, dev_GetLightState, dev_SetLightState, dev_SetTransform, dev_GetTransform,
                          dev_MultiplyTransform, dev2_DrawPrimitive, dev2_DrawIndexedPrimitive, dev_SetClipStatus,
                          dev_GetClipStatus});
    g_vt_d3d3 = com_vtbl({d3d_QueryInterface, m_AddRef, m_Release, d3d_EnumDevices, d3d_CreateLight, d3d_CreateMaterial,
                          d3d_CreateViewport, d3d_FindDevice, d3d_CreateDevice, d3d_CreateVertexBuffer, d3d_EnumZBufferFormats, m_Ok1});
    g_vt_dev = com_vtbl({dev_QueryInterface, m_AddRef, m_Release, dev_GetCaps, dev_GetStats, dev_AddViewport, dev_DeleteViewport,
                         dev_NextViewport, dev_EnumTextureFormats, dev_BeginScene, dev_EndScene, dev_GetDirect3D,
                         dev_SetCurrentViewport, dev_GetCurrentViewport, dev_SetRenderTarget, dev_GetRenderTarget, dev_Begin,
                         dev_BeginIndexed, dev_Vertex, dev_Index, dev_End, dev_GetRenderState, dev_SetRenderState,
                         dev_GetLightState, dev_SetLightState, dev_SetTransform, dev_GetTransform, dev_MultiplyTransform,
                         dev_DrawPrimitive, dev_DrawIndexedPrimitive, dev_SetClipStatus, dev_GetClipStatus,
                         dev_DrawPrimitiveStrided, dev_DrawIndexedPrimitiveStrided, dev_DrawPrimitiveVB,
                         dev_DrawIndexedPrimitiveVB, dev_ComputeSphereVisibility, dev_GetTexture, dev_SetTexture,
                         dev_GetTextureStageState, dev_SetTextureStageState, dev_ValidateDevice});
    g_vt_vp = com_vtbl({m_QINone, m_AddRef, m_Release, m_Ok2, vp_GetViewport, vp_SetViewport, vp_TransformVertices, m_Unsup3,
                        vp_SetBackground, vp_GetBackground, m_Ok2, vp_GetBackgroundDepth, vp_Clear, vp_AddLight, vp_DeleteLight,
                        vp_NextLight, vp_GetViewport2, vp_SetViewport2, m_Ok2, vp_GetBackgroundDepth, vp_Clear2});
    g_vt_light = com_vtbl({m_QINone, m_AddRef, m_Release, m_Ok2, light_SetLight, light_GetLight});
    g_vt_mat = com_vtbl({m_QINone, m_AddRef, m_Release, mat_SetMaterial, mat_GetMaterial, mat_GetHandle});
    g_vt_tex1 = com_vtbl({tex_QueryInterface, tex_AddRef, tex_Release, tex1_Initialize, tex1_GetHandle, tex_PaletteChanged, tex1_Load, m_Ok1});
    g_vt_tex2 = com_vtbl({tex_QueryInterface, tex_AddRef, tex_Release, tex2_GetHandle, tex_PaletteChanged, tex2_Load});
    g_vt_vb = com_vtbl({m_QINone, m_AddRef, m_Release, vb_Lock, m_Ok1, vb_ProcessVertices, vb_GetDesc, m_Ok3});
    (void)m_Unsup2;
}


// ================================================================ Direct3D 7 (PC Fútbol 2000: dcsfx.dll, dispositivo RGB)
// Mismo rasterizador y mismo Device que DirectX 6: en DX7 el viewport, las luces y el material ya no son interfaces
// sino estado del dispositivo (aquí objetos internos que el dispositivo posee), SetTexture recibe superficies
// (IDirectDrawSurface7) y las estructuras son las de DX7 (D3DDEVICEDESC7, D3DVIEWPORT7, D3DLIGHT7, D3DMATERIAL7).
// Solo lo usa PCF2000: los juegos de DX5/DX6 no pasan por aquí.
u32 dd_get_dd7();   // ddraw.cpp

const ComGuid IID_IDirect3D7 = {0xF5049E77, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8}};
const ComGuid IID_IDirect3DRGBDevice = {0xA4665C60, 0x2673, 0x11CF, {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
u32 g_vt_d3d7 = 0, g_vt_dev7 = 0, g_vt_vb7 = 0, g_d3d7 = 0;
void m_Ok4() { RET(D3D_OK, 4); }
void m_Ok8() { RET(D3D_OK, 8); }

struct Dev7 { u32 viewport = 0, material = 0; u32 light[8] = {}; bool light_on[8] = {}; };
std::map<u32, Dev7> g_dev7;
Dev7 *dev7(u32 o) { auto it = g_dev7.find(o); return it == g_dev7.end() ? nullptr : &it->second; }

void write_guid(u32 p, const ComGuid &g) { wr32(p, g.a); wr16(p + 4, g.b); wr16(p + 6, g.c); std::memcpy(MEM + p + 8, g.d, 8); }

// D3DDEVICEDESC7 (236 B): las mismas capacidades que fill_d3d_desc, en el formato de DX7
void fill_d3d_desc7(u32 p) {
    std::memset(MEM + p, 0, 236);
    wr32(p + 0x00, 0x1 | 0x2 | 0x10 | 0x40 | 0x80 | 0x200 | 0x400 | 0x800 | 0x4000 | 0x10000);   // dwDevCaps
    for (u32 pc : {0x04u, 0x3Cu}) {   // dpcLineCaps, dpcTriCaps (D3DPRIMCAPS, 56 B)
        wr32(p + pc, 56); wr32(p + pc + 4, 0x7F); wr32(p + pc + 8, 0x1 | 0x2 | 0x10 | 0x80 | 0x100 | 0x8000);
        wr32(p + pc + 12, 0xFF); wr32(p + pc + 16, 0x1FFF); wr32(p + pc + 20, 0x1FFF); wr32(p + pc + 24, 0xFF);
        wr32(p + pc + 28, 0x7FFFF); wr32(p + pc + 32, 0x1 | 0x2 | 0x4 | 0x20 | 0x40); wr32(p + pc + 36, 0x3F);
        wr32(p + pc + 40, 0xFF); wr32(p + pc + 44, 0x1F);
    }
    wr32(p + 0x74, 0x400 | 0x100);   // dwDeviceRenderBitDepth: DDBD_16 | DDBD_32
    wr32(p + 0x78, 0x400);           // dwDeviceZBufferBitDepth: DDBD_16
    wr32(p + 0x7C, 1); wr32(p + 0x80, 1); wr32(p + 0x84, 1024); wr32(p + 0x88, 1024);   // texturas mín/máx
    wr32(p + 0x94, 1);               // dwMaxAnisotropy
    wr32(p + 0xB0, 8);               // dwFVFCaps: 8 juegos de coordenadas
    wr32(p + 0xB4, 0x1FFFFFF);       // dwTextureOpCaps
    wr16(p + 0xB8, 2); wr16(p + 0xBA, 1);   // wMaxTextureBlendStages, wMaxSimultaneousTextures
    wr32(p + 0xBC, 8);               // dwMaxActiveLights
    wrf(p + 0xC0, 1e10f);            // dvMaxVertexW
    write_guid(p + 0xC4, IID_IDirect3DRGBDevice);
    wr32(p + 0xD8, 0x1 | 0x2 | 0x8 | 0x10 | 0x20);   // dwVertexProcessingCaps
}

// ---- IDirect3D7
void d3d7_QueryInterface() { trace(__func__);
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IDirect3D7)) { com_addref(arg(0)); wr32(out, arg(0)); RET(D3D_OK, 3); }
    d3d_QueryInterface();
}
void d3d7_EnumDevices() { trace(__func__);   // callback(desc, name, D3DDEVICEDESC7*, ctx): solo el dispositivo RGB por software
    u32 cb = arg(1), ctx = arg(2);
    u32 buf = heap_alloc(0x40 + 236, true), desc = buf, name = buf + 0x20, dd = buf + 0x40;
    gstrcpy(desc, "PCF Direct3D RGB (software)");
    gstrcpy(name, "RGB Emulation");
    fill_d3d_desc7(dd);
    guest_call(cb, {desc, name, dd, ctx});
    heap_free(buf);
    RET(D3D_OK, 3);
}
void d3d7_CreateDevice() { trace(__func__);   // (rclsid, superficie destino, &dispositivo)
    u32 target = arg(2), out = arg(3);
    Surface *s = dd_surf(target);
    if (!s) { wr32(out, 0); RET(DDERR_INVALIDPARAMS, 4); }
    u32 o = com_object(g_vt_dev7);
    Device &d = g_dev[o];
    d.obj = o;
    d.target = target;
    dd_surface_addref(target);
    dev_defaults(d);
    Dev7 &x = g_dev7[o];
    x.viewport = com_object(g_vt_vp);
    Viewport &v = g_vp[x.viewport];
    v.device = o; v.x = 0; v.y = 0; v.w = s->w; v.h = s->h;
    d.viewport = x.viewport;
    x.material = com_object(g_vt_mat);
    g_mat[x.material] = Material{};
    d.ls[LS_MATERIAL] = x.material;
    d.rs[137] = 1;   // D3DRENDERSTATE_LIGHTING
    wr32(out, o);
    plog("Direct3D 7: CreateDevice sobre %dx%dx%d%s -> 0x%x", s->w, s->h, s->bpp, s->zbuf ? " con z-buffer" : "", o);
    RET(D3D_OK, 4);
}
void d3d7_CreateVertexBuffer() { trace(__func__);   // (D3DVERTEXBUFFERDESC*, &vb, flags)
    u32 desc = arg(1), out = arg(2);
    VBuffer vb;
    vb.caps = rd32(desc + 4); vb.fvf = rd32(desc + 8); vb.n = rd32(desc + 12);
    vb.mem = heap_alloc(std::max(1u, vb.n * fvf_size(vb.fvf)), true);
    u32 o = com_object(g_vt_vb7);
    g_vb[o] = vb;
    wr32(out, o);
    RET(D3D_OK, 4);
}

// ---- IDirect3DDevice7
void dev7_GetCaps() { trace(__func__); if (arg(1)) fill_d3d_desc7(arg(1)); RET(D3D_OK, 2); }
void dev7_GetDirect3D() { trace(__func__); if (g_d3d7) com_addref(g_d3d7); wr32(arg(1), g_d3d7); RET(D3D_OK, 2); }
void dev7_Clear() { trace(__func__);   // (n, rects, flags, color, z, stencil)
    Device *d = dev(arg(0));
    if (d) if (Viewport *v = vp(d->viewport)) vp_clear(*v, arg(1), arg(2), arg(3), arg(4), u2f(arg(5)));
    RET(D3D_OK, 7);
}
void dev7_SetViewport() { trace(__func__);   // D3DVIEWPORT7: dwX, dwY, dwWidth, dwHeight, dvMinZ, dvMaxZ
    Device *d = dev(arg(0));
    u32 p = arg(1);
    if (d && p) if (Viewport *v = vp(d->viewport)) {
        v->x = (i32)rd32(p); v->y = (i32)rd32(p + 4); v->w = (i32)rd32(p + 8); v->h = (i32)rd32(p + 12);
        v->minz = rdf(p + 16); v->maxz = rdf(p + 20);
        if (v->maxz <= v->minz) { v->minz = 0; v->maxz = 1; }
        v->cx = -1; v->cy = 1; v->cw = 2; v->ch = 2;   // volumen de recorte de DX7: -1..1
    }
    RET(D3D_OK, 2);
}
void dev7_GetViewport() { trace(__func__);
    Device *d = dev(arg(0));
    u32 p = arg(1);
    if (d && p) if (Viewport *v = vp(d->viewport)) {
        wr32(p, (u32)v->x); wr32(p + 4, (u32)v->y); wr32(p + 8, (u32)v->w); wr32(p + 12, (u32)v->h);
        wrf(p + 16, v->minz); wrf(p + 20, v->maxz);
    }
    RET(D3D_OK, 2);
}
void dev7_SetMaterial() { trace(__func__);   // D3DMATERIAL7: diffuse, ambient, specular, emissive (D3DCOLORVALUE), power
    Dev7 *x = dev7(arg(0));
    u32 p = arg(1);
    if (x && p) if (auto it = g_mat.find(x->material); it != g_mat.end()) {
        Material &m = it->second;
        for (int i = 0; i < 4; i++) {
            m.diffuse[i] = rdf(p + 4 * (u32)i); m.ambient[i] = rdf(p + 16 + 4 * (u32)i);
            m.specular[i] = rdf(p + 32 + 4 * (u32)i); m.emissive[i] = rdf(p + 48 + 4 * (u32)i);
        }
    }
    RET(D3D_OK, 2);
}
void dev7_GetMaterial() { trace(__func__);
    Dev7 *x = dev7(arg(0));
    u32 p = arg(1);
    if (x && p) if (auto it = g_mat.find(x->material); it != g_mat.end()) {
        const Material &m = it->second;
        for (int i = 0; i < 4; i++) {
            wrf(p + 4 * (u32)i, m.diffuse[i]); wrf(p + 16 + 4 * (u32)i, m.ambient[i]);
            wrf(p + 32 + 4 * (u32)i, m.specular[i]); wrf(p + 48 + 4 * (u32)i, m.emissive[i]);
        }
        wrf(p + 64, 0);
    }
    RET(D3D_OK, 2);
}
Light *dev7_light(Dev7 &x, u32 i) {
    if (i >= 8) return nullptr;
    if (!x.light[i]) { x.light[i] = com_object(g_vt_light); g_light[x.light[i]] = Light{}; g_light[x.light[i]].active = false; }
    return &g_light[x.light[i]];
}
void dev7_SetLight() { trace(__func__);   // (índice, D3DLIGHT7*): dltType, diffuse, specular, ambient, position, direction, range, falloff, att0..2, theta, phi
    Dev7 *x = dev7(arg(0));
    u32 i = arg(1), p = arg(2);
    if (x && p) if (Light *l = dev7_light(*x, i)) {
        l->type = rd32(p);
        for (int k = 0; k < 4; k++) l->col[k] = rdf(p + 4 + 4 * (u32)k);
        for (int k = 0; k < 3; k++) { l->pos[k] = rdf(p + 52 + 4 * (u32)k); l->dir[k] = rdf(p + 64 + 4 * (u32)k); }
        l->range = rdf(p + 76);
        if (!(l->range > 0)) l->range = 1e30f;
        for (int k = 0; k < 3; k++) l->att[k] = rdf(p + 84 + 4 * (u32)k);
        if (l->att[0] == 0 && l->att[1] == 0 && l->att[2] == 0) l->att[0] = 1;
        l->active = x->light_on[i];
    }
    RET(D3D_OK, 3);
}
void dev7_GetLight() { trace(__func__);
    Dev7 *x = dev7(arg(0));
    u32 i = arg(1), p = arg(2);
    if (x && p && i < 8 && x->light[i]) {
        const Light &l = g_light[x->light[i]];
        std::memset(MEM + p, 0, 104);
        wr32(p, l.type);
        for (int k = 0; k < 4; k++) wrf(p + 4 + 4 * (u32)k, l.col[k]);
        for (int k = 0; k < 3; k++) { wrf(p + 52 + 4 * (u32)k, l.pos[k]); wrf(p + 64 + 4 * (u32)k, l.dir[k]); }
        wrf(p + 76, l.range);
        for (int k = 0; k < 3; k++) wrf(p + 84 + 4 * (u32)k, l.att[k]);
    }
    RET(D3D_OK, 3);
}
void dev7_LightEnable() { trace(__func__);   // (índice, activa)
    Device *d = dev(arg(0));
    Dev7 *x = dev7(arg(0));
    u32 i = arg(1);
    bool on = arg(2) != 0;
    if (d && x) if (Light *l = dev7_light(*x, i)) {
        x->light_on[i] = on;
        l->active = on;
        if (Viewport *v = vp(d->viewport)) {
            auto &ls = v->lights;
            auto it = std::find(ls.begin(), ls.end(), x->light[i]);
            if (on && it == ls.end()) ls.push_back(x->light[i]);
            if (!on && it != ls.end()) ls.erase(it);
        }
    }
    RET(D3D_OK, 3);
}
void dev7_GetLightEnable() { trace(__func__); Dev7 *x = dev7(arg(0)); wr32(arg(2), x && arg(1) < 8 && x->light_on[arg(1)] ? 1 : 0); RET(D3D_OK, 3); }
void dev7_SetRenderState() {   // los estados nuevos de DX7 que el pipeline toma de los light states de DX6
    Device *d = dev(arg(0));
    if (d) {
        if (arg(1) == 139) d->ls[LS_AMBIENT] = arg(2);        // D3DRENDERSTATE_AMBIENT
        else if (arg(1) == 140) d->ls[LS_FOGMODE] = arg(2);   // D3DRENDERSTATE_FOGVERTEXMODE
    }
    dev_SetRenderState();
}
void dev7_GetTexture() { trace(__func__); Device *d = dev(arg(0)); u32 t = d && arg(1) < 8 ? d->tex[arg(1)] : 0; if (t) dd_surface_addref(t); wr32(arg(2), t); RET(D3D_OK, 3); }
void dev7_EndStateBlock() { trace(__func__); if (arg(1)) wr32(arg(1), 1); RET(D3D_OK, 2); }
void dev7_CreateStateBlock() { trace(__func__); if (arg(2)) wr32(arg(2), 1); RET(D3D_OK, 3); }
void dev7_DrawIndexedPrimitiveVB() { trace(__func__);   // (tipo, vb, primer vértice, nº vértices, índices, nº índices, flags)
    Device *d = dev(arg(0));
    auto it = g_vb.find(arg(2));
    if (d && it != g_vb.end()) { auto ix = read_idx(arg(5), arg(6)); draw(*d, arg(1), vsrc_flat(it->second.fvf, it->second.mem), arg(3), arg(4), ix.data(), arg(6), arg(7)); }
    RET(D3D_OK, 8);
}
void dev7_Load() { trace(__func__);   // (destino, punto, origen, rect, flags): copia de textura (superficies)
    Surface *dst = dd_surf(arg(1)), *src = dd_surf(arg(3));
    if (dst && src && dst->mem && src->mem) {
        int rows = std::min(dst->h, src->h);
        size_t bytes = (size_t)std::min(dst->w, src->w) * (size_t)(std::min(dst->bpp, src->bpp) / 8);
        for (int y = 0; y < rows; y++)
            std::memmove(MEM + dst->mem + (size_t)y * (size_t)dst->pitch, MEM + src->mem + (size_t)y * (size_t)src->pitch, bytes);
        if (src->palette && !dst->palette) { dst->palette = src->palette; com_addref(src->palette); }
        dst->has_ck = src->has_ck; dst->ck_lo = src->ck_lo; dst->ck_hi = src->ck_hi;
    }
    RET(D3D_OK, 6);
}

void init_vtables7() {
    if (g_vt_d3d7) return;
    init_vtables();
    g_vt_d3d7 = com_vtbl({d3d7_QueryInterface, m_AddRef, m_Release, d3d7_EnumDevices, d3d7_CreateDevice, d3d7_CreateVertexBuffer,
                          d3d_EnumZBufferFormats, m_Ok1});
    g_vt_dev7 = com_vtbl({dev_QueryInterface, m_AddRef, m_Release, dev7_GetCaps, dev_EnumTextureFormats, dev_BeginScene,
                          dev_EndScene, dev7_GetDirect3D, dev_SetRenderTarget, dev_GetRenderTarget, dev7_Clear,
                          dev_SetTransform, dev_GetTransform, dev7_SetViewport, dev_MultiplyTransform, dev7_GetViewport,
                          dev7_SetMaterial, dev7_GetMaterial, dev7_SetLight, dev7_GetLight, dev7_SetRenderState,
                          dev_GetRenderState, m_Ok1, dev7_EndStateBlock, m_Ok2, dev_DrawPrimitive, dev_DrawIndexedPrimitive,
                          dev_SetClipStatus, dev_GetClipStatus, dev_DrawPrimitiveStrided, dev_DrawIndexedPrimitiveStrided,
                          dev_DrawPrimitiveVB, dev7_DrawIndexedPrimitiveVB, dev_ComputeSphereVisibility, dev7_GetTexture,
                          dev_SetTexture, dev_GetTextureStageState, dev_SetTextureStageState, dev_ValidateDevice, m_Ok2,
                          m_Ok2, m_Ok2, dev7_CreateStateBlock, dev7_Load, dev7_LightEnable, dev7_GetLightEnable, m_Ok3,
                          m_Ok3, m_Unsup3});
    g_vt_vb7 = com_vtbl({m_QINone, m_AddRef, m_Release, vb_Lock, m_Ok1, vb_ProcessVertices, vb_GetDesc, m_Ok3, m_Ok8});
    (void)m_Ok4;
}
u32 d3d7_object() {
    init_vtables7();
    if (!g_d3d7) g_d3d7 = com_object(g_vt_d3d7); else com_addref(g_d3d7);
    return g_d3d7;
}

std::map<u32, u32> g_tex1_of;   // superficie → interfaz IDirect3DTexture (v1)

}  // namespace

void d3d_stats() {
    for (auto &[k, v] : g_calls) plog("  D3D %-36s %llu", k.c_str(), (unsigned long long)v);
}

u32 dd_create_d3d(u32 iid) {
    if (com_guid_eq(iid, IID_IDirect3D7)) return d3d7_object();   // DirectX 7 (PCF2000)
    bool v3 = com_guid_eq(iid, IID_IDirect3D3), v2 = com_guid_eq(iid, IID_IDirect3D2);
    if (!v3 && !v2 && !com_guid_eq(iid, IID_IDirect3D)) return 0;
    init_vtables();
    u32 &o = v3 ? g_d3d3 : v2 ? g_d3d2 : g_d3d1;
    if (!o) o = com_object(v3 ? g_vt_d3d3 : v2 ? g_vt_d3d2 : g_vt_d3d1); else com_addref(o);
    return o;
}

u32 d3d_surface_qi(u32 surface, u32 iid) {
    Surface *s = dd_surf(surface);
    if (!s) return 0;
    init_vtables();
    if (com_guid_eq(iid, IID_IDirect3DTexture2)) {
        if (!s->tex) { s->tex = com_object(g_vt_tex2); g_tex_iface[s->tex] = surface; }
        dd_surface_addref(surface);
        return s->tex;
    }
    if (com_guid_eq(iid, IID_IDirect3DTexture)) {
        u32 &t = g_tex1_of[surface];
        if (!t) { t = com_object(g_vt_tex1); g_tex_iface[t] = surface; }
        dd_surface_addref(surface);
        return t;
    }
    return 0;
}

void d3d_surface_gone(u32 surface) {
    Surface *s = dd_surf(surface);
    if (s && s->tex) g_tex_iface.erase(s->tex);
    if (auto it = g_tex1_of.find(surface); it != g_tex1_of.end()) { g_tex_iface.erase(it->second); g_tex1_of.erase(it); }
    for (auto &[o, d] : g_dev) {
        for (u32 &t : d.tex) if (t == surface) t = 0;
        if (d.rs[RS_TEXTUREHANDLE] == surface) d.rs[RS_TEXTUREHANDLE] = 0;
    }
}
