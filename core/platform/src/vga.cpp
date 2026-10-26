// PCF7 — tarjeta VGA para los juegos de MS-DOS (PCF_DOS): la ventana 0xA0000-0xBFFFF de la memoria del juego
// (rt.h desvía ahí los accesos a vga_rd8/vga_wr8) y los puertos 0x3C0-0x3DF.
//
// Se emula el hardware de verdad: cuatro planos de 64 KiB, latches, máscara de mapa del secuenciador, modos de
// escritura 0-3 (rotación, set/reset, operación lógica, máscara de bits) y de lectura 0-1 (comparación de color),
// el controlador de atributos (paleta de 16 → índice del DAC) y el DAC de 256 colores de 6 bits. Modos: 12h
// (640x480x16 planar, el de PC Fútbol 4), 13h (320x200x256, chain-4) y texto (pantalla negra).
//
// La imagen se compone en el framebuffer de 8 bits de la plataforma (display.h) desde dos sitios: vga_present()
// (lo llama el temporizador de dos.cpp ~60 veces por segundo si algo ha cambiado).
#ifdef PCF_DOS
#include "display.h"
#include "plat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

u32 ticks_ms();

namespace {

u8 planes[4][0x10000];
u8 latch[4];
u8 seq[8], gc[16], crtc[32], ac[32];
u8 seq_i, gc_i, crtc_i, ac_i;
bool ac_flip = false;              // 3C0: false = índice, true = dato (se reinicia leyendo 3DA)
u8 misc = 0x63;
u8 dac[256][3];                    // 6 bits por componente
u8 dac_wi, dac_wc, dac_ri, dac_rc;  // índice y componente de escritura/lectura
u8 modo = 3;                       // modo de BIOS actual
std::atomic<bool> sucio{true};

// paleta por defecto del modo 12h/13h (los 16 colores de EGA/VGA y la escala de grises e intensidades de 13h)
const u8 EGA16[16][3] = {{0, 0, 0}, {0, 0, 42}, {0, 42, 0}, {0, 42, 42}, {42, 0, 0}, {42, 0, 42}, {42, 21, 0}, {42, 42, 42},
                         {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63}, {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63}};

inline bool chain4() { return seq[4] & 0x08; }

// desplazamiento dentro de la ventana según el mapa de memoria del controlador gráfico (GC 6, bits 2-3)
inline bool ventana(u32 a, u32 &off) {
    u32 o = a - 0xA0000u;
    switch ((gc[6] >> 2) & 3) {
    case 0: off = o & 0xFFFF; return o < 0x20000;
    case 1: off = o; return o < 0x10000;
    case 2: if (o < 0x10000 || o >= 0x18000) return false; off = o - 0x10000; return true;
    default: if (o < 0x18000) return false; off = o - 0x18000; return true;
    }
}

inline u8 logica(u8 v, u8 l) {
    switch ((gc[3] >> 3) & 3) {
    case 1: return v & l;
    case 2: return v | l;
    case 3: return v ^ l;
    default: return v;
    }
}

}  // namespace

u32 vga_rd8(u32 a) {
    u32 off;
    if (!ventana(a, off)) return 0xFF;
    if (chain4()) return planes[off & 3][off >> 2];
    for (int i = 0; i < 4; i++) latch[i] = planes[i][off];
    if (gc[5] & 0x08) {   // modo de lectura 1: comparación de color
        u8 r = 0xFF;
        for (int i = 0; i < 4; i++) {
            if (!(gc[7] & (1 << i))) continue;   // plano que no importa
            u8 c = (gc[2] & (1 << i)) ? 0xFF : 0x00;
            r &= ~(latch[i] ^ c);
        }
        return r;
    }
    return latch[gc[4] & 3];
}

