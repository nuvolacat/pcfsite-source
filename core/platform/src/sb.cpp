// PCF7 — Sound Blaster (DSP 2.0/Pro) para los juegos de MS-DOS (PCF_DOS): puertos 0x220-0x22F, el controlador
// de DMA 8237 (canales de 8 bits, puertos 0x00-0x0F y páginas 0x80-0x8F) y la IRQ 5 (vector 0Dh).
//
// El juego hace el apretón de manos del DSP (reset por base+6, 0xAA en base+0xA), manda órdenes por base+0xC
// (constante de tiempo, reproducción por DMA de 8 bits en un ciclo o continua, altavoz…) y programa el DMA con la
// dirección física del búfer (su memoria DOS: aquí la misma dirección lineal). Las muestras se leen de MEM al ritmo
// pedido, se remuestrean a 44100 Hz y suenan por un flujo de SDL; al acabar cada bloque se avisa con la IRQ, que
// dos.cpp despacha al manejador que instaló el juego (lo atiende en los saltos hacia atrás, DOS_POLL).
#ifdef PCF_DOS
#include "plat.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

u32 ticks_ms();
extern volatile u32 g_irq_pend;   // dos.cpp: hay IRQ que atender (DOS_POLL)

namespace {

constexpr u32 BASE = 0x220;
constexpr int OUT_RATE = 44100;

std::mutex g_mx;
std::deque<u8> g_out;      // datos que el DSP devuelve (base+0xA)
u8 g_cmd = 0;              // orden en curso y sus parámetros
int g_faltan = 0;
u8 g_par[4];
int g_npar = 0;
bool g_reset = false;
bool g_altavoz = false;
u32 g_tc = 0xA6;           // constante de tiempo: 1e6/(256-tc) Hz
u32 g_bloque = 0;          // tamaño de bloque (auto-init) - 1

// DMA 8237: canales 0-3
struct Dma { u32 base_addr = 0, base_cnt = 0, addr = 0, cnt = 0; u8 page = 0, modo = 0; bool mask = true; } g_dma[4];
bool g_flip = false;

// reproducción en curso
struct Play { bool on = false, auto_init = false; u32 lin = 0, len = 0, pos = 0; double frac = 0; int canal = 1; } g_play;
std::atomic<bool> g_irq{false};
// El DMA va por delante de lo que suena: las muestras leídas del bloque esperan en esta cola (a la frecuencia del DSP)
// unos ADELANTO_MS. Así la IRQ de fin de bloque llega antes de que se agote el sonido y el juego encadena el
// siguiente sin hueco (en la tarjeta real la IRQ se atiende en microsegundos; aquí, en el siguiente DOS_POLL).
std::deque<u8> g_cola;
double g_frac_salida = 0;
// El DMA avanza al ritmo de los ticks del temporizador que se le entregan al juego (sb_avance_tick desde dos.cpp): el
// juego mezcla el sonido por ticks, un trozo por delante del DMA, y así los dos van en el mismo reloj, como en el PC.
// La salida lleva un colchón de COLCHON_MS para absorber el retraso con que se entregan los ticks.
constexpr u32 COLCHON_MS = 120;
double g_resto_tick = 0;
u8 g_ultima = 128;
u32 g_t_irq = 0;           // diagnóstico: cuándo saltó la última IRQ de fin de bloque y huecos en la salida
u32 g_huecos = 0;
u32 g_ult_pull = 0;        // última vez que el dispositivo pidió audio (si no pide, avanza con el reloj)
SDL_AudioStream *g_stream = nullptr;
u32 g_vol_pct = 100;

const char *nombre_orden(u8 c) {
    switch (c) {
    case 0x10: return "DAC directo"; case 0x14: return "DMA 8 bits"; case 0x1C: return "DMA 8 bits continuo"; case 0x40: return "constante de tiempo";
    case 0x48: return "tamaño de bloque"; case 0xD0: return "pausa DMA"; case 0xD1: return "altavoz on"; case 0xD3: return "altavoz off";
    case 0xD4: return "continúa DMA"; case 0xDA: return "fin auto-init"; case 0xE1: return "versión"; default: return "?";
    }
}
bool tracing() { static bool on = std::getenv("PCF7_TRACE_SB") != nullptr; return on; }

void empezar(bool auto_init) {   // con g_mx
    Dma &d = g_dma[g_play.canal];
    g_play.lin = ((u32)d.page << 16) | (d.addr & 0xFFFF);
    g_play.len = (d.cnt & 0xFFFF) + 1;
    g_play.pos = 0;
    g_play.frac = 0;
    g_play.auto_init = auto_init;
    g_play.on = true;
    if (g_cola.empty()) { double rate = 1000000.0 / (256 - g_tc); for (u32 i = 0; i < COLCHON_MS * rate / 1000; i++) g_cola.push_back(128); }
    if (tracing()) {
        u32 mn = 255, mx = 0, n40 = 0, suma = 0;
        for (u32 i = 0; i < g_play.len && g_play.lin + i < MEMSZ; i++) { u8 v = MEM[g_play.lin + i]; mn = std::min<u32>(mn, v); mx = std::max<u32>(mx, v); n40 += v == 0x40; suma += v; }
        plog("SB: reproduce %u muestras desde %x a %u Hz%s; %u ms tras la IRQ, cola %zu; bloque min %u max %u media %u, bytes 0x40: %u; primeros %02x %02x %02x %02x %02x %02x %02x %02x",
             g_play.len, g_play.lin, 1000000 / (256 - g_tc), auto_init ? " (continuo)" : "", g_t_irq ? ticks_ms() - g_t_irq : 0, g_cola.size(), mn, mx, g_play.len ? suma / g_play.len : 0, n40,
             MEM[g_play.lin], MEM[g_play.lin + 1], MEM[g_play.lin + 2], MEM[g_play.lin + 3], MEM[g_play.lin + 4], MEM[g_play.lin + 5], MEM[g_play.lin + 6], MEM[g_play.lin + 7]);
    }
}

void abrir_audio();

void orden(u8 v) {   // byte escrito en base+0xC, con g_mx
    abrir_audio();   // el Manager no reinicia el DSP (ya lo hizo el menú, en otro proceso): la salida se abre aquí
    if (g_faltan) {
        g_par[g_npar++] = v;
        if (--g_faltan) return;
        switch (g_cmd) {
        case 0x10: break;   // DAC directo: se ignora (no lo usa el juego)
        case 0x14: g_bloque = g_par[0] | (g_par[1] << 8); empezar(false); break;
        case 0x40: g_tc = v; break;
        case 0x48: g_bloque = g_par[0] | (g_par[1] << 8); break;
        case 0x80: break;   // silencio de n muestras
        case 0xE0: g_out.push_back((u8)~v); break;
        }
        return;
    }
    g_cmd = v;
    g_npar = 0;
    switch (v) {
    case 0x10: g_faltan = 1; break;
    case 0x14: case 0x48: case 0x80: g_faltan = 2; break;
    case 0x40: case 0xE0: g_faltan = 1; break;
    case 0x1C: empezar(true); break;
    case 0xD0: g_play.on = false; break;
    case 0xD1: g_altavoz = true; break;
    case 0xD3: g_altavoz = false; break;
    case 0xD4: g_play.on = g_play.len > 0; break;
    case 0xD8: g_out.push_back(g_altavoz ? 0xFF : 0x00); break;
    case 0xDA: g_play.auto_init = false; break;
    case 0xE1: g_out.push_back(3); g_out.push_back(1); break;   // DSP 3.01 (Sound Blaster Pro)
    default: if (tracing()) plog("SB: orden %02x sin emular", v); break;
    }
    if (tracing() && !g_faltan) plog("SB: orden %02x (%s)", v, nombre_orden(v));
}

// Lleva la cola hasta `objetivo` muestras leyendo del bloque DMA en curso (con g_mx); al agotar el bloque, IRQ
void bombear(size_t objetivo) {
    while (g_cola.size() < objetivo && g_play.on) {
        u32 a = g_play.lin + g_play.pos;
        g_cola.push_back(a < MEMSZ ? MEM[a] : 128);
        if (++g_play.pos >= g_play.len) {
            g_irq = true;
            g_irq_pend = 1;
            g_t_irq = ticks_ms();
            if (g_play.auto_init) g_play.pos = 0;
            else g_play.on = false;
        }
    }
}

// n tramas estéreo float: el DMA avanza (por delante) y la cola se remuestrea a la salida (con g_mx tomado)
void generar(std::vector<float> &out, int n) {
    out.assign((size_t)n * 2, 0.f);
    double rate = 1000000.0 / (256 - g_tc), paso = rate / OUT_RATE;
    float vol = g_altavoz ? g_vol_pct / 100.f : 0.f;
    for (int i = 0; i < n; i++) {
        g_frac_salida += paso;
        while (g_frac_salida >= 1) {
            g_frac_salida -= 1;
            if (!g_cola.empty()) { g_ultima = g_cola.front(); g_cola.pop_front(); }
            else if (tracing() && g_huecos++ < 30) plog("SB: hueco en la salida (IRQ hace %u ms, bloque %s)", ticks_ms() - g_t_irq, g_play.on ? "en curso" : "acabado");   // se mantiene la última muestra
        }
        out[2 * i] = out[2 * i + 1] = ((int)g_ultima - 128) / 128.f * vol;
    }
    static FILE *dump = [] { const char *p = std::getenv("PCF7_SB_DUMP"); return p ? std::fopen(p, "wb") : nullptr; }();
    if (dump) { std::fwrite(out.data(), sizeof(float), out.size(), dump); std::fflush(dump); }
}

// Hilo de audio de SDL: n tramas estéreo float
void SDLCALL mezcla(void *, SDL_AudioStream *stream, int additional, int) {
    int n = additional / (int)(2 * sizeof(float));
    if (n <= 0) return;
    static std::vector<float> out;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_ult_pull = ticks_ms();
        generar(out, n);
    }
    SDL_PutAudioStreamData(stream, out.data(), (int)(out.size() * sizeof(float)));
}

