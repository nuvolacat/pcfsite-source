// PCF7 — MS-DOS con DOS/4GW para los juegos de DOS (PCF_DOS; p. ej. PC Fútbol 4).
//
// El programa es un LE de Watcom convertido en PE (tools/le2pe.py) y traducido como cualquier otro; el traductor
// convierte `int n` en dos_int(n), `in`/`out` en port_in/port_out y lleva los registros de segmento (sreg/sbase:
// las direcciones son planas más la base del segmento, 0 en el código normal). Aquí está lo que en un PC de 1995
// hacían DOS, DOS/4GW (DPMI), la BIOS, el controlador del ratón y el hardware:
//   - int 21h: ficheros (sobre platform/vfs: C:\FUTBOL4 → instalación), búsquedas, fecha, hora, memoria, salida
//   - int 31h: DPMI (descriptores y sus bases, memoria, vectores de interrupción, interrupciones en modo real)
//   - int 10h (vídeo, vga.cpp), 16h (teclado), 1Ah (reloj), 33h (ratón), 2Fh (ceder la CPU)
//   - puertos: temporizador 8253 (40h-43h), controlador de interrupciones (20h/21h), teclado (60h/64h), VGA
//   - IRQ 0 (temporizador) e IRQ 1 (teclado): un hilo del host las marca pendientes y el juego las atiende en los
//     saltos hacia atrás de sus bucles (DOS_POLL) llamando a los manejadores que instaló, como haría la CPU.
// Memoria baja: PSP en 0x8000 (selector 0x18), entorno en 0x8100 (0x20), memoria DOS (DPMI 0100h) desde 0x20000.
#ifdef PCF_DOS
#include "display.h"
#include "plat.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <setjmp.h>
#include <thread>
#include <vector>

u32 ticks_ms();
void process_exit(u32 code);   // loader.cpp
u32 vga_port_in(u32 port);     // vga.cpp
void vga_port_out(u32 port, u32 v);
void vga_set_mode(u8 m);
u8 vga_mode();
void vga_set_dac(u8 i, u8 r, u8 g, u8 b);
void vga_get_dac(u8 i, u8 &r, u8 &g, u8 &b);
void vga_set_ac(u8 i, u8 v);
u8 vga_get_ac(u8 i);
void vga_present();
bool sb_puerto(u32 port);      // sb.cpp: Sound Blaster (puertos 0x220-0x22F, DMA 8237, IRQ 5)
u32 sb_in(u32 port);
void sb_out(u32 port, u32 v);
bool sb_irq_pendiente();
void sb_tick();
void sb_avance_tick(double hz);

u32 g_ret_expected = 0xC0DEF00Du;
u32 g_sombra[4096];
u32 g_sombra_n = 0;
u32 g_ret_jump = 0;
u32 sreg[6] = {0x18, 0x08, 0x10, 0x10, 0, 0};   // es cs ss ds fs gs (es = PSP al arrancar, como en DOS/4GW)
u32 sbase[6];
u32 g_if = 1;
volatile u32 g_irq_pend = 0;

