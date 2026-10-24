// PCF2001 — música por DirectShow (d2s2.dll, CD2S2MP3Stream::Open). Qué hace el juego:
//   1. CoCreateInstance(CLSID_FilterGraph) y QI a IGraphBuilder e IMediaControl.
//   2. CoCreateInstance del decodificador de audio MPEG (CLSID_CMpegAudioCodec) y del divisor MPEG-1
//      (CLSID_MPEG1Splitter) como IBaseFilter, y AddFilter de los dos (si no existen, no hay música).
//   3. Crea su filtro fuente (IAsyncReader sobre el paquete SFX\Musicas: pista MP3 cruda) y su renderer
//      (acepta solo MEDIATYPE_Audio / MEDIASUBTYPE_PCM con WAVEFORMATEX), AddFilter de ambos y
//      IGraphBuilder::Render(pin de salida de la fuente).
//   4. Mira el tipo conectado en su renderer (frecuencia, canales, bits), IMediaSeeking::GetDuration y Pause.
//   5. Un hilo consumidor hace Run y espera un evento: el renderer junta en Receive los bytes que le pidieron,
//      y al tenerlos hace Pause y SetEvent.
// Aquí el grafo lee la pista entera por el IAsyncReader, la decodifica con minimp3 (third_party, CC0) y, en Run,
// entrega al IMemInputPin del renderer muestras PCM de 1152 cuadros, de forma sincrónica, hasta que el renderer
// para el grafo o se acaba la pista (EndOfStream). Los filtros del sistema son objetos vacíos.
#include "dshow.h"
#include "com.h"

#if defined(__EMSCRIPTEN__)
#define MINIMP3_NO_SIMD
#endif
#define MINIMP3_IMPLEMENTATION
#include "../third_party/minimp3.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <map>
#include <vector>

