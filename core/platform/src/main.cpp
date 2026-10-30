// PCF7 — punto de entrada del host (SDL3).
//
// El hilo principal lleva la ventana, presenta la pantalla emulada (escalada) y convierte la entrada de
// SDL en eventos para el juego. El juego corre en su propio hilo con una pila de C grande: cada llamada
// x86 traducida es una llamada de C, y los bucles de mensajes y los saltos de cola anidan mucho.
//
// Variables de entorno:
//   PCF7_MEMBIN    imagen de memoria (por defecto: build/gen/mem.bin junto al ejecutable; si no existe, se construye con
//                  los ficheros del juego: imagen.cpp). PCF7_WORK: la carpeta de trabajo (la app la pone)
//   PCF7_HEADLESS  1 = sin ventana (pruebas)
//   PCF7_SHOT      prefijo de capturas BMP; PCF7_SHOT_MS = intervalo (ms)
//   PCF7_SCRIPT    entrada automática: "1500:click:320:240;3000:key:13;…" (ms desde el arranque)
//   PCF7_TIMEOUT   segundos hasta cerrar (pruebas)
//   PCF7_MANDO_PRUEBA  pruebas sin mando: el juego ve uno conectado (el guion lo mueve con «joy»)
#include "cursor.h"
#include "display.h"
#include "threads.h"

#include <SDL3/SDL.h>
#ifndef PCF_DLL
#include <SDL3/SDL_main.h>
#endif
#include <climits>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <process.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <new>
#include <string>
#include <vector>

extern std::string g_imagen_error;   // imagen.cpp: por qué no se ha podido construir la imagen de memoria

int run_game(const char *membin);
void dump_ring();
void mem_hashes();               // core.cpp: diagnóstico (comparar nativo y web)
void mem_dump(u32 a, u32 n);
void mem_find(const char *t);   // core.cpp (PCF7_MEMFIND)
#ifdef __EMSCRIPTEN__
bool web_mount(const char *base);   // web.cpp
extern u32 g_traza_paso, g_traza_desde, g_traza_cuantas;   // core.cpp: traza comparable (diagnóstico)
EM_JS(int, web_traza_js, (int que), { var t = Module.pcf7Traza || ""; var p = t.split(":"); return parseInt(p[que] || "0", 10) || 0; });
EM_JS(int, web_reloj_js, (), { return Module.pcf7Reloj ? 1 : 0; });
// URL de los datos del juego (Module.pcf7Datos; por defecto «datos», junto a la página). Se lee en el hilo principal:
// en los hilos (Web Workers) el objeto Module no tiene lo que puso la página.
EM_JS_DEPS(pcf7_datos, "$stringToNewUTF8");
EM_JS(char *, web_datos_js, (), { return stringToNewUTF8(Module.pcf7Datos || "datos"); });
extern bool g_reloj_virtual;
#endif
u32 ticks_ms();
u8 unicode_to_cp1252(u32 cp);

namespace {
std::string g_membin;
#ifdef __EMSCRIPTEN__
std::string g_datos_url;
#endif
std::atomic<bool> g_game_done{false};
int g_rc = 0;

void *game_thread(void *) {
#ifdef __EMSCRIPTEN__
    if (!web_mount(g_datos_url.c_str())) { g_rc = 1; g_game_done = true; return nullptr; }
#endif
    g_rc = run_game(g_membin.c_str());
    g_game_done = true;
    return nullptr;
}

const char *env(const char *k) { const char *v = std::getenv(k); return v && *v ? v : nullptr; }

// SDL (USB HID) → tecla virtual de Windows y código de rastreo del juego de 1 (PC/AT)
struct KeyMap { SDL_Scancode sc; u32 vk, scan; bool ext; };
const KeyMap KEYS[] = {
    {SDL_SCANCODE_ESCAPE, 0x1B, 0x01, false}, {SDL_SCANCODE_RETURN, 0x0D, 0x1C, false}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E, false},
    {SDL_SCANCODE_TAB, 0x09, 0x0F, false}, {SDL_SCANCODE_SPACE, 0x20, 0x39, false}, {SDL_SCANCODE_LSHIFT, 0x10, 0x2A, false},
    {SDL_SCANCODE_RSHIFT, 0x10, 0x36, false}, {SDL_SCANCODE_LCTRL, 0x11, 0x1D, false}, {SDL_SCANCODE_RCTRL, 0x11, 0x1D, true},
    {SDL_SCANCODE_LALT, 0x12, 0x38, false}, {SDL_SCANCODE_RALT, 0x12, 0x38, true}, {SDL_SCANCODE_LEFT, 0x25, 0x4B, true},
    {SDL_SCANCODE_UP, 0x26, 0x48, true}, {SDL_SCANCODE_RIGHT, 0x27, 0x4D, true}, {SDL_SCANCODE_DOWN, 0x28, 0x50, true},
    {SDL_SCANCODE_INSERT, 0x2D, 0x52, true}, {SDL_SCANCODE_DELETE, 0x2E, 0x53, true}, {SDL_SCANCODE_HOME, 0x24, 0x47, true},
    {SDL_SCANCODE_END, 0x23, 0x4F, true}, {SDL_SCANCODE_PAGEUP, 0x21, 0x49, true}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, true},
    {SDL_SCANCODE_KP_ENTER, 0x0D, 0x1C, true}, {SDL_SCANCODE_MINUS, 0xBD, 0x0C, false}, {SDL_SCANCODE_EQUALS, 0xBB, 0x0D, false},
    {SDL_SCANCODE_COMMA, 0xBC, 0x33, false}, {SDL_SCANCODE_PERIOD, 0xBE, 0x34, false}, {SDL_SCANCODE_SLASH, 0xBF, 0x35, false},
    {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27, false}, {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28, false}, {SDL_SCANCODE_GRAVE, 0xC0, 0x29, false},
    {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A, false}, {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B, false}, {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B, false},
};
bool map_key(SDL_Scancode sc, u32 &vk, u32 &scan, bool &ext) {
    static const u8 LETTER_SCAN[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                       0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    ext = false;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { vk = 'A' + (sc - SDL_SCANCODE_A); scan = LETTER_SCAN[sc - SDL_SCANCODE_A]; return true; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) { int n = sc - SDL_SCANCODE_1; vk = n == 9 ? '0' : '1' + n; scan = 0x02 + n; return true; }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) { vk = 0x70 + (sc - SDL_SCANCODE_F1); scan = 0x3B + (sc - SDL_SCANCODE_F1); return true; }
    if (sc == SDL_SCANCODE_F11) { vk = 0x7A; scan = 0x57; return true; }
    if (sc == SDL_SCANCODE_F12) { vk = 0x7B; scan = 0x58; return true; }
    for (auto &k : KEYS) if (k.sc == sc) { vk = k.vk; scan = k.scan; ext = k.ext; return true; }
    return false;
}

