// PCF2000 — DirectMusic (DirectX 7) para la música MIDI de d2s2.dll: IDirectMusic (puertos), IDirectMusicPort,
// IDirectMusicPerformance, IDirectMusicLoader (segmentos desde memoria), IDirectMusicSegment y SegmentState.
// El sintetizador es TinySoundFont (third_party/tsf.h) con la soundfont General MIDI platform/fonts/gm.sf2
// (GeneralUser GS) y TinyMidiLoader (tml.h) lee el MIDI; el audio sale por un flujo propio de SDL.
// Lo que usa d2s2: EnumPort (se queda con los puertos de clase OUTPUT no externos), CreatePort, Port::Activate/
// SetDirectSound/GetFormat, Performance::Init/AddPort/AssignPChannelBlock/PlaySegment/Stop/IsPlaying/
// SetGlobalParam(GUID_PerfMasterVolume), Loader::GetObject(DMUS_OBJ_MEMORY, CLSID_DirectMusicSegment),
// Segment::SetParam(GUID_StandardMIDIFile, GUID_Download)/SetRepeats y SegmentState::GetSeek.
#include "com.h"
#include "reverb.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#define TSF_IMPLEMENTATION
#define TML_IMPLEMENTATION
#include "../third_party/tsf.h"
#include "../third_party/tml.h"

u32 ticks_ms();

namespace {

constexpr u32 S_OK = 0, S_FALSE = 1, E_NOTIMPL = 0x80004001u, E_NOINTERFACE = 0x80004002u, E_FAIL = 0x80004005u;

const ComGuid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid CLSID_DirectMusic = {0x636B9F10, 0x0C7D, 0x11D1, {0x95, 0xB2, 0x00, 0x20, 0xAF, 0xDC, 0x74, 0x21}};
const ComGuid IID_IDirectMusic = {0x6536115A, 0x7B2D, 0x11D2, {0xBA, 0x18, 0x00, 0x00, 0xF8, 0x75, 0xAC, 0x12}};
const ComGuid IID_IDirectMusicPort = {0x08F2D8C9, 0x37C2, 0x11D2, {0xB9, 0xF9, 0x00, 0x00, 0xF8, 0x75, 0xAC, 0x12}};
const ComGuid CLSID_DirectMusicPerformance = {0xD2AC2881, 0xB39B, 0x11D1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD}};
const ComGuid IID_IDirectMusicPerformance = {0x07D43D03, 0x6523, 0x11D2, {0x87, 0x1D, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD}};
const ComGuid CLSID_DirectMusicLoader = {0xD2AC2892, 0xB39B, 0x11D1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD}};
const ComGuid IID_IDirectMusicLoader = {0x2FFAACA2, 0x5DCA, 0x11D2, {0xAF, 0xA6, 0x00, 0xAA, 0x00, 0x24, 0xD8, 0xB6}};
const ComGuid CLSID_DirectMusicSegment = {0xD2AC2882, 0xB39B, 0x11D1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD}};
const ComGuid IID_IDirectMusicSegment = {0xF96029A2, 0x4282, 0x11D2, {0x87, 0x17, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD}};
const ComGuid IID_IDirectMusicSegmentState = {0xA3AFDCC7, 0xD3EE, 0x11D1, {0xBC, 0x8D, 0x00, 0xA0, 0xC9, 0x22, 0xE6, 0xEB}};
const ComGuid CLSID_DirectMusicCollection = {0xD2AC287C, 0xB39B, 0x11D1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD}};
const ComGuid IID_IDirectMusicCollection = {0x480FF4B0, 0x28B2, 0x11D1, {0xBE, 0xF7, 0x00, 0xC0, 0x4F, 0xBF, 0x8F, 0xEF}};
const ComGuid GUID_PerfMasterVolume = {0xD2AC28B1, 0xB39B, 0x11D1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD}};
const ComGuid GUID_PCFPort = {0x50434600, 0x2000, 0x4d49, {0x44, 0x49, 0x50, 0x43, 0x46, 0x53, 0x59, 0x4e}};   // "PCF SYN"

