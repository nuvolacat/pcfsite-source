// PCF7 — vídeo MPEG-1 (DirectShow): CLSID_FilterGraph con IGraphBuilder, IMediaControl, IMediaEventEx,
// IVideoWindow, IBasicVideo, IBasicAudio, IMediaPosition e IMediaSeeking.
//
// Los programas del CD reproducen ficheros .MPG (sistema MPEG-1: vídeo MPEG-1 + audio MP2) con el grafo
// de filtros de DirectShow: RenderFile, la ventana de vídeo dentro de una ventana suya o a pantalla
// completa, Run, y un bucle de GetEvent hasta EC_COMPLETE o hasta que el usuario pulsa una tecla. Aquí
// el grafo es un reproductor de pl_mpeg en el host: el hilo del host decodifica a tiempo real (video_tick),
// pinta el fotograma como capa superpuesta encima de la pantalla emulada (display.h) en el rectángulo de
// la ventana de vídeo y manda el audio a un flujo propio de SDL.
#include "com.h"
#include "display.h"
#include "dshow.h"
#include "win.h"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include "../third_party/pl_mpeg.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

u32 ticks_ms();

namespace {

constexpr u32 S_OK = 0, S_FALSE = 1, E_NOTIMPL = 0x80004001u, E_NOINTERFACE = 0x80004002u,
              E_FAIL = 0x80004005u, E_ABORT = 0x80004004u, VFW_E_NOT_FOUND = 0x80040216u;
constexpr u32 EC_COMPLETE = 1;

const ComGuid CLSID_FilterGraph = {0xE436EBB3, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IFilterGraph = {0x56A8689F, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IGraphBuilder = {0x56A868A9, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IMediaControl = {0x56A868B1, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IMediaPosition = {0x56A868B2, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IBasicAudio = {0x56A868B3, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IVideoWindow = {0x56A868B4, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IBasicVideo = {0x56A868B5, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IMediaEvent = {0x56A868B6, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IMediaEventEx = {0x56A868C0, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
const ComGuid IID_IMediaSeeking = {0x36B73880, 0xC2C8, 0x11CF, {0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60}};

enum Iface { I_GRAPH, I_CONTROL, I_EVENT, I_WINDOW, I_BVIDEO, I_BAUDIO, I_POSITION, I_SEEKING, I_COUNT };

struct Graph {
    u32 obj[I_COUNT] = {};      // una interfaz COM (objeto en MEM) por interfaz; todas comparten `refs`
    u32 refs = 1;
    plm_t *plm = nullptr;
    int state = 0;              // State_Stopped 0, State_Paused 1, State_Running 2
    double t = 0;               // segundos reproducidos
    u32 last_ms = 0;
    bool complete = false, event_pending = false;
    // ventana de vídeo
    u32 owner = 0;
    bool visible = true, fullscreen = false;
    i32 x = 0, y = 0, w = 0, h = 0;   // relativo al área cliente del propietario
    bool pos_set = false;
    // audio
    SDL_AudioStream *audio = nullptr;
    float volume = 1;
    DshowMusic *music = nullptr;   // PCF2001: grafo de la música con los filtros del juego (dshow.cpp)
};

std::mutex g_mx;                      // protege g_graphs (lo usan el hilo del juego y el del host)
std::map<u32, Graph *> g_graphs;      // objeto de interfaz → grafo
Graph *g_active = nullptr;            // el que pinta la capa de vídeo
u32 g_vt[I_COUNT] = {};

Graph *graph(u32 o) { auto it = g_graphs.find(o); return it == g_graphs.end() ? nullptr : it->second; }
inline double rdd(u32 a) { u64 v = rd64(a); double d; std::memcpy(&d, &v, 8); return d; }
inline void wrd(u32 a, double d) { u64 v; std::memcpy(&v, &d, 8); wr64(a, v); }
inline double argd(u32 i) { u64 v = (u64)arg(i) | (u64)arg(i + 1) << 32; double d; std::memcpy(&d, &v, 8); return d; }

std::string wstr(u32 p) {   // UTF-16 del invitado → Windows-1252 (las rutas son ASCII)
    std::string s;
    for (u32 i = 0; p && i < 1024; i++) {
        u32 c = rd16(p + 2 * i);
        if (!c) break;
        s.push_back(c < 256 ? (char)c : '?');
    }
    return s;
}

// rectángulo de la capa en la pantalla emulada (en el hilo del juego: toca el estado de user32)
void place(Graph &g) {
    Display &d = display();
    int sw, sh;
    { std::lock_guard<std::mutex> lk(d.mx); sw = d.screen.w; sh = d.screen.h; }
    int vw = g.plm ? plm_get_width(g.plm) : 320, vh = g.plm ? plm_get_height(g.plm) : 240;
    Rect r{0, 0, sw, sh};
    if (!g.fullscreen) {
        Rect o;
        int ox = 0, oy = 0;
        if (g.owner && wnd_screen_rect(g.owner, true, o)) { ox = o.l; oy = o.t; }
        if (g.pos_set && g.w > 0 && g.h > 0) r = {ox + g.x, oy + g.y, ox + g.x + g.w, oy + g.y + g.h};
        else if (g.owner) r = o;
        else r = {(sw - vw) / 2, (sh - vh) / 2, (sw - vw) / 2 + vw, (sh - vh) / 2 + vh};
    }
    {
        std::lock_guard<std::mutex> lk(d.mx);
        VideoOverlay &v = d.video;
        v.dx = r.l; v.dy = r.t; v.dw = r.w(); v.dh = r.h();
    }
    Rect wr{}, cr{};
    if (g.owner) { wnd_screen_rect(g.owner, false, wr); wnd_screen_rect(g.owner, true, cr); }
    plog("DirectShow: ventana de vídeo %d,%d %dx%d (dueño 0x%x ventana %d,%d cliente %d,%d; pos %d,%d %dx%d%s)", r.l, r.t, r.w(), r.h(),
         g.owner, wr.l, wr.t, cr.l, cr.t, g.x, g.y, g.w, g.h, g.fullscreen ? ", pantalla completa" : "");
}

void show(Graph &g, bool on) {
    Display &d = display();
    std::lock_guard<std::mutex> lk(d.mx);
    d.video.on = on;
    d.screen.frame++;
}

void SDLCALL on_video(plm_t *, plm_frame_t *f, void *) {
    Display &d = display();
    static std::vector<u8> tmp;
    tmp.resize((size_t)f->width * f->height * 3);
    plm_frame_to_rgb(f, tmp.data(), (int)f->width * 3);
    std::lock_guard<std::mutex> lk(d.mx);
    VideoOverlay &v = d.video;
    v.w = (int)f->width; v.h = (int)f->height;
    v.rgb.resize((size_t)v.w * v.h);
    for (size_t i = 0; i < v.rgb.size(); i++) v.rgb[i] = (u32)tmp[3 * i] << 16 | (u32)tmp[3 * i + 1] << 8 | tmp[3 * i + 2];
    d.screen.frame++;
}

void SDLCALL on_audio(plm_t *, plm_samples_t *s, void *user) {
    Graph *g = (Graph *)user;
    if (!g->audio) return;
    if (g->volume >= 0.999f) {
        SDL_PutAudioStreamData(g->audio, s->interleaved, (int)(s->count * 2 * sizeof(float)));
        return;
    }
    float buf[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
    for (unsigned i = 0; i < s->count * 2; i++) buf[i] = s->interleaved[i] * g->volume;
    SDL_PutAudioStreamData(g->audio, buf, (int)(s->count * 2 * sizeof(float)));
}

bool open_file(Graph &g, const std::string &winpath) {
    std::string p = host_path(winpath);
    if (g.plm) { plm_destroy(g.plm); g.plm = nullptr; }
    g.plm = plm_create_with_filename(p.c_str());
    if (!g.plm || !plm_probe(g.plm, 5000 * 1024)) {
        plog("DirectShow: no se puede abrir el vídeo \"%s\" (%s)", winpath.c_str(), p.c_str());
        if (g.plm) { plm_destroy(g.plm); g.plm = nullptr; }
        return false;
    }
    plm_set_video_decode_callback(g.plm, on_video, &g);
    if (plm_get_num_audio_streams(g.plm) > 0) {
        plm_set_audio_decode_callback(g.plm, on_audio, &g);
        plm_set_audio_lead_time(g.plm, 0.1);
        if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, plm_get_samplerate(g.plm)};
        g.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (g.audio) SDL_ResumeAudioStreamDevice(g.audio);
    } else {
        plm_set_audio_enabled(g.plm, 0);
    }
    plog("DirectShow: %s, %dx%d, %.1f s%s", winpath.c_str(), plm_get_width(g.plm), plm_get_height(g.plm),
         plm_get_duration(g.plm), g.audio ? ", con audio" : "");
    g.t = 0; g.complete = false; g.event_pending = false; g.state = 0;
    return true;
}

void close_graph(Graph *g) {
    if (g->music) { dshow_free(g->music); g->music = nullptr; }
    if (g_active == g) { g_active = nullptr; show(*g, false); }
    if (g->audio) { SDL_DestroyAudioStream(g->audio); g->audio = nullptr; }
    if (g->plm) { plm_destroy(g->plm); g->plm = nullptr; }
}

void set_state(Graph &g, int st) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (st == 2 && g.plm) {
        if (g.complete) { plm_rewind(g.plm); g.t = 0; g.complete = false; }
        g.last_ms = ticks_ms();
        if (g_active && g_active != &g) show(*g_active, false);
        g_active = &g;
        place(g);
        show(g, g.visible);
    }
    if (st == 0 && g_active == &g) {
        show(g, false);
        if (g.audio) SDL_ClearAudioStream(g.audio);
    }
    g.state = st;
}

// ---------------------------------------------------------------- IUnknown común

Graph *self() { return graph(arg(0)); }
void m_QueryInterface() {
    Graph *g = self();
    u32 iid = arg(1), out = arg(2);
    static const std::pair<const ComGuid *, Iface> map[] = {
        {&IID_IUnknown, I_GRAPH}, {&IID_IFilterGraph, I_GRAPH}, {&IID_IGraphBuilder, I_GRAPH}, {&IID_IMediaControl, I_CONTROL},
        {&IID_IMediaEvent, I_EVENT}, {&IID_IMediaEventEx, I_EVENT}, {&IID_IVideoWindow, I_WINDOW}, {&IID_IBasicVideo, I_BVIDEO},
        {&IID_IBasicAudio, I_BAUDIO}, {&IID_IMediaPosition, I_POSITION}, {&IID_IMediaSeeking, I_SEEKING}};
    if (g)
        for (auto &[guid, i] : map)
            if (com_guid_eq(iid, *guid)) { g->refs++; wr32(out, g->obj[i]); RET(S_OK, 3); }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void m_AddRef() { Graph *g = self(); RET(g ? ++g->refs : 1, 1); }
void m_Release() {
    Graph *g = self();
    if (!g) RET(0, 1);
    u32 n = g->refs ? --g->refs : 0;
    if (!n) {
        std::lock_guard<std::mutex> lk(g_mx);
        close_graph(g);
        for (u32 o : g->obj) g_graphs.erase(o);
        delete g;
    }
    RET(n, 1);
}
template <int N> void m_Ok() { RET(S_OK, N); }
template <int N> void m_NotImpl() { RET(E_NOTIMPL, N); }
template <int N> void m_Get0() { if (arg(1)) wr32(arg(1), 0); RET(S_OK, N); }   // get_* que devuelve 0

// ---------------------------------------------------------------- IGraphBuilder

void gb_AddFilter() {   // (IBaseFilter *, LPCWSTR nombre): filtros del juego (PCF2001: su fuente y su renderer de la música)
    if (Graph *g = self()) g->music = dshow_add_filter(g->music, g->obj[I_GRAPH], arg(1), arg(2));
    RET(S_OK, 3);
}
void gb_RemoveFilter() { if (Graph *g = self()) dshow_remove_filter(g->music, arg(1)); RET(S_OK, 2); }
void gb_Render() {   // (IPin *de salida): conecta la fuente del juego con su renderer pasando por el decodificador MP3
    Graph *g = self();
    RET(g && g->music ? dshow_render(g->music, arg(1)) : E_NOTIMPL, 2);
}
void gb_RenderFile() {   // (LPCWSTR fichero, LPCWSTR lista)
    Graph *g = self();
    bool ok;
    { std::lock_guard<std::mutex> lk(g_mx); ok = g && open_file(*g, wstr(arg(1))); }
    RET(ok ? S_OK : VFW_E_NOT_FOUND, 3);
}

// ---------------------------------------------------------------- IMediaControl

void mc_set(int st) { if (Graph *g = self()) { if (g->music) dshow_set_state(g->music, g->state, st); else set_state(*g, st); } RET(S_OK, 1); }
void mc_Run() { mc_set(2); }
void mc_Pause() { mc_set(1); }
void mc_Stop() { mc_set(0); }
void mc_GetState() { Graph *g = self(); if (arg(2)) wr32(arg(2), g ? (u32)g->state : 0); RET(S_OK, 3); }
void mc_RenderFile() {   // (BSTR)
    Graph *g = self();
    bool ok;
    { std::lock_guard<std::mutex> lk(g_mx); ok = g && open_file(*g, wstr(arg(1))); }
    RET(ok ? S_OK : VFW_E_NOT_FOUND, 2);
}

// ---------------------------------------------------------------- IMediaEventEx

void me_GetEventHandle() { if (arg(1)) wr32(arg(1), 0); RET(S_OK, 2); }
void me_GetEvent() {   // (long *code, LONG_PTR *p1, LONG_PTR *p2, long timeout)
    Graph *g = self();
    bool ev;
    { std::lock_guard<std::mutex> lk(g_mx); ev = g && g->event_pending; if (ev) g->event_pending = false; }
    if (!ev) RET(E_ABORT, 5);
    wr32(arg(1), EC_COMPLETE);
    if (arg(2)) wr32(arg(2), 0);
    if (arg(3)) wr32(arg(3), 0);
    RET(S_OK, 5);
}
void me_WaitForCompletion() {   // (long timeout, long *code): el juego no la usa con el bucle de mensajes
    Graph *g = self();
    u32 timeout = arg(1), until = ticks_ms() + timeout;
    for (;;) {
        { std::lock_guard<std::mutex> lk(g_mx); if (!g || g->complete || !g->plm) break; }
        if (timeout != 0xFFFFFFFFu && (i32)(until - ticks_ms()) <= 0) RET(E_ABORT, 3);
        SDL_Delay(5);
    }
    if (arg(2)) wr32(arg(2), EC_COMPLETE);
    RET(S_OK, 3);
}

// ---------------------------------------------------------------- IVideoWindow

void vw_put_Visible() {
    Graph *g = self();
    if (g) { std::lock_guard<std::mutex> lk(g_mx); g->visible = arg(1) != 0; if (g_active == g) show(*g, g->visible); }
    RET(S_OK, 2);
}
void vw_get_Visible() { Graph *g = self(); wr32(arg(1), g && g->visible ? 0xFFFFFFFFu : 0); RET(S_OK, 2); }
void vw_put_Owner() { if (Graph *g = self()) { std::lock_guard<std::mutex> lk(g_mx); g->owner = arg(1); place(*g); } RET(S_OK, 2); }
void vw_get_Owner() { Graph *g = self(); wr32(arg(1), g ? g->owner : 0); RET(S_OK, 2); }
void vw_put_FullScreenMode() { if (Graph *g = self()) { std::lock_guard<std::mutex> lk(g_mx); g->fullscreen = arg(1) != 0; place(*g); } RET(S_OK, 2); }
void vw_get_FullScreenMode() { Graph *g = self(); wr32(arg(1), g && g->fullscreen ? 0xFFFFFFFFu : 0); RET(S_OK, 2); }
void vw_SetWindowPosition() {
    if (Graph *g = self()) {
        std::lock_guard<std::mutex> lk(g_mx);
        g->x = (i32)arg(1); g->y = (i32)arg(2); g->w = (i32)arg(3); g->h = (i32)arg(4); g->pos_set = true;
        place(*g);
    }
    RET(S_OK, 5);
}
void vw_GetWindowPosition() {
    Graph *g = self();
    i32 w = g && g->pos_set ? g->w : g && g->plm ? plm_get_width(g->plm) : 0;
    i32 h = g && g->pos_set ? g->h : g && g->plm ? plm_get_height(g->plm) : 0;
    wr32(arg(1), g ? (u32)g->x : 0); wr32(arg(2), g ? (u32)g->y : 0); wr32(arg(3), (u32)w); wr32(arg(4), (u32)h);
    RET(S_OK, 5);
}
void vw_put_Left() { if (Graph *g = self()) { std::lock_guard<std::mutex> lk(g_mx); g->x = (i32)arg(1); g->pos_set = true; place(*g); } RET(S_OK, 2); }
void vw_put_Top() { if (Graph *g = self()) { std::lock_guard<std::mutex> lk(g_mx); g->y = (i32)arg(1); g->pos_set = true; place(*g); } RET(S_OK, 2); }
void vw_put_Width() { if (Graph *g = self()) { std::lock_guard<std::mutex> lk(g_mx); g->w = (i32)arg(1); g->pos_set = true; place(*g); } RET(S_OK, 2); }
void vw_put_Height() { if (Graph *g = self()) { std::lock_guard<std::mutex> lk(g_mx); g->h = (i32)arg(1); g->pos_set = true; place(*g); } RET(S_OK, 2); }
void vw_get_Size(bool width) {
    Graph *g = self();
    i32 v = g ? (width ? (g->pos_set ? g->w : g->plm ? plm_get_width(g->plm) : 0) : (g->pos_set ? g->h : g->plm ? plm_get_height(g->plm) : 0)) : 0;
    wr32(arg(1), (u32)v);
    RET(S_OK, 2);
}
void vw_get_Width() { vw_get_Size(true); }
void vw_get_Height() { vw_get_Size(false); }
void vw_get_Left() { Graph *g = self(); wr32(arg(1), g ? (u32)g->x : 0); RET(S_OK, 2); }
void vw_get_Top() { Graph *g = self(); wr32(arg(1), g ? (u32)g->y : 0); RET(S_OK, 2); }
void vw_IdealSize() {
    Graph *g = self();
    wr32(arg(1), g && g->plm ? (u32)plm_get_width(g->plm) : 0);
    wr32(arg(2), g && g->plm ? (u32)plm_get_height(g->plm) : 0);
    RET(S_OK, 3);
}

// ---------------------------------------------------------------- IBasicVideo

void bv_get_AvgTimePerFrame() { Graph *g = self(); wrd(arg(1), g && g->plm ? 1.0 / plm_get_framerate(g->plm) : 0.04); RET(S_OK, 2); }
void bv_get_VideoWidth() { Graph *g = self(); wr32(arg(1), g && g->plm ? (u32)plm_get_width(g->plm) : 0); RET(S_OK, 2); }
void bv_get_VideoHeight() { Graph *g = self(); wr32(arg(1), g && g->plm ? (u32)plm_get_height(g->plm) : 0); RET(S_OK, 2); }
void bv_GetVideoSize() { vw_IdealSize(); }
void bv_GetPos() {
    Graph *g = self();
    wr32(arg(1), 0); wr32(arg(2), 0);
    wr32(arg(3), g && g->plm ? (u32)plm_get_width(g->plm) : 0); wr32(arg(4), g && g->plm ? (u32)plm_get_height(g->plm) : 0);
    RET(S_OK, 5);
}

// ---------------------------------------------------------------- IBasicAudio

void ba_put_Volume() {   // centésimas de dB, 0 = máximo, -10000 = silencio
    if (Graph *g = self()) { i32 db = (i32)arg(1); g->volume = db <= -10000 ? 0.f : std::pow(10.f, (float)db / 2000.f); }
    RET(S_OK, 2);
}
void ba_get_Volume() { Graph *g = self(); wr32(arg(1), g && g->volume > 0 ? (u32)(i32)(2000.f * std::log10(g->volume)) : (u32)-10000); RET(S_OK, 2); }

// ---------------------------------------------------------------- IMediaPosition / IMediaSeeking

void seek(Graph &g, double t) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g.plm) return;
    t = std::clamp(t, 0.0, plm_get_duration(g.plm));
    plm_seek(g.plm, t, 0);
    g.t = t;
    g.complete = false;
    if (g.audio) SDL_ClearAudioStream(g.audio);
}
double duration(Graph *g) { return !g ? 0 : g->music ? dshow_duration(g->music) / 1e7 : g->plm ? plm_get_duration(g->plm) : 0; }
double position(Graph *g) { return !g ? 0 : g->music ? dshow_position(g->music) / 1e7 : g->t; }
void mp_get_Duration() { Graph *g = self(); wrd(arg(1), duration(g)); RET(S_OK, 2); }
void mp_put_CurrentPosition() { if (Graph *g = self()) seek(*g, argd(1)); RET(S_OK, 3); }
void mp_get_CurrentPosition() { Graph *g = self(); wrd(arg(1), position(g)); RET(S_OK, 2); }
void mp_get_StopTime() { mp_get_Duration(); }
void mp_get_Rate() { wrd(arg(1), 1.0); RET(S_OK, 2); }
void mp_CanSeek() { wr32(arg(1), 0xFFFFFFFFu); RET(S_OK, 2); }

// tiempos de IMediaSeeking en unidades de 100 ns (TIME_FORMAT_MEDIA_TIME)
void ms_GetCapabilities() { wr32(arg(1), 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x80 | 0x100); RET(S_OK, 2); }
void ms_GetDuration() { Graph *g = self(); wr64(arg(1), (u64)(duration(g) * 1e7)); RET(S_OK, 2); }
void ms_GetCurrentPosition() { Graph *g = self(); wr64(arg(1), (u64)(position(g) * 1e7)); RET(S_OK, 2); }
void ms_SetPositions() {   // (LONGLONG *cur, DWORD flags, LONGLONG *stop, DWORD flags)
    Graph *g = self();
    if (g && arg(1) && (arg(2) & 3)) {
        double t = (double)(i64)rd64(arg(1)) / 1e7;
        if ((arg(2) & 3) == 2) t += g->t;   // AM_SEEKING_RelativePositioning
        seek(*g, t);
    }
    RET(S_OK, 5);
}
void ms_GetPositions() {
    Graph *g = self();
    if (arg(1)) wr64(arg(1), (u64)(position(g) * 1e7));
    if (arg(2)) wr64(arg(2), (u64)(duration(g) * 1e7));
    RET(S_OK, 3);
}
void ms_GetRate() { wrd(arg(1), 1.0); RET(S_OK, 2); }

void init_vtables() {
    if (g_vt[0]) return;
    using F = void (*)(void);
    auto disp = [](std::initializer_list<F> rest) {   // IUnknown + IDispatch (4 métodos que no se usan) + los propios
        std::vector<F> v = {m_QueryInterface, m_AddRef, m_Release, m_Get0<2>, m_NotImpl<4>, m_NotImpl<6>, m_NotImpl<9>};
        v.insert(v.end(), rest);
        return v;
    };
    auto mk = [](const std::vector<F> &v) {
        u32 t = heap_alloc(4 * (u32)v.size(), true);
        for (size_t i = 0; i < v.size(); i++) wr32(t + 4 * (u32)i, host_fn(v[i]));
        return t;
    };
    g_vt[I_GRAPH] = mk({m_QueryInterface, m_AddRef, m_Release, gb_AddFilter, gb_RemoveFilter, m_NotImpl<2>, m_NotImpl<3>, m_NotImpl<4>,
                        m_Ok<2>, m_Ok<2>, m_Ok<1>, m_Ok<3>, gb_Render, gb_RenderFile, m_NotImpl<4>, m_Ok<2>, m_Ok<1>, m_Ok<1>});
    g_vt[I_CONTROL] = mk(disp({mc_Run, mc_Pause, mc_Stop, mc_GetState, mc_RenderFile, m_NotImpl<3>, m_NotImpl<2>,
                               m_NotImpl<2>, mc_Stop}));
    g_vt[I_EVENT] = mk(disp({me_GetEventHandle, me_GetEvent, me_WaitForCompletion, m_Ok<2>, m_Ok<2>, m_Ok<4>, m_Ok<4>,
                             m_Ok<2>, m_Get0<2>}));
    g_vt[I_WINDOW] = mk(disp({m_Ok<2>, m_Get0<2>, m_Ok<2>, m_Get0<2>, m_Ok<2>, m_Get0<2>, m_Ok<2>, m_Get0<2>, m_Ok<2>,
                              m_Get0<2>, m_Ok<2>, m_Get0<2>, vw_put_Visible, vw_get_Visible, vw_put_Left, vw_get_Left,
                              vw_put_Width, vw_get_Width, vw_put_Top, vw_get_Top, vw_put_Height, vw_get_Height, vw_put_Owner,
                              vw_get_Owner, m_Ok<2>, m_Get0<2>, m_Get0<2>, m_Ok<2>, vw_get_FullScreenMode,
                              vw_put_FullScreenMode, m_Ok<2>, m_Ok<5>, vw_SetWindowPosition, vw_GetWindowPosition,
                              vw_IdealSize, vw_IdealSize, vw_GetWindowPosition, m_Ok<2>, m_Get0<2>}));
    g_vt[I_BVIDEO] = mk(disp({bv_get_AvgTimePerFrame, m_Get0<2>, m_Get0<2>, bv_get_VideoWidth, bv_get_VideoHeight,
                              m_Ok<2>, m_Get0<2>, m_Ok<2>, bv_get_VideoWidth, m_Ok<2>, m_Get0<2>, m_Ok<2>, bv_get_VideoHeight,
                              m_Ok<2>, m_Get0<2>, m_Ok<2>, bv_get_VideoWidth, m_Ok<2>, m_Get0<2>, m_Ok<2>, bv_get_VideoHeight,
                              m_Ok<5>, bv_GetPos, m_Ok<1>, m_Ok<5>, bv_GetPos, m_Ok<1>, bv_GetVideoSize, m_NotImpl<5>,
                              m_NotImpl<3>, m_Ok<1>, m_Ok<1>}));
    g_vt[I_BAUDIO] = mk(disp({ba_put_Volume, ba_get_Volume, m_Ok<2>, m_Get0<2>}));
    g_vt[I_POSITION] = mk(disp({mp_get_Duration, mp_put_CurrentPosition, mp_get_CurrentPosition, mp_get_StopTime, m_Ok<3>,
                                mp_get_Rate /*preroll*/, m_Ok<3>, m_Ok<3>, mp_get_Rate, mp_CanSeek, mp_CanSeek}));
    g_vt[I_SEEKING] = mk({m_QueryInterface, m_AddRef, m_Release, ms_GetCapabilities, m_Ok<2>, m_Ok<2>, m_Ok<2>, m_Get0<2>,
                          m_Ok<2>, m_Ok<2>, ms_GetDuration, ms_GetDuration, ms_GetCurrentPosition, m_NotImpl<5>,
                          ms_SetPositions, ms_GetPositions, ms_GetPositions, m_Ok<3>, ms_GetRate, m_Get0<2>});
}

}  // namespace

// CoCreateInstance(CLSID_FilterGraph, …): devuelve 0 si la clase no es esta
u32 video_create(u32 clsid, u32 iid) {
    if (!com_guid_eq(clsid, CLSID_FilterGraph)) return 0;
    init_vtables();
    Graph *g = new Graph;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (int i = 0; i < I_COUNT; i++) { g->obj[i] = com_object(g_vt[i]); g_graphs[g->obj[i]] = g; }
    }
    static const std::pair<const ComGuid *, Iface> map[] = {
        {&IID_IUnknown, I_GRAPH}, {&IID_IFilterGraph, I_GRAPH}, {&IID_IGraphBuilder, I_GRAPH}, {&IID_IMediaControl, I_CONTROL},
        {&IID_IMediaEvent, I_EVENT}, {&IID_IMediaEventEx, I_EVENT}, {&IID_IVideoWindow, I_WINDOW}, {&IID_IBasicVideo, I_BVIDEO},
        {&IID_IBasicAudio, I_BAUDIO}, {&IID_IMediaPosition, I_POSITION}, {&IID_IMediaSeeking, I_SEEKING}};
    for (auto &[guid, i] : map)
        if (com_guid_eq(iid, *guid)) return g->obj[i];
    return g->obj[I_GRAPH];
}

// Hilo del host: avanza el vídeo que suena según el reloj real
void video_tick() {
    std::lock_guard<std::mutex> lk(g_mx);
    Graph *g = g_active;
    if (!g || !g->plm || g->state != 2 || g->complete) return;
    u32 now = ticks_ms();
    double dt = std::min(0.25, (now - g->last_ms) / 1000.0);
    g->last_ms = now;
    plm_decode(g->plm, dt);
    g->t += dt;
    if (plm_has_ended(g->plm)) {
        g->complete = true;
        g->event_pending = true;
        plog("DirectShow: vídeo terminado (%.1f s)", g->t);
    }
}