void key_event(bool down, u32 vk, u32 scan, bool ext, bool repeat) {
    Display &d = display();
    { std::lock_guard<std::mutex> lk(d.mx); d.keys[vk & 0xFF] = down; }
    dinput_key((scan & 0x7F) | (ext ? 0x80 : 0), down);   // DirectInput (dinput.cpp): códigos DIK
    InEvent e{down ? InKind::KeyDown : InKind::KeyUp};
    e.vk = vk; e.scan = scan; e.ext = ext; e.repeat = repeat;
    d.push(e);
    // TranslateMessage: las teclas de control también llegan como WM_CHAR (el texto lo da SDL_EVENT_TEXT_INPUT)
    if (down && (vk == 0x08 || vk == 0x09 || vk == 0x0D || vk == 0x1B)) {
        InEvent c{InKind::Char};
        c.vk = vk;
        d.push(c);
    }
}

// pantalla emulada (+ cursor) → RGBA
void compose(std::vector<u32> &rgb, int &w, int &h) {
    Display &d = display();
    u32 cur;
    bool vis;
    int mx, my;
    {
        std::lock_guard<std::mutex> lk(d.mx);
        const Screen &s = d.screen;
        w = s.w; h = s.h;
        rgb.assign((size_t)w * h, 0xFF000000u);
        if (s.fb) {
            for (int y = 0; y < h; y++) {
                const u8 *row = MEM + s.fb + (size_t)y * s.pitch;
                u32 *out = rgb.data() + (size_t)y * w;
                if (s.bpp == 8) {
                    for (int x = 0; x < w; x++) out[x] = 0xFF000000u | s.pal[row[x]];
                } else if (s.bpp == 16) {
                    for (int x = 0; x < w; x++) {
                        u32 v = (u32)row[2 * x] | (u32)row[2 * x + 1] << 8;
                        u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                        out[x] = 0xFF000000u | (r << 19 | (r >> 2) << 16) | (g << 10 | (g >> 4) << 8) | (b << 3 | b >> 2);
                    }
                } else {
                    for (int x = 0; x < w; x++) { u32 v; std::memcpy(&v, row + 4 * x, 4); out[x] = 0xFF000000u | v; }
                }
            }
        }
        const VideoOverlay &v = d.video;
        if (v.on && v.w > 0 && v.h > 0 && v.dw > 0 && v.dh > 0 && v.rgb.size() >= (size_t)v.w * v.h) {
            for (int y = std::max(0, v.dy); y < std::min(h, v.dy + v.dh); y++) {
                int sy = (int)((i64)(y - v.dy) * v.h / v.dh);
                const u32 *src = v.rgb.data() + (size_t)sy * v.w;
                u32 *out = rgb.data() + (size_t)y * w;
                for (int x = std::max(0, v.dx); x < std::min(w, v.dx + v.dw); x++)
                    out[x] = 0xFF000000u | src[(i64)(x - v.dx) * v.w / v.dw];
            }
        }
        if (d.caret.on) {
            const CaretOverlay &c = d.caret;
            for (int y = std::max(0, c.y); y < std::min(h, c.y + c.h); y++)
                for (int x = std::max(0, c.x); x < std::min(w, c.x + c.w); x++) rgb[(size_t)y * w + x] ^= 0xFFFFFF;
        }
        cur = d.cursor; vis = d.cursor_visible; mx = d.mouse_x; my = d.mouse_y;
    }
    const CursorImage *c = vis ? cursor_image(cur) : nullptr;
    if (!c) return;
    for (int y = 0; y < c->h; y++) {
        int py = my - c->hot_y + y;
        if (py < 0 || py >= h) continue;
        for (int x = 0; x < c->w; x++) {
            int px = mx - c->hot_x + x;
            u32 v = c->rgba[(size_t)y * c->w + x];
            if (px >= 0 && px < w && (v >> 24)) rgb[(size_t)py * w + px] = v;
        }
    }
}

void save_shot(const std::string &path) {
    std::vector<u32> rgb;
    int w, h;
    compose(rgb, w, h);
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, rgb.data(), w * 4);
    if (s) { SDL_SaveBMP(s, path.c_str()); SDL_DestroySurface(s); }
}