void write_guid(u32 p, const ComGuid &g) { wr32(p, g.a); wr16(p + 4, g.b); wr16(p + 6, g.c); std::memcpy(MEM + p + 8, g.d, 8); }
void gwstrcpy(u32 p, const char *s, u32 cap) { u32 i = 0; for (; s[i] && i + 1 < cap; i++) wr16(p + 2 * i, (u8)s[i]); wr16(p + 2 * i, 0); }

template <int N> void m_Ok() { RET(S_OK, N); }
template <int N> void m_NotImpl() { RET(E_NOTIMPL, N); }
template <int N> void m_False() { RET(S_FALSE, N); }
template <int N> void m_Get0() { if (arg(1)) wr32(arg(1), 0); RET(S_OK, N); }

// ---------------------------------------------------------------- el sintetizador
struct Segment { std::vector<u8> midi; i32 repeats = 0; u32 state = 0; };
struct Synth {
    std::mutex mx;
    tsf *sf = nullptr;
    tml_message *midi = nullptr, *cur = nullptr;
    double msec = 0;
    bool playing = false, loop = false, tried = false;
    u32 segment = 0;   // objeto IDirectMusicSegment que suena
    SDL_AudioStream *stream = nullptr;
    Freeverb reverb;
    float gain = 1.0f;   // volumen maestro del juego (GUID_PerfMasterVolume)
} g_synth;
std::map<u32, Segment> g_segments;   // objeto → segmento
std::map<u32, u32> g_states;         // objeto SegmentState → segmento

void SDLCALL audio_cb(void *, SDL_AudioStream *st, int add, int) {
    static float buf[64 * 2];
    std::lock_guard<std::mutex> lk(g_synth.mx);
    Synth &s = g_synth;
    for (int left = add / (int)(2 * sizeof(float)); left > 0; left -= 64) {
        int n = std::min(64, left);
        if (!s.playing || !s.sf) {
            std::memset(buf, 0, sizeof buf);
        } else {
            for (; s.cur && s.cur->time <= s.msec; s.cur = s.cur->next) {
                const tml_message *m = s.cur;
                switch (m->type) {
                case TML_PROGRAM_CHANGE:
                    if (!tsf_channel_set_presetnumber(s.sf, m->channel, m->program, m->channel == 9) && m->channel == 9)
                        tsf_channel_set_bank_preset(s.sf, 9, 128, 0);   // kit desconocido: el estándar
                    break;
                case TML_NOTE_ON: tsf_channel_note_on(s.sf, m->channel, m->key, m->velocity / 127.0f); break;
                case TML_NOTE_OFF: tsf_channel_note_off(s.sf, m->channel, m->key); break;
                case TML_PITCH_BEND: tsf_channel_set_pitchwheel(s.sf, m->channel, m->pitch_bend); break;
                case TML_CONTROL_CHANGE: tsf_channel_midi_control(s.sf, m->channel, m->control, m->control_value); break;
                }
            }
            tsf_render_float(s.sf, buf, n, 0);
            s.reverb.process(buf, n);
            s.msec += n * (1000.0 / 44100.0);
            if (!s.cur) {   // fin de la canción
                if (s.loop) { s.cur = s.midi; s.msec = 0; tsf_note_off_all(s.sf); }
                else s.playing = false;
            }
        }
        SDL_PutAudioStreamData(st, buf, n * (int)(2 * sizeof(float)));
    }
}