void abrir_audio() {
    if (g_stream) return;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) { plog("SB: audio: %s", SDL_GetError()); return; }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, OUT_RATE};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mezcla, nullptr);
    if (!g_stream) { plog("SB: audio: %s", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(g_stream);
    plog("SB: Sound Blaster en 0x%x, IRQ 5, DMA 1 (%s a %d Hz)", BASE, SDL_GetCurrentAudioDriver(), OUT_RATE);
}

}  // namespace

// Un tick del temporizador (8253) entregado al juego: el DMA lee su parte del bloque (o, sin bloque, mantiene la
// latencia con la última muestra)
void sb_avance_tick(double hz) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_stream && !g_play.on) return;
    double rate = 1000000.0 / (256 - g_tc);
    g_resto_tick += rate / hz;
    size_t n = (size_t)g_resto_tick;
    g_resto_tick -= n;
    size_t objetivo = g_cola.size() + n;
    bombear(objetivo);
    while (g_cola.size() < objetivo) g_cola.push_back(g_ultima);
    // deriva lenta entre el reloj del temporizador y el del audio: se recorta o rellena alrededor del colchón
    size_t colchon = (size_t)(COLCHON_MS * rate / 1000);
    if (g_cola.size() > colchon * 2) g_cola.erase(g_cola.begin(), g_cola.begin() + (g_cola.size() - colchon));
}