namespace {

constexpr u32 PSP = 0x8000, ENV = 0x8100, DOSMEM_LO = 0x20000, DOSMEM_HI = 0x9F000;
constexpr u32 SEL_CODE = 0x08, SEL_DATA = 0x10, SEL_PSP = 0x18, SEL_ENV = 0x20;

// ---------------------------------------------------------------- registros
inline u32 AL() { return eax & 0xFF; }
inline u32 AH() { return (eax >> 8) & 0xFF; }
inline u32 AX() { return eax & 0xFFFF; }
inline u32 BL() { return ebx & 0xFF; }
inline u32 BX() { return ebx & 0xFFFF; }
inline u32 CX() { return ecx & 0xFFFF; }
inline u32 DX() { return edx & 0xFFFF; }
inline u32 DL() { return edx & 0xFF; }
inline void setAX(u32 v) { eax = (eax & 0xFFFF0000u) | (v & 0xFFFF); }
inline void setAL(u32 v) { eax = (eax & 0xFFFFFF00u) | (v & 0xFF); }
inline void setBX(u32 v) { ebx = (ebx & 0xFFFF0000u) | (v & 0xFFFF); }
inline void setCX(u32 v) { ecx = (ecx & 0xFFFF0000u) | (v & 0xFFFF); }
inline void setDX(u32 v) { edx = (edx & 0xFFFF0000u) | (v & 0xFFFF); }
inline u32 lin_ds(u32 off) { return sbase[3] + off; }
inline u32 lin_es(u32 off) { return sbase[0] + off; }
inline void ok() { CF = 0; }
inline void err(u32 code) { CF = 1; setAX(code); }

// ---------------------------------------------------------------- descriptores (LDT)
struct Desc { u32 base = 0, limit = 0xFFFFFFFFu; bool used = false; };
Desc g_desc[8192];
u32 sel_base(u32 sel) { u32 i = (sel & 0xFFFF) >> 3; return i < 8192 && g_desc[i].used ? g_desc[i].base : 0; }
u32 desc_new(u32 base) {
    for (u32 i = 8; i < 8192; i++) if (!g_desc[i].used) { g_desc[i] = Desc{base, 0xFFFFFFFFu, true}; return i << 3; }
    fatal("DPMI: sin descriptores libres");
}

// ---------------------------------------------------------------- vectores de interrupción
struct Vec { u32 off = 0, sel = 0; };
Vec g_pm[256];     // modo protegido (DPMI 0204/0205, int 21h 25/35)
Vec g_rm[256];     // modo real (DPMI 0200/0201): solo se guardan
u32 g_vec_default = 0;   // manejador por defecto (no hace nada; vuelve con iretd)
void vec_default() { esp += 12; }   // como `iretd` (lo llaman con el marco eflags/cs/eip)

// ---------------------------------------------------------------- temporizador, teclado y ratón
std::mutex g_mx;
std::atomic<u32> g_pit_div{65536};
std::atomic<u32> g_timer_pend{0};
std::deque<u8> g_scan;          // códigos de rastreo pendientes de IRQ 1
u8 g_port60 = 0;
std::deque<u16> g_bios_keys;    // búfer de la BIOS (int 16h): (scan << 8) | ascii
u8 g_shift = 0;                 // estado de mayúsculas/ctrl/alt (BDA 0x417)
struct Raton { int x = 320, y = 240, botones = 0, minx = 0, maxx = 639, miny = 0, maxy = 479, visible = -1;
               int ex = 0, ey = 0;   // unidades del juego por píxel (0 = aún sin fijar)
               int pulsa[3] = {}, suelta[3] = {}; int px[3] = {}, py[3] = {}; u32 cb_mask = 0; Vec cb; } g_raton;
struct EvRaton { u32 ev; int botones, x, y; };
std::deque<EvRaton> g_ev_raton;   // eventos para la rutina del juego (int 33h AX=000Ch), uno a uno
std::atomic<bool> g_fin{false};
bool g_en_irq = false;

const char ASCII_MIN[0x3A] = {0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', '\xa1', 8, 9, 'q', 'w', 'e', 'r', 't', 'y',
                              'u', 'i', 'o', 'p', '`', '+', 13, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\xf1', '\xb4',
                              '\xba', 0, '\xe7', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '};

// Evento del host (display().input) → teclado/ratón de DOS. Lo llama el hilo del temporizador.
void host_input() {
    Display &d = display();
    std::deque<InEvent> evs;
    { std::lock_guard<std::mutex> lk(d.mx); evs.swap(d.input); }
    if (evs.empty()) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (const InEvent &e : evs) {
        switch (e.kind) {
        case InKind::KeyDown: case InKind::KeyUp: {
            bool down = e.kind == InKind::KeyDown;
            if (e.ext) g_scan.push_back(0xE0);
            g_scan.push_back((u8)((e.scan & 0x7F) | (down ? 0 : 0x80)));
            u8 bit = e.scan == 0x2A ? 2 : e.scan == 0x36 ? 1 : e.scan == 0x1D ? 4 : e.scan == 0x38 ? 8 : 0;
            if (bit) { if (down) g_shift |= bit; else g_shift &= (u8)~bit; }
            if (down && !bit) {   // la BIOS lo deja en su búfer (int 16h), con su carácter
                u8 sc = (u8)(e.scan & 0x7F), a = sc < sizeof ASCII_MIN ? (u8)ASCII_MIN[sc] : 0;
                if (e.ext) a = 0;
                if ((g_shift & 3) && a >= 'a' && a <= 'z') a = (u8)(a - 32);
                if (g_bios_keys.size() < 16) g_bios_keys.push_back((u16)((sc << 8) | a));
            }
            g_irq_pend = 1;
            break;
        }
        case InKind::MouseMove: case InKind::MouseDown: case InKind::MouseUp: {
            Raton &r = g_raton;
            // el ratón del host es absoluto (píxeles de la pantalla): se pasa a las unidades del juego con la escala
            // de su primer rango completo (PC Fútbol 4: 0-2512 x 0-1916, cuatro por píxel) y se limita al rango
            // vigente (el juego lo encierra a veces en una zona, como un ratón de verdad que topa con el borde)
            const Screen &sc = display().screen;
            int ex = r.ex ? r.ex : std::max(1, (r.maxx + 1 + sc.w / 2) / std::max(1, sc.w));
            int ey = r.ey ? r.ey : std::max(1, (r.maxy + 1 + sc.h / 2) / std::max(1, sc.h));
            r.x = std::max(r.minx, std::min(r.maxx, e.x * ex));
            r.y = std::max(r.miny, std::min(r.maxy, e.y * ey));
            u32 ev = 1;
            if (e.kind != InKind::MouseMove && e.button < 3) {
                int b = e.button == 1 ? 1 : e.button == 2 ? 2 : 0;   // InEvent: 0 izq, 1 der, 2 central
                if (e.kind == InKind::MouseDown) { r.botones |= 1 << b; r.pulsa[b]++; r.px[b] = r.x; r.py[b] = r.y; ev = 2u << (2 * b); }
                else { r.botones &= ~(1 << b); r.suelta[b]++; r.px[b] = r.x; r.py[b] = r.y; ev = 4u << (2 * b); }
            }
            if (r.cb.off && (r.cb_mask & ev)) {
                // los movimientos se fusionan (solo importa la última posición); pulsar y soltar van cada uno a su evento
                if (ev == 1 && !g_ev_raton.empty() && g_ev_raton.back().ev == 1) g_ev_raton.back() = {ev, r.botones, r.x, r.y};
                else if (g_ev_raton.size() < 64) g_ev_raton.push_back({ev, r.botones, r.x, r.y});
                g_irq_pend = 1;
            }
            break;
        }
        case InKind::Quit: g_fin = true; break;
        default: break;
        }
    }
}

// Hilo del host: IRQ 0 al ritmo del 8253, ticks de la BIOS (18,2 Hz), entrada y pantalla
void temporizador() {
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    double hechos = 0, bios = 0;
    u32 ult_pant = 0, div_ant = 0;
    auto t_div = t0;
    while (!g_fin) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        double s = std::chrono::duration<double>(clk::now() - t0).count();
        u32 div = g_pit_div ? g_pit_div.load() : 65536;
        if (div != div_ant) { div_ant = div; t_div = clk::now(); hechos = 0; }   // divisor nuevo: la cuenta empieza de cero
        double debidos = std::chrono::duration<double>(clk::now() - t_div).count() * 1193182.0 / div;
        if (debidos - hechos >= 1) {
            u32 n = (u32)(debidos - hechos);
            hechos += n;
            if (g_timer_pend < 200) g_timer_pend += n;   // si el juego se para del todo, no se acumulan sin fin (2 s)
            g_irq_pend = 1;
        }
        double tb = s * 1193182.0 / 65536;
        while (bios + 1 <= tb) { bios += 1; wr32(0x46C, rd32(0x46C) + 1); }
        host_input();
        sb_tick();
        u32 ms = ticks_ms();
        if (ms - ult_pant >= 16) { ult_pant = ms; vga_present(); }
    }
}

// Llama a un manejador de interrupción del juego como la CPU: marco eflags/cs/eip, IF a 0, y se restaura todo
void llamar_manejador(const Vec &v, bool lejano_retf = false) {
    u32 r[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi};
    u32 fl[7] = {CF, ZF, SF, OF, PF, DF, g_if};
    u32 sr[6], sb[6];
    std::memcpy(sr, sreg, sizeof sr); std::memcpy(sb, sbase, sizeof sb);
    u32 saved_esp = esp, saved_fn = cur_fn;
    if (!lejano_retf) { esp -= 4; wr32(esp, 0x202); }   // eflags (iretd)
    esp -= 4; wr32(esp, sreg[1]);
    esp -= 4; wr32(esp, 0xC0DEF00Du);
    u32 saved_ret = g_ret_expected, saved_n = g_sombra_n;
    DOS_CALL_PRE(0xC0DEF00Du);
    g_if = 0;
    sreg[1] = v.sel; sbase[1] = sel_base(v.sel);
    if (!icall_try(v.off)) plog_once("irq" + std::to_string(v.off), "manejador de interrupción 0x%x sin traducir", v.off);
    eax = r[0]; ecx = r[1]; edx = r[2]; ebx = r[3]; ebp = r[5]; esi = r[6]; edi = r[7];
    esp = saved_esp;
    cur_fn = saved_fn;
    g_ret_expected = saved_ret;
    g_sombra_n = saved_n;
    if (g_ret_jump == 0xC0DEF00Du) g_ret_jump = 0;
    CF = fl[0]; ZF = fl[1]; SF = fl[2]; OF = fl[3]; PF = fl[4]; DF = fl[5]; g_if = fl[6];
    std::memcpy(sreg, sr, sizeof sr); std::memcpy(sbase, sb, sizeof sb);
}

}  // namespace