void vga_wr8(u32 a, u32 v8) {
    u32 off;
    if (!ventana(a, off)) return;
    // PCF7_VGA_TRAZA=desde-hasta (desplazamientos en hex): qué función escribe ahí (una vez por función)
    static const char *tz = std::getenv("PCF7_VGA_TRAZA");
    if (tz) {
        static u32 lo = (u32)std::strtoul(tz, nullptr, 16), hi = std::strchr(tz, '-') ? (u32)std::strtoul(std::strchr(tz, '-') + 1, nullptr, 16) : 0;
        static const bool todas = std::getenv("PCF7_VGA_TRAZA_TODAS") != nullptr;
        // PCF7_VGA_TRAZA_DESDE=ms y PCF7_VGA_TRAZA_MAX=n: desde ese momento, hasta n escrituras
        static const u32 desde = std::getenv("PCF7_VGA_TRAZA_DESDE") ? (u32)std::atoi(std::getenv("PCF7_VGA_TRAZA_DESDE")) : 0;
        static u32 restantes = std::getenv("PCF7_VGA_TRAZA_MAX") ? (u32)std::atoi(std::getenv("PCF7_VGA_TRAZA_MAX")) : 100000;
        if (off >= lo && off < hi && todas && ticks_ms() >= desde && restantes && restantes--) {
            std::string ra;   // direcciones de retorno probables en la pila (quién llama de verdad)
            for (u32 p = esp, n = 0; p < esp + 0x200 && n < 6; p += 4) {
                u32 r = rd32(p);
                if (r > 0x401000 && r < 0x480000 && (MEM[r - 5] == 0xE8)) { ra += " " + std::to_string(0) ; ra.pop_back(); char b[12]; snprintf(b, sizeof b, "%x", r); ra += b; n++; }
            }
            plog("VGA: f_%x escribe en %x valor %02x mapa %x modo %u máscara %02x s/r %x/%x; retornos%s", cur_fn, off, v8 & 0xFF, seq[2], gc[5] & 3, gc[8], gc[1], gc[0], ra.c_str());
        }
        else if (off >= lo && off < hi) plog_once("vgaw" + std::to_string(cur_fn), "VGA: f_%x escribe en %x (modo %u, mapa %x, máscara %02x, valor %02x; ds %x base %x, es %x base %x)", cur_fn, off, gc[5] & 3, seq[2], gc[8], v8 & 0xFF, sreg[3], sbase[3], sreg[0], sbase[0]);
    }
    u8 v = (u8)v8;
    sucio = true;
    if (chain4()) { planes[off & 3][off >> 2] = v; return; }
    u8 mapa = seq[2] & 0x0F;
    u8 rot = gc[3] & 7;
    u8 rv = (u8)((v >> rot) | (v << (8 - rot)));
    u8 mask = gc[8];
    switch (gc[5] & 3) {
    case 0:
        for (int i = 0; i < 4; i++) {
            if (!(mapa & (1 << i))) continue;
            u8 d = (gc[1] & (1 << i)) ? ((gc[0] & (1 << i)) ? 0xFF : 0x00) : rv;
            d = logica(d, latch[i]);
            planes[i][off] = (u8)((d & mask) | (latch[i] & ~mask));
        }
        break;
    case 1:
        for (int i = 0; i < 4; i++) if (mapa & (1 << i)) planes[i][off] = latch[i];
        break;
    case 2:
        for (int i = 0; i < 4; i++) {
            if (!(mapa & (1 << i))) continue;
            u8 d = (v & (1 << i)) ? 0xFF : 0x00;
            d = logica(d, latch[i]);
            planes[i][off] = (u8)((d & mask) | (latch[i] & ~mask));
        }
        break;
    case 3: {
        u8 m = rv & mask;
        for (int i = 0; i < 4; i++) {
            if (!(mapa & (1 << i))) continue;
            u8 d = (gc[0] & (1 << i)) ? 0xFF : 0x00;
            d = logica(d, latch[i]);
            planes[i][off] = (u8)((d & m) | (latch[i] & ~m));
        }
        break;
    }
    }
}

// retrazado vertical a 70 Hz: activo el primer ~1,4 ms de cada cuadro (los juegos esperan el flanco)
static u8 estado1() {
    using namespace std::chrono;
    u64 us = (u64)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    u64 f = us % 14286;
    u8 r = 0;
    if (f < 1400) r |= 0x08 | 0x01;
    else if ((us / 32) & 1) r |= 0x01;   // display enable (líneas horizontales)
    return r;
}

u32 vga_port_in(u32 port) {
    switch (port) {
    case 0x3C1: return ac[ac_i & 0x1F];
    case 0x3C4: return seq_i;
    case 0x3C5: return seq[seq_i & 7];
    case 0x3C7: return 3;   // estado del DAC: lectura
    case 0x3C8: return dac_wi;
    case 0x3C9: {
        u8 v = dac[dac_ri][dac_rc];
        if (++dac_rc == 3) { dac_rc = 0; dac_ri++; }
        return v;
    }
    case 0x3CC: return misc;
    case 0x3CE: return gc_i;
    case 0x3CF: return gc[gc_i & 15];
    case 0x3D4: case 0x3B4: return crtc_i;
    case 0x3D5: case 0x3B5: return crtc[crtc_i & 31];
    case 0x3DA: case 0x3BA: ac_flip = false; return estado1();
    }
    return 0xFF;
}