// Guion de pruebas (PCF7_SCRIPT="ms:op:a:b[:c];…"): click/rclick/move/down/up x y, key vk scan, char c, shot n,
// wait x y RRGGBB (espera hasta que ese píxel tenga ese color ±24 y desplaza los pasos siguientes) e
// ifpx x y RRGGBB (el paso siguiente solo se ejecuta si el píxel tiene ese color).
struct ScriptStep { u32 at; std::string op; int a = 0, b = 0; u32 c = 0; };
bool pixel_is(int x, int y, u32 rgb) {
    std::vector<u32> img;
    int w, h;
    compose(img, w, h);
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    u32 v = img[(size_t)y * w + x];
    for (int sh = 0; sh < 24; sh += 8)
        if (std::abs((int)((v >> sh) & 0xFF) - (int)((rgb >> sh) & 0xFF)) > 24) return false;
    return true;
}
std::vector<ScriptStep> parse_script(const char *s) {
    std::vector<ScriptStep> out;
    std::stringstream ss(s ? s : "");
    std::string item;
    while (std::getline(ss, item, ';')) {
        std::stringstream is(item);
        std::string t, op, a, b, c;
        std::getline(is, t, ':'); std::getline(is, op, ':'); std::getline(is, a, ':'); std::getline(is, b, ':'); std::getline(is, c, ':');
        if (t.empty() || op.empty()) continue;
        int ia = op == "poke" ? (int)std::strtoul(a.c_str(), nullptr, 16) : std::atoi(a.c_str());   // poke: dirección en hex
        out.push_back({(u32)std::atoi(t.c_str()), op, ia, std::atoi(b.c_str()), (u32)std::strtoul(c.c_str(), nullptr, 16)});
    }
    return out;
}
void run_script_step(const ScriptStep &st) {
    Display &d = display();
    plog("guion: %s %d %d", st.op.c_str(), st.a, st.b);
    if (st.op == "click" || st.op == "rclick") {
        int btn = st.op == "click" ? 0 : 1;
        InEvent m{InKind::MouseMove}; m.x = st.a; m.y = st.b; d.push(m);
        InEvent dn{InKind::MouseDown}; dn.x = st.a; dn.y = st.b; dn.button = btn; d.push(dn);
        InEvent up{InKind::MouseUp}; up.x = st.a; up.y = st.b; up.button = btn; d.push(up);
    } else if (st.op == "move") {
        InEvent m{InKind::MouseMove}; m.x = st.a; m.y = st.b; d.push(m);
    } else if (st.op == "down" || st.op == "up") {   // botón izquierdo pulsado o soltado (arrastrar: down, move…, up)
        InEvent m{InKind::MouseMove}; m.x = st.a; m.y = st.b; d.push(m);
        InEvent e{st.op == "down" ? InKind::MouseDown : InKind::MouseUp}; e.x = st.a; e.y = st.b; e.button = 0; d.push(e);
    } else if (st.op == "key") {
        key_event(true, (u32)st.a, (u32)st.b, false, false);
        key_event(false, (u32)st.a, (u32)st.b, false, false);
    } else if (st.op == "keydown" || st.op == "keyup") {   // mantener una tecla pulsada (partido interactivo; código + 0x100 = extendida)
        key_event(st.op == "keydown", (u32)st.a, (u32)st.b & 0xFF, (st.b & 0x100) != 0, false);
    } else if (st.op == "poke") {   // pruebas: escribe un byte en la memoria del juego (dirección en hex)
        wr8((u32)st.a, (u32)st.b);
    } else if (st.op == "char") {
        InEvent c{InKind::Char}; c.vk = (u32)st.a; d.push(c);
    } else if (st.op == "shot") {
        save_shot("shot_" + std::to_string(st.a) + ".bmp");
    } else if (st.op == "joy") {   // mando 1 en x y (0..65535) con los botones c (hex, bit 0 = botón 1)
        InEvent j{InKind::Joy}; j.x = st.a; j.y = st.b; j.vk = st.c; d.push(j);
    }
}
}  // namespace

// ---------------------------------------------------------------- lanzador (PCF7.EXE)
//
// En el original, PCF7.EXE arranca la intro (playvid) y MANAGER; MANAGER sale con el número del apartado
// del menú principal y el lanzador abre el programa que toca (tabla de PCF7.EXE: "PCF5%X"); esos
// programas, al terminar, vuelven a lanzar PCF7.EXE, que abre otra vez MANAGER (o ACTWIN si hay
// jornadas descargadas por aplicar). Aquí cada programa es un ejecutable hermano (pcf7-<nombre>) y el
// paso de uno a otro es un execv.
// La tabla apartado → programa es la de cada juego (juegos/<id>/juego.json, «lanzador»; juego.c la trae).
#ifdef PCF_DOS
std::string dos_siguiente();   // dos.cpp
#endif
struct Launch { const char *exe, *arg; };
Launch launch_for_code(u32 code) {
    for (u32 i = 0; JUEGO_LANZADOR[i].programa; i++)
        if (JUEGO_LANZADOR[i].apartado == code) return {JUEGO_LANZADOR[i].programa, JUEGO_LANZADOR[i].arg};
    return {nullptr, nullptr};
}

// La carpeta del ejecutable de verdad (donde están los programas hermanos <id>-<programa>). SDL_GetBasePath() da, en
// un .app de macOS, Contents/Resources (ahí van las fuentes), no Contents/MacOS.
std::string exe_real_dir(const char *argv0) {
    std::string p;
#ifdef __APPLE__
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0) { char r[PATH_MAX]; p = realpath(buf, r) ? r : buf; }
#elif defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; p = buf; }
#elif defined(_WIN32)
    char buf[4096];
    if (plat_exe_path(buf, sizeof buf)) { p = buf; for (auto &c : p) if (c == '\\') c = '/'; }