bool synth_ready() {
    Synth &s = g_synth;
    if (s.sf) return true;
    if (s.tried) return false;
    s.tried = true;
    std::string dir = SDL_GetBasePath() ? SDL_GetBasePath() : "./";
    for (std::string p : {dir + "fonts/gm.sf2", dir + "../../platform/fonts/gm.sf2", std::string("/pcf7/ro/fonts/gm.sf2")}) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        s.sf = tsf_load_memory(data.data(), (int)data.size());
        if (s.sf) { plog("DirectMusic: soundfont %s (%d presets)", p.c_str(), tsf_get_presetcount(s.sf)); break; }
    }
    if (!s.sf) { plog("DirectMusic: no se encuentra platform/fonts/gm.sf2: sin música"); return false; }
    tsf_set_output(s.sf, TSF_STEREO_INTERLEAVED, 44100, -6.0f);   // margen: la soundfont satura a 0 dB
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 44100};
    s.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (s.stream) SDL_ResumeAudioStreamDevice(s.stream);
    else plog("DirectMusic: sin dispositivo de audio (%s)", SDL_GetError());
    return true;
}
void synth_play(u32 segobj) {
    auto it = g_segments.find(segobj);
    if (it == g_segments.end() || !synth_ready()) return;
    std::lock_guard<std::mutex> lk(g_synth.mx);
    Synth &s = g_synth;
    if (s.midi) { tml_free(s.midi); s.midi = nullptr; }
    s.midi = tml_load_memory(it->second.midi.data(), (int)it->second.midi.size());
    tsf_reset(s.sf);
    tsf_channel_set_bank_preset(s.sf, 9, 128, 0);   // canal 10 = percusión (kit estándar) aunque la canción no mande programa
    s.cur = s.midi; s.msec = 0; s.loop = it->second.repeats != 0; s.playing = s.midi != nullptr; s.segment = segobj;
    unsigned ms = 0; if (s.midi) tml_get_info(s.midi, nullptr, nullptr, nullptr, nullptr, &ms);
    plog("DirectMusic: PlaySegment 0x%x (%u ms%s)", segobj, ms, s.loop ? ", en bucle" : "");
}
void synth_stop() {
    std::lock_guard<std::mutex> lk(g_synth.mx);
    Synth &s = g_synth;
    if (s.playing) plog("DirectMusic: Stop");
    s.playing = false;
    if (s.sf) tsf_note_off_all(s.sf);
}

// ---------------------------------------------------------------- objetos COM (un solo QI por tipo)
u32 g_vt_dm = 0, g_vt_port = 0, g_vt_perf = 0, g_vt_loader = 0, g_vt_seg = 0, g_vt_state = 0, g_vt_coll = 0;
u32 g_dm = 0, g_loader = 0;

void qi_one(const ComGuid &iid_ok) {   // QueryInterface de un objeto con una sola interfaz (más IUnknown)
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, iid_ok) || com_guid_eq(iid, IID_IUnknown)) { com_addref(arg(0)); wr32(out, arg(0)); RET(S_OK, 3); }
    plog_once("dmqi" + std::to_string(rd32(iid)), "DirectMusic: QueryInterface {%08x-%04x-%04x} no soportada", rd32(iid), rd16(iid + 4), rd16(iid + 6));
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void m_AddRef() { RET(com_addref(arg(0)), 1); }
void m_Release() { RET(com_release(arg(0)), 1); }

// ---- IDirectMusic
void fill_port_caps(u32 p) {   // DMUS_PORTCAPS (0x134): dwSize, dwFlags, guidPort, dwClass, dwType, memoria, grupos, voces, canales, efectos, descripción
    wr32(p + 4, 0x1 | 0x8 | 0x80 | 0x100);   // DLS | MEMORYSIZEFIXED | DIRECTSOUND | SHAREABLE (no EXTERNAL)
    write_guid(p + 8, GUID_PCFPort);
    wr32(p + 0x18, 1);        // DMUS_PC_OUTPUTCLASS
    wr32(p + 0x1C, 2);        // DMUS_PORT_USER_MODE_SYNTH
    wr32(p + 0x20, 0x7FFFFFFF);   // memoria: sistema
    wr32(p + 0x24, 2); wr32(p + 0x28, 64); wr32(p + 0x2C, 2); wr32(p + 0x30, 1);   // grupos, voces, canales, DMUS_EFFECT_REVERB
    gwstrcpy(p + 0x34, "PCF Synth (General MIDI)", 128);
}
void dm_QueryInterface() { qi_one(IID_IDirectMusic); }
void dm_EnumPort() {   // (índice, DMUS_PORTCAPS*): tres puertos iguales (el juego guarda un índice de dispositivo)
    u32 i = arg(1), caps = arg(2);
    if (i >= 3 || !caps) RET(S_FALSE, 3);
    fill_port_caps(caps);
    RET(S_OK, 3);
}
void dm_CreatePort() {   // (guid, DMUS_PORTPARAMS*, IDirectMusicPort**, unk)
    u32 o = com_object(g_vt_port);
    wr32(arg(3), o);
    RET(S_OK, 5);
}
void dm_GetDefaultPort() { write_guid(arg(1), GUID_PCFPort); RET(S_OK, 2); }

