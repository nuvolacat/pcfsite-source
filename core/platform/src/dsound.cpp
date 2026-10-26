// PCF7 — DirectSound (dsound.dll!DirectSoundCreate / DirectSoundEnumerateA) sobre SDL3.
//
// Los búferes de sonido viven en la memoria del juego (Lock devuelve punteros a MEM). El hilo de audio
// de SDL mezcla todos los búferes que suenan leyendo MEM directamente, con remuestreo, volumen,
// panorámica y bucle; su cursor de reproducción es el que devuelve GetCurrentPosition (el juego hace
// streaming sobre búferes circulares y se guía por él).
#include "com.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

u32 ticks_ms();
extern bool g_reloj_virtual;   // kernel32.cpp: reloj virtual (diagnóstico)

void register_dynamic_export(const char *dll, const char *name, void (*f)(void));

namespace {

constexpr u32 DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057u, E_NOINTERFACE = 0x80004002u, DSERR_UNSUPPORTED = 0x80004001u;
constexpr int OUT_RATE = 44100;

struct Vec3 { float x = 0, y = 0, z = 0; };

struct Buffer {
    u32 obj = 0;
    u32 obj3d = 0;           // interfaz IDirectSound3DBuffer (DSBCAPS_CTRL3D)
    Vec3 pos3d;
    float min_dist = 1, max_dist = 1e9f;
    u32 mode3d = 0;          // DS3DMODE_NORMAL 0, HEADRELATIVE 1, DISABLE 2
    float g3d_l = 1, g3d_r = 1;   // ganancia por canal del posicionamiento 3D
    u32 mem = 0, size = 0;
    int rate = 22050, channels = 1, bits = 16;
    bool primary = false;
    bool playing = false, looping = false;
    double pos = 0;          // en tramas de origen
    // streaming: búfer en bucle que el juego rellena mientras suena (lo desbloquea varias veces sonando). Cada
    // trama escrita se marca como nueva y al sonar deja de serlo; si el cursor llega a una que ya sonó y el juego no
    // ha vuelto a escribir (se ha parado: un fichero que aún baja, un aviso…), suena silencio en vez de repetirla.
    // El cursor sigue avanzando, como en una tarjeta de sonido, para que el juego vuelva a rellenar.
    int unlocks_sonando = 0;
    std::vector<u8> nueva;   // por trama
    u32 ultima = ~0u;        // última trama que ha sonado
    float vol = 1, pan_l = 1, pan_r = 1;
    i32 vol_db = 0, pan_db = 0;
    u32 caps = 0;
};

std::mutex g_mx;
std::map<u32, Buffer> g_bufs;
std::map<u32, u32> g_3d_of;     // interfaz IDirectSound3DBuffer → búfer
SDL_AudioStream *g_stream = nullptr;
u32 g_vt_ds = 0, g_vt_buf = 0, g_vt_l3d = 0, g_vt_b3d = 0, g_ds = 0, g_listener = 0;

// oyente 3D (IDirectSound3DListener del búfer primario)
struct Listener {
    Vec3 pos, front{0, 0, 1}, top{0, 1, 0};
    float rolloff = 1, distance_factor = 1, doppler = 1;
} g_lis;

const ComGuid IID_IDirectSound3DListener = {0x279AFA84, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const ComGuid IID_IDirectSound3DBuffer = {0x279AFA86, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

bool tracing() { static bool on = std::getenv("PCF7_TRACE_AUDIO") != nullptr; return on; }

Buffer *buf(u32 o) { auto it = g_bufs.find(o); return it == g_bufs.end() ? nullptr : &it->second; }

u32 frame_bytes(const Buffer &b) { return (u32)(b.channels * b.bits / 8); }

float db_to_gain(i32 db) { return db <= -10000 ? 0.f : std::pow(10.f, (float)db / 2000.f); }

void update_gain(Buffer &b) {
    b.vol = db_to_gain(b.vol_db);
    b.pan_l = b.pan_db > 0 ? db_to_gain(-b.pan_db) : 1.f;
    b.pan_r = b.pan_db < 0 ? db_to_gain(b.pan_db) : 1.f;
}

inline float u2f(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }

// atenuación por distancia (ley inversa con el factor de caída) y panorámica según el oyente; con g_mx
void update_3d(Buffer &b) {
    if (!b.obj3d || b.mode3d == 2) { b.g3d_l = b.g3d_r = 1; return; }
    Vec3 d = b.pos3d;
    if (b.mode3d == 0) { d.x -= g_lis.pos.x; d.y -= g_lis.pos.y; d.z -= g_lis.pos.z; }
    float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) * g_lis.distance_factor;
    float md = std::max(b.min_dist * g_lis.distance_factor, 1e-3f);
    float eff = std::min(std::max(dist, md), std::max(b.max_dist * g_lis.distance_factor, md));
    float gain = md / (md + g_lis.rolloff * (eff - md));
    // derecha del oyente = arriba × delante (sistema de mano izquierda de DirectSound)
    const Vec3 &f = g_lis.front, &t = g_lis.top;
    Vec3 right{t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x};
    float rl = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    float pan = 0;
    if (b.mode3d == 1) pan = dist > 0 ? d.x / std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) : 0;
    else if (rl > 0 && dist > 0) pan = (d.x * right.x + d.y * right.y + d.z * right.z) / (rl * std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
    pan = std::clamp(pan, -1.f, 1.f);
    b.g3d_l = gain * std::min(1.f, 1 - pan);
    b.g3d_r = gain * std::min(1.f, 1 + pan);
}
void update_all_3d() { for (auto &kv : g_bufs) update_3d(kv.second); }

inline float sample(const Buffer &b, u32 frame, int ch) {
    u32 off = b.mem + frame * frame_bytes(b) + (u32)(ch * b.bits / 8);
    if (b.bits == 8) return ((int)MEM[off] - 128) / 128.f;
    i16 v;
    std::memcpy(&v, MEM + off, 2);
    return v / 32768.f;
}

u64 g_calladas = 0;   // tramas que se habrían repetido (el juego no rellenó a tiempo): se anotan en el registro

// Mezcla `n` tramas estéreo float de todos los búferes que suenan y avanza sus cursores (con g_mx tomado)
void mix_frames(std::vector<float> &out, int n) {
    out.assign((size_t)n * 2, 0.f);
    for (auto &kv : g_bufs) {
        Buffer &b = kv.second;
        if (!b.playing || b.primary || !b.size) continue;
        u32 frames = b.size / frame_bytes(b);
        if (!frames) continue;
        double step = (double)b.rate / OUT_RATE;
        for (int i = 0; i < n; i++) {
            u32 f = (u32)b.pos;
            if (f >= frames) {
                if (b.looping) { b.pos -= frames; f = (u32)b.pos; }
                else { b.playing = false; b.pos = 0; break; }
            }
            if (b.looping && b.unlocks_sonando >= 2 && b.nueva.size() == frames) {
                if (f != b.ultima) { if (b.ultima < frames) b.nueva[b.ultima] = 0; b.ultima = f; }
                if (!b.nueva[f]) { b.pos += step; g_calladas++; continue; }   // ya sonó y no se ha vuelto a escribir: silencio
            }
            float l = sample(b, f, 0), r = b.channels > 1 ? sample(b, f, 1) : l;
            out[2 * i] += l * b.vol * b.pan_l * b.g3d_l;
            out[2 * i + 1] += r * b.vol * b.pan_r * b.g3d_r;
            b.pos += step;
        }
    }
}

// Último momento en que el dispositivo pidió audio. Si se para (el navegador no deja sonar nada hasta que el
// usuario toca la página), los cursores avanzan igualmente con el reloj, como en una tarjeta de sonido: el juego
// sondea GetCurrentPosition y si no se mueve se queda esperando para siempre.
u32 g_last_pull_ms = 0;

void catch_up() {   // con g_mx tomado
    u32 now = ticks_ms();
    if (!g_last_pull_ms) { g_last_pull_ms = now; return; }
    u32 idle = now - g_last_pull_ms;
    if (idle < 100) return;
    static std::vector<float> scratch;
    mix_frames(scratch, (int)((u64)idle * OUT_RATE / 1000));
    g_last_pull_ms = now;
}

// Hilo de audio de SDL: mezcla `n` tramas estéreo float
void SDLCALL mix(void *, SDL_AudioStream *stream, int additional, int) {
    int n = additional / (int)(2 * sizeof(float));
    if (n <= 0) return;
    static std::vector<float> out;
    if (g_reloj_virtual) {   // con el reloj virtual los cursores solo los mueve catch_up (determinista)
        out.assign((size_t)n * 2, 0.f);
        SDL_PutAudioStreamData(stream, out.data(), (int)(out.size() * sizeof(float)));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        mix_frames(out, n);
        g_last_pull_ms = ticks_ms();
    }
    for (auto &v : out) v = std::max(-1.f, std::min(1.f, v));
    static FILE *dump = [] { const char *p = std::getenv("PCF7_AUDIO_DUMP"); return p ? std::fopen(p, "wb") : nullptr; }();
    if (dump) {   // diagnóstico: PCM float estéreo a 44100 Hz sin cabecera
        std::fwrite(out.data(), sizeof(float), out.size(), dump);
        std::fflush(dump);
    }
    SDL_PutAudioStreamData(stream, out.data(), (int)(out.size() * sizeof(float)));
}

void open_audio() {
    if (g_stream) return;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) { plog("audio: %s", SDL_GetError()); return; }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, OUT_RATE};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix, nullptr);
    if (!g_stream) { plog("audio: %s", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(g_stream);
    plog("audio: %s a %d Hz", SDL_GetCurrentAudioDriver(), OUT_RATE);
}

void read_format(Buffer &b, u32 wfx) {
    if (!wfx) return;
    b.channels = std::max<int>(1, (int)rd16(wfx + 2));
    b.rate = std::max<int>(1, (int)rd32(wfx + 4));
    b.bits = rd16(wfx + 14) == 8 ? 8 : 16;
}

// ---------------------------------------------------------------- IUnknown

void m_AddRef() { RET(com_addref(arg(0)), 1); }
// La última referencia de un búfer libera su memoria (salvo que la comparta un duplicado)
void m_Release() {
    u32 o = arg(0), n = com_release(o);
    if (!n) {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_bufs.find(o);
        if (it != g_bufs.end() && !it->second.primary) {
            u32 mem = it->second.mem;
            if (it->second.obj3d) g_3d_of.erase(it->second.obj3d);
            g_bufs.erase(it);
            bool shared = false;
            for (auto &kv : g_bufs) shared |= kv.second.mem == mem;
            if (mem && !shared) heap_free(mem);
        }
    }
    RET(n, 1);
}
void m_QI() { wr32(arg(2), 0); RET(E_NOINTERFACE, 3); }
void b_QueryInterface() {
    u32 o = arg(0), iid = arg(1), out = arg(2);
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = buf(o);
    if (b && b->primary && com_guid_eq(iid, IID_IDirectSound3DListener)) {
        if (!g_listener) g_listener = com_object(g_vt_l3d); else com_addref(g_listener);
        wr32(out, g_listener);
        RET(DS_OK, 3);
    }
    if (b && !b->primary && com_guid_eq(iid, IID_IDirectSound3DBuffer)) {
        if (!b->obj3d) { b->obj3d = com_object(g_vt_b3d); g_3d_of[b->obj3d] = o; } else com_addref(b->obj3d);
        update_3d(*b);
        wr32(out, b->obj3d);
        RET(DS_OK, 3);
    }
    if (tracing()) plog("DirectSound: QueryInterface no soportada en 0x%x", o);
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}

// ---------------------------------------------------------------- IDirectSound3DListener / IDirectSound3DBuffer

Vec3 rd_vec(u32 p) { return {u2f(rd32(p)), u2f(rd32(p + 4)), u2f(rd32(p + 8))}; }
void wr_vec(u32 p, const Vec3 &v) { u32 a, b, c; std::memcpy(&a, &v.x, 4); std::memcpy(&b, &v.y, 4); std::memcpy(&c, &v.z, 4); wr32(p, a); wr32(p + 4, b); wr32(p + 8, c); }
void wr_f(u32 p, float f) { u32 v; std::memcpy(&v, &f, 4); wr32(p, v); }

void l_GetAllParameters() {   // DS3DLISTENER: dwSize, vPosition, vVelocity, vOrientFront, vOrientTop, flDistanceFactor, flRolloffFactor, flDopplerFactor
    std::lock_guard<std::mutex> lk(g_mx);
    u32 p = arg(1);
    wr_vec(p + 4, g_lis.pos); wr_vec(p + 16, {}); wr_vec(p + 28, g_lis.front); wr_vec(p + 40, g_lis.top);
    wr_f(p + 52, g_lis.distance_factor); wr_f(p + 56, g_lis.rolloff); wr_f(p + 60, g_lis.doppler);
    RET(DS_OK, 2);
}
void l_GetF1() { wr32(arg(1), 0x3F800000); RET(DS_OK, 2); }
void l_GetOrientation() { std::lock_guard<std::mutex> lk(g_mx); wr_vec(arg(1), g_lis.front); wr_vec(arg(2), g_lis.top); RET(DS_OK, 3); }
void l_GetPosition() { std::lock_guard<std::mutex> lk(g_mx); wr_vec(arg(1), g_lis.pos); RET(DS_OK, 2); }
void l_GetVelocity() { wr_vec(arg(1), {}); RET(DS_OK, 2); }
void l_SetAllParameters() {
    std::lock_guard<std::mutex> lk(g_mx);
    u32 p = arg(1);
    g_lis.pos = rd_vec(p + 4); g_lis.front = rd_vec(p + 28); g_lis.top = rd_vec(p + 40);
    g_lis.distance_factor = u2f(rd32(p + 52)); g_lis.rolloff = u2f(rd32(p + 56)); g_lis.doppler = u2f(rd32(p + 60));
    update_all_3d();
    RET(DS_OK, 3);
}
void l_SetDistanceFactor() { std::lock_guard<std::mutex> lk(g_mx); g_lis.distance_factor = u2f(arg(1)); update_all_3d(); RET(DS_OK, 3); }
void l_SetDopplerFactor() { std::lock_guard<std::mutex> lk(g_mx); g_lis.doppler = u2f(arg(1)); RET(DS_OK, 3); }
void l_SetOrientation() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_lis.front = {u2f(arg(1)), u2f(arg(2)), u2f(arg(3))};
    g_lis.top = {u2f(arg(4)), u2f(arg(5)), u2f(arg(6))};
    update_all_3d();
    RET(DS_OK, 8);
}
void l_SetPosition() { std::lock_guard<std::mutex> lk(g_mx); g_lis.pos = {u2f(arg(1)), u2f(arg(2)), u2f(arg(3))}; update_all_3d(); RET(DS_OK, 5); }
void l_SetRolloffFactor() { std::lock_guard<std::mutex> lk(g_mx); g_lis.rolloff = u2f(arg(1)); update_all_3d(); RET(DS_OK, 3); }
void l_SetVelocity() { RET(DS_OK, 5); }
void l_Commit() { RET(DS_OK, 1); }