namespace {

constexpr u32 S_OK = 0, S_FALSE = 1, E_NOTIMPL = 0x80004001u, E_NOINTERFACE = 0x80004002u, E_FAIL = 0x80004005u,
              VFW_E_NOT_FOUND = 0x80040216u, VFW_E_CANNOT_CONNECT = 0x80040217u, VFW_E_MEDIA_TIME_NOT_SET = 0x80040251u;

const ComGuid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IPersist = {0x0000010C, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IPin = {0x56A86891, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IMediaFilter = {0x56A86899, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IMediaSample = {0x56A8689A, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IBaseFilter = {0x56A86895, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IMemInputPin = {0x56A8689D, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IAsyncReader = {0x56A868AA, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid CLSID_MPEG1Splitter = {0x336475D0, 0x942A, 0x11CE, {0xA8, 0x70, 0x00, 0xAA, 0x00, 0x2F, 0xEA, 0xB5}};
const ComGuid CLSID_CMpegAudioCodec = {0x38BE3000, 0xDBF4, 0x11D0, {0x86, 0x0E, 0x00, 0xA0, 0x24, 0xCF, 0xEF, 0x6D}};
const ComGuid MEDIATYPE_Audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const ComGuid MEDIASUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const ComGuid FORMAT_WaveFormatEx = {0x05589F81, 0xC356, 0x11CE, {0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A}};

// GUID en la memoria del invitado (para pasarlo a QueryInterface del juego)
u32 gguid(const ComGuid &g) {
    static std::map<const ComGuid *, u32> cache;
    auto it = cache.find(&g);
    if (it != cache.end()) return it->second;
    u32 p = heap_alloc(16);
    std::memcpy(MEM + p, &g, 16);
    cache[&g] = p;
    return p;
}
void gwrite_guid(u32 p, const ComGuid &g) { std::memcpy(MEM + p, &g, 16); }

// vtable: this en arg(0); en RET(v, n), n cuenta también el this
u32 vt(u32 obj, u32 slot) { return rd32(rd32(obj) + 4 * slot); }
u32 qi(u32 obj, const ComGuid &iid) {   // IUnknown::QueryInterface: el objeto pedido o 0
    u32 out = heap_alloc(4, true);
    u32 hr = guest_call(vt(obj, 0), {obj, gguid(iid), out});
    u32 r = hr == S_OK ? rd32(out) : 0;
    heap_free(out);
    return r;
}
void release(u32 obj) { if (obj) guest_call(vt(obj, 2), {obj}); }

}  // namespace

// ---------------------------------------------------------------- el grafo de la música

struct DshowMusic {
    u32 graph = 0;
    std::vector<u32> filters;      // IBaseFilter del juego (fuente y renderer), por orden de AddFilter
    std::vector<u8> mp3;
    mp3dec_t dec{};
    size_t pos = 0;                // byte de la pista por el que va la decodificación
    int hz = 0, ch = 0;
    u64 frames_total = 0, frames_done = 0;   // cuadros PCM
    u32 in_pin = 0, mem_in = 0;    // pin de entrada del renderer (IPin) y su IMemInputPin
    u32 out_pin = 0;               // nuestro pin de salida (lo que el renderer ve conectado)
    u32 mt = 0;                    // AM_MEDIA_TYPE de la conexión (PCM)
    u32 sample = 0, buf = 0, cap = 0;   // IMediaSample que se entrega, su búfer en MEM y su capacidad
    bool rendered = false, delivering = false, eos = false;
};

namespace {

std::map<u32, DshowMusic *> g_by_sample;    // IMediaSample → grafo
std::map<u32, DshowMusic *> g_by_pin;       // pin de salida → grafo
std::map<u32, u32> g_filters;               // filtros del sistema (vacíos) → su CLSID en MEM
u32 g_vt_filter = 0, g_vt_pin = 0, g_vt_sample = 0;

// ---- muestra (IMediaSample)
struct SampleState { u32 actual = 0; u64 t0 = 0, t1 = 0; bool times = false; };
std::map<u32, SampleState> g_sample_state;
SampleState &ss() { return g_sample_state[arg(0)]; }
void smp_QueryInterface() {
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_IMediaSample)) { com_addref(arg(0)); wr32(out, arg(0)); RET(S_OK, 3); }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void smp_AddRef() { RET(com_addref(arg(0)), 1); }
void smp_Release() { RET(com_release(arg(0)), 1); }   // vive mientras viva el grafo
void smp_GetPointer() { auto it = g_by_sample.find(arg(0)); wr32(arg(1), it != g_by_sample.end() ? it->second->buf : 0); RET(S_OK, 2); }
void smp_GetSize() { auto it = g_by_sample.find(arg(0)); RET(it != g_by_sample.end() ? it->second->cap : 0, 1); }
void smp_GetTime() {   // (REFERENCE_TIME *inicio, *fin)
    SampleState &s = ss();
    if (!s.times) RET(VFW_E_MEDIA_TIME_NOT_SET, 3);
    wr64(arg(1), s.t0); wr64(arg(2), s.t1);
    RET(S_OK, 3);
}
void smp_SetTime() { SampleState &s = ss(); s.times = arg(1) && arg(2); if (s.times) { s.t0 = rd64(arg(1)); s.t1 = rd64(arg(2)); } RET(S_OK, 3); }
void smp_IsSyncPoint() { RET(S_OK, 1); }
void smp_SetSyncPoint() { RET(S_OK, 2); }
void smp_IsPreroll() { RET(S_FALSE, 1); }
void smp_SetPreroll() { RET(S_OK, 2); }
void smp_GetActualDataLength() { RET(ss().actual, 1); }
void smp_SetActualDataLength() { ss().actual = arg(1); RET(S_OK, 2); }
void smp_GetMediaType() { wr32(arg(1), 0); RET(S_FALSE, 2); }   // sin cambio de tipo
void smp_SetMediaType() { RET(S_OK, 2); }
void smp_IsDiscontinuity() { RET(S_FALSE, 1); }
void smp_SetDiscontinuity() { RET(S_OK, 2); }
void smp_GetMediaTime() { RET(VFW_E_MEDIA_TIME_NOT_SET, 3); }
void smp_SetMediaTime() { RET(S_OK, 3); }

// ---- nuestro pin de salida (IPin): solo lo que un renderer puede preguntarle
void copy_mt(u32 dst, u32 src) {   // AM_MEDIA_TYPE (0x48 B) con copia del formato (CoTaskMemAlloc = heap_alloc)
    std::memcpy(MEM + dst, MEM + src, 0x48);
    u32 cb = rd32(src + 0x40), pb = rd32(src + 0x44);
    if (cb && pb) { u32 p = heap_alloc(cb); std::memcpy(MEM + p, MEM + pb, cb); wr32(dst + 0x44, p); }
}
void pin_QueryInterface() {
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_IPin)) { com_addref(arg(0)); wr32(out, arg(0)); RET(S_OK, 3); }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void pin_AddRef() { RET(com_addref(arg(0)), 1); }
void pin_Release() { RET(com_release(arg(0)), 1); }
void pin_Connect() { RET(E_NOTIMPL, 3); }
void pin_ReceiveConnection() { RET(E_NOTIMPL, 3); }
void pin_Disconnect() { RET(S_OK, 1); }
void pin_ConnectedTo() {   // (IPin **)
    auto it = g_by_pin.find(arg(0));
    u32 p = it != g_by_pin.end() ? it->second->in_pin : 0;
    if (p) guest_call(vt(p, 1), {p});   // AddRef
    wr32(arg(1), p);
    RET(p ? S_OK : VFW_E_NOT_FOUND, 2);
}
void pin_ConnectionMediaType() {   // (AM_MEDIA_TYPE *)
    auto it = g_by_pin.find(arg(0));
    if (it == g_by_pin.end() || !it->second->mt) { std::memset(MEM + arg(1), 0, 0x48); RET(VFW_E_NOT_FOUND, 2); }
    copy_mt(arg(1), it->second->mt);
    RET(S_OK, 2);
}
void pin_QueryPinInfo() {   // (PIN_INFO *: IBaseFilter *, dir, WCHAR[128])
    u32 p = arg(1);
    std::memset(MEM + p, 0, 8 + 256);
    wr32(p + 4, 1);   // PINDIR_OUTPUT
    const char *n = "Output";
    for (u32 i = 0; n[i]; i++) wr16(p + 8 + 2 * i, (u8)n[i]);
    RET(S_OK, 2);
}
void pin_QueryDirection() { wr32(arg(1), 1); RET(S_OK, 2); }
void pin_QueryId() { RET(E_NOTIMPL, 2); }
void pin_QueryAccept() { RET(S_OK, 2); }
void pin_EnumMediaTypes() { RET(E_NOTIMPL, 2); }
void pin_QueryInternalConnections() { RET(E_NOTIMPL, 3); }
void pin_EndOfStream() { RET(S_OK, 1); }
void pin_BeginFlush() { RET(S_OK, 1); }
void pin_EndFlush() { RET(S_OK, 1); }
void pin_NewSegment() { RET(S_OK, 7); }   // (REFERENCE_TIME, REFERENCE_TIME, double)

// ---- filtros del sistema (IBaseFilter) vacíos: el divisor MPEG-1 y el decodificador de audio
void flt_QueryInterface() {
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_IPersist) || com_guid_eq(iid, IID_IMediaFilter) || com_guid_eq(iid, IID_IBaseFilter)) {
        com_addref(arg(0)); wr32(out, arg(0)); RET(S_OK, 3);
    }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void flt_AddRef() { RET(com_addref(arg(0)), 1); }
void flt_Release() {
    u32 o = arg(0), n = com_release(o);
    if (!n) { g_filters.erase(o); heap_free(o); }
    RET(n, 1);
}
void flt_GetClassID() { auto it = g_filters.find(arg(0)); if (it != g_filters.end()) std::memcpy(MEM + arg(1), MEM + it->second, 16); RET(S_OK, 2); }
void flt_Stop() { RET(S_OK, 1); }
void flt_Pause() { RET(S_OK, 1); }
void flt_Run() { RET(S_OK, 3); }            // (REFERENCE_TIME)
void flt_GetState() { wr32(arg(2), 0); RET(S_OK, 3); }
void flt_SetSyncSource() { RET(S_OK, 2); }
void flt_GetSyncSource() { wr32(arg(1), 0); RET(S_OK, 2); }
void flt_EnumPins() { wr32(arg(1), 0); RET(E_NOTIMPL, 2); }
void flt_FindPin() { wr32(arg(2), 0); RET(VFW_E_NOT_FOUND, 3); }
void flt_QueryFilterInfo() { std::memset(MEM + arg(1), 0, 256 + 4); RET(S_OK, 2); }   // FILTER_INFO: WCHAR[128] + IFilterGraph*
void flt_JoinFilterGraph() { RET(S_OK, 3); }
void flt_QueryVendorInfo() { RET(E_NOTIMPL, 2); }

void init_vtables() {
    if (g_vt_filter) return;
    g_vt_filter = com_vtbl({flt_QueryInterface, flt_AddRef, flt_Release, flt_GetClassID, flt_Stop, flt_Pause, flt_Run, flt_GetState,
                            flt_SetSyncSource, flt_GetSyncSource, flt_EnumPins, flt_FindPin, flt_QueryFilterInfo, flt_JoinFilterGraph,
                            flt_QueryVendorInfo});
    g_vt_pin = com_vtbl({pin_QueryInterface, pin_AddRef, pin_Release, pin_Connect, pin_ReceiveConnection, pin_Disconnect, pin_ConnectedTo,
                         pin_ConnectionMediaType, pin_QueryPinInfo, pin_QueryDirection, pin_QueryId, pin_QueryAccept, pin_EnumMediaTypes,
                         pin_QueryInternalConnections, pin_EndOfStream, pin_BeginFlush, pin_EndFlush, pin_NewSegment});
    g_vt_sample = com_vtbl({smp_QueryInterface, smp_AddRef, smp_Release, smp_GetPointer, smp_GetSize, smp_GetTime, smp_SetTime,
                            smp_IsSyncPoint, smp_SetSyncPoint, smp_IsPreroll, smp_SetPreroll, smp_GetActualDataLength,
                            smp_SetActualDataLength, smp_GetMediaType, smp_SetMediaType, smp_IsDiscontinuity, smp_SetDiscontinuity,
                            smp_GetMediaTime, smp_SetMediaTime});
}

bool ours(u32 filter) { return g_filters.count(filter) != 0; }

// IBaseFilter::Stop/Pause/Run de los filtros del juego (los del sistema no hacen nada)
void filters_state(DshowMusic &m, int st) {
    for (u32 f : m.filters) {
        if (ours(f)) continue;
        if (rd32(f) < 0x400000 || !vt(f, 4)) { plog_once("dsflt0", "DirectShow: filtro 0x%x ya destruido (vtable 0x%x)", f, rd32(f)); continue; }
        u32 hr;
        if (st == 0) hr = guest_call(vt(f, 4), {f});
        else if (st == 1) hr = guest_call(vt(f, 5), {f});
        else hr = guest_call(vt(f, 6), {f, 0, 0});
        (void)hr;
    }
}

// pin de entrada del renderer: el primer pin de entrada con IMemInputPin de los filtros del juego
bool find_input(DshowMusic &m) {
    for (u32 f : m.filters) {
        if (ours(f)) continue;
        u32 out = heap_alloc(4, true);
        u32 hr = guest_call(vt(f, 10), {f, out});   // EnumPins
        u32 en = hr == S_OK ? rd32(out) : 0;
        heap_free(out);
        if (!en) continue;
        u32 pp = heap_alloc(8, true);
        for (int i = 0; i < 16; i++) {
            wr32(pp, 0); wr32(pp + 4, 0);
            if (guest_call(vt(en, 3), {en, 1, pp, pp + 4}) != S_OK || !rd32(pp)) break;   // IEnumPins::Next
            u32 pin = rd32(pp);
            u32 dir = heap_alloc(4, true);
            guest_call(vt(pin, 9), {pin, dir});   // QueryDirection
            u32 d = rd32(dir);
            heap_free(dir);
            if (d == 0) {   // PINDIR_INPUT
                if (u32 mi = qi(pin, IID_IMemInputPin)) {
                    // punteros sin referencia propia: los objetos son del juego, que los destruye con delete (sin
                    // mirar el contador) al cerrar la pista, antes de soltar el grafo
                    release(mi); release(pin);
                    m.in_pin = pin; m.mem_in = mi;
                    heap_free(pp);
                    release(en);
                    return true;
                }
            }
            release(pin);
        }
        heap_free(pp);
        release(en);
    }
    return false;
}

// lee la pista entera por el IAsyncReader del pin de la fuente
bool read_track(DshowMusic &m, u32 pin) {
    u32 rd = qi(pin, IID_IAsyncReader);
    if (!rd) { plog("DirectShow: el pin de la fuente no tiene IAsyncReader"); return false; }
    u32 len = heap_alloc(16, true);
    bool ok = guest_call(vt(rd, 8), {rd, len, len + 8}) == S_OK;   // Length(total, disponible)
    u64 total = rd64(len);
    heap_free(len);
    if (!ok || !total || total > (64u << 20)) { plog("DirectShow: Length de la pista -> %s %llu", ok ? "ok" : "error", (unsigned long long)total); release(rd); return false; }
    m.mp3.resize((size_t)total);
    const u32 CH = 64 << 10;
    u32 b = heap_alloc(CH);
    u64 pos = 0;
    while (pos < total) {
        u32 n = (u32)std::min<u64>(CH, total - pos);
        if (guest_call(vt(rd, 7), {rd, (u32)pos, (u32)(pos >> 32), n, b}) != S_OK) { ok = false; break; }   // SyncRead(pos, len, buf)
        std::memcpy(m.mp3.data() + pos, MEM + b, n);
        pos += n;
    }
    heap_free(b);
    release(rd);
    return ok;
}

// un cuadro MP3 → PCM s16 en el búfer de la muestra; cuadros PCM escritos (0 = fin)
int decode_frame(DshowMusic &m, short *pcm) {
    mp3dec_frame_info_t info{};
    while (m.pos < m.mp3.size()) {
        int n = mp3dec_decode_frame(&m.dec, m.mp3.data() + m.pos, (int)(m.mp3.size() - m.pos), pcm, &info);
        m.pos += (size_t)info.frame_bytes;
        if (n > 0) return n;
        if (!info.frame_bytes) break;   // ni cuadro ni datos que saltar
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------- API para el FilterGraph (video.cpp)

u32 dshow_create_filter(u32 clsid, u32 iid) {
    if (!com_guid_eq(clsid, CLSID_MPEG1Splitter) && !com_guid_eq(clsid, CLSID_CMpegAudioCodec)) return 0;
    init_vtables();
    u32 o = com_object(g_vt_filter);
    u32 c = heap_alloc(16);
    std::memcpy(MEM + c, MEM + clsid, 16);
    g_filters[o] = c;
    (void)iid;   // IBaseFilter, IMediaFilter, IPersist e IUnknown son el mismo objeto
    plog("DirectShow: filtro del sistema %s -> 0x%x", com_guid_eq(clsid, CLSID_MPEG1Splitter) ? "divisor MPEG-1" : "decodificador de audio MPEG", o);
    return o;
}

DshowMusic *dshow_add_filter(DshowMusic *m, u32 graph, u32 filter, u32 name) {
    init_vtables();
    if (!m) { m = new DshowMusic; m->graph = graph; }
    if (filter) {
        m->filters.push_back(filter);
        if (ours(filter)) com_addref(filter);   // el grafo retiene sus filtros; los del juego no (los destruye con delete)
        else guest_call(vt(filter, 13), {filter, graph, name});   // JoinFilterGraph
    }
    return m;
}

void dshow_remove_filter(DshowMusic *m, u32 filter) {
    if (!m) return;
    auto it = std::find(m->filters.begin(), m->filters.end(), filter);
    if (it == m->filters.end()) return;
    m->filters.erase(it);
    if (ours(filter)) call_stub(flt_Release, {filter});
}

u32 dshow_render(DshowMusic *m, u32 pin) {
    if (!m || !pin) return E_FAIL;
    if (m->rendered) return S_OK;
    if (!read_track(*m, pin)) return VFW_E_CANNOT_CONNECT;
    // formato y duración: un barrido de cabeceras (sin decodificar)
    mp3dec_init(&m->dec);
    mp3dec_frame_info_t info{};
    size_t p = 0;
    while (p < m->mp3.size()) {
        int n = mp3dec_decode_frame(&m->dec, m->mp3.data() + p, (int)(m->mp3.size() - p), nullptr, &info);
        if (!info.frame_bytes) break;
        p += (size_t)info.frame_bytes;
        if (n > 0) { m->frames_total += (u64)n; if (!m->hz) { m->hz = info.hz; m->ch = info.channels; } }
    }
    if (!m->hz || !m->ch) { plog("DirectShow: la pista (%zu B) no es MP3", m->mp3.size()); return VFW_E_CANNOT_CONNECT; }
    mp3dec_init(&m->dec);
    m->pos = 0;
    if (!find_input(*m)) { plog("DirectShow: ningún filtro del juego tiene un pin de entrada"); return VFW_E_CANNOT_CONNECT; }
    // AM_MEDIA_TYPE PCM + WAVEFORMATEX (18 B)
    u32 wfx = heap_alloc(18, true);
    u32 align = (u32)m->ch * 2;
    wr16(wfx, 1); wr16(wfx + 2, (u32)m->ch); wr32(wfx + 4, (u32)m->hz); wr32(wfx + 8, (u32)m->hz * align);
    wr16(wfx + 12, align); wr16(wfx + 14, 16); wr16(wfx + 16, 0);
    m->mt = heap_alloc(0x48, true);
    gwrite_guid(m->mt, MEDIATYPE_Audio); gwrite_guid(m->mt + 0x10, MEDIASUBTYPE_PCM);
    wr32(m->mt + 0x20, 1); wr32(m->mt + 0x24, 0); wr32(m->mt + 0x28, align);
    gwrite_guid(m->mt + 0x2C, FORMAT_WaveFormatEx); wr32(m->mt + 0x3C, 0); wr32(m->mt + 0x40, 18); wr32(m->mt + 0x44, wfx);
    m->out_pin = com_object(g_vt_pin);
    g_by_pin[m->out_pin] = m;
    u32 hr = guest_call(vt(m->in_pin, 4), {m->in_pin, m->out_pin, m->mt});   // IPin::ReceiveConnection(pin, mt)
    if (hr != S_OK) {
        plog("DirectShow: el renderer no acepta PCM %d Hz x%d: 0x%x", m->hz, m->ch, hr);
        return hr;
    }
    m->cap = MINIMP3_MAX_SAMPLES_PER_FRAME * 2;
    m->buf = heap_alloc(m->cap);
    m->sample = com_object(g_vt_sample);
    g_by_sample[m->sample] = m;
    m->rendered = true;
    plog("DirectShow: pista MP3 de %zu B, %d Hz x%d, %.1f s, conectada al renderer del juego", m->mp3.size(), m->hz, m->ch,
         (double)m->frames_total / m->hz);
    return S_OK;
}

void dshow_set_state(DshowMusic *m, int &state, int st) {
    if (!m) { state = st; return; }
    if (st == 2 && state == 0) { filters_state(*m, 1); state = 1; }   // Stopped → Paused → Running, como el grafo real
    filters_state(*m, st);
    if (st == 0) { m->pos = 0; m->frames_done = 0; m->eos = false; mp3dec_init(&m->dec); }
    state = st;
    if (st != 2 || !m->rendered || m->delivering || m->eos) return;
    // entrega sincrónica: cada Receive deja al renderer parar el grafo (Pause) cuando tiene lo que quería
    m->delivering = true;
    std::vector<short> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    while (state == 2 && !m->eos) {
        int n = decode_frame(*m, pcm.data());
        if (n <= 0) {
            m->eos = true;
            guest_call(vt(m->in_pin, 14), {m->in_pin});   // IPin::EndOfStream
            break;
        }
        u32 bytes = (u32)n * (u32)m->ch * 2;
        std::memcpy(MEM + m->buf, pcm.data(), bytes);
        SampleState &s = g_sample_state[m->sample];
        s.actual = bytes;
        s.times = true;
        s.t0 = m->frames_done * 10000000ull / (u64)m->hz;
        m->frames_done += (u64)n;
        s.t1 = m->frames_done * 10000000ull / (u64)m->hz;
        u32 hr = guest_call(vt(m->mem_in, 6), {m->mem_in, m->sample});   // IMemInputPin::Receive
        if (hr != S_OK) { plog_once("dsrecv", "DirectShow: Receive -> 0x%x (muestra %u B, estado %d)", hr, bytes, state); break; }
    }
    m->delivering = false;
}

u64 dshow_duration(const DshowMusic *m) { return m && m->hz ? m->frames_total * 10000000ull / (u64)m->hz : 0; }
u64 dshow_position(const DshowMusic *m) { return m && m->hz ? m->frames_done * 10000000ull / (u64)m->hz : 0; }

void dshow_free(DshowMusic *m) {
    if (!m) return;
    for (u32 f : m->filters) if (ours(f)) call_stub(flt_Release, {f});
    if (m->out_pin) { g_by_pin.erase(m->out_pin); heap_free(m->out_pin); }
    if (m->sample) { g_by_sample.erase(m->sample); g_sample_state.erase(m->sample); heap_free(m->sample); }
    if (m->buf) heap_free(m->buf);
    if (m->mt) { if (u32 pb = rd32(m->mt + 0x44)) heap_free(pb); heap_free(m->mt); }
    delete m;
}
