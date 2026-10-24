// PCF7 — cursores del juego (CURSORES\*.CUR): se cargan del fichero y los pinta el host encima de la
// pantalla emulada (el juego va a pantalla completa y no hay cursor del sistema).
#include "cursor.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

u32 ticks_ms();

namespace {
struct Cursor { std::vector<CursorImage> frames; u32 ms_per_frame = 100; };
std::map<u32, Cursor> g_cursors;
u32 g_next = 0x50000;   // cursores de fichero (fuera del rango de los del sistema)

// .CUR = cabecera ICONDIR + entrada (con punto caliente) + DIB (XOR + AND, alto doble)
bool parse_cur(const std::vector<u8> &f, CursorImage &out) {
    auto u16at = [&](size_t o) { return (u32)f[o] | (u32)f[o + 1] << 8; };
    auto u32at = [&](size_t o) { return u16at(o) | u16at(o + 2) << 16; };
    if (f.size() < 22 || u16at(2) != 2) return false;
    out.hot_x = (int)u16at(10);
    out.hot_y = (int)u16at(12);
    size_t off = u32at(18);
    if (off + 40 > f.size()) return false;
    int w = (int)u32at(off + 4), h = (int)u32at(off + 8) / 2;
    int bpp = (int)u16at(off + 14);
    u32 hdr = u32at(off);
    u32 ncol = u32at(off + 32);
    if (bpp <= 8 && !ncol) ncol = 1u << bpp;
    size_t pal = off + hdr, xr = pal + 4 * (bpp <= 8 ? ncol : 0);
    size_t xstride = (((size_t)w * bpp + 31) / 32) * 4, astride = (((size_t)w + 31) / 32) * 4;
    size_t an = xr + xstride * h;
    if (an + astride * h > f.size()) return false;
    out.w = w; out.h = h;
    out.rgba.assign((size_t)w * h, 0);
    for (int y = 0; y < h; y++) {
        int sy = h - 1 - y;
        for (int x = 0; x < w; x++) {
            bool transp = (f[an + astride * sy + x / 8] >> (7 - (x & 7))) & 1;
            u32 c;
            if (bpp == 32) c = u32at(xr + xstride * sy + 4 * x) & 0xFFFFFF;
            else if (bpp == 24) { size_t o = xr + xstride * sy + 3 * x; c = (u32)f[o + 2] << 16 | (u32)f[o + 1] << 8 | f[o]; }
            else {
                u32 idx = bpp == 8 ? f[xr + xstride * sy + x]
                        : bpp == 4 ? (f[xr + xstride * sy + x / 2] >> ((x & 1) ? 0 : 4)) & 15
                                   : (f[xr + xstride * sy + x / 8] >> (7 - (x & 7))) & 1;
                size_t q = pal + 4 * idx;
                c = (u32)f[q + 2] << 16 | (u32)f[q + 1] << 8 | f[q];
            }
            if (transp && c == 0) continue;            // transparente
            if (transp) c ^= 0xFFFFFF;                 // invertir: se aproxima con el complementario
            out.rgba[(size_t)y * w + x] = 0xFF000000u | c;
        }
    }
    return true;
}

// .ANI = RIFF "ACON": "anih" (nº de fotogramas, velocidad en 1/60 s) + LIST "fram" con un "icon" (.CUR) por fotograma
bool parse_ani(const std::vector<u8> &f, Cursor &out) {
    auto u32at = [&](size_t o) { return (u32)f[o] | (u32)f[o + 1] << 8 | (u32)f[o + 2] << 16 | (u32)f[o + 3] << 24; };
    if (f.size() < 12 || std::memcmp(f.data(), "RIFF", 4) || std::memcmp(f.data() + 8, "ACON", 4)) return false;
    for (size_t o = 12; o + 8 <= f.size();) {
        u32 n = u32at(o + 4);
        if (!std::memcmp(f.data() + o, "anih", 4) && o + 8 + 36 <= f.size()) {
            u32 rate = u32at(o + 8 + 28);
            out.ms_per_frame = rate ? rate * 1000 / 60 : 100;
        } else if (!std::memcmp(f.data() + o, "LIST", 4) && o + 12 <= f.size() && !std::memcmp(f.data() + o + 8, "fram", 4)) {
            for (size_t q = o + 12; q + 8 <= o + 8 + n && q + 8 <= f.size();) {
                u32 m = u32at(q + 4);
                if (!std::memcmp(f.data() + q, "icon", 4) && q + 8 + m <= f.size()) {
                    std::vector<u8> cur(f.begin() + (long)q + 8, f.begin() + (long)(q + 8 + m));
                    CursorImage img;
                    if (parse_cur(cur, img)) out.frames.push_back(std::move(img));
                }
                q += 8 + ((m + 1) & ~1u);
            }
        }
        o += 8 + ((n + 1) & ~1u);
    }
    return !out.frames.empty();
}
// Cursores del sistema (LoadCursorA(NULL, IDC_*)): Windows los pinta él; aquí se dibuja una flecha
// (y un reloj de arena para IDC_WAIT/IDC_APPSTARTING) para que el ratón no desaparezca.
const char *const ARROW[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
    "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      X..X  ",
    "       XX   ",
};
const char *const IBEAM[] = {
    "XXX XXX", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ",
    "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "XXX XXX",
};
const char *const HOURGLASS[] = {
    "XXXXXXXXXXX", "X.........X", " X.......X ", " X.......X ", "  X.....X  ", "   X...X   ", "    X.X    ",
    "    X.X    ", "   X...X   ", "  X.....X  ", " X.......X ", " X.......X ", "X.........X", "XXXXXXXXXXX",
};
template <size_t N>
CursorImage make_system(const char *const (&rows)[N], int hx, int hy) {
    CursorImage c;
    c.w = (int)std::strlen(rows[0]);
    c.h = (int)N;
    c.hot_x = hx; c.hot_y = hy;
    c.rgba.assign((size_t)c.w * c.h, 0);
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++)
            c.rgba[(size_t)y * c.w + x] = rows[y][x] == 'X' ? 0xFF000000u : rows[y][x] == '.' ? 0xFFFFFFFFu : 0;
    return c;
}
const CursorImage *system_cursor(u32 h) {
    static const CursorImage arrow = make_system(ARROW, 0, 0), wait = make_system(HOURGLASS, 5, 7), ibeam = make_system(IBEAM, 3, 8);
    u32 id = h - 0x30000;   // LoadCursorA devuelve 0x30000 + IDC_*
    if (id == 32513) return &ibeam;   // IDC_IBEAM
    return id == 32514 || id == 32650 ? &wait : &arrow;
}
}  // namespace

u32 load_cursor_file(const std::string &winpath) {
    std::ifstream in(host_path(winpath), std::ios::binary);
    if (!in) { plog("LoadCursorFromFileA(\"%s\") -> no existe", winpath.c_str()); return 0; }
    std::vector<u8> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Cursor c;
    CursorImage img;
    if (parse_cur(f, img)) c.frames.push_back(std::move(img));
    else if (!parse_ani(f, c)) { plog("LoadCursorFromFileA(\"%s\"): formato no soportado", winpath.c_str()); return 0; }
    u32 h = g_next++;
    g_cursors[h] = std::move(c);
    return h;
}

const CursorImage *cursor_image(u32 h) {
    auto it = g_cursors.find(h);
    if (it == g_cursors.end() || it->second.frames.empty()) {
        // LoadCursorA devuelve 0x30000 + IDC_* (IDC_ARROW = 32512 → 0x37F00): cursores del sistema
        if (h >= 0x30000 && h < 0x40000) return system_cursor(h);
        return nullptr;
    }
    const Cursor &c = it->second;
    return &c.frames[(ticks_ms() / std::max<u32>(c.ms_per_frame, 1)) % c.frames.size()];
}