Buffer *b3d(u32 o) { auto it = g_3d_of.find(o); return it == g_3d_of.end() ? nullptr : buf(it->second); }
void b3_GetAllParameters() {   // DS3DBUFFER: dwSize, vPosition, vVelocity, dwInsideConeAngle, dwOutsideConeAngle, vConeOrientation, lConeOutsideVolume, flMinDistance, flMaxDistance, dwMode
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = b3d(arg(0));
    u32 p = arg(1);
    if (b) {
        wr_vec(p + 4, b->pos3d); wr_vec(p + 16, {}); wr32(p + 28, 360); wr32(p + 32, 360); wr_vec(p + 36, {0, 0, 1});
        wr32(p + 48, 0); wr_f(p + 52, b->min_dist); wr_f(p + 56, b->max_dist); wr32(p + 60, b->mode3d);
    }
    RET(DS_OK, 2);
}
void b3_GetConeAngles() { wr32(arg(1), 360); wr32(arg(2), 360); RET(DS_OK, 3); }
void b3_GetConeOrientation() { wr_vec(arg(1), {0, 0, 1}); RET(DS_OK, 2); }
void b3_GetZero() { wr32(arg(1), 0); RET(DS_OK, 2); }
void b3_GetMaxDistance() { std::lock_guard<std::mutex> lk(g_mx); Buffer *b = b3d(arg(0)); wr_f(arg(1), b ? b->max_dist : 1e9f); RET(DS_OK, 2); }
void b3_GetMinDistance() { std::lock_guard<std::mutex> lk(g_mx); Buffer *b = b3d(arg(0)); wr_f(arg(1), b ? b->min_dist : 1.f); RET(DS_OK, 2); }
void b3_GetMode() { std::lock_guard<std::mutex> lk(g_mx); Buffer *b = b3d(arg(0)); wr32(arg(1), b ? b->mode3d : 0); RET(DS_OK, 2); }
void b3_GetPosition() { std::lock_guard<std::mutex> lk(g_mx); Buffer *b = b3d(arg(0)); wr_vec(arg(1), b ? b->pos3d : Vec3{}); RET(DS_OK, 2); }
void b3_GetVelocity() { wr_vec(arg(1), {}); RET(DS_OK, 2); }
void b3_SetAllParameters() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (Buffer *b = b3d(arg(0))) {
        u32 p = arg(1);
        b->pos3d = rd_vec(p + 4); b->min_dist = u2f(rd32(p + 52)); b->max_dist = u2f(rd32(p + 56)); b->mode3d = rd32(p + 60);
        update_3d(*b);
    }
    RET(DS_OK, 3);
}
void b3_SetConeAngles() { RET(DS_OK, 4); }
void b3_SetConeOrientation() { RET(DS_OK, 5); }
void b3_SetConeOutsideVolume() { RET(DS_OK, 3); }
void b3_SetMaxDistance() { std::lock_guard<std::mutex> lk(g_mx); if (Buffer *b = b3d(arg(0))) { b->max_dist = u2f(arg(1)); update_3d(*b); } RET(DS_OK, 3); }
void b3_SetMinDistance() { std::lock_guard<std::mutex> lk(g_mx); if (Buffer *b = b3d(arg(0))) { b->min_dist = u2f(arg(1)); update_3d(*b); } RET(DS_OK, 3); }
void b3_SetMode() { std::lock_guard<std::mutex> lk(g_mx); if (Buffer *b = b3d(arg(0))) { b->mode3d = arg(1); update_3d(*b); } RET(DS_OK, 3); }
void b3_SetPosition() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (Buffer *b = b3d(arg(0))) { b->pos3d = {u2f(arg(1)), u2f(arg(2)), u2f(arg(3))}; update_3d(*b); }
    RET(DS_OK, 5);
}
void b3_SetVelocity() { RET(DS_OK, 5); }
void m_Ok1() { RET(DS_OK, 1); }
void m_Ok2() { RET(DS_OK, 2); }
void m_Ok3() { RET(DS_OK, 3); }