// PCF7_SEHCHECK=1 (activa g_watch): llamadas directas tras las que esi/ebx/edi cambian (en DOS no se restauran)
void call_regchg(u32 at, u32 s, u32 b, u32 d) {
    static int n = 0;
    if (n++ < 400) plog("regs tras la llamada en %x (a f_%x): esi %x->%x ebx %x->%x edi %x->%x", at, cur_fn, s, esi, b, ebx, d, edi);
}

void seg_load(u32 i, u32 sel) { sreg[i] = sel & 0xFFFF; sbase[i] = sel_base(sel); }

// Atiende las IRQ pendientes (desde DOS_POLL: el juego está entre dos instrucciones, con IF=1)
void irq_servir() {
    if (g_en_irq) return;
    g_en_irq = true;
    g_irq_pend = 0;
    // todos los ticks pendientes: el juego mezcla el sonido por ticks y un tick perdido deja un hueco en el búfer
    for (int vueltas = 0; vueltas < 200 && g_timer_pend; vueltas++) {
        g_timer_pend--;
        static const bool sin_irq0 = std::getenv("PCF7_SIN_IRQ0") != nullptr;   // diagnóstico
        if (g_pm[8].off && !sin_irq0) llamar_manejador(g_pm[8]);
        sb_avance_tick(1193182.0 / (g_pit_div ? g_pit_div.load() : 65536));   // el DMA de la Sound Blaster va con los ticks
    }
    if (g_timer_pend) g_irq_pend = 1;
    if (sb_irq_pendiente()) {   // IRQ 5: fin de bloque de la Sound Blaster
        static int n = 0;
        if (n++ < 3) plog("SB: IRQ 5 → manejador %x:%x", g_pm[0x0D].sel, g_pm[0x0D].off);
        if (g_pm[0x0D].off) llamar_manejador(g_pm[0x0D]);
    }
    for (;;) {
        u8 sc;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_scan.empty()) break;
            sc = g_scan.front();
            g_scan.pop_front();
        }
        g_port60 = sc;
        if (g_pm[9].off) llamar_manejador(g_pm[9]);
    }
    // controlador del ratón: la rutina del juego (int 33h AX=000Ch) con AX=eventos, BX=botones, CX/DX=posición
    // pulsar y soltar, de uno en uno y separados (≥ 40 ms, como una mano): la rutina del juego solo anota el último
    // estado y el bucle del menú lo mira después; una pulsación y su suelta seguidas no llegaría a verse. Los
    // movimientos van sin espera.
    static u32 ult_ev = 0;
    for (;;) {
        EvRaton e;
        Vec cb;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_ev_raton.empty()) break;
            if (g_ev_raton.front().ev != 1 && ticks_ms() - ult_ev < 40) { g_irq_pend = 1; break; }
            if (g_ev_raton.front().ev != 1) ult_ev = ticks_ms();
            e = g_ev_raton.front();
            g_ev_raton.pop_front();
            cb = g_raton.cb;
        }
        if (!cb.off) continue;
        u32 r[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi};
        eax = e.ev; ebx = (u32)e.botones; ecx = (u32)e.x; edx = (u32)e.y; esi = edi = 0;
        llamar_manejador(cb, true);
        eax = r[0]; ecx = r[1]; edx = r[2]; ebx = r[3]; esi = r[6]; edi = r[7];
    }
    if (g_fin) process_exit(0);
    g_en_irq = false;
}

// ---------------------------------------------------------------- puertos
u32 port_in(u32 port, u32 size) {
    if (size == 2) return port_in(port, 1) | (port_in(port + 1, 1) << 8);
    if (size == 4) return port_in(port, 2) | (port_in(port + 2, 2) << 16);
    if (port >= 0x3B0 && port <= 0x3DF) return vga_port_in(port);
    if (sb_puerto(port)) return sb_in(port);
    switch (port) {
    case 0x20: return 0;
    case 0x21: return 0;
    case 0x40: { static u32 c = 0; c += 37; return c & 0xFF; }   // contador del 8253: avanza
    case 0x60: return g_port60;
    case 0x61: return 0x20;
    case 0x64: { std::lock_guard<std::mutex> lk(g_mx); return g_scan.empty() ? 0x14u : 0x15u; }
    case 0x201: return 0xF0;   // puerto de juegos sin joystick: ejes a 0 (nunca «cargando»), botones sin pulsar
    }
    plog_once("in" + std::to_string(port), "DOS: lectura del puerto 0x%x sin emular (f_%x)", port, cur_fn);
    return 0xFF;
}

void port_out(u32 port, u32 v, u32 size) {
    if (size == 2) { port_out(port, v & 0xFF, 1); port_out(port + 1, (v >> 8) & 0xFF, 1); return; }
    if (size == 4) { port_out(port, v & 0xFFFF, 2); port_out(port + 2, v >> 16, 2); return; }
    v &= 0xFF;
    if (port >= 0x3B0 && port <= 0x3DF) { vga_port_out(port, v); return; }
    if (sb_puerto(port)) { sb_out(port, v); return; }
    switch (port) {
    case 0x20: case 0x21: case 0xA0: case 0xA1: return;   // PIC: EOI y máscaras
    case 0x43: return;                                     // 8253: modo (el divisor llega por 40h, bajo y alto)
    case 0x40: {
        static bool alto = false;
        static u32 bajo = 0;
        if (!alto) bajo = v;
        else { u32 d = bajo | (v << 8); g_pit_div = d ? d : 65536; plog("8253: temporizador a %.1f Hz", 1193182.0 / g_pit_div); }
        alto = !alto;
        return;
    }
    case 0x61: return;   // altavoz
    case 0x201: return;  // disparo del joystick: no hay
    }
    plog_once("out" + std::to_string(port), "DOS: escritura en el puerto 0x%x sin emular (f_%x)", port, cur_fn);
}