#endif
#ifndef _WIN32
    if (p.empty() && argv0 && std::strchr(argv0, '/')) { char r[PATH_MAX]; p = realpath(argv0, r) ? r : argv0; }
#endif
    if (p.empty()) return SDL_GetBasePath() ? SDL_GetBasePath() : "./";
    return p.substr(0, p.find_last_of('/') + 1);
}

static std::string util_nombre_base(const std::string &p) { size_t i = p.find_last_of("/\\"); return i == std::string::npos ? p : p.substr(i + 1); }

[[noreturn]] void exec_program(const std::string &dir, const char *exe, const char *a1) {
    std::string principal = JUEGO_PRINCIPAL;
    principal.resize(principal.rfind('.'));
    std::string path = dir + (exe != principal ? std::string(JUEGO_ID) + "-" + exe : std::string(JUEGO_ID));
    plog("lanzador: %s%s%s", path.c_str(), a1 ? " " : "", a1 ? a1 : "");
    setenv("PCF7_RELAUNCHED", "1", 1);
    // guion de pruebas: cada programa puede llevar el suyo (PCF7_SCRIPT_DBASEWIN…), que se consume
    unsetenv("PCF7_SCRIPT");
    std::string key = std::string("PCF7_SCRIPT_") + exe;
    for (auto &c : key) c = (char)std::toupper((unsigned char)c);
    if (const char *sc = std::getenv(key.c_str())) { setenv("PCF7_SCRIPT", sc, 1); unsetenv(key.c_str()); }
#ifdef _WIN32
    // Windows no tiene execv: el programa hermano corre como hijo y este proceso espera y sale con su código (así
    // el lanzador de la app sigue viendo un solo proceso). Como DLL (PCF_DLL), el hijo es el mismo ejecutable
    // anfitrión («PCFSite - Source.exe --programa <id>-<programa>»), que carga la DLL del hermano.
    std::string prog = util_nombre_base(path);
    std::vector<std::string> args;
#ifdef PCF_DLL
    char self[4096];
    if (!plat_exe_path(self, sizeof self)) { plog("lanzador: no sé qué ejecutable soy"); std::_Exit(1); }
    path = self;
    args = {"--programa", prog};
#else
    path += ".exe";
#endif
    if (a1) args.push_back(a1);
    // _spawnv junta los argumentos con espacios sin comillas: van entre comillas los que llevan espacios (reglas del CRT
    // de Microsoft; los nuestros no llevan comillas), también el argv[0] («…/PCFSite - Source.exe»: sin comillas el
    // hijo veía «-» como primera opción y salía con el código 2)
    args.insert(args.begin(), path);
    std::vector<std::string> cit;
    for (const std::string &s : args) cit.push_back(s.find_first_of(" \t") == std::string::npos ? s : "\"" + s + "\"");
    std::vector<const char *> av;
    for (const std::string &s : cit) av.push_back(s.c_str());
    av.push_back(nullptr);
    intptr_t rc = _spawnv(_P_WAIT, path.c_str(), av.data());
    if (rc == -1) { plog("lanzador: no se puede ejecutar %s", path.c_str()); std::_Exit(1); }
    std::_Exit((int)rc);
#else
    std::vector<char *> av{(char *)path.c_str()};
    if (a1) av.push_back((char *)a1);
    av.push_back(nullptr);
    execv(path.c_str(), av.data());
    plog("lanzador: no se puede ejecutar %s", path.c_str());
    std::_Exit(1);
#endif
}

// ¿hay jornadas descargadas (ACTUALIZ\JORNADAS\*.ACT) que ACTWIN deba aplicar?
bool pending_updates() {
    std::string d = host_path(std::string("C:\\") + JUEGO_CARPETA + "\\ACTUALIZ\\JORNADAS");
    DIR *dir = opendir(d.c_str());
    if (!dir) return false;
    bool any = false;
    while (dirent *e = readdir(dir)) {
        std::string n = e->d_name;
        if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".ACT") == 0) {
            struct stat st;
            if (stat((d + "/" + n).c_str(), &st) == 0 && st.st_size > 1) any = true;
        }
    }
    closedir(dir);
    return any;
}

void d3d_stats();   // d3d.cpp (PCF7_TRACE_D3D)
void video_tick();  // video.cpp: avanza el vídeo de DirectShow que esté sonando

#ifdef __EMSCRIPTEN__
// El juego ha terminado: la página decide (volver al menú recargando, o enseñar «Has salido»). Recibe el apartado
// que el manager pidió al lanzador (0 o 1 = salir; 2… = otro programa del CD, que en la web aún no existe).
EM_JS(void, web_game_ended_js, (int code), { if (Module.pcf7Terminado) Module.pcf7Terminado(code); });
// el lanzador en la web: la página carga otro programa (juego-<programa>.js) con su argumento (Module.pcf7Lanzar)
EM_JS(void, web_lanzar_js, (const char *prog, const char *arg), {
    if (Module.pcf7Lanzar) Module.pcf7Lanzar(UTF8ToString(prog), arg ? UTF8ToString(arg) : "");
});
void web_game_ended() {
    web_persistir(true);
    std::string target = TARGET_EXE, principal = JUEGO_PRINCIPAL;
    principal.resize(principal.rfind('.'));
#ifdef PCF_DOS
    std::string sig = dos_siguiente();
    if (!sig.empty()) { web_lanzar_js(sig.c_str(), nullptr); return; }
    web_game_ended_js(0);
    return;
#endif
    if (JUEGO_SALIDA_MENU && target == JUEGO_PRINCIPAL) {
        u32 code = rd32(JUEGO_SALIDA_MENU);   // apartado elegido en el menú principal (0 o 1 = salir)
        Launch l = launch_for_code(code);
        if (l.exe) { web_lanzar_js(l.exe, l.arg); return; }
        if (code >= 2) { web_lanzar_js(principal.c_str(), nullptr); return; }   // apartado sin programa: al menú
        web_game_ended_js((int)code);
        return;
    }
    if (target != JUEGO_PRINCIPAL) {   // cualquier otro programa vuelve al lanzador: ACTWIN si hay jornadas, si no el principal
        web_lanzar_js(target != "actwin.exe" && pending_updates() ? "actwin" : principal.c_str(), nullptr);
        return;
    }
    web_game_ended_js(0);
}
// Diagnóstico desde la página (Module.ccall("pcf7_volcado")): dónde está cada hilo, como al agotar el tiempo
extern "C" EMSCRIPTEN_KEEPALIVE void pcf7_volcado() { dump_ring(); threads_dump(); mem_hashes(); }
extern "C" EMSCRIPTEN_KEEPALIVE void pcf7_mem(u32 a, u32 n) { mem_dump(a, n); }
#endif