// ---------------------------------------------------------------- IDirectSound

void ds_CreateSoundBuffer() {
    u32 desc = arg(1), out = arg(2);
    u32 flags = rd32(desc + 4), bytes = rd32(desc + 8), wfx = rd32(desc + 16);
    Buffer b;
    b.obj = com_object(g_vt_buf);
    b.caps = flags;
    b.primary = flags & 1;   // DSBCAPS_PRIMARYBUFFER
    read_format(b, wfx);
    if (!b.primary) {
        b.size = bytes;
        b.mem = heap_alloc(bytes ? bytes : 16, true);
    }
    update_gain(b);
    u32 o = b.obj;
    { std::lock_guard<std::mutex> lk(g_mx); g_bufs[o] = b; }
    if (tracing()) plog("DirectSound: búfer 0x%x de %u B, %d Hz %d canales %d bits%s (ret 0x%x)", o, bytes, b.rate, b.channels, b.bits,
                        b.primary ? " (primario)" : "", rd32(esp));
    wr32(out, o);
    RET(DS_OK, 4);
}
void ds_DuplicateSoundBuffer() {
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *src = buf(arg(1));
    if (!src) RET(DSERR_INVALIDPARAM, 3);
    Buffer b = *src;          // comparte la memoria del original, como DirectSound
    b.obj = com_object(g_vt_buf);
    b.playing = false;
    b.pos = 0;
    b.unlocks_sonando = 0;
    b.nueva.clear();
    g_bufs[b.obj] = b;
    wr32(arg(2), b.obj);
    RET(DS_OK, 3);
}
void ds_GetCaps() {
    u32 p = arg(1);
    u32 size = rd32(p);
    if (size < 8 || size > 0x100) size = 96;
    std::memset(MEM + p, 0, size);
    wr32(p, size);
    wr32(p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x400 | 0x800);   // mono/estéreo, 8/16 bits, software
    wr32(p + 8, 100); wr32(p + 12, 100000);                               // dwMinSecondarySampleRate / Max
    wr32(p + 16, 1);                                                      // dwPrimaryBuffers
    wr32(p + 20, 64); wr32(p + 24, 64); wr32(p + 28, 64);                 // dwMaxHwMixingAllBuffers…
    RET(DS_OK, 2);
}
void ds_SetCooperativeLevel() { open_audio(); RET(DS_OK, 3); }
void ds_GetSpeakerConfig() { wr32(arg(1), 4); RET(DS_OK, 2); }   // DSSPEAKER_STEREO