// Si el dispositivo de audio no pide datos (sin salida, «dummy», o el navegador aún sin permiso), la reproducción
// avanza con el reloj: la tarjeta real sigue y el juego espera sus IRQ. Lo llama el temporizador de dos.cpp.
void sb_tick() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_play.on) return;
    u32 now = ticks_ms();
    if (g_ult_pull && now - g_ult_pull < 200) return;
    static u32 ult = 0;
    if (!ult || now - ult > 1000) { ult = now; return; }
    u32 ms = now - ult;
    ult = now;
    static std::vector<float> out;
    generar(out, (int)(ms * OUT_RATE / 1000));
}

bool sb_puerto(u32 port) { return (port >= BASE && port < BASE + 0x10) || port < 0x10 || (port >= 0x80 && port < 0x90); }

// ¿IRQ pendiente de la tarjeta? (la consume dos.cpp)
bool sb_irq_pendiente() { return g_irq.exchange(false); }

u32 sb_in(u32 port) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (port >= BASE) {
        switch (port - BASE) {
        case 0xA: { u8 v = g_out.empty() ? 0xFF : g_out.front(); if (!g_out.empty()) g_out.pop_front(); return v; }
        case 0xC: return 0x7F;                                     // listo para recibir
        case 0xE: { static int n = 0; if (tracing() && n++ < 40) plog("SB: lectura de estado (datos %s) en f_%x", g_out.empty() ? "no" : "sí", cur_fn);
                    return g_out.empty() ? 0x7Fu : 0xFFu; }        // bit 7: hay datos; leerlo reconoce la IRQ
        case 0x4: case 0x5: return 0;                              // mezclador (Pro): nada
        }
        return 0xFF;
    }
    if (port < 8) {   // dirección/cuenta actual del canal (16 bits, dos lecturas)
        Dma &d = g_dma[port >> 1];
        u32 v = (port & 1) ? d.cnt : d.addr;
        if (port & 1 && g_play.on && (port >> 1) == (u32)g_play.canal) v = (g_play.len - 1 - g_play.pos) & 0xFFFF;
        if (tracing() && (port & 1) && g_flip) { static u32 n = 0; if (n++ < 100) plog("SB: cuenta DMA leída: quedan %u (pos %u de %u, cola %zu)", v, g_play.pos, g_play.len, g_cola.size()); }
        u32 b = g_flip ? (v >> 8) & 0xFF : v & 0xFF;
        g_flip = !g_flip;
        return b;
    }
    if (port == 0x08) return g_play.on ? 0 : 0x02;   // estado: canal 1 terminado
    return 0;
}