// ---------------------------------------------------------------- int 21h (DOS)
namespace {

struct Fich { int fd = -1; std::string ruta; };
std::map<u32, Fich> g_fich;
u32 g_dta = PSP + 0x80;
struct Busca { std::vector<std::pair<std::string, struct stat>> res; size_t i = 0; };
std::map<u32, Busca> g_busca;   // por dirección del DTA
u32 g_dosmem = DOSMEM_LO;

std::string dos_str(u32 lin) { return gstr(lin, 260); }

bool comodin(const std::string &pat, const std::string &nom) {   // *.* y ? de DOS (8.3, sin distinguir mayúsculas)
    auto parte = [](const std::string &s, std::string &n, std::string &e) {
        size_t p = s.rfind('.');
        n = p == std::string::npos ? s : s.substr(0, p);
        e = p == std::string::npos ? "" : s.substr(p + 1);
    };
    auto casa = [](const std::string &p, const std::string &s) {
        size_t i = 0;
        for (; i < p.size(); i++) {
            if (p[i] == '*') return true;
            if (i >= s.size()) { for (size_t k = i; k < p.size(); k++) if (p[k] != '?' && p[k] != '*') return false; return true; }
            if (p[i] != '?' && std::toupper((unsigned char)p[i]) != std::toupper((unsigned char)s[i])) return false;
        }
        return i == s.size();
    };
    std::string pn, pe, nn, ne;
    parte(pat, pn, pe);
    parte(nom, nn, ne);
    if (pat == "*.*" || pat == "*") return true;
    return casa(pn, nn) && casa(pe, ne);
}

void dta_rellenar(u32 dta) {
    Busca &b = g_busca[dta];
    if (b.i >= b.res.size()) { err(0x12); return; }
    auto &[nom, st] = b.res[b.i++];
    std::tm tm{};
    localtime_r(&st.st_mtime, &tm);
    wr8(dta + 21, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
    wr16(dta + 22, (u32)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
    wr16(dta + 24, (u32)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
    wr32(dta + 26, (u32)st.st_size);
    std::string up = nom;
    for (auto &c : up) c = (char)std::toupper((unsigned char)c);
    gstrcpy(dta + 30, up, 13);
    ok();
}

void int16();

void int21() {
    switch (AH()) {
    case 0x02: std::fputc((int)DL(), stderr); return;
    case 0x06: if (DL() != 0xFF) { std::fputc((int)DL(), stderr); return; } ZF = 1; setAL(0); return;
    case 0x09: { std::string s; for (u32 p = lin_ds(edx); rd8(p) != '$' && s.size() < 1000; p++) s += (char)rd8(p); plog("DOS: %s", s.c_str()); return; }
    case 0x01: case 0x07: case 0x08: {   // leer una tecla (espera), como int 16h AH=0
        u32 ax = eax;
        eax = (eax & 0xFFFF00FFu);   // AH=0
        int16();
        u32 c = AL();
        eax = (ax & 0xFFFFFF00u) | c;
        return;
    }
    case 0x0B: { std::lock_guard<std::mutex> lk(g_mx); setAL(g_bios_keys.empty() ? 0 : 0xFF); return; }
    case 0x0E: setAL(26); return;
    case 0x19: setAL(2); return;   // C:
    case 0x1A: g_dta = lin_ds(edx); return;
    case 0x25: g_pm[AL()] = {edx, sreg[3]}; plog("DOS: vector %02xh -> %x:%x", AL(), sreg[3], edx); return;
    case 0x2A: { std::time_t t = std::time(nullptr); std::tm tm{}; localtime_r(&t, &tm);
                 setCX((u32)(tm.tm_year + 1900)); setDX((u32)(((tm.tm_mon + 1) << 8) | tm.tm_mday)); setAL((u32)tm.tm_wday); return; }
    case 0x2C: { auto n = std::chrono::system_clock::now(); std::time_t t = std::chrono::system_clock::to_time_t(n); std::tm tm{}; localtime_r(&t, &tm);
                 u32 cs = (u32)(std::chrono::duration_cast<std::chrono::milliseconds>(n.time_since_epoch()).count() % 1000 / 10);
                 setCX((u32)((tm.tm_hour << 8) | tm.tm_min)); setDX((u32)((tm.tm_sec << 8) | cs)); return; }
    case 0x2F: seg_load(0, SEL_DATA); ebx = g_dta; return;
    case 0x30: eax = 0x0006u; ebx = 0; ecx = 0; return;   // DOS 6.0 (la palabra alta de eax a 0: ni Phar Lap ni Ergo)
    case 0x35: { Vec v = g_pm[AL()]; seg_load(0, v.sel ? v.sel : SEL_CODE); ebx = v.off; return; }
    case 0x36: setAX(64); setBX(0x7FFF); setCX(512); setDX(0xFFFF); return;   // 1 GB libre
    case 0x39: { std::string p = host_path_w(dos_str(lin_ds(edx))); if (mkdir(p.c_str(), 0755) && errno != EEXIST) err(3); else ok(); return; }
    case 0x3A: { std::string p = host_path_w(dos_str(lin_ds(edx))); if (rmdir(p.c_str())) err(3); else ok(); return; }
    case 0x3B: {   // chdir: «X:\\abs», «X:rel» (relativa a la carpeta actual de esa unidad), «\\abs», «rel», con «..»
        std::string p = dos_str(lin_ds(edx)), abs;
        if (p.size() >= 2 && p[1] == ':') {
            std::string resto = p.substr(2);
            abs = !resto.empty() && resto[0] == '\\' ? p.substr(0, 2) + resto : g_cwd + "\\" + resto;
        } else if (!p.empty() && p[0] == '\\') abs = g_cwd.substr(0, 2) + p;
        else abs = g_cwd + "\\" + p;
        std::vector<std::string> partes;
        std::string cur;
        for (size_t i = 3; i <= abs.size(); i++) {
            if (i == abs.size() || abs[i] == '\\' || abs[i] == '/') {
                if (cur == "..") { if (!partes.empty()) partes.pop_back(); }
                else if (!cur.empty() && cur != ".") partes.push_back(cur);
                cur.clear();
            } else cur += abs[i];
        }
        std::string norm = abs.substr(0, 2) + "\\";
        for (size_t i = 0; i < partes.size(); i++) norm += (i ? "\\" : "") + partes[i];
        struct stat st;
        if (stat(host_path(norm).c_str(), &st) || !S_ISDIR(st.st_mode)) { err(3); return; }
        g_cwd = norm.size() > 3 && norm.back() == '\\' ? norm.substr(0, norm.size() - 1) : norm;
        ok();
        return;
    }
    case 0x3C: case 0x3D: case 0x5B: {
        std::string w = dos_str(lin_ds(edx));
        bool crea = AH() != 0x3D, escribe = crea || (AL() & 3);
        std::string p = escribe ? host_path_w(w) : host_path(w);
        int fl = crea ? (O_RDWR | O_CREAT | (AH() == 0x5B ? O_EXCL : O_TRUNC)) : ((AL() & 3) == 0 ? O_RDONLY : (AL() & 3) == 1 ? O_WRONLY : O_RDWR);
        int fd = ::open(p.c_str(), fl | O_BINARY, 0644);
        if (fd < 0) { err(errno == ENOENT ? 2 : 5); return; }
        u32 h = 5;
        while (g_fich.count(h)) h++;
        g_fich[h] = {fd, w};
        setAX(h);
        ok();
        return;
    }
    case 0x3E: { auto it = g_fich.find(BX()); if (it == g_fich.end()) { if (BX() < 5) { ok(); return; } err(6); return; }
                 ::close(it->second.fd); g_fich.erase(it); ok(); return; }
    case 0x3F: {
        auto it = g_fich.find(BX());
        if (it == g_fich.end()) { if (BX() == 0) { setAX(0); ok(); return; } err(6); return; }
        u32 dst = lin_ds(edx), n = ecx, hecho = 0;
        std::vector<u8> buf(n);
        while (hecho < n) { ssize_t r = ::read(it->second.fd, buf.data() + hecho, n - hecho); if (r <= 0) break; hecho += (u32)r; }
        for (u32 i = 0; i < hecho; i++) wr8(dst + i, buf[i]);   // byte a byte: puede caer en la ventana VGA
        eax = hecho;
        ok();
        return;
    }
    case 0x40: {
        u32 src = lin_ds(edx), n = ecx;
        if (BX() == 1 || BX() == 2) { std::string s; for (u32 i = 0; i < n && i < 2000; i++) s += (char)rd8(src + i); std::fputs(s.c_str(), stderr); eax = n; ok(); return; }
        auto it = g_fich.find(BX());
        if (it == g_fich.end()) { err(6); return; }
        if (n == 0) { off_t pos = lseek(it->second.fd, 0, SEEK_CUR); if (ftruncate(it->second.fd, pos)) {} eax = 0; ok(); return; }
        std::vector<u8> buf(n);
        for (u32 i = 0; i < n; i++) buf[i] = (u8)rd8(src + i);
        ssize_t r = ::write(it->second.fd, buf.data(), n);
        eax = r > 0 ? (u32)r : 0;
        ok();
        return;
    }
    case 0x41: { std::string p = host_path_w(dos_str(lin_ds(edx))); if (::unlink(p.c_str())) err(2); else ok(); return; }
    case 0x42: {
        auto it = g_fich.find(BX());
        if (it == g_fich.end()) { err(6); return; }
        off_t off = (off_t)(i32)((CX() << 16) | DX());
        off_t r = lseek(it->second.fd, off, AL() == 0 ? SEEK_SET : AL() == 1 ? SEEK_CUR : SEEK_END);
        if (r < 0) { err(0x19); return; }
        setAX((u32)r & 0xFFFF); setDX(((u32)r >> 16) & 0xFFFF);
        ok();
        return;
    }
    case 0x43: {
        std::string p = host_path(dos_str(lin_ds(edx)));
        struct stat st;
        if (stat(p.c_str(), &st)) { err(2); return; }
        if (AL() == 0) setCX(S_ISDIR(st.st_mode) ? 0x10 : 0x20);
        ok();
        return;
    }
    case 0x44:
        if (AL() == 0) { setDX(BX() < 3 ? 0x80D3 : 0x0002); ok(); return; }   // dispositivo (consola) o fichero en C:
        if (AL() == 8) { setAX(1); ok(); return; }   // unidad no extraíble
        ok();
        return;
    case 0x47: gstrcpy(lin_ds(esi), g_cwd.size() > 3 ? g_cwd.substr(3) : "", 64); ok(); return;
    case 0x48: {   // DOS/4GW: memoria en bytes redondeada a párrafos → selector con base en la memoria del montículo
        u32 p = heap_alloc(BX() * 16 + 16, true);
        if (!p) { err(8); setBX(0); return; }
        setAX(desc_new(p));
        ok();
        return;
    }
    case 0x49: ok(); return;
    case 0x4A: ok(); return;
    case 0x4C: plog("DOS: fin del programa (código %u)", AL()); process_exit(AL()); return;
    case 0x4E: {
        std::string pat = dos_str(lin_ds(edx));
        size_t bs = pat.find_last_of("\\:");
        std::string dir = bs == std::string::npos ? "." : pat.substr(0, bs + (pat[bs] == ':' ? 1 : 0));
        std::string mask = bs == std::string::npos ? pat : pat.substr(bs + 1);
        Busca b;
        std::set<std::string> vistos;   // con mods la carpeta son varias capas: manda la primera
        for (const std::string &hd : host_dirs(dir == "." ? g_cwd : dir)) {
            DIR *dd = opendir(hd.c_str());
            if (!dd) continue;
            while (dirent *e = readdir(dd)) {
                std::string n = e->d_name;
                if (n == "." || n == ".." || n[0] == '.') continue;
                if (!comodin(mask, n)) continue;
                std::string up = n;
                for (auto &ch : up) ch = (char)std::toupper((unsigned char)ch);
                if (!vistos.insert(up).second) continue;
                struct stat st;
                if (stat((hd + "/" + n).c_str(), &st)) continue;
                if (S_ISDIR(st.st_mode) && !(CX() & 0x10)) continue;
                b.res.push_back({n, st});
            }
            closedir(dd);
        }
        std::sort(b.res.begin(), b.res.end(), [](auto &x, auto &y) { return x.first < y.first; });
        g_busca[g_dta] = std::move(b);
        dta_rellenar(g_dta);
        if (CF) setAX(2);
        return;
    }
    case 0x4F: dta_rellenar(g_dta); return;
    case 0x56: {
        std::string a = host_path(dos_str(lin_ds(edx))), b = host_path_w(dos_str(lin_es(edi)));
        if (pcf_rename(a.c_str(), b.c_str())) err(5); else ok();
        return;
    }
    case 0x57: {
        auto it = g_fich.find(BX());
        if (it == g_fich.end()) { err(6); return; }
        if (AL() == 0) {
            struct stat st;
            fstat(it->second.fd, &st);
            std::tm tm{};
            localtime_r(&st.st_mtime, &tm);
            setCX((u32)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
            setDX((u32)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
        }
        ok();
        return;
    }
    case 0x62: setBX(SEL_PSP); return;
    case 0xFF:
        if (AX() == 0xFF00 && DX() == 0x78) { eax = 0xFFFF3447u; return; }   // ¿DOS/4GW? Sí ("G4")
        break;
    }
    plog_once("i21_" + std::to_string(AH()), "DOS: int 21h AH=%02xh sin emular (f_%x)", AH(), cur_fn);
    err(1);
}

// ---------------------------------------------------------------- int 31h (DPMI 0.9 de DOS/4GW)
void dpmi_rm_int(u32 n);

void int31() {
    switch (AX()) {
    case 0x0000: {   // reservar CX descriptores seguidos
        u32 c = std::max<u32>(1, CX());
        for (u32 i = 8; i + c <= 8192; i++) {
            bool libres = true;
            for (u32 k = 0; k < c; k++) if (g_desc[i + k].used) { libres = false; break; }
            if (!libres) continue;
            for (u32 k = 0; k < c; k++) g_desc[i + k] = Desc{0, 0xFFFFFFFFu, true};
            setAX(i << 3);
            ok();
            return;
        }
        err(0x8011);
        return;
    }
    case 0x0001: { u32 i = BX() >> 3; if (i >= 8 && i < 8192) g_desc[i].used = false; ok(); return; }
    case 0x0002: setAX(desc_new(BX() << 4)); ok(); return;
    case 0x0003: setAX(8); ok(); return;
    case 0x0006: { u32 b = sel_base(BX()); setCX(b >> 16); setDX(b & 0xFFFF); ok(); return; }
    case 0x0007: {
        u32 i = BX() >> 3;
        if (i < 8192) { g_desc[i].base = (CX() << 16) | DX(); g_desc[i].used = true; }
        for (int k = 0; k < 6; k++) if ((sreg[k] >> 3) == i) sbase[k] = g_desc[i].base;
        ok();
        return;
    }
    case 0x0008: { u32 i = BX() >> 3; if (i < 8192) g_desc[i].limit = (CX() << 16) | DX(); ok(); return; }
    case 0x0009: ok(); return;
    case 0x000A: setAX(desc_new(sel_base(BX()))); ok(); return;
    case 0x000B: {   // descriptor en es:edi (base y límite; acceso: datos de 32 bits)
        u32 i = BX() >> 3, p = lin_es(edi), b = i < 8192 ? g_desc[i].base : 0, l = 0xFFFFF;
        wr16(p, l & 0xFFFF); wr16(p + 2, b & 0xFFFF); wr8(p + 4, (b >> 16) & 0xFF); wr8(p + 5, 0xF2);
        wr8(p + 6, 0xCF); wr8(p + 7, b >> 24);
        ok();
        return;
    }
    case 0x000C: {
        u32 i = BX() >> 3, p = lin_es(edi);
        if (i < 8192) { g_desc[i].base = rd16(p + 2) | (rd8(p + 4) << 16) | (rd8(p + 7) << 24); g_desc[i].used = true; }
        ok();
        return;
    }
    case 0x0100: {   // memoria DOS: BX párrafos → AX segmento, DX selector
        u32 n = BX() * 16;
        if (g_dosmem + n > DOSMEM_HI) { err(8); setBX((DOSMEM_HI - g_dosmem) / 16); return; }
        u32 p = g_dosmem;
        g_dosmem = (g_dosmem + n + 15) & ~15u;
        std::memset(MEM + p, 0, n);
        setAX(p >> 4);
        setDX(desc_new(p));
        ok();
        return;
    }
    case 0x0101: case 0x0102: ok(); return;
    case 0x0200: setCX(g_rm[BL()].sel); setDX(g_rm[BL()].off); ok(); return;
    case 0x0201: g_rm[BL()] = {DX(), CX()}; ok(); return;
    case 0x0204: { Vec v = g_pm[BL()]; if (!v.off) v = {g_vec_default, SEL_CODE}; setCX(v.sel); edx = v.off; ok(); return; }
    case 0x0205: g_pm[BL()] = {edx, CX()}; plog("DPMI: vector %02xh -> %x:%x", BL(), CX(), edx); ok(); return;
    case 0x0300: case 0x0301: case 0x0302: dpmi_rm_int(BL()); return;
    case 0x0303: setCX(0); setDX(0); err(0x8015); return;   // sin retrollamadas de modo real
    case 0x0400: setAX(0x005A); setBX(0x0005); setCX(4); setDX(0x0870); ok(); return;
    case 0x0500: {   // información de memoria libre (es:edi, 48 bytes)
        u32 p = lin_es(edi);
        for (int i = 0; i < 12; i++) wr32(p + 4 * i, 0xFFFFFFFFu);
        wr32(p, 256u << 20);
        wr32(p + 4, 65536);
        wr32(p + 8, 65536);
        ok();
        return;
    }
    case 0x0501: {   // memoria: BX:CX bytes → BX:CX dirección lineal, SI:DI identificador
        u32 n = (BX() << 16) | CX();
        u32 p = heap_alloc(n + 16, true);
        if (!p) { err(0x8013); return; }
        setBX(p >> 16); setCX(p & 0xFFFF);
        esi = (esi & 0xFFFF0000u) | (p >> 16); edi = (edi & 0xFFFF0000u) | (p & 0xFFFF);
        ok();
        return;
    }
    case 0x0502: { u32 h = ((esi & 0xFFFF) << 16) | (edi & 0xFFFF); heap_free(h); ok(); return; }
    case 0x0503: {
        u32 h = ((esi & 0xFFFF) << 16) | (edi & 0xFFFF), n = (BX() << 16) | CX();
        u32 p = heap_realloc(h, n + 16, true);
        if (!p) { err(0x8013); return; }
        setBX(p >> 16); setCX(p & 0xFFFF);
        esi = (esi & 0xFFFF0000u) | (p >> 16); edi = (edi & 0xFFFF0000u) | (p & 0xFFFF);
        ok();
        return;
    }
    case 0x0600: case 0x0601: case 0x0702: case 0x0703: ok(); return;
    case 0x0604: setBX(0); setCX(4096); ok(); return;
    case 0x0800: ok(); return;   // dirección física → lineal: la misma (memoria de vídeo)
    case 0x0900: setAL(g_if); g_if = 0; ok(); return;
    case 0x0901: setAL(g_if); g_if = 1; ok(); return;
    case 0x0902: setAL(g_if); ok(); return;
    }
    plog_once("i31_" + std::to_string(AX()), "DPMI: int 31h AX=%04xh sin emular (f_%x)", AX(), cur_fn);
    err(0x8001);
}

// ---------------------------------------------------------------- BIOS y ratón
void int10() {
    switch (AH()) {
    case 0x00: vga_set_mode((u8)AL()); wr8(0x449, AL() & 0x7F); return;
    case 0x0F: setAX(((vga_mode() == 0x13 ? 40 : 80) << 8) | vga_mode()); setBX(BX() & 0xFF); return;
    case 0x10:
        switch (AL()) {
        case 0x00: vga_set_ac(BL(), (ebx >> 8) & 0xFF); return;
        case 0x01: vga_set_ac(0x11, (ebx >> 8) & 0xFF); return;
        case 0x02: { u32 p = lin_es(edx); for (int i = 0; i < 16; i++) vga_set_ac((u8)i, (u8)rd8(p + i)); vga_set_ac(0x11, (u8)rd8(p + 16)); return; }
        case 0x03: return;
        case 0x07: ebx = (ebx & 0xFFFF00FFu) | ((u32)vga_get_ac(BL()) << 8); return;
        case 0x10: vga_set_dac((u8)BX(), (ecx >> 8) & 0xFF, ecx & 0xFF, (edx >> 8) & 0xFF); return;
        case 0x12: { u32 p = lin_es(edx); for (u32 i = 0; i < CX(); i++) vga_set_dac((u8)(BX() + i), rd8(p + 3 * i), rd8(p + 3 * i + 1), rd8(p + 3 * i + 2)); return; }
        case 0x15: { u8 r, g, b; vga_get_dac((u8)BX(), r, g, b); ecx = (ecx & 0xFFFF0000u) | (r << 8) | g; edx = (edx & 0xFFFF00FFu) | (b << 8); return; }
        case 0x17: { u32 p = lin_es(edx); for (u32 i = 0; i < CX(); i++) { u8 r, g, b; vga_get_dac((u8)(BX() + i), r, g, b); wr8(p + 3 * i, r); wr8(p + 3 * i + 1, g); wr8(p + 3 * i + 2, b); } return; }
        }
        break;
    case 0x11: case 0x12: return;
    case 0x1A: if (AL() == 0) { setAL(0x1A); setBX(0x0008); } return;   // VGA en color
    case 0x4F: setAX(0x014F); return;   // sin VESA
    case 0x02: case 0x03: case 0x01: case 0x06: case 0x09: case 0x0E: return;
    }
    plog_once("i10_" + std::to_string(AX()), "BIOS: int 10h AX=%04xh sin emular (f_%x)", AX(), cur_fn);
}

void int16() {
    for (int espera = 0;; espera++) {
        bool hay;
        u16 k = 0;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            hay = !g_bios_keys.empty();
            if (hay) k = g_bios_keys.front();
            if (hay && (AH() == 0x00 || AH() == 0x10)) g_bios_keys.pop_front();
        }
        switch (AH()) {
        case 0x00: case 0x10:
            if (hay) { setAX(k); return; }
            irq_servir();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        case 0x01: case 0x11: ZF = !hay; if (hay) setAX(k); return;
        case 0x02: case 0x12: setAL(g_shift); return;
        default: return;
        }
    }
}

void int33() {
    Raton &r = g_raton;
    std::lock_guard<std::mutex> lk(g_mx);
    switch (AX()) {
    case 0x00: setAX(0xFFFF); setBX(2); r.visible = -1; r.cb = {}; g_ev_raton.clear(); r.ex = r.ey = 0; return;
    case 0x01: r.visible = std::min(0, r.visible + 1); display().cursor_visible = r.visible >= 0; return;
    case 0x02: r.visible--; display().cursor_visible = false; return;
    case 0x03: setBX((u32)r.botones); setCX((u32)r.x); setDX((u32)r.y); return;
    case 0x04: r.x = std::max(r.minx, std::min(r.maxx, (int)(i16)CX())); r.y = std::max(r.miny, std::min(r.maxy, (int)(i16)DX())); return;
    case 0x05: case 0x06: {
        int b = std::min(2u, BX());
        int *cnt = AX() == 5 ? r.pulsa : r.suelta;
        setAX((u32)r.botones); setBX((u32)cnt[b]); setCX((u32)r.px[b]); setDX((u32)r.py[b]);
        cnt[b] = 0;
        return;
    }
    case 0x07: r.minx = (i16)CX(); r.maxx = (i16)DX(); if (r.minx > r.maxx) std::swap(r.minx, r.maxx);
               if (!r.ex && r.minx == 0) r.ex = std::max(1, (r.maxx + 1 + display().screen.w / 2) / std::max(1, display().screen.w));
               return;
    case 0x08: r.miny = (i16)CX(); r.maxy = (i16)DX(); if (r.miny > r.maxy) std::swap(r.miny, r.maxy);
               if (!r.ey && r.miny == 0) r.ey = std::max(1, (r.maxy + 1 + display().screen.h / 2) / std::max(1, display().screen.h));
               return;
    case 0x0B: setCX(0); setDX(0); return;
    case 0x0C: r.cb_mask = CX(); r.cb = {edx, sreg[0]}; plog("ratón: rutina del juego en %x:%x (eventos %x)", sreg[0], edx, CX()); return;
    case 0x0F: case 0x1A: case 0x1D: return;
    case 0x21: setAX(0xFFFF); setBX(2); return;
    case 0x24: setBX(0x0626); setCX(0x0400); return;
    }
    plog_once("i33_" + std::to_string(AX()), "ratón: int 33h AX=%04xh sin emular", AX());
}

void dos_int_core(u32 n) {
    switch (n) {
    case 0x21: int21(); return;
    case 0x31: int31(); return;
    case 0x10: int10(); return;
    case 0x16: int16(); return;
    case 0x33: int33(); return;
    case 0x1A:
        if (AH() == 0) { u32 t = rd32(0x46C); setCX(t >> 16); setDX(t & 0xFFFF); setAL(0); return; }
        if (AH() == 2) { std::time_t t = std::time(nullptr); std::tm tm{}; localtime_r(&t, &tm);
                         auto bcd = [](int v) { return (u32)(((v / 10) << 4) | (v % 10)); };
                         setCX((bcd(tm.tm_hour) << 8) | bcd(tm.tm_min)); setDX(bcd(tm.tm_sec) << 8); CF = 0; return; }
        return;
    case 0x2F:
        if (AX() == 0x1680) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); setAL(0); return; }   // ceder la CPU
        return;
    case 0x11: setAX(0x4426); return;      // equipo: VGA, coprocesador
    case 0x12: setAX(640); return;         // memoria convencional
    case 0x15: CF = 1; setAX(0x8600); return;
    case 0x14: case 0x17: return;
    }
    plog_once("int" + std::to_string(n), "DOS: int %02xh sin emular (f_%x)", n, cur_fn);
}

// DPMI 0300h: interrupción en modo real con los registros de la estructura de es:edi (los punteros son
// segmento:desplazamiento de modo real → lineal = segmento*16 + desplazamiento)
void dpmi_rm_int(u32 n) {
    u32 st = lin_es(edi);
    u32 r[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi};
    u32 sr[6], sb[6];
    std::memcpy(sr, sreg, sizeof sr); std::memcpy(sb, sbase, sizeof sb);
    edi = rd32(st + 0x00); esi = rd32(st + 0x04); ebp = rd32(st + 0x08); ebx = rd32(st + 0x10);
    edx = rd32(st + 0x14); ecx = rd32(st + 0x18); eax = rd32(st + 0x1C);
    sbase[0] = rd16(st + 0x22) << 4; sbase[3] = rd16(st + 0x24) << 4;
    CF = 0;
    dos_int_core(n);
    wr32(st + 0x00, edi); wr32(st + 0x04, esi); wr32(st + 0x08, ebp); wr32(st + 0x10, ebx);
    wr32(st + 0x14, edx); wr32(st + 0x18, ecx); wr32(st + 0x1C, eax);
    wr16(st + 0x20, (rd16(st + 0x20) & ~1u) | (CF & 1) | (ZF << 6));
    eax = r[0]; ecx = r[1]; edx = r[2]; ebx = r[3]; ebp = r[5]; esi = r[6]; edi = r[7];
    std::memcpy(sreg, sr, sizeof sr); std::memcpy(sbase, sb, sizeof sb);
    CF = 0;
}

}  // namespace

void dos_int(u32 n) {
    // PCF7_TRACE_DOS: cada interrupción con sus registros (y la cadena de ds:edx si la hay)
    // (PCF7_TRACE_DOS=21: solo esa interrupción)
    static const char *tz = std::getenv("PCF7_TRACE_DOS");
    static const u32 solo = tz ? (u32)std::strtoul(tz, nullptr, 16) : 0;
    static const u32 desde = std::getenv("PCF7_TRACE_DOS_DESDE") ? (u32)std::atoi(std::getenv("PCF7_TRACE_DOS_DESDE")) : 0;
    const bool traza = tz != nullptr && ticks_ms() >= desde;
    if (traza && (solo <= 1 || n == solo) && !(n == 0x33 && AX() == 3) && !(n == 0x16 && (AH() == 1 || AH() == 0x11))) {
        u32 p = lin_ds(edx);
        std::string s = (n == 0x21 && (AH() == 0x3D || AH() == 0x3C || AH() == 0x4E || AH() == 0x41 || AH() == 0x3B || AH() == 0x43)) ? gstr(p, 80) : "";
        plog("int %02xh ax=%04x bx=%04x ecx=%x edx=%08x ds=%04x(base %x) %s (f_%x)", n, AX(), BX(), ecx, edx, sreg[3], sbase[3], s.c_str(), cur_fn);
        dos_int_core(n);
        plog("   -> ax=%04x bx=%04x cx=%04x dx=%04x CF=%u", AX(), BX(), CX(), DX(), CF);
        if (g_irq_pend && g_if) irq_servir();
        return;
    }
    dos_int_core(n);
    if (g_irq_pend && g_if) irq_servir();
}

// Arranque: PSP, entorno y registros como los deja DOS/4GW, y salto al punto de entrada del LE
int run_game_dos() {
    g_desc[1] = {0, 0xFFFFFFFFu, true};
    g_desc[2] = {0, 0xFFFFFFFFu, true};
    g_desc[3] = {PSP, 0xFF, true};
    g_desc[4] = {ENV, 0xFFF, true};
    g_vec_default = host_fn(vec_default);
    // BDA: modo de vídeo, columnas, puerto del CRTC, ticks
    wr8(0x449, 3); wr16(0x44A, 80); wr16(0x463, 0x3D4); wr8(0x484, 24);
    // PSP: int 20h, tope de memoria, entorno y línea de órdenes
    std::memset(MEM + PSP, 0, 0x100);
    wr16(PSP, 0x20CD);
    wr16(PSP + 2, 0x9FFF);
    wr16(PSP + 0x2C, SEL_ENV);
    wr32(PSP + 0x5C, 0x00100000);   // (DOS/4GW) tamaño del programa: Watcom lo redondea a páginas
    // argumentos de FUTBOL4.EXE: «0», la unidad de la instalación y la del CD (aquí la misma: la instalación completa)
    std::string args = std::getenv("PCF7_ARGS") ? std::getenv("PCF7_ARGS") : "0 C C";
    if (!args.empty()) args = " " + args;
    wr8(PSP + 0x80, (u32)std::min<size_t>(args.size(), 126));
    gstrcpy(PSP + 0x81, args.substr(0, 126) + "\r", 127);
    // entorno: VAR=valor\0 … \0, una palabra a 1 y la ruta del programa
    std::string prog = std::string("C:\\") + JUEGO_CARPETA + "\\" + JUEGO_PRINCIPAL;
    for (auto &c : prog) c = (char)std::toupper((unsigned char)c);
    if (prog.size() > 4 && prog.compare(prog.size() - 4, 4, ".EXE") == 0) prog = prog.substr(0, prog.size() - 4) + ".DAT";
    const char *vars[] = {"COMSPEC=C:\\COMMAND.COM", "PATH=C:\\DOS", "PROMPT=$P$G", "DOS4G=QUIET", "BLASTER=A220 I5 D1 T4", nullptr};
    u32 p = ENV;
    for (int i = 0; vars[i]; i++) p += gstrcpy(p, vars[i]) + 1;
    wr8(p++, 0);
    wr16(p, 1); p += 2;
    gstrcpy(p, prog);
    // registros: cs código plano, ds = ss datos planos, es = PSP; esp, la pila del LE
    seg_load(0, SEL_PSP); seg_load(1, SEL_CODE); seg_load(2, SEL_DATA); seg_load(3, SEL_DATA); seg_load(4, 0); seg_load(5, 0);
    vga_set_mode(3);
    display().cursor_visible = false;
    std::thread(temporizador).detach();
    eax = ebx = ecx = edx = esi = edi = ebp = 0;
    esp = PCF_DOS_PILA;
    plog("DOS: %s%s (entrada 0x%x, pila 0x%x)", prog.c_str(), args.c_str(), PCF_DOS_ENTRADA, esp);
    guest_call(PCF_DOS_ENTRADA, {});
    plog("DOS: el programa ha vuelto sin int 21h/4Ch");
    return 0;
}

// El lanzador (FUTBOL4.EXE): al acabar un programa, el estado que dejó en COMFILE.DAT (palabra en 0x50) dice cuál
// sigue (tabla «lanzador» de juego.json). Devuelve el programa ("" = el juego termina). Los que no vienen en el CD
// (HobbyLink, actualizaciones, InfoFútbol) vuelven al menú principal con el estado a 0.
std::string dos_siguiente() {
    std::string ruta = host_path(std::string("C:\\") + JUEGO_CARPETA + "\\COMFILE.DAT");
    FILE *f = std::fopen(ruta.c_str(), "r+b");
    if (!f) return JUEGO_LANZADOR[0].programa ? JUEGO_LANZADOR[0].programa : "";
    u8 b[2] = {};
    std::fseek(f, 0x50, SEEK_SET);
    size_t n = std::fread(b, 1, 2, f);
    u32 estado = n == 2 ? (u32)(b[0] | (b[1] << 8)) : 0;
    plog("lanzador: estado %u en COMFILE.DAT", estado);
    if (estado >= 10) { std::fclose(f); return ""; }
    for (u32 i = 0; JUEGO_LANZADOR[i].programa; i++)
        if (JUEGO_LANZADOR[i].apartado == estado) { std::fclose(f); return JUEGO_LANZADOR[i].programa; }   // "" = salir
    plog("lanzador: el programa del estado %u no está en el CD: vuelta al menú", estado);
    b[0] = b[1] = 0;
    std::fseek(f, 0x50, SEEK_SET);
    std::fwrite(b, 1, 2, f);
    std::fclose(f);
    for (u32 i = 0; JUEGO_LANZADOR[i].programa; i++)
        if (JUEGO_LANZADOR[i].apartado == 0) return JUEGO_LANZADOR[i].programa;
    return "";
}
#endif