// ---------------------------------------------------------------- IDirectSoundBuffer

void b_GetCaps() {
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = buf(arg(0));
    u32 p = arg(1);
    wr32(p, 20); wr32(p + 4, b ? b->caps : 0); wr32(p + 8, b ? b->size : 0); wr32(p + 12, 0); wr32(p + 16, 0);
    RET(DS_OK, 2);
}
void b_GetCurrentPosition() {
    std::lock_guard<std::mutex> lk(g_mx);
    catch_up();
    static u64 anotadas = 0;
    if (g_calladas - anotadas > 4410) {   // más de 0,1 s
        plog("audio: el juego no rellenó a tiempo; %.1f s en silencio en vez de repetir", (double)(g_calladas - anotadas) / OUT_RATE);
        anotadas = g_calladas;
    }
    Buffer *b = buf(arg(0));
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    u32 play = b->size ? ((u32)b->pos * frame_bytes(*b)) % b->size : 0;
    u32 ahead = (u32)(b->rate / 50) * frame_bytes(*b);   // el cursor de escritura va ~20 ms por delante
    if (arg(1)) wr32(arg(1), play);
    if (arg(2)) wr32(arg(2), b->size ? (play + ahead) % b->size : 0);
    RET(DS_OK, 3);
}
void b_GetFormat() {
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = buf(arg(0));
    u32 p = arg(1), cap = arg(2);
    if (arg(3)) wr32(arg(3), 18);
    if (b && p && cap >= 16) {
        wr16(p, 1); wr16(p + 2, (u32)b->channels); wr32(p + 4, (u32)b->rate);
        wr32(p + 8, (u32)b->rate * frame_bytes(*b)); wr16(p + 12, frame_bytes(*b)); wr16(p + 14, (u32)b->bits);
        if (cap >= 18) wr16(p + 16, 0);
    }
    RET(DS_OK, 4);
}
void b_GetVolume() { std::lock_guard<std::mutex> lk(g_mx); Buffer *b = buf(arg(0)); wr32(arg(1), b ? (u32)b->vol_db : 0); RET(DS_OK, 2); }
void b_GetPan() { std::lock_guard<std::mutex> lk(g_mx); Buffer *b = buf(arg(0)); wr32(arg(1), b ? (u32)b->pan_db : 0); RET(DS_OK, 2); }
void b_GetFrequency() { std::lock_guard<std::mutex> lk(g_mx); Buffer *b = buf(arg(0)); wr32(arg(1), b ? (u32)b->rate : 0); RET(DS_OK, 2); }
void b_GetStatus() {
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = buf(arg(0));
    wr32(arg(1), b && b->playing ? (1u | (b->looping ? 4u : 0u)) : 0u);   // DSBSTATUS_PLAYING | LOOPING
    RET(DS_OK, 2);
}
void b_Lock() {
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = buf(arg(0));
    u32 off = arg(1), n = arg(2), p1 = arg(3), n1 = arg(4), p2 = arg(5), n2 = arg(6), flags = arg(7);
    if (!b || !b->size) RET(DSERR_INVALIDPARAM, 8);
    if (flags & 2) n = b->size;                                                  // DSBLOCK_ENTIREBUFFER
    if (flags & 1) off = b->size ? ((u32)b->pos * frame_bytes(*b) + (u32)(b->rate / 50) * frame_bytes(*b)) % b->size : 0;   // FROMWRITECURSOR
    off %= b->size;
    if (n > b->size) n = b->size;
    u32 first = std::min(n, b->size - off);
    wr32(p1, b->mem + off);
    wr32(n1, first);
    if (p2) wr32(p2, first < n ? b->mem : 0);
    if (n2) wr32(n2, n - first);
    RET(DS_OK, 8);
}
void b_Unlock() {
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = buf(arg(0));
    u32 p1 = arg(1), n1 = arg(2), p2 = arg(3), n2 = arg(4);
    if (b && b->size && frame_bytes(*b)) {
        u32 fb = frame_bytes(*b), frames = b->size / fb;
        if (b->nueva.size() != frames) b->nueva.assign(frames, 1);
        auto marca = [&](u32 p, u32 n) {
            if (!p || !n || p < b->mem) return;
            for (u32 k = (p - b->mem) / fb, e = std::min(frames, (p - b->mem + n + fb - 1) / fb); k < e; k++) b->nueva[k] = 1;
        };
        marca(p1, n1);
        marca(p2, n2);
        if (b->playing && b->looping && b->unlocks_sonando < 2) b->unlocks_sonando++;
    }
    RET(DS_OK, 5);
}
void b_Play() {
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = buf(arg(0));
    if (b) { if (!b->playing) b->unlocks_sonando = 0; b->playing = true; b->looping = arg(3) & 1; }   // DSBPLAY_LOOPING
    if (tracing()) { static int n = 0; if (n++ < 30) plog("DirectSound: Play 0x%x%s (ret 0x%x)", arg(0), arg(3) & 1 ? " en bucle" : "", rd32(esp)); }
    RET(DS_OK, 4);
}
void b_Stop() { std::lock_guard<std::mutex> lk(g_mx); if (Buffer *b = buf(arg(0))) b->playing = false; RET(DS_OK, 1); }
void b_SetCurrentPosition() {
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer *b = buf(arg(0));
    if (b && frame_bytes(*b)) b->pos = (double)(arg(1) / frame_bytes(*b));
    RET(DS_OK, 2);
}
void b_SetFormat() { std::lock_guard<std::mutex> lk(g_mx); if (Buffer *b = buf(arg(0))) read_format(*b, arg(1)); RET(DS_OK, 2); }
void b_SetVolume() { std::lock_guard<std::mutex> lk(g_mx); if (Buffer *b = buf(arg(0))) { b->vol_db = (i32)arg(1); update_gain(*b); } RET(DS_OK, 2); }
void b_SetPan() { std::lock_guard<std::mutex> lk(g_mx); if (Buffer *b = buf(arg(0))) { b->pan_db = (i32)arg(1); update_gain(*b); } RET(DS_OK, 2); }
void b_SetFrequency() { std::lock_guard<std::mutex> lk(g_mx); if (Buffer *b = buf(arg(0)); b && arg(1)) b->rate = (int)arg(1); RET(DS_OK, 2); }