// ---- IDirectMusicPort
// IKsControl del puerto: d2s2 fija los parámetros de la reverberación (GUID_DMUS_PROP_WavesReverb, DMUS_WAVES_REVERB_PARAMS:
// ganancia de entrada dB, mezcla dB, tiempo ms, proporción de agudos) y consulta/activa los efectos (GUID_DMUS_PROP_Effects).
// El juego pide (0 dB, -5 dB, 4356 ms, 0,001): esos valores se hacen corresponder con la reverberación afinada a oído
// (wet 0,22, realimentación 0,84), que es la que suena como el juego en Windows 98; otros valores se escalan respecto a ellos.
const ComGuid IID_IKsControl = {0x28F54685, 0x06FD, 0x11D2, {0xB2, 0x7A, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const ComGuid GUID_DMUS_PROP_WavesReverb = {0x04CB5622, 0x32E5, 0x11D2, {0xAF, 0xA6, 0x00, 0xAA, 0x00, 0x24, 0xD8, 0xB6}};
const ComGuid GUID_DMUS_PROP_Effects = {0xCDA8D611, 0x684F, 0x11D2, {0x87, 0x1E, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD}};
u32 g_vt_ks = 0;
static float rdf(u32 a) { u32 v = rd32(a); float f; std::memcpy(&f, &v, 4); return f; }
static void wrf(u32 a, float f) { u32 v; std::memcpy(&v, &f, 4); wr32(a, v); }
float g_rev_gain = 0, g_rev_mix = -5, g_rev_time = 4356, g_rev_hf = 0.001f;
u32 g_rev_effects = 1;   // DMUS_EFFECT_REVERB
void aplicar_reverb() {
    std::lock_guard<std::mutex> lk(g_synth.mx);
    Freeverb &r = g_synth.reverb;
    float on = (g_rev_effects & 1) ? 1.0f : 0.0f;
    r.wet = on * 0.22f * std::pow(10.0f, (g_rev_mix + 5.0f) / 20.0f) * std::pow(10.0f, g_rev_gain / 20.0f);
    float t = std::max(1.0f, std::min(3000.0f, g_rev_time));
    r.feedback = std::max(0.5f, std::min(0.95f, 0.84f + 0.14f * std::log10(t / 4356.0f)));
    r.damp = std::max(0.05f, std::min(0.9f, 0.4f + 0.5f * (std::max(0.001f, std::min(0.999f, g_rev_hf)) - 0.001f)));
}
void ks_QueryInterface() { qi_one(IID_IKsControl); }
void ks_KsProperty() {   // (KSPROPERTY*, len, datos, len, &devueltos)
    u32 prop = arg(1), datos = arg(3), dlen = arg(4), ret = arg(5);
    u32 flags = prop ? rd32(prop + 20) : 0;   // 1 GET, 2 SET, 4 BASICSUPPORT
    if (ret) wr32(ret, 0);
    if (prop && com_guid_eq(prop, GUID_DMUS_PROP_WavesReverb)) {
        if ((flags & 2) && datos && dlen >= 16) {
            g_rev_gain = rdf(datos); g_rev_mix = rdf(datos + 4); g_rev_time = rdf(datos + 8); g_rev_hf = rdf(datos + 12);
            aplicar_reverb();
            plog("DirectMusic: reverberación del juego: entrada %.1f dB, mezcla %.1f dB, %.0f ms, agudos %.3f", g_rev_gain, g_rev_mix, g_rev_time, g_rev_hf);
        } else if ((flags & 1) && datos && dlen >= 16) {
            wrf(datos, g_rev_gain); wrf(datos + 4, g_rev_mix); wrf(datos + 8, g_rev_time); wrf(datos + 12, g_rev_hf);
            if (ret) wr32(ret, 16);
        } else if (flags & 4) { if (datos && dlen >= 4) wr32(datos, 3); if (ret) wr32(ret, 4); }   // GET | SET
        RET(S_OK, 6);
    }
    if (prop && com_guid_eq(prop, GUID_DMUS_PROP_Effects)) {
        if ((flags & 2) && datos && dlen >= 4) { g_rev_effects = rd32(datos); aplicar_reverb(); plog("DirectMusic: efectos del puerto: 0x%x", g_rev_effects); }
        else if ((flags & 1) && datos && dlen >= 4) { wr32(datos, g_rev_effects); if (ret) wr32(ret, 4); }
        else if (flags & 4) { if (datos && dlen >= 4) wr32(datos, 3); if (ret) wr32(ret, 4); }
        RET(S_OK, 6);
    }
    if (prop) plog_once("dmks" + std::to_string(rd32(prop)), "DirectMusic: KsProperty {%08x} no soportada", rd32(prop));
    RET(0x80070490, 6);   // HRESULT_FROM_WIN32(ERROR_NOT_FOUND): la propiedad no existe
}
void port_QueryInterface() {
    u32 iid = arg(1), out = arg(2);
    if (!std::getenv("PCF7_SIN_KS") && com_guid_eq(iid, IID_IKsControl)) { if (!g_vt_ks) g_vt_ks = com_vtbl({ks_QueryInterface, m_AddRef, m_Release, ks_KsProperty, m_NotImpl<6>, m_NotImpl<6>}); wr32(out, com_object(g_vt_ks)); RET(S_OK, 3); }
    qi_one(IID_IDirectMusicPort);
}
void port_GetCaps() { if (arg(1)) fill_port_caps(arg(1)); RET(S_OK, 2); }
void port_GetFormat() {   // (WAVEFORMATEX*, &tamaño, &tamaño del búfer)
    u32 wfx = arg(1), wfsize = arg(2), bufsize = arg(3);
    if (wfx) { wr16(wfx, 1); wr16(wfx + 2, 2); wr32(wfx + 4, 44100); wr32(wfx + 8, 176400); wr16(wfx + 12, 4); wr16(wfx + 14, 16); wr16(wfx + 16, 0); }
    if (wfsize) wr32(wfsize, 18);
    if (bufsize) wr32(bufsize, 44100 * 4);
    RET(S_OK, 4);
}
void port_GetNumChannelGroups() { if (arg(1)) wr32(arg(1), 1); RET(S_OK, 2); }

// ---- IDirectMusicPerformance
void perf_QueryInterface() { qi_one(IID_IDirectMusicPerformance); }
void perf_Init() { plog("DirectMusic: Performance::Init"); RET(S_OK, 4); }
void perf_PlaySegment() {   // (segmento, flags, i64 inicio (2), IDirectMusicSegmentState**)
    u32 seg = arg(1), out = arg(5);
    synth_play(seg);
    if (out) {
        auto it = g_segments.find(seg);
        if (it != g_segments.end()) {
            if (!it->second.state) { it->second.state = com_object(g_vt_state, 4); g_states[it->second.state] = seg; }
            com_addref(it->second.state);
            wr32(out, it->second.state);
        } else wr32(out, 0);
    }
    RET(S_OK, 6);
}
void perf_Stop() { synth_stop(); RET(S_OK, 5); }   // (segmento, estado, mt, flags)
void perf_GetSegmentState() { if (arg(1)) wr32(arg(1), 0); RET(E_FAIL, 4); }   // (out, mt (2))
void perf_IsPlaying() {   // (segmento, estado)
    std::lock_guard<std::mutex> lk(g_synth.mx);
    bool p = g_synth.playing && (!arg(1) || g_synth.segment == arg(1));
    RET(p ? S_OK : S_FALSE, 3);
}
void perf_GetTime() {   // (REFERENCE_TIME*, MUSIC_TIME*)
    u64 rt = (u64)ticks_ms() * 10000ull;
    if (arg(1)) { wr32(arg(1), (u32)rt); wr32(arg(1) + 4, (u32)(rt >> 32)); }
    if (arg(2)) wr32(arg(2), ticks_ms());
    RET(S_OK, 3);
}
void perf_SetGlobalParam() {   // (guid, datos, tamaño)
    u32 guid = arg(1), data = arg(2);
    if (com_guid_eq(guid, GUID_PerfMasterVolume) && data) {
        i32 v = (i32)rd32(data);   // centésimas de dB (0 .. -10000)
        float gain = v <= -10000 ? 0.f : std::pow(10.f, (float)v / 2000.f);
        std::lock_guard<std::mutex> lk(g_synth.mx);
        g_synth.gain = gain;
        if (synth_ready()) tsf_set_volume(g_synth.sf, gain);
    }
    RET(S_OK, 4);
}
void perf_GetGlobalParam() { if (arg(2) && arg(3) >= 4) wr32(arg(2), 0); RET(S_OK, 4); }
void perf_CloseDown() { synth_stop(); RET(S_OK, 1); }

// ---- IDirectMusicLoader
void loader_QueryInterface() { qi_one(IID_IDirectMusicLoader); }
void loader_GetObject() {   // (DMUS_OBJECTDESC*, riid, out): segmento o colección DLS desde memoria
    u32 desc = arg(1), iid = arg(2), out = arg(3);
    u32 valid = rd32(desc + 4), cls = desc + 24, len = rd32(desc + 0x340), mem = rd32(desc + 0x348);
    wr32(out, 0);
    if (com_guid_eq(cls, CLSID_DirectMusicSegment)) {
        // solo MIDI (MThd) o RIFF (RMID/segmentos de DirectMusic): d2s2 del PCF2001 prueba a cargar aquí sus
        // pistas MP3 antes de pasar a DirectShow, y DirectMusic las rechaza
        if ((valid & 0x400) && mem && len >= 4 && std::memcmp(MEM + mem, "MThd", 4) != 0 && std::memcmp(MEM + mem, "RIFF", 4) != 0) {
            plog_once("dmnomidi", "DirectMusic: Loader::GetObject de datos que no son MIDI (%u B): rechazado", len);
            RET(0x88781101u, 4);   // DMUS_E_UNSUPPORTED_STREAM
        }
        u32 o = com_object(g_vt_seg, 4);
        Segment &s = g_segments[o];
        if ((valid & 0x400) && mem && len) s.midi.assign(MEM + mem, MEM + mem + len);   // DMUS_OBJ_MEMORY
        wr32(out, o);
        plog("DirectMusic: segmento 0x%x desde memoria (%u B)", o, len);
        RET(S_OK, 4);
    }
    if (com_guid_eq(cls, CLSID_DirectMusicCollection)) { wr32(out, com_object(g_vt_coll)); RET(S_OK, 4); }
    (void)iid;
    plog_once("dmgetobj", "DirectMusic: Loader::GetObject de una clase no soportada {%08x}", rd32(cls));
    RET(E_NOTIMPL, 4);
}

// ---- IDirectMusicSegment
void seg_QueryInterface() { qi_one(IID_IDirectMusicSegment); }
void seg_GetLength() {   // MUSIC_TIME*: en ms (aprox.)
    auto it = g_segments.find(arg(0));
    unsigned ms = 0;
    if (it != g_segments.end() && !it->second.midi.empty())
        if (tml_message *m = tml_load_memory(it->second.midi.data(), (int)it->second.midi.size())) { tml_get_info(m, nullptr, nullptr, nullptr, nullptr, &ms); tml_free(m); }
    if (arg(1)) wr32(arg(1), ms);
    RET(S_OK, 2);
}
void seg_GetRepeats() { auto it = g_segments.find(arg(0)); if (arg(1)) wr32(arg(1), it != g_segments.end() ? (u32)it->second.repeats : 0); RET(S_OK, 2); }
void seg_SetRepeats() { auto it = g_segments.find(arg(0)); if (it != g_segments.end()) it->second.repeats = (i32)arg(1); RET(S_OK, 2); }
void seg_Release() {
    u32 o = arg(0);
    u32 n = com_release(o);
    if (!n) {
        std::lock_guard<std::mutex> lk(g_synth.mx);
        if (g_synth.segment == o) g_synth.playing = false;
        g_segments.erase(o);
    }
    RET(n, 1);
}

// ---- IDirectMusicSegmentState
void state_QueryInterface() { qi_one(IID_IDirectMusicSegmentState); }
void state_GetSegment() { auto it = g_states.find(arg(0)); u32 s = it == g_states.end() ? 0 : it->second; if (s) com_addref(s); wr32(arg(1), s); RET(s ? S_OK : E_FAIL, 2); }
void state_GetSeek() {   // MUSIC_TIME*: posición (ms)
    std::lock_guard<std::mutex> lk(g_synth.mx);
    if (arg(1)) wr32(arg(1), (u32)g_synth.msec);
    RET(S_OK, 2);
}

// ---- IDirectMusicCollection (DLS: no hay; el sintetizador usa su soundfont)
void coll_QueryInterface() { qi_one(IID_IDirectMusicCollection); }

void init() {
    if (g_vt_dm) return;
    // IDirectMusic (12): QI, AddRef, Release, EnumPort, CreateMusicBuffer, CreatePort, EnumMasterClock, GetMasterClock,
    // SetMasterClock, Activate, GetDefaultPort, SetDirectSound
    g_vt_dm = com_vtbl({dm_QueryInterface, m_AddRef, m_Release, dm_EnumPort, m_NotImpl<4>, dm_CreatePort, m_False<3>, m_NotImpl<3>,
                        m_Ok<2>, m_Ok<2>, dm_GetDefaultPort, m_Ok<3>});
    // IDirectMusicPort (20): QI, AddRef, Release, PlayBuffer, SetReadNotificationHandle, Read, DownloadInstrument,
    // UnloadInstrument, GetLatencyClock, GetRunningStats, Compact, GetCaps, DeviceIoControl, SetNumChannelGroups,
    // GetNumChannelGroups, Activate, SetChannelPriority, GetChannelPriority, SetDirectSound, GetFormat
    g_vt_port = com_vtbl({port_QueryInterface, m_AddRef, m_Release, m_Ok<2>, m_Ok<2>, m_Ok<2>, m_NotImpl<5>, m_Ok<2>, m_NotImpl<2>,
                          m_Ok<2>, m_Ok<1>, port_GetCaps, m_NotImpl<8>, m_Ok<2>, port_GetNumChannelGroups, m_Ok<2>, m_Ok<4>,
                          m_Get0<4>, m_Ok<3>, port_GetFormat});
    // IDirectMusicPerformance (44)
    g_vt_perf = com_vtbl({perf_QueryInterface, m_AddRef, m_Release, perf_Init, perf_PlaySegment, perf_Stop, perf_GetSegmentState,
                          m_Ok<2> /*SetPrepareTime*/, m_Get0<2> /*GetPrepareTime*/, m_Ok<2> /*SetBumperLength*/, m_Get0<2>,
                          m_Ok<2> /*SendPMsg*/, m_Get0<3> /*MusicToReferenceTime*/, m_Get0<4> /*ReferenceToMusicTime*/, perf_IsPlaying,
                          perf_GetTime, m_NotImpl<3> /*AllocPMsg*/, m_Ok<2> /*FreePMsg*/, m_Get0<2> /*GetGraph*/, m_Ok<2> /*SetGraph*/,
                          m_Ok<4> /*SetNotificationHandle*/, m_NotImpl<2> /*GetNotificationPMsg*/, m_Ok<2> /*AddNotificationType*/,
                          m_Ok<2> /*RemoveNotificationType*/, m_Ok<2> /*AddPort*/, m_Ok<2> /*RemovePort*/, m_Ok<4> /*AssignPChannelBlock*/,
                          m_Ok<4> /*AssignPChannel*/, m_NotImpl<5> /*PChannelInfo*/, m_NotImpl<9> /*DownloadInstrument*/, m_Ok<3> /*Invalidate*/,
                          m_NotImpl<6> /*GetParam*/, m_Ok<6> /*SetParam*/, perf_GetGlobalParam, perf_SetGlobalParam, m_Get0<2> /*GetLatencyTime*/,
                          m_Get0<2> /*GetQueueTime*/, m_Ok<3> /*AdjustTime*/, perf_CloseDown, m_Get0<5> /*GetResolvedTime*/,
                          m_NotImpl<6> /*MIDIToMusic*/, m_NotImpl<6> /*MusicToMIDI*/, m_NotImpl<6> /*TimeToRhythm*/, m_NotImpl<7> /*RhythmToTime*/});
    // IDirectMusicLoader (12)
    g_vt_loader = com_vtbl({loader_QueryInterface, m_AddRef, m_Release, loader_GetObject, m_Ok<2>, m_Ok<4>, m_Ok<4>, m_Ok<2>, m_Ok<2>,
                            m_Ok<2>, m_Ok<3>, m_False<4>});
    // IDirectMusicSegment (26)
    g_vt_seg = com_vtbl({seg_QueryInterface, m_AddRef, seg_Release, seg_GetLength, m_Ok<2>, seg_GetRepeats, seg_SetRepeats,
                         m_Get0<2>, m_Ok<2>, m_NotImpl<5> /*GetTrack*/, m_NotImpl<3>, m_Ok<3>, m_Ok<2>, m_NotImpl<4> /*InitPlay*/,
                         m_Get0<2>, m_Ok<2>, m_Ok<2>, m_Ok<2>, m_NotImpl<7> /*GetParam*/, m_Ok<6> /*SetParam*/, m_NotImpl<4> /*Clone*/,
                         m_Ok<2>, m_Get0<2>, m_Ok<3>, m_Ok<3>, m_Ok<3>});
    // IDirectMusicSegmentState (8)
    g_vt_state = com_vtbl({state_QueryInterface, m_AddRef, m_Release, m_Get0<2>, state_GetSegment, m_Get0<2>, state_GetSeek, m_Get0<2>});
    // IDirectMusicCollection (5)
    g_vt_coll = com_vtbl({coll_QueryInterface, m_AddRef, m_Release, m_NotImpl<3>, m_False<5>});
}

}  // namespace

// CoCreateInstance de las clases de DirectMusic: el objeto (con una referencia) o 0 si no es una de ellas
u32 dmusic_create(u32 clsid, u32 iid) {
    (void)iid;
    if (com_guid_eq(clsid, CLSID_DirectMusic)) { init(); if (!g_dm) g_dm = com_object(g_vt_dm); else com_addref(g_dm); plog("DirectMusic: IDirectMusic 0x%x", g_dm); return g_dm; }
    if (com_guid_eq(clsid, CLSID_DirectMusicPerformance)) { init(); return com_object(g_vt_perf); }
    if (com_guid_eq(clsid, CLSID_DirectMusicLoader)) { init(); if (!g_loader) g_loader = com_object(g_vt_loader); else com_addref(g_loader); return g_loader; }
    if (com_guid_eq(clsid, CLSID_DirectMusicSegment)) { init(); u32 o = com_object(g_vt_seg, 4); g_segments[o]; return o; }
    if (com_guid_eq(clsid, CLSID_DirectMusicCollection)) { init(); return com_object(g_vt_coll); }
    return 0;
}