// Estado del hilo principal entre fotogramas (en el navegador, host_frame lo llama el bucle del navegador)
struct Host {
    bool headless = false;
    const char *shot = nullptr;
    u32 shot_ms = 0, timeout_ms = 0, next_shot = 0, quit_at = 0, last_frame = ~0u;
    std::vector<ScriptStep> script;
    size_t script_i = 0;
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr;
    int tex_w = 0, tex_h = 0, last_mx = -1, last_my = -1;
    bool host_cursor_shown = false;
    std::vector<u32> rgb;
    // mandos del host (los dos primeros: JOYSTICK 1 y 2 del juego, ver user32.cpp joy_event)
    struct Mando { SDL_Gamepad *g = nullptr; SDL_Joystick *j = nullptr; SDL_JoystickID id = 0; int x = -1, y = -1; u32 b = 0; };
    Mando mandos[2];
} H;

// Mandos: se abren al conectarse (un mando conocido por SDL como gamepad: stick izquierdo o cruceta y A, B, X, Y;
// otro joystick: ejes 0 y 1, el primer hat y los cuatro primeros botones). El estado va al juego al cambiar.
void mandos_contar() {
    int n = 0;
    for (auto &m : H.mandos) n += m.j != nullptr;
    Display &d = display();
    std::lock_guard<std::mutex> lk(d.mx);
    d.mandos = n;
}
void mando_conectado(SDL_JoystickID id) {
    for (auto &m : H.mandos) if (m.j && m.id == id) return;
    for (auto &m : H.mandos) {
        if (m.j) continue;
        if (SDL_IsGamepad(id)) { m.g = SDL_OpenGamepad(id); m.j = m.g ? SDL_GetGamepadJoystick(m.g) : nullptr; }
        else m.j = SDL_OpenJoystick(id);
        if (!m.j) { plog("mando: %s", SDL_GetError()); m.g = nullptr; return; }
        m.id = id; m.x = m.y = -1; m.b = 0;
        plog("mando %d: %s", (int)(&m - H.mandos) + 1, m.g ? SDL_GetGamepadName(m.g) : SDL_GetJoystickName(m.j));
        break;
    }
    mandos_contar();
}
void mando_desconectado(SDL_JoystickID id) {
    for (auto &m : H.mandos) {
        if (!m.j || m.id != id) continue;
        if (m.g) SDL_CloseGamepad(m.g); else SDL_CloseJoystick(m.j);
        m = Host::Mando{};
    }
    mandos_contar();
}
void mandos_leer() {
    auto eje = [](int v) { return std::abs(v) < 8000 ? 32767 : std::clamp(v + 32768, 0, 65535); };   // zona muerta
    for (int i = 0; i < 2; i++) {
        Host::Mando &m = H.mandos[i];
        if (!m.j) continue;
        int x, y;
        u32 b = 0;
        bool izq, der, arr, aba;
        if (m.g) {
            x = eje(SDL_GetGamepadAxis(m.g, SDL_GAMEPAD_AXIS_LEFTX)); y = eje(SDL_GetGamepadAxis(m.g, SDL_GAMEPAD_AXIS_LEFTY));
            izq = SDL_GetGamepadButton(m.g, SDL_GAMEPAD_BUTTON_DPAD_LEFT); der = SDL_GetGamepadButton(m.g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            arr = SDL_GetGamepadButton(m.g, SDL_GAMEPAD_BUTTON_DPAD_UP); aba = SDL_GetGamepadButton(m.g, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
            static const SDL_GamepadButton bot[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                                     SDL_GAMEPAD_BUTTON_NORTH};
            for (int k = 0; k < 4; k++) if (SDL_GetGamepadButton(m.g, bot[k])) b |= 1u << k;
        } else {
            x = eje(SDL_GetNumJoystickAxes(m.j) > 0 ? SDL_GetJoystickAxis(m.j, 0) : 0);
            y = eje(SDL_GetNumJoystickAxes(m.j) > 1 ? SDL_GetJoystickAxis(m.j, 1) : 0);
            u8 h = SDL_GetNumJoystickHats(m.j) > 0 ? SDL_GetJoystickHat(m.j, 0) : 0;
            izq = h & SDL_HAT_LEFT; der = h & SDL_HAT_RIGHT; arr = h & SDL_HAT_UP; aba = h & SDL_HAT_DOWN;
            for (int k = 0; k < 4 && k < SDL_GetNumJoystickButtons(m.j); k++) if (SDL_GetJoystickButton(m.j, k)) b |= 1u << k;
        }
        if (izq) x = 0; else if (der) x = 65535;
        if (arr) y = 0; else if (aba) y = 65535;
        if (x == m.x && y == m.y && b == m.b) continue;
        m.x = x; m.y = y; m.b = b;
        InEvent e{InKind::Joy}; e.button = i; e.x = x; e.y = y; e.vk = b;
        display().push(e);
    }
}

// Un fotograma del hilo principal: guion de pruebas, capturas, eventos de SDL y presentación. false = parar.
bool host_frame() {
    if (g_game_done) return false;
    // latido (diagnóstico; en la web siempre, en nativo con PCF7_LATIDO): cada 5 s, fotogramas presentados, fotograma
    // del juego y funciones traducidas ejecutadas en ese tramo (para comparar el rendimiento de dos traducciones)
    {
#ifdef __EMSCRIPTEN__
        static const bool latido = true;
#else
        static const bool latido = env("PCF7_LATIDO") != nullptr;
#endif
        static u32 n = 0, next = 5000, frame0 = 0, fn0 = 0;
        n++;
        if (latido && ticks_ms() >= next) {
            u32 fr = display().screen.frame, fn = fnring_i;
            plog("latido: %u fotogramas, juego en el %u (+%u), %u funciones", n, fr, fr - frame0, fn - fn0);
            next += 5000; frame0 = fr; fn0 = fn;
        }
    }
#ifdef __EMSCRIPTEN__
    web_persistir();   // partidas: lo que el juego ha escrito, a la página (web.cpp)
#endif
    u32 now = ticks_ms();
    if (H.win) { static bool reclamado = false; if (!reclamado && now > 400) { reclamado = true; SDL_RaiseWindow(H.win); } }   // el foco, ya con la app registrada
    if (H.timeout_ms && now > H.timeout_ms && !H.quit_at) { plog("tiempo agotado; el juego estaba en:"); dump_ring(); threads_dump(); d3d_stats(); if (env("PCF7_HASHES")) mem_hashes(); if (env("PCF7_MEMFIND")) mem_find(env("PCF7_MEMFIND"));
            if (const char *r = env("PCF7_DUMPMEM")) { u32 a = 0, n = 0; std::sscanf(r, "%x:%x", &a, &n); mem_dump(a, n); } display().push({InKind::Quit}); H.quit_at = now; }
    if (H.quit_at && now - H.quit_at > 3000) { plog("el juego no se cierra: saliendo"); return false; }
    video_tick();
    while (H.script_i < H.script.size() && H.script[H.script_i].at <= now) {
        const ScriptStep &st = H.script[H.script_i];
        if (st.op == "wait") {
            if (!pixel_is(st.a, st.b, st.c)) break;
            plog("guion: wait %d %d %06x cumplido a %u ms", st.a, st.b, st.c, now);
            for (size_t k = H.script_i + 1; k < H.script.size(); k++) H.script[k].at += now - st.at;
            H.script_i++;
            continue;
        }
        if (st.op == "ifpx") {
            bool ok = pixel_is(st.a, st.b, st.c);
            plog("guion: ifpx %d %d %06x -> %s", st.a, st.b, st.c, ok ? "sí" : "no");
            H.script_i += ok ? 1 : 2;
            continue;
        }
        run_script_step(H.script[H.script_i++]);
    }
    if (H.shot && H.shot_ms && now >= H.next_shot) { save_shot(std::string(H.shot) + std::to_string(H.next_shot) + ".bmp"); H.next_shot += H.shot_ms; }

    if (H.headless) { SDL_Delay(10); return true; }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        Display &d = display();
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                d.push({InKind::Quit});
                if (!H.quit_at) H.quit_at = now;
                break;
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                float fx = ev.type == SDL_EVENT_MOUSE_MOTION ? ev.motion.x : ev.button.x;
                float fy = ev.type == SDL_EVENT_MOUSE_MOTION ? ev.motion.y : ev.button.y;
                float lx, ly;
                SDL_RenderCoordinatesFromWindow(H.ren, fx, fy, &lx, &ly);
                InEvent e{ev.type == SDL_EVENT_MOUSE_MOTION ? InKind::MouseMove
                          : ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? InKind::MouseDown : InKind::MouseUp};
                e.x = std::max(0, std::min(H.tex_w - 1, (int)lx));
                e.y = std::max(0, std::min(H.tex_h - 1, (int)ly));
                // en las bandas negras (fuera de la pantalla del juego) se ve el puntero del sistema
                bool outside = lx < 0 || ly < 0 || lx >= H.tex_w || ly >= H.tex_h;
                if (outside != H.host_cursor_shown) { H.host_cursor_shown = outside; if (outside) SDL_ShowCursor(); else SDL_HideCursor(); }
                if (ev.type != SDL_EVENT_MOUSE_MOTION) e.button = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                { std::lock_guard<std::mutex> lk(d.mx); d.mouse_x = e.x; d.mouse_y = e.y; }
                d.push(e);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                InEvent e{InKind::Wheel};
                e.delta = ev.wheel.y > 0 ? 1 : -1;
                e.x = d.mouse_x; e.y = d.mouse_y;
                d.push(e);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                // pantalla completa: F11 o Alt+Intro (no llegan al juego); la imagen mantiene su proporción (bandas negras)
#ifndef __EMSCRIPTEN__   // (en el navegador, la página tiene su botón)
                bool intro = ev.key.scancode == SDL_SCANCODE_RETURN || ev.key.scancode == SDL_SCANCODE_KP_ENTER;
                if (ev.key.scancode == SDL_SCANCODE_F11 || (intro && (ev.key.mod & SDL_KMOD_ALT))) {
                    if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat)
                        SDL_SetWindowFullscreen(H.win, !(SDL_GetWindowFlags(H.win) & SDL_WINDOW_FULLSCREEN));
                    break;
                }
#endif
                u32 vk, scan;
                bool ext;
                if (map_key(ev.key.scancode, vk, scan, ext)) key_event(ev.type == SDL_EVENT_KEY_DOWN, vk, scan, ext, ev.key.repeat);
                break;
            }
            case SDL_EVENT_JOYSTICK_ADDED: mando_conectado(ev.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: mando_desconectado(ev.jdevice.which); break;
            case SDL_EVENT_TEXT_INPUT: {
                // UTF-8 → Windows-1252
                const unsigned char *s = (const unsigned char *)ev.text.text;
                while (*s) {
                    u32 cp;
                    if (*s < 0x80) cp = *s++;
                    else if ((*s & 0xE0) == 0xC0 && s[1]) { cp = (u32)(s[0] & 0x1F) << 6 | (s[1] & 0x3F); s += 2; }
                    else if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) { cp = (u32)(s[0] & 0x0F) << 12 | (u32)(s[1] & 0x3F) << 6 | (s[2] & 0x3F); s += 3; }
                    else { s++; continue; }
                    InEvent e{InKind::Char};
                    e.vk = unicode_to_cp1252(cp);
                    d.push(e);
                }
                break;
            }
        }
    }

    mandos_leer();

    Display &d = display();
    u32 frame;
    int mx, my;
    { std::lock_guard<std::mutex> lk(d.mx); frame = d.screen.frame; mx = d.mouse_x; my = d.mouse_y; }
    if (frame != H.last_frame || mx != H.last_mx || my != H.last_my) {
        H.last_frame = frame; H.last_mx = mx; H.last_my = my;
        int w, h;
        compose(H.rgb, w, h);
        if (w != H.tex_w || h != H.tex_h) {
            if (H.tex) SDL_DestroyTexture(H.tex);
            H.tex = SDL_CreateTexture(H.ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            SDL_SetTextureScaleMode(H.tex, SDL_SCALEMODE_NEAREST);
            SDL_SetRenderLogicalPresentation(H.ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            H.tex_w = w; H.tex_h = h;
        }
        SDL_UpdateTexture(H.tex, nullptr, H.rgb.data(), w * 4);
#ifdef __EMSCRIPTEN__
        static bool listo = false;   // primera imagen del juego: la página quita el «Cargando…»
        if (!listo) { listo = true; EM_ASM({ if (Module.pcf7Listo) Module.pcf7Listo(); }); }
#endif
    }
    SDL_SetRenderDrawColor(H.ren, 0, 0, 0, 255);
    SDL_RenderClear(H.ren);
    if (H.tex) SDL_RenderTexture(H.ren, H.tex, nullptr, nullptr);
    SDL_RenderPresent(H.ren);
    return true;
}

#ifdef PCF_DLL
// la DLL de un programa: la carga «PCFSite - Source.exe --programa <nombre> [argumento]» y llama a esto
extern "C" __declspec(dllexport) int pcf_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    // una reserva imposible en el host (tamaño sacado de la memoria del juego): dónde estaba el juego
    std::set_new_handler([] { plog("sin memoria en el host (reserva imposible) en f_%x:", cur_fn); dump_ring(); std::abort(); });
#ifdef __EMSCRIPTEN__
    g_traza_paso = (u32)web_traza_js(0); g_traza_desde = (u32)web_traza_js(1); g_traza_cuantas = (u32)web_traza_js(2);   // diagnóstico (?traza=paso:desde)
    if (g_traza_paso) g_watch = 1;
    if (web_reloj_js()) g_reloj_virtual = true;
    { char *d = web_datos_js(); g_datos_url = d; std::free(d); }
#endif
    std::string exe_dir = exe_real_dir(argc > 0 ? argv[0] : nullptr);
#ifdef __EMSCRIPTEN__
    {   // llega con los datos (web.cpp): mem.bin el principal, mem-<programa>.bin los demás
        std::string t = TARGET_EXE, pr = JUEGO_PRINCIPAL;
        g_membin = t == pr ? std::string("/pcf7/ro/mem.bin") : "/pcf7/ro/mem-" + t.substr(0, t.rfind('.')) + ".bin";
    }
#else
    g_membin = env("PCF7_MEMBIN") ? env("PCF7_MEMBIN") : exe_dir + "../" PCF7_GENDIR "/mem.bin";
#endif
    for (int i = 1; i < argc; i++) g_guest_args += (i > 1 ? " " : "") + std::string(argv[i]);   // línea de órdenes del invitado
    bool headless = env("PCF7_HEADLESS") != nullptr;
    // como el acceso directo del instalador ("PCF7.EXE video"): la intro antes de entrar al juego
#ifndef __EMSCRIPTEN__
    if (JUEGO_SALIDA_MENU && !headless && !env("PCF7_RELAUNCHED") && !env("PCF7_NOINTRO") &&
        access((exe_dir + JUEGO_ID + "-playvid").c_str(), X_OK) == 0)
        exec_program(exe_dir, "playvid", "D:\\INTROPCF.MPG");
#endif
    H.headless = headless;
    H.shot = env("PCF7_SHOT");
    H.shot_ms = env("PCF7_SHOT_MS") ? (u32)std::atoi(env("PCF7_SHOT_MS")) : 0;
    H.timeout_ms = env("PCF7_TIMEOUT") ? (u32)(std::atof(env("PCF7_TIMEOUT")) * 1000) : 0;
    H.script = parse_script(env("PCF7_SCRIPT"));

    if (headless) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");   // el audio avanza a tiempo real sin altavoz
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");         // Windows: el icono del .exe (platform/win/juego.rc) en la ventana
    if (!SDL_Init(headless ? SDL_INIT_AUDIO : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) { plog("SDL_Init: %s", SDL_GetError()); return 1; }
    if (!headless) {   // los mandos ya conectados, antes de que el juego llame a joySetCapture
        int n = 0;
        if (SDL_JoystickID *ids = SDL_GetJoysticks(&n)) { for (int i = 0; i < n; i++) mando_conectado(ids[i]); SDL_free(ids); }
    }
    if (env("PCF7_MANDO_PRUEBA")) { Display &d = display(); std::lock_guard<std::mutex> lk(d.mx); d.mandos = 1; }
    if (!headless) {
        // 1280×960 (el doble de 640×480) si cabe; si no, lo más grande en 4:3 que quepa en el área útil de la pantalla
        // (portátiles de 768 px de alto, o Windows con la escala al 125-150 %: una ventana más alta salía cortada)
        int ww = 1280, wh = 960;
#ifndef __EMSCRIPTEN__
        SDL_Rect util_;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &util_)) {
            wh = std::max(480, std::min(wh, util_.h - 60));
            ww = wh * 4 / 3;
            if (ww > util_.w - 20) { ww = std::max(640, util_.w - 20); wh = ww * 3 / 4; }
        }
#endif
        bool completa = env("PCF7_PANTALLA_COMPLETA") && std::string(env("PCF7_PANTALLA_COMPLETA")) == "1";   // ajuste de la app
        if (!SDL_CreateWindowAndRenderer(JUEGO_NOMBRE, ww, wh, SDL_WINDOW_RESIZABLE | (completa ? SDL_WINDOW_FULLSCREEN : 0), &H.win, &H.ren)) {
            plog("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
            return 1;
        }
        SDL_SetWindowPosition(H.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);   // siempre en el centro de la pantalla
        SDL_RaiseWindow(H.win);   // y en primer plano (lanzado desde la app, el proceso hijo no lo tendría solo)
        SDL_SetRenderVSync(H.ren, 1);
        SDL_HideCursor();   // el puntero lo pinta el host encima de la pantalla del juego
        SDL_StartTextInput(H.win);
    }

#ifdef __EMSCRIPTEN__
    // la pila de C del hilo va en la memoria de WebAssembly: aquí solo guarda lo que no cabe en registros (las
    // llamadas anidadas usan la pila del propio navegador)
    size_t pila = 64u << 20;
#else
    size_t pila = 1024u << 20;   // 1 GB de pila de C (reservada; se compromete según se usa)
#endif
    plat_thread_t th;
    if (!plat_thread_start(&th, pila, game_thread, nullptr)) { plog("no se puede crear el hilo del juego"); return 1; }

    H.next_shot = H.shot_ms;
#ifdef __EMSCRIPTEN__
    // en el navegador el hilo principal no puede bloquearse: un fotograma por cada repintado de la página
    emscripten_set_main_loop([] { if (!host_frame()) { emscripten_cancel_main_loop(); web_game_ended(); } }, 0, false);
    return 0;
#else
    while (host_frame()) {}
#endif
    if (H.shot) save_shot(std::string(H.shot) + "final.bmp");
    if (g_game_done) plat_thread_join(th);
    if (!g_imagen_error.empty()) {   // no se ha podido construir la imagen de memoria con los ficheros del juego
        std::string m = std::string(JUEGO_NOMBRE) + " no puede arrancar.\n\n" + g_imagen_error + "\n\nDetalles en " + juego_trabajo() + "/" + JUEGO_ID + ".log";
        plog("%s", m.c_str());
        if (!headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, JUEGO_NOMBRE, m.c_str(), H.win);
        SDL_Quit();
        std::_Exit(2);
    }
    if (g_game_done && !H.quit_at && !env("PCF7_NOLAUNCHER")) {
        std::string target = TARGET_EXE;
#ifdef PCF_DOS
        std::string sig = dos_siguiente();   // FUTBOL4.EXE: el estado de COMFILE.DAT dice qué programa sigue
        if (!sig.empty()) { SDL_Quit(); exec_program(exe_dir, sig.c_str(), nullptr); }
#else
        if (!JUEGO_SALIDA_MENU && target == JUEGO_PRINCIPAL) {
            // juego sin lanzador (aún): al salir del programa principal se acaba
        } else if (target == JUEGO_PRINCIPAL) {
            u32 code = rd32(JUEGO_SALIDA_MENU);   // apartado elegido en el menú principal (0 o 1 = salir)
            Launch l = launch_for_code(code);
            if (l.exe) { SDL_Quit(); exec_program(exe_dir, l.exe, l.arg); }
            if (code >= 2) {
                plog("lanzador: apartado %u sin programa: se vuelve al menú", code);
                SDL_Quit();
                std::string principal = JUEGO_PRINCIPAL;
                principal.resize(principal.rfind('.'));
                exec_program(exe_dir, principal.c_str(), nullptr);
            }
        } else {
            // al terminar cualquier otro programa se vuelve al lanzador: ACTWIN si hay jornadas, si no MANAGER
            SDL_Quit();
            std::string principal = JUEGO_PRINCIPAL;
            principal.resize(principal.rfind('.'));
            exec_program(exe_dir, target != "actwin.exe" && pending_updates() ? "actwin" : principal.c_str(), nullptr);
        }
#endif
    }
    SDL_Quit();
    std::_Exit(g_rc);   // el hilo del juego puede seguir vivo si no ha querido cerrarse
}