void init_vtables() {
    if (g_vt_ds) return;
    g_vt_ds = com_vtbl({m_QI, m_AddRef, m_Release, ds_CreateSoundBuffer, ds_GetCaps, ds_DuplicateSoundBuffer,
                        ds_SetCooperativeLevel, m_Ok1, ds_GetSpeakerConfig, m_Ok2, m_Ok2});
    g_vt_l3d = com_vtbl({m_QI, m_AddRef, m_Release, l_GetAllParameters, l_GetF1, l_GetF1, l_GetOrientation, l_GetPosition,
                         l_GetF1, l_GetVelocity, l_SetAllParameters, l_SetDistanceFactor, l_SetDopplerFactor,
                         l_SetOrientation, l_SetPosition, l_SetRolloffFactor, l_SetVelocity, l_Commit});
    g_vt_b3d = com_vtbl({m_QI, m_AddRef, m_Release, b3_GetAllParameters, b3_GetConeAngles, b3_GetConeOrientation,
                         b3_GetZero, b3_GetMaxDistance, b3_GetMinDistance, b3_GetMode, b3_GetPosition, b3_GetVelocity,
                         b3_SetAllParameters, b3_SetConeAngles, b3_SetConeOrientation, b3_SetConeOutsideVolume,
                         b3_SetMaxDistance, b3_SetMinDistance, b3_SetMode, b3_SetPosition, b3_SetVelocity});
    g_vt_buf = com_vtbl({b_QueryInterface, m_AddRef, m_Release, b_GetCaps, b_GetCurrentPosition, b_GetFormat, b_GetVolume, b_GetPan,
                         b_GetFrequency, b_GetStatus, m_Ok3, b_Lock, b_Play, b_SetCurrentPosition, b_SetFormat,
                         b_SetVolume, b_SetPan, b_SetFrequency, b_Stop, b_Unlock, m_Ok1});
}

// ---------------------------------------------------------------- exportaciones de dsound.dll

void DirectSoundCreate() {
    init_vtables();
    open_audio();
    if (!g_ds) g_ds = com_object(g_vt_ds); else com_addref(g_ds);
    wr32(arg(1), g_ds);
    plog("DirectSoundCreate -> 0x%x", g_ds);
    RET(DS_OK, 3);
}
void DirectSoundEnumerateA() {
    u32 s = heap_alloc(64, true);
    gstrcpy(s, "Primary Sound Driver");
    gstrcpy(s + 40, "");
    guest_call(arg(0), {0, s, s + 40, arg(1)});
    heap_free(s);
    RET(DS_OK, 2);
}

struct Registrar {
    Registrar() {
        register_dynamic_export("dsound.dll", "DirectSoundCreate", DirectSoundCreate);
        register_dynamic_export("dsound.dll", "DirectSoundEnumerateA", DirectSoundEnumerateA);
    }
} g_registrar;

}  // namespace