void vga_port_out(u32 port, u32 v) {
    v &= 0xFF;
    switch (port) {
    case 0x3C0:
        if (!ac_flip) ac_i = (u8)v;
        else { ac[ac_i & 0x1F] = (u8)v; sucio = true; }
        ac_flip = !ac_flip;
        break;
    case 0x3C2: misc = (u8)v; break;
    case 0x3C4: seq_i = (u8)v; break;
    case 0x3C5:
        if ((seq_i & 7) == 4 && seq[4] != (u8)v) plog("VGA: secuenciador 4 = %02x (%s) en f_%x", (u8)v, (v & 8) ? "encadenado" : "desencadenado", cur_fn);
        seq[seq_i & 7] = (u8)v;
        break;
    case 0x3C7: dac_ri = (u8)v; dac_rc = 0; break;
    case 0x3C8: dac_wi = (u8)v; dac_wc = 0; break;
    case 0x3C9:
        dac[dac_wi][dac_wc] = (u8)(v & 0x3F);
        if (++dac_wc == 3) { dac_wc = 0; dac_wi++; }
        sucio = true;
        break;
    case 0x3CE: gc_i = (u8)v; break;
    case 0x3CF: gc[gc_i & 15] = (u8)v; break;
    case 0x3D4: case 0x3B4: crtc_i = (u8)v; break;
    case 0x3D5: case 0x3B5: crtc[crtc_i & 31] = (u8)v; sucio = true; break;
    }
}

// int 10h: cambio de modo con los registros que deja la BIOS
void vga_set_mode(u8 m) {
    modo = m & 0x7F;
    std::memset(seq, 0, sizeof seq); std::memset(gc, 0, sizeof gc); std::memset(crtc, 0, sizeof crtc); std::memset(ac, 0, sizeof ac);
    seq[2] = 0x0F;
    gc[6] = 0x05; gc[7] = 0x0F; gc[8] = 0xFF;
    ac[0x12] = 0x0F;
    for (int i = 0; i < 16; i++) ac[i] = (u8)i;
    if (modo == 0x12 || modo == 0x11 || modo == 0x10 || modo == 0x0D || modo == 0x0E) {
        seq[4] = 0x06;
        static const u8 ega[16] = {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};
        for (int i = 0; i < 16; i++) ac[i] = ega[i];
        ac[0x10] = 0x01;
        // DAC: los 64 colores de EGA en los índices que usa la paleta de atributos
        for (int i = 0; i < 64; i++) {
            int r = ((i >> 2) & 1) * 2 + ((i >> 5) & 1), g = ((i >> 1) & 1) * 2 + ((i >> 4) & 1), b = (i & 1) * 2 + ((i >> 3) & 1);
            dac[i][0] = (u8)(r * 21); dac[i][1] = (u8)(g * 21); dac[i][2] = (u8)(b * 21);
        }
    } else if (modo == 0x13) {
        seq[4] = 0x0E;
        gc[5] = 0x40;
        ac[0x10] = 0x41;
        for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) dac[i][c] = EGA16[i][c];
        for (int i = 16; i < 32; i++) for (int c = 0; c < 3; c++) dac[i][c] = (u8)((i - 16) * 63 / 15);
    }
    if (!(m & 0x80)) for (auto &p : planes) std::memset(p, 0, sizeof p);
    if (modo == 0x13) set_display_mode(320, 200, 8);
    else set_display_mode(640, 480, 8);
    sucio = true;
    plog("VGA: modo %02xh", modo);
}
u8 vga_mode() { return modo; }

// int 10h AX=10xx: paleta (los valores en bruto: 6 bits)
void vga_set_dac(u8 i, u8 r, u8 g, u8 b) { dac[i][0] = r & 0x3F; dac[i][1] = g & 0x3F; dac[i][2] = b & 0x3F; sucio = true; }
void vga_get_dac(u8 i, u8 &r, u8 &g, u8 &b) { r = dac[i][0]; g = dac[i][1]; b = dac[i][2]; }
void vga_set_ac(u8 i, u8 v) { ac[i & 0x1F] = v; sucio = true; }
u8 vga_get_ac(u8 i) { return ac[i & 0x1F]; }