void sb_out(u32 port, u32 v) {
    std::lock_guard<std::mutex> lk(g_mx);
    v &= 0xFF;
    if (port >= BASE) {
        switch (port - BASE) {
        case 0x6:   // reset del DSP: 1 y luego 0 → 0xAA en la cola
            if (v == 1) { g_reset = true; g_out.clear(); g_play.on = false; g_faltan = 0; g_cola.clear(); }
            else if (g_reset) { g_reset = false; g_out.push_back(0xAA); abrir_audio(); }
            return;
        case 0xC: orden((u8)v); return;
        case 0x4: case 0x5: return;   // mezclador
        }
        return;
    }
    if (tracing() && (port < 8 || port == 0x83 || port == 0x0A)) { static int n = 0; if (n++ < 60) plog("SB: DMA puerto %02x <- %02x", port, v); }
    if (port < 8) {
        Dma &d = g_dma[port >> 1];
        u32 &r = (port & 1) ? d.base_cnt : d.base_addr;
        r = g_flip ? (r & 0xFF) | (v << 8) : (r & 0xFF00) | v;
        g_flip = !g_flip;
        if (port & 1) d.cnt = d.base_cnt; else d.addr = d.base_addr;
        return;
    }
    switch (port) {
    case 0x08: return;                                              // orden/estado
    case 0x0A: g_dma[v & 3].mask = v & 4; return;                   // máscara de un canal
    case 0x0B: g_dma[v & 3].modo = (u8)v; return;                   // modo
    case 0x0C: g_flip = false; return;                              // reinicio del biestable
    case 0x0D: g_flip = false; for (auto &d : g_dma) d.mask = true; return;   // reinicio general
    case 0x0F: for (int i = 0; i < 4; i++) g_dma[i].mask = v & (1 << i); return;
    case 0x87: g_dma[0].page = (u8)v; return;
    case 0x83: g_dma[1].page = (u8)v; return;
    case 0x81: g_dma[2].page = (u8)v; return;
    case 0x82: g_dma[3].page = (u8)v; return;
    }
}
#endif