// Compone la imagen en el framebuffer de la plataforma si algo ha cambiado
void vga_present() {
    // PCF7_VGA_ESTADO: cada ~5 s, registros y contenido (diagnóstico de pantallas negras)
    static const bool estado = std::getenv("PCF7_VGA_ESTADO") != nullptr;
    static u32 ult = 0;
    if (estado && ticks_ms() - ult > 5000) {
        ult = ticks_ms();
        u32 nz[4] = {}, dz = 0;
        for (int p = 0; p < 4; p++) for (u32 i = 0; i < 0x10000; i++) nz[p] += planes[p][i] != 0;
        for (int i = 0; i < 256; i++) dz += dac[i][0] | dac[i][1] | dac[i][2] ? 1 : 0;
        plog("VGA: modo %02xh seq4 %02x mapa %x gc5 %02x gc6 %02x crtc inicio %02x%02x offset %02x alto %02x; planos %u %u %u %u; DAC no negros %u",
             modo, seq[4], seq[2], gc[5], gc[6], crtc[0x0C], crtc[0x0D], crtc[0x13], crtc[0x12], nz[0], nz[1], nz[2], nz[3], dz);
    }
    if (!sucio.exchange(false)) return;
    Display &d = display();
    std::lock_guard<std::mutex> lk(d.mx);
    Screen &s = d.screen;
    if (!s.fb) return;
    for (int i = 0; i < 256; i++) {
        u32 r = dac[i][0] * 255 / 63, g = dac[i][1] * 255 / 63, b = dac[i][2] * 255 / 63;
        s.pal[i] = (r << 16) | (g << 8) | b;
    }
    u8 *fb = MEM + s.fb;
    u32 inicio = ((u32)crtc[0x0C] << 8) | crtc[0x0D];
    if (modo == 0x13 && s.w == 320) {
        if (chain4()) {
            for (int y = 0; y < 200; y++)
                for (int x = 0; x < 320; x++) {
                    u32 o = (u32)(y * 320 + x) + inicio * 4;
                    fb[y * s.pitch + x] = planes[o & 3][(o >> 2) & 0xFFFF];
                }
        } else {
            // desencadenado («modo X» de 200 líneas): un byte por plano = 4 píxeles; la anchura de fila la da el
            // registro de desplazamiento del CRTC (en palabras) y el desplazamiento horizontal fino el de píxel del AC
            u32 pitch = crtc[0x13] ? (u32)crtc[0x13] * 2 : 80;
            u32 pan = ac[0x13] & 3;
            for (int y = 0; y < 200; y++) {
                u32 fila = inicio + (u32)y * pitch;
                for (int x = 0; x < 320; x++) {
                    u32 px = (u32)x + pan;
                    fb[y * s.pitch + x] = planes[px & 3][(fila + (px >> 2)) & 0xFFFF];
                }
            }
        }
    } else if ((modo == 0x12 || modo == 0x11 || modo == 0x10 || modo == 0x0D || modo == 0x0E) && s.w == 640) {
        u8 hi = (ac[0x10] & 0x80) ? (u8)((ac[0x14] & 0x0F) << 4) : (u8)((ac[0x14] & 0x0C) << 4);
        u8 col[16];
        for (int c = 0; c < 16; c++) {
            u8 v = ac[c] & 0x3F;
            col[c] = (ac[0x10] & 0x80) ? (u8)(hi | (v & 0x0F)) : (u8)(hi | v);
        }
        int alto = modo == 0x12 || modo == 0x11 ? 480 : 350;
        for (int y = 0; y < alto && y < s.h; y++) {
            u32 fila = inicio + (u32)y * 80;
            u8 *dst = fb + y * s.pitch;
            for (int bx = 0; bx < 80; bx++) {
                u32 o = (fila + bx) & 0xFFFF;
                u8 p0 = planes[0][o], p1 = planes[1][o], p2 = planes[2][o], p3 = planes[3][o];
                for (int k = 0; k < 8; k++) {
                    int sh = 7 - k;
                    u8 c = (u8)(((p0 >> sh) & 1) | (((p1 >> sh) & 1) << 1) | (((p2 >> sh) & 1) << 2) | (((p3 >> sh) & 1) << 3));
                    dst[bx * 8 + k] = col[c & ac[0x12]];
                }
            }
        }
    } else {
        std::memset(fb, 0, (size_t)s.pitch * s.h);   // modo de texto: negro
    }
    s.frame++;
}
#endif
