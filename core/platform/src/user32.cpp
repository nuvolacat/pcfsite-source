// PCF7 — USER32: clases y ventanas, mensajes, ganchos (los usa MFC), entrada, temporizadores, cursor,
// rectángulos y cadenas.
//
// Un solo hilo de interfaz (el del juego). Las ventanas no se componen entre sí: cada una dibuja
// directamente en la pantalla emulada a través de su DC (gdi32.cpp), como un juego a pantalla completa.
#include "cursor.h"
#include "display.h"
#include "handles.h"
#include "threads.h"
#include "ctlutil.h"
#include "win.h"

#include <algorithm>
#include <cstdarg>
#include <map>
#include <vector>

u32 ticks_ms();
static void set_pos(u32 hwnd, i32 x, i32 y, i32 cw, i32 ch, u32 flags);
static void ensure_system_classes();
void menu_bar_paint(u32 hwnd, u32 hmenu, u32 window_dc, const Rect &bar);   // menu.cpp
void menu_bar_click(u32 hwnd, u32 hmenu, const Rect &bar, i32 sx, i32 sy);

namespace {

// ---------------------------------------------------------------- clases y ventanas

struct WndClass {
    std::string name;   // en minúsculas
    u32 style = 0, wndproc = 0, cls_extra = 0, wnd_extra = 0, hinst = 0, icon = 0, cursor = 0, brush = 0, menu = 0;
    u32 atom = 0;
};

struct Wnd {
    u32 hwnd = 0;
    WndClass *cls = nullptr;
    u32 wndproc = 0, style = 0, exstyle = 0, parent = 0, owner = 0, id = 0, hinst = 0, userdata = 0;
    i32 x = 0, y = 0, w = 0, h = 0;          // relativo al padre (o a la pantalla si no tiene)
    std::string text;
    std::vector<u8> extra;
    std::map<std::string, u32> props;
    bool invalid = false, erase = false;
    Rect upd;                                // zona inválida (coordenadas cliente)
    bool nc_dirty = true;                    // falta pintar el marco y el título
    bool destroyed = false;
    bool no_nc = false;                      // ventana de DirectDraw a pantalla completa: sin marco
    u32 menu = 0;                            // barra de menú (HMENU) de una ventana de nivel superior
    std::vector<u32> children;               // en orden Z: el último está encima
};

std::map<std::string, WndClass> g_classes;
std::map<u32, Wnd> g_wnds;
std::vector<u32> g_top;                      // ventanas de nivel superior en orden Z
u32 g_next_hwnd = 0x10010;
u32 g_atom = 0xC100;

u32 g_focus = 0, g_active = 0, g_capture = 0;
struct Drag { u32 hwnd = 0; i32 dx = 0, dy = 0; } g_drag;   // mover una ventana por el título

constexpr u32 DESKTOP = 0x10000;

std::string lc(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }

Wnd *wnd(u32 h) {
    auto it = g_wnds.find(h);
    return it == g_wnds.end() || it->second.destroyed ? nullptr : &it->second;
}

// nombre de clase: cadena o átomo
std::string class_key(u32 p) {
    if (p < 0x10000) {
        for (auto &c : g_classes) if (c.second.atom == p) return c.first;
        return "#" + std::to_string(p);
    }
    return lc(gstr(p));
}

// ---------------------------------------------------------------- mensajes

struct Msg { u32 hwnd, msg, wp, lp, time; i32 x, y; };
std::deque<Msg> g_queue;
bool g_quit = false;
u32 g_quit_code = 0;
Msg g_last{};

struct Timer { u32 hwnd, id, period, proc, next; };
std::vector<Timer> g_timers;

struct Hook { u32 id, type, proc; };
std::vector<Hook> g_hooks;
u32 g_next_hook = 0x5000;

constexpr u32 WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_MOVE = 0x03, WM_SIZE = 0x05, WM_ACTIVATE = 0x06,
              WM_SETFOCUS = 0x07, WM_KILLFOCUS = 0x08, WM_ENABLE = 0x0A, WM_SETTEXT = 0x0C, WM_GETTEXT = 0x0D,
              WM_GETTEXTLENGTH = 0x0E, WM_PAINT = 0x0F, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14,
              WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20, WM_MOUSEACTIVATE = 0x21,
              WM_GETMINMAXINFO = 0x24, WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47,
              WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83, WM_NCHITTEST = 0x84,
              WM_NCACTIVATE = 0x86, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104,
              WM_SYSKEYUP = 0x105, WM_SYSCOMMAND = 0x112, WM_TIMER = 0x113, WM_MOUSEMOVE = 0x200,
              WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203, WM_RBUTTONDOWN = 0x204,
              WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208, WM_MOUSEWHEEL = 0x20A,
              WM_PARENTNOTIFY = 0x210;

u32 send(u32 hwnd, u32 msg, u32 wp, u32 lp) {
    Wnd *w = wnd(hwnd);
    if (!w || !w->wndproc) return 0;
    return guest_call(w->wndproc, {hwnd, msg, wp, lp});
}

void post(u32 hwnd, u32 msg, u32 wp, u32 lp) {
    g_queue.push_back({hwnd, msg, wp, lp, ticks_ms(), display().mouse_x, display().mouse_y});
}

void call_hooks(u32 type, u32 code, u32 wp, u32 lp) {
    for (auto it = g_hooks.rbegin(); it != g_hooks.rend(); ++it)
        if (it->type == type) { guest_call(it->proc, {code, wp, lp}); return; }   // el gancho llama a CallNextHookEx si quiere
}

// ---------------------------------------------------------------- área no cliente (marcos, título, menú)
//
// Métricas de Windows 98: marco redimensionable 4, de diálogo 3, borde 1, borde de cliente 3D 2, título 18
// (+1 de línea: SM_CYCAPTION = 19) y barra de menú 19.
constexpr i32 CAPTION_H = 19, MENU_H = 19;
void nc_insets(const Wnd &w, i32 &l, i32 &t, i32 &r, i32 &b) {
    l = t = r = b = 0;
    if (w.no_nc) return;
    u32 st = w.style, ex = w.exstyle;
    i32 bd = 0;
    if (st & 0x40000) bd = 4;                                                   // WS_THICKFRAME
    else if ((st & 0xC00000) == 0xC00000 || (st & 0x400000) || (ex & 1)) bd = 3;   // WS_CAPTION / WS_DLGFRAME / WS_EX_DLGMODALFRAME
    else if (st & 0x800000) bd = 1;                                             // WS_BORDER
    if (ex & 0x200) bd += 2;                                                    // WS_EX_CLIENTEDGE
    if (ex & 0x20000) bd += 1;                                                  // WS_EX_STATICEDGE
    l = t = r = b = bd;
    if ((st & 0xC00000) == 0xC00000) t += (ex & 0x80) ? 15 : CAPTION_H;         // WS_CAPTION (WS_EX_TOOLWINDOW: pequeño)
    if (w.menu && !(st & 0x40000000u)) t += MENU_H;
    if (st & 0x200000) r += 16;                                                 // WS_VSCROLL
    if (st & 0x100000) b += 16;                                                 // WS_HSCROLL
}

// esquina de la ventana en pantalla (las hijas van relativas al área cliente del padre)
void window_origin(u32 hwnd, i32 &ox, i32 &oy);
void client_origin(u32 hwnd, i32 &ox, i32 &oy) {
    window_origin(hwnd, ox, oy);
    if (Wnd *w = wnd(hwnd)) { i32 l, t, r, b; nc_insets(*w, l, t, r, b); ox += l; oy += t; }
}
void window_origin(u32 hwnd, i32 &ox, i32 &oy) {
    ox = oy = 0;
    Wnd *w = wnd(hwnd);
    if (!w) return;
    if (w->style & 0x40000000u) client_origin(w->parent, ox, oy);   // WS_CHILD
    ox += w->x; oy += w->y;
}
// origen del área cliente en pantalla (coordenadas de los mensajes de ratón, ClientToScreen…)
bool screen_origin(u32 hwnd, i32 &ox, i32 &oy) {
    client_origin(hwnd, ox, oy);
    return wnd(hwnd) != nullptr;
}

// ---------------------------------------------------------------- qué ventana se ve en cada píxel
//
// Se rehace cuando cambian ventanas (crear, destruir, mostrar, mover, orden Z). Las hijas tapan al padre
// dentro de su área cliente; las de nivel superior, por orden Z. Los DC de ventana solo pintan donde se
// ve su ventana (o sus hijas, si la ventana no tiene WS_CLIPCHILDREN): así una ventana no pinta encima
// de otra que la tapa, como en Windows.
std::vector<u32> g_owner;
int g_ow = 0, g_oh = 0;
bool g_owner_dirty = true;
void owners_changed() { g_owner_dirty = true; }
bool visible_chain(u32 hwnd);
void paint_owner(u32 hwnd, Rect clip) {
    Wnd *w = wnd(hwnd);
    if (!w || !(w->style & 0x10000000u)) return;
    i32 ox, oy;
    window_origin(hwnd, ox, oy);
    Rect wr = rect_and({ox, oy, ox + w->w, oy + w->h}, clip);
    if (rect_empty(wr)) return;
    for (i32 y = wr.t; y < wr.b; y++) std::fill(g_owner.begin() + (long)y * g_ow + wr.l, g_owner.begin() + (long)y * g_ow + wr.r, hwnd);
    i32 l, t, r, b;
    nc_insets(*w, l, t, r, b);
    Rect cr = rect_and({ox + l, oy + t, ox + w->w - r, oy + w->h - b}, wr);
    std::vector<u32> kids = w->children;
    for (u32 c : kids) paint_owner(c, cr);
}
void rebuild_owners() {
    Screen &sc = display().screen;
    g_ow = sc.w; g_oh = sc.h;
    g_owner.assign((size_t)g_ow * g_oh, 0);
    for (u32 t : g_top) paint_owner(t, {0, 0, g_ow, g_oh});
    g_owner_dirty = false;
}

// ---------------------------------------------------------------- zonas inválidas y repintado

// invalida una zona del área cliente (nullptr = toda) y, si se pide, el marco
void invalidate(u32 hwnd, const Rect *r, bool erase, bool nc = false) {
    Wnd *w = wnd(hwnd);
    if (!w) return;
    i32 l, t, rr, b;
    nc_insets(*w, l, t, rr, b);
    Rect all{0, 0, std::max(0, w->w - l - rr), std::max(0, w->h - t - b)};
    Rect z = r ? rect_and(*r, all) : all;
    if (!rect_empty(z)) { w->upd = rect_or(w->upd, z); w->invalid = true; if (erase) w->erase = true; }
    if (nc) w->nc_dirty = true;
}
// lo que había debajo de un rectángulo de pantalla queda al descubierto: se repinta lo que se vea ahí
void expose(const Rect &screen) {
    if (rect_empty(screen)) return;
    for (auto &kv : g_wnds) {
        Wnd &w = kv.second;
        if (w.destroyed || !visible_chain(w.hwnd)) continue;
        Rect wr;
        i32 ox, oy;
        window_origin(w.hwnd, ox, oy);
        wr = {ox, oy, ox + w.w, oy + w.h};
        Rect hit = rect_and(wr, screen);
        if (rect_empty(hit)) continue;
        i32 cx, cy;
        client_origin(w.hwnd, cx, cy);
        Rect local{hit.l - cx, hit.t - cy, hit.r - cx, hit.b - cy};
        invalidate(w.hwnd, &local, true, true);
    }
}
Rect window_screen_rect(u32 hwnd) {
    Wnd *w = wnd(hwnd);
    if (!w) return {};
    i32 ox, oy;
    window_origin(hwnd, ox, oy);
    return {ox, oy, ox + w->w, oy + w->h};
}

bool visible_chain(u32 hwnd) {
    for (Wnd *w = wnd(hwnd); w; w = (w->style & 0x40000000u) ? wnd(w->parent) : nullptr)
        if (!(w->style & 0x10000000u)) return false;   // WS_VISIBLE
    return wnd(hwnd) != nullptr;
}

// ventana (la más profunda) bajo un punto de pantalla
u32 hit_child(u32 hwnd, i32 x, i32 y) {
    Wnd *w = wnd(hwnd);
    if (!w) return 0;
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) {
        Wnd *c = wnd(*it);
        if (!c || !(c->style & 0x10000000u) || (c->style & 0x08000000u)) continue;   // invisible / WS_DISABLED
        Rect r;
        wnd_screen_rect(*it, false, r);
        if (x >= r.l && x < r.r && y >= r.t && y < r.b) return hit_child(*it, x, y);
    }
    return hwnd;
}
u32 window_from_point(i32 x, i32 y) {
    for (auto it = g_top.rbegin(); it != g_top.rend(); ++it) {
        Wnd *w = wnd(*it);
        if (!w || !(w->style & 0x10000000u)) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return hit_child(*it, x, y);
    }
    return 0;
}

void set_focus(u32 h) {
    if (h == g_focus) return;
    u32 old = g_focus;
    g_focus = h;
    if (old) send(old, WM_KILLFOCUS, h, 0);
    if (h) send(h, WM_SETFOCUS, old, 0);
}

void send_size_move(u32 hwnd) {
    Wnd *w = wnd(hwnd);
    if (!w) return;
    send(hwnd, WM_SIZE, 0, ((u32)(u16)w->h << 16) | (u16)w->w);
    w = wnd(hwnd);
    if (w) send(hwnd, WM_MOVE, 0, ((u32)(u16)w->y << 16) | (u16)w->x);
}

void destroy_window(u32 hwnd) {
    Wnd *w = wnd(hwnd);
    if (!w) return;
    Rect gone = visible_chain(hwnd) ? window_screen_rect(hwnd) : Rect{};
    std::vector<u32> kids = w->children;
    send(hwnd, WM_DESTROY, 0, 0);
    for (u32 c : kids) destroy_window(c);
    send(hwnd, WM_NCDESTROY, 0, 0);
    w = wnd(hwnd);
    if (!w) return;
    if (Wnd *p = wnd(w->parent)) p->children.erase(std::remove(p->children.begin(), p->children.end(), hwnd), p->children.end());
    g_top.erase(std::remove(g_top.begin(), g_top.end(), hwnd), g_top.end());
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](const Timer &t) { return t.hwnd == hwnd; }), g_timers.end());
    w->destroyed = true;
    owners_changed();
    expose(gone);
    if (g_focus == hwnd) g_focus = 0;
    if (g_active == hwnd) g_active = 0;
    if (g_capture == hwnd) g_capture = 0;
}

// activa una ventana de nivel superior: arriba del todo, WM_ACTIVATE a la anterior y a ella
void activate_window(u32 hwnd) {
    if (hwnd == g_active) return;
    u32 old = g_active;
    g_active = hwnd;
    if (Wnd *o = wnd(old)) { o->nc_dirty = true; send(old, WM_NCACTIVATE, 0, 0); send(old, WM_ACTIVATE, 0, hwnd); }
    if (Wnd *w = wnd(hwnd)) {
        g_top.erase(std::remove(g_top.begin(), g_top.end(), hwnd), g_top.end());
        g_top.push_back(hwnd);
        owners_changed();
        expose(window_screen_rect(hwnd));
        (void)w;
        send(hwnd, WM_NCACTIVATE, 1, 0);
        send(hwnd, WM_ACTIVATE, 1, old);
    }
}

// ---------------------------------------------------------------- entrada del host → mensajes

u32 g_mouse_buttons = 0;   // MK_LBUTTON=1, MK_RBUTTON=2, MK_MBUTTON=0x10

void pump_input(bool wait, u32 timeout_ms) {
    Display &d = display();
    if (wait && other_threads()) {   // no esperar con el testigo: que corran los demás hilos
        cooperative_sleep(std::min<u32>(timeout_ms, 2));
        wait = false;
    }
    std::unique_lock<std::mutex> lk(d.mx);
    if (wait && d.input.empty())
        d.input_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !d.input.empty(); });
    std::deque<InEvent> evs;
    evs.swap(d.input);
    lk.unlock();
    sb_track_event(2, d.mouse_x, d.mouse_y);   // repetición de las flechas de una barra mantenidas

    for (auto &e : evs) {
        switch (e.kind) {
            case InKind::Quit:
                for (u32 h : g_top) post(h, WM_CLOSE, 0, 0);
                break;
            case InKind::MouseMove:
            case InKind::MouseDown:
            case InKind::MouseUp:
            case InKind::Wheel: {
                d.mouse_x = e.x; d.mouse_y = e.y;
                // siguiendo una barra de desplazamiento
                if (e.kind != InKind::Wheel && sb_track_event(e.kind == InKind::MouseUp ? 1 : e.kind == InKind::MouseMove ? 0 : 3, e.x, e.y)) break;
                // arrastrando una ventana por el título
                if (g_drag.hwnd) {
                    if (e.kind == InKind::MouseMove) set_pos(g_drag.hwnd, e.x - g_drag.dx, e.y - g_drag.dy, 0, 0, 1 | 4);   // SWP_NOSIZE | SWP_NOZORDER
                    if (e.kind == InKind::MouseUp) g_drag.hwnd = 0;
                    break;
                }
                u32 target = g_capture ? g_capture : window_from_point(e.x, e.y);
                if (!target) break;
                // al pulsar se activa la ventana de nivel superior
                if (e.kind == InKind::MouseDown && !g_capture) {
                    u32 top = target;
                    while (Wnd *tw = wnd(top)) { if (!(tw->style & 0x40000000u)) break; top = tw->parent; }
                    if (top && top != g_active && wnd(top) && !(wnd(top)->style & 0x08000000u)) activate_window(top);
                }
                // fuera del área cliente: mensajes WM_NC* con la zona (WM_NCHITTEST) y coordenadas de pantalla
                if (!g_capture && e.kind != InKind::Wheel) {
                    u32 hit = send(target, WM_NCHITTEST, 0, ((u32)(u16)e.y << 16) | (u16)e.x);
                    // HTTRANSPARENT (estáticos, marcos de grupo): el ratón es de la ventana de debajo, el padre
                    while (hit == 0xFFFFFFFFu && wnd(target) && (wnd(target)->style & 0x40000000u) && wnd(wnd(target)->parent)) {
                        target = wnd(target)->parent;
                        hit = send(target, WM_NCHITTEST, 0, ((u32)(u16)e.y << 16) | (u16)e.x);
                    }
                    if (hit != 1) {
                        u32 ncmsg = e.kind == InKind::MouseMove ? 0xA0 : e.kind == InKind::MouseDown ? (e.button == 0 ? 0xA1 : e.button == 1 ? 0xA4 : 0xA7)
                                                                                                    : (e.button == 0 ? 0xA2 : e.button == 1 ? 0xA5 : 0xA8);
                        if (e.kind == InKind::MouseDown) g_mouse_buttons |= e.button == 0 ? 1 : e.button == 1 ? 2 : 0x10;
                        if (e.kind == InKind::MouseUp) g_mouse_buttons &= ~(u32)(e.button == 0 ? 1 : e.button == 1 ? 2 : 0x10);
                        if (hit) post(target, ncmsg, hit, ((u32)(u16)e.y << 16) | (u16)e.x);
                        break;
                    }
                    if (e.kind == InKind::MouseMove || e.kind == InKind::MouseDown) send(target, WM_SETCURSOR, target, (e.kind == InKind::MouseMove ? WM_MOUSEMOVE : WM_LBUTTONDOWN) << 16 | 1);
                }
                static const u32 bit[3] = {1, 2, 0x10};
                u32 msg = WM_MOUSEMOVE;
                if (e.kind == InKind::MouseDown) {
                    g_mouse_buttons |= bit[e.button];
                    msg = e.button == 0 ? WM_LBUTTONDOWN : e.button == 1 ? WM_RBUTTONDOWN : WM_MBUTTONDOWN;
                    // doble clic como Windows: misma ventana con CS_DBLCLKS, mismo botón, < 500 ms y ±4 píxeles
                    static u32 last_t = 0, last_w = 0;
                    static int last_b = -1, last_x = 0, last_y = 0;
                    u32 now = ticks_ms();
                    Wnd *tw = wnd(target);
                    bool dbl = tw && tw->cls && (tw->cls->style & 8) && last_w == target && last_b == e.button &&
                               now - last_t < 500 && std::abs(e.x - last_x) <= 4 && std::abs(e.y - last_y) <= 4;
                    if (dbl) { msg += 2; last_b = -1; }   // WM_xBUTTONDOWN + 2 = WM_xBUTTONDBLCLK
                    else { last_t = now; last_w = target; last_b = e.button; last_x = e.x; last_y = e.y; }
                } else if (e.kind == InKind::MouseUp) {
                    g_mouse_buttons &= ~bit[e.button];
                    msg = e.button == 0 ? WM_LBUTTONUP : e.button == 1 ? WM_RBUTTONUP : WM_MBUTTONUP;
                } else if (e.kind == InKind::Wheel) {
                    post(g_focus ? g_focus : target, WM_MOUSEWHEEL, ((u32)(u16)(i16)(e.delta * 120) << 16) | g_mouse_buttons,
                         ((u32)(u16)e.y << 16) | (u16)e.x);
                    break;
                }
                i32 ox, oy;
                screen_origin(target, ox, oy);
                post(target, msg, g_mouse_buttons, ((u32)(u16)(e.y - oy) << 16) | (u16)(e.x - ox));
                break;
            }
            case InKind::KeyDown:
            case InKind::KeyUp: {
                u32 target = g_focus ? g_focus : g_active;
                static bool tr = std::getenv("PCF7_TRACE_KEYS") != nullptr;
                if (tr) plog("tecla %s vk 0x%x -> ventana 0x%x (foco 0x%x, activa 0x%x)", e.kind == InKind::KeyDown ? "abajo" : "arriba", e.vk, target, g_focus, g_active);
                if (!target) break;
                u32 lp = 1 | (e.scan << 16) | (e.ext ? 0x01000000u : 0);
                if (e.kind == InKind::KeyUp) lp |= 0xC0000000u;
                else if (e.repeat) lp |= 0x40000000u;
                bool alt = d.keys[0x12];
                u32 msg = e.kind == InKind::KeyDown ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP);
                post(target, msg, e.vk, lp);
                break;
            }
            case InKind::Char: {
                u32 target = g_focus ? g_focus : g_active;
                if (target) post(target, WM_CHAR, e.vk, 1);
                break;
            }
        }
    }
}

// mensaje sintetizado si la cola está vacía: temporizadores y luego WM_PAINT
bool synth(Msg &m, u32 filter_hwnd) {
    u32 now = ticks_ms();
    for (auto &t : g_timers) {
        if ((i32)(now - t.next) >= 0 && (!filter_hwnd || t.hwnd == filter_hwnd)) {
            t.next = now + (t.period ? t.period : 1);
            m = {t.hwnd, WM_TIMER, t.id, t.proc, now, display().mouse_x, display().mouse_y};
            return true;
        }
    }
    // primero los marcos (WM_NCPAINT se envía directamente, como hace Windows antes de WM_PAINT)
    std::vector<u32> nc;
    for (auto &kv : g_wnds) {
        Wnd &w = kv.second;
        if (!w.destroyed && w.nc_dirty && (!filter_hwnd || w.hwnd == filter_hwnd) && visible_chain(w.hwnd)) nc.push_back(w.hwnd);
    }
    for (u32 h : nc) {
        Wnd *w = wnd(h);
        if (!w) continue;
        w->nc_dirty = false;
        i32 l, t, r, b;
        nc_insets(*w, l, t, r, b);
        if (l || t || r || b) send(h, 0x85, 1, 0);   // WM_NCPAINT
    }
    // padres antes que hijas (std::map ordena por HWND, que crece con la creación)
    for (auto &kv : g_wnds) {
        Wnd &w = kv.second;
        if (w.destroyed || !w.invalid || (filter_hwnd && w.hwnd != filter_hwnd)) continue;
        if (!visible_chain(w.hwnd)) { w.invalid = false; w.upd = {}; continue; }
        m = {w.hwnd, WM_PAINT, 0, 0, now, display().mouse_x, display().mouse_y};
        return true;
    }
    return false;
}

u32 next_timer_wait() {
    u32 now = ticks_ms(), best = 50;
    for (auto &t : g_timers) { i32 d = (i32)(t.next - now); if (d < 0) d = 0; if ((u32)d < best) best = (u32)d; }
    return best;
}

bool get_msg(Msg &out, u32 hwnd, u32 fmin, u32 fmax, bool remove) {
    pump_input(false, 0);
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        if (hwnd && it->hwnd != hwnd) continue;
        if ((fmin || fmax) && (it->msg < fmin || it->msg > fmax)) continue;
        out = *it;
        if (remove) g_queue.erase(it);
        return true;
    }
    if (g_quit) {
        out = {0, WM_QUIT, g_quit_code, 0, ticks_ms(), 0, 0};
        if (remove) g_quit = false;
        return true;
    }
    return synth(out, hwnd);
}

void write_msg(u32 p, const Msg &m) {
    wr32(p, m.hwnd); wr32(p + 4, m.msg); wr32(p + 8, m.wp); wr32(p + 12, m.lp);
    wr32(p + 16, m.time); wr32(p + 20, (u32)m.x); wr32(p + 24, (u32)m.y);
}

}  // namespace

// ---------------------------------------------------------------- para ddraw: ventana a pantalla completa
//
// SetCooperativeLevel(DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) cubre la pantalla con la ventana (como el
// DirectDraw de Windows); el juego la crea con título y marco, pero ya no se ven.
void user32_fullscreen_window(u32 hwnd) {
    Wnd *w = wnd(hwnd);
    if (!w || (w->style & 0x40000000u)) return;
    w->no_nc = true;
    w->x = 0; w->y = 0;
    w->w = display().screen.w; w->h = display().screen.h;
    owners_changed();
}

// ---------------------------------------------------------------- para gdi32

const u32 *wm_owner_map(int &w, int &h) {
    if (g_owner_dirty || g_ow != display().screen.w || g_oh != display().screen.h) rebuild_owners();
    w = g_ow; h = g_oh;
    return g_owner.data();
}
static void add_descendants(u32 hwnd, std::vector<u32> &out) {
    Wnd *w = wnd(hwnd);
    if (!w) return;
    for (u32 c : w->children) { out.push_back(c); add_descendants(c, out); }
}
void wm_dc_owners(u32 hwnd, std::vector<u32> &out) {
    out.clear();
    Wnd *w = wnd(hwnd);
    if (!w) return;
    out.push_back(hwnd);
    if (!(w->style & 0x02000000u)) add_descendants(hwnd, out);   // sin WS_CLIPCHILDREN: también pinta sobre las hijas
}

bool wnd_screen_rect(u32 hwnd, bool client, Rect &out) {
    if (!hwnd || hwnd == DESKTOP) { Screen &s = display().screen; out = {0, 0, s.w, s.h}; return true; }
    Wnd *w = wnd(hwnd);
    if (!w) return false;
    i32 ox, oy;
    window_origin(hwnd, ox, oy);
    out = {ox, oy, ox + w->w, oy + w->h};
    if (client) { i32 l, t, r, b; nc_insets(*w, l, t, r, b); out = {ox + l, oy + t, std::max(ox + l, ox + w->w - r), std::max(oy + t, oy + w->h - b)}; }
    return true;
}

u32 sys_color(int i) {
    static const u32 C[] = {0xC0C0C0, 0x808000, 0x800000, 0x808080, 0xC0C0C0, 0xFFFFFF, 0x000000, 0x000000,
                            0x000000, 0xFFFFFF, 0xC0C0C0, 0xC0C0C0, 0x808080, 0x800000, 0xFFFFFF, 0xC0C0C0,
                            0x808080, 0x808080, 0x000000, 0xC0C0C0, 0xFFFFFF, 0x000000, 0xDFDFDF, 0x000000,
                            0xE1FFFF, 0xB5B5B5, 0x800000, 0xD08410, 0xB5B5B5};   // … INFOBK, —, HOTLIGHT, GRADIENT(IN)ACTIVECAPTION
    return i >= 0 && i < (int)(sizeof C / sizeof C[0]) ? C[i] : 0;   // en COLORREF (0x00BBGGRR)
}

// ---------------------------------------------------------------- clases

static u32 register_class(u32 wc, bool ex) {
    u32 o = ex ? 4 : 0;   // WNDCLASSEXA tiene cbSize delante
    WndClass c;
    c.style = rd32(wc + o); c.wndproc = rd32(wc + o + 4); c.cls_extra = rd32(wc + o + 8); c.wnd_extra = rd32(wc + o + 12);
    c.hinst = rd32(wc + o + 16); c.icon = rd32(wc + o + 20); c.cursor = rd32(wc + o + 24); c.brush = rd32(wc + o + 28);
    c.menu = rd32(wc + o + 32);
    u32 name = rd32(wc + o + 36);
    c.name = name < 0x10000 ? "#" + std::to_string(name) : lc(gstr(name));
    if (g_classes.count(c.name)) { set_last_error(1410); return 0; }   // ERROR_CLASS_ALREADY_EXISTS
    c.atom = g_atom++;
    u32 a = c.atom;
    g_classes[c.name] = c;
    return a;
}
STUB(ext_user32_dll_RegisterClassA) { RET(register_class(arg(0), false), 1); }
STUB(ext_user32_dll_UnregisterClassA) { RET(g_classes.erase(class_key(arg(0))) ? 1 : 0, 2); }
STUB(ext_user32_dll_GetClassInfoA) {
    ensure_system_classes();
    auto it = g_classes.find(class_key(arg(1)));
    if (it == g_classes.end()) { set_last_error(1411); RET(0, 3); }   // ERROR_CLASS_DOES_NOT_EXIST
    const WndClass &c = it->second;
    u32 p = arg(2);
    wr32(p, c.style); wr32(p + 4, c.wndproc); wr32(p + 8, c.cls_extra); wr32(p + 12, c.wnd_extra);
    wr32(p + 16, c.hinst); wr32(p + 20, c.icon); wr32(p + 24, c.cursor); wr32(p + 28, c.brush);
    wr32(p + 32, c.menu); wr32(p + 36, arg(1));
    RET(c.atom, 3);
}
STUB(ext_user32_dll_RegisterClassExA) { RET(register_class(arg(0), true), 1); }
STUB(ext_user32_dll_GetClassInfoExA) {
    ensure_system_classes();
    auto it = g_classes.find(class_key(arg(1)));
    if (it == g_classes.end()) { set_last_error(1411); RET(0, 3); }
    const WndClass &c = it->second;
    u32 p = arg(2);   // WNDCLASSEXA: cbSize delante y hIconSm detrás
    wr32(p + 4, c.style); wr32(p + 8, c.wndproc); wr32(p + 12, c.cls_extra); wr32(p + 16, c.wnd_extra);
    wr32(p + 20, c.hinst); wr32(p + 24, c.icon); wr32(p + 28, c.cursor); wr32(p + 32, c.brush);
    wr32(p + 36, c.menu); wr32(p + 40, arg(1)); wr32(p + 44, c.icon);
    RET(c.atom, 3);
}
STUB(ext_user32_dll_GetClassLongA) {
    Wnd *w = wnd(arg(0));
    i32 i = (i32)arg(1);
    if (!w || !w->cls) RET(0, 2);
    switch (i) {
        case -24: RET(w->cls->wndproc, 2);   // GCL_WNDPROC
        case -26: RET(w->cls->style, 2);     // GCL_STYLE
        case -12: RET(w->cls->cursor, 2);    // GCL_HCURSOR
        case -14: RET(w->cls->icon, 2);      // GCL_HICON
        case -10: RET(w->cls->brush, 2);     // GCL_HBRBACKGROUND
        case -32: RET(w->cls->atom, 2);      // GCW_ATOM
    }
    RET(0, 2);
}
STUB(ext_user32_dll_GetClassNameA) {
    Wnd *w = wnd(arg(0));
    RET(w && w->cls ? gstrcpy(arg(1), w->cls->name, arg(2)) : 0, 3);
}

// ---------------------------------------------------------------- crear y destruir

// Clases predefinidas de Windows que usan los programas (la ventana contenedora del vídeo es un
// "STATIC"): se registran la primera vez con DefWindowProc como procedimiento.
extern "C" void ext_user32_dll_DefWindowProcA(void);
u32 ctl_proc(const std::string &cls);   // controls.cpp
static void ensure_system_classes() {
    static bool done = false;
    if (done) return;
    done = true;
    for (const char *n : {"static", "button", "edit", "listbox", "combobox", "combolbox", "scrollbar", "msctls_statusbar32", "#32768"}) {
        if (g_classes.count(n)) continue;
        WndClass c;
        c.name = n;
        c.style = 0x8 | 0x80;   // CS_DBLCLKS | CS_PARENTDC
        c.wndproc = ctl_proc(n);
        c.cursor = std::string(n) == "edit" ? 0x37F01u : 0x37F00u;   // IDC_IBEAM / IDC_ARROW
        c.atom = g_atom++;
        g_classes[n] = c;
    }
}

STUB(ext_user32_dll_CreateWindowExA) {
    ensure_system_classes();
    u32 exstyle = arg(0), clsname = arg(1), title = arg(2), style = arg(3);
    i32 x = (i32)arg(4), y = (i32)arg(5), cw = (i32)arg(6), ch = (i32)arg(7);
    u32 parent = arg(8), menu = arg(9), hinst = arg(10), param = arg(11);
    std::string key = class_key(clsname);
    auto it = g_classes.find(key);
    if (it == g_classes.end()) {
        plog("CreateWindowExA: clase \"%s\" no registrada", key.c_str());
        set_last_error(1407);
        RET(0, 12);
    }
    if (x == (i32)0x80000000) { x = 0; y = 0; }                  // CW_USEDEFAULT
    if (cw == (i32)0x80000000) { cw = display().screen.w; ch = display().screen.h; }

    u32 hwnd = g_next_hwnd;
    g_next_hwnd += 4;
    Wnd &w = g_wnds[hwnd];
    w.hwnd = hwnd; w.cls = &it->second; w.wndproc = it->second.wndproc; w.style = style; w.exstyle = exstyle;
    w.x = x; w.y = y; w.w = cw; w.h = ch; w.hinst = hinst;
    if (parent == DESKTOP) parent = 0;
    if (style & 0x40000000u) { w.parent = parent; w.id = menu; } else { w.owner = parent; w.menu = menu; }
    w.extra.assign(it->second.wnd_extra, 0);
    if (title) w.text = gstr(title);
    if (w.parent) { if (Wnd *p = wnd(w.parent)) p->children.push_back(hwnd); }
    else g_top.push_back(hwnd);

    // CREATESTRUCTA en la pila del invitado (por debajo de esp)
    u32 cs = heap_alloc(48 + 8, true);
    wr32(cs, param); wr32(cs + 4, hinst); wr32(cs + 8, menu); wr32(cs + 12, parent); wr32(cs + 16, (u32)ch);
    wr32(cs + 20, (u32)cw); wr32(cs + 24, (u32)y); wr32(cs + 28, (u32)x); wr32(cs + 32, style);
    wr32(cs + 36, title); wr32(cs + 40, clsname); wr32(cs + 44, exstyle);
    u32 cbt = cs + 48;
    wr32(cbt, cs); wr32(cbt + 4, 0);
    call_hooks(5, 3, hwnd, cbt);                                   // WH_CBT, HCBT_CREATEWND (MFC subclasifica aquí)

    u32 ok = send(hwnd, WM_NCCREATE, 0, cs);
    if (ok) ok = send(hwnd, WM_CREATE, 0, cs) != 0xFFFFFFFFu;
    heap_free(cs);
    if (!ok) { destroy_window(hwnd); plog("CreateWindowExA(%s): WM_CREATE ha fallado", key.c_str()); RET(0, 12); }
    if (!wnd(hwnd)) RET(0, 12);
    send_size_move(hwnd);
    if (Wnd *p = wnd(wnd(hwnd)->parent); p && (style & 0x40000000u) && !(exstyle & 4))   // WS_EX_NOPARENTNOTIFY
        send(p->hwnd, WM_PARENTNOTIFY, WM_CREATE | (menu << 16), hwnd);
    if (Wnd *nw = wnd(hwnd); nw && (nw->style & 0x10000000u)) {
        invalidate(hwnd, nullptr, true, true);
        owners_changed();
        send(hwnd, WM_SHOWWINDOW, 1, 0);
    }
    plog("CreateWindowExA(\"%s\", \"%s\", %dx%d+%d+%d, estilo 0x%x) -> 0x%x", key.c_str(), gstr(title).c_str(), cw, ch, x, y, style, hwnd);
    RET(hwnd, 12);
}

STUB(ext_user32_dll_DestroyWindow) {
    if (!wnd(arg(0))) RET(0, 1);
    destroy_window(arg(0));
    RET(1, 1);
}
STUB(ext_user32_dll_CloseWindow) { RET(1, 1); }
STUB(ext_user32_dll_IsWindow) { RET(wnd(arg(0)) != nullptr, 1); }

// ---------------------------------------------------------------- mensajes

// ---------------------------------------------------------------- marco y título (WM_NCPAINT) y zonas (WM_NCHITTEST)

namespace {
struct NcBox { Rect frame, caption, close, max, min, client; i32 bd = 0; bool has_caption = false; };
NcBox nc_layout(const Wnd &w) {   // coordenadas de la ventana
    NcBox n;
    i32 l, t, r, b;
    nc_insets(w, l, t, r, b);
    n.client = {l, t, w.w - r, w.h - b};
    if (w.no_nc) return n;
    u32 st = w.style, ex = w.exstyle;
    i32 bd = 0;
    if (st & 0x40000) bd = 4; else if ((st & 0xC00000) == 0xC00000 || (st & 0x400000) || (ex & 1)) bd = 3; else if (st & 0x800000) bd = 1;
    n.bd = bd;
    n.has_caption = (st & 0xC00000) == 0xC00000;
    if (n.has_caption) {
        i32 ch = (ex & 0x80) ? 14 : CAPTION_H - 1;
        n.caption = {bd, bd, w.w - bd, bd + ch};
        i32 bw = (ex & 0x80) ? 12 : 16, bh = ch - 4, y = bd + 2, x = w.w - bd - 2 - bw;
        if (st & 0x80000) { n.close = {x, y, x + bw, y + bh}; x -= bw + 2; }                  // WS_SYSMENU
        if ((st & 0x80000) && (st & 0x30000)) { n.max = {x, y, x + bw, y + bh}; x -= bw; n.min = {x, y, x + bw, y + bh}; }
    }
    return n;
}
bool in(const Rect &r, i32 x, i32 y) { return x >= r.l && x < r.r && y >= r.t && y < r.b; }

void caption_button(u32 dc, const Rect &r, int kind) {   // 0 cerrar, 1 maximizar, 2 minimizar
    gdi_draw_edge(dc, r, 5, 0x80F);   // EDGE_RAISED | BF_RECT | BF_MIDDLE
    u32 ink = sys_color(18);
    i32 cx = (r.l + r.r) / 2 - 1, cy = (r.t + r.b) / 2 - 1;
    if (kind == 0) for (int i = -3; i <= 3; i++) { gdi_hline(dc, cx + i - 1, cx + i + 1, cy + i, ink); gdi_hline(dc, cx - i - 1, cx - i + 1, cy + i, ink); }
    else if (kind == 1) { gdi_draw_edge(dc, {cx - 4, cy - 4, cx + 5, cy + 4}, 0, 0); gdi_hline(dc, cx - 4, cx + 5, cy - 4, ink); gdi_hline(dc, cx - 4, cx + 5, cy - 3, ink); gdi_hline(dc, cx - 4, cx + 5, cy + 3, ink); gdi_vline(dc, cx - 4, cy - 4, cy + 4, ink); gdi_vline(dc, cx + 4, cy - 4, cy + 4, ink); }
    else { gdi_hline(dc, cx - 3, cx + 3, cy + 3, ink); gdi_hline(dc, cx - 3, cx + 3, cy + 2, ink); }
}

void paint_nc(u32 hwnd) {
    Wnd *w = wnd(hwnd);
    if (!w || w->no_nc) return;
    NcBox n = nc_layout(*w);
    u32 dc = gdi_window_dc(hwnd, false);
    u32 st = w->style, ex = w->exstyle;
    Rect all{0, 0, w->w, w->h};
    if (n.bd == 1) {
        gdi_draw_edge(dc, all, 1, 0x800F);   // BF_MONO: borde negro de 1 píxel
    } else if (n.bd >= 3) {
        gdi_draw_edge(dc, all, 5, 0xF);       // EDGE_RAISED
        Rect in1{2, 2, w->w - 2, w->h - 2};
        for (i32 k = 2; k < n.bd; k++) { gdi_draw_edge(dc, in1, 0, 0); gdi_hline(dc, in1.l, in1.r, in1.t, sys_color(15)); gdi_hline(dc, in1.l, in1.r, in1.b - 1, sys_color(15)); gdi_vline(dc, in1.l, in1.t, in1.b, sys_color(15)); gdi_vline(dc, in1.r - 1, in1.t, in1.b, sys_color(15)); in1 = {in1.l + 1, in1.t + 1, in1.r - 1, in1.b - 1}; }
    }
    if (n.has_caption) {
        bool active = hwnd == g_active || (w->owner && w->owner == g_active && false);
        u32 c0 = sys_color(active ? 2 : 3), c1 = sys_color(active ? 27 : 28);
        Rect c = n.caption;
        i32 width = std::max(1, c.w());
        for (i32 x = c.l; x < c.r; x++) {   // degradado horizontal (Windows 98)
            i32 k = x - c.l;
            u32 col = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                i32 a = (i32)((c0 >> sh) & 0xFF), b = (i32)((c1 >> sh) & 0xFF);
                col |= (u32)(a + (b - a) * k / width) << sh;
            }
            gdi_vline(dc, x, c.t, c.b, col);
        }
        gdi_hline(dc, c.l, c.r, c.b, sys_color(15));   // línea bajo el título
        Rect tclip{c.l + 2, c.t, (n.min.r ? n.min.l : n.close.r ? n.close.l : c.r) - 2, c.b};
        gdi_text(dc, c.l + 3, c.t + (c.h() - 13) / 2, w->text, sys_color(active ? 9 : 19), true, &tclip);
        if (n.close.r) caption_button(dc, n.close, 0);
        if (n.max.r) caption_button(dc, n.max, 1);
        if (n.min.r) caption_button(dc, n.min, 2);
    }
    if (w->menu && !(st & 0x40000000u)) {   // barra de menú (menu.cpp)
        i32 mt = n.bd + (n.has_caption ? (ex & 0x80 ? 15 : CAPTION_H) : 0);
        menu_bar_paint(hwnd, w->menu, dc, Rect{n.bd, mt, w->w - n.bd, mt + MENU_H});
    }
    // borde 3D del área cliente
    i32 cl = n.bd, ct = n.bd + (n.has_caption ? (ex & 0x80 ? 15 : CAPTION_H) : 0) + (w->menu && !(st & 0x40000000u) ? MENU_H : 0);
    Rect inner{cl, ct, w->w - n.bd, w->h - n.bd};
    if (ex & 0x200) { gdi_draw_edge(dc, inner, 10, 0xF); inner = {inner.l + 2, inner.t + 2, inner.r - 2, inner.b - 2}; }   // EDGE_SUNKEN
    if (ex & 0x20000) gdi_draw_edge(dc, inner, 2, 0xF);                                                                      // BDR_SUNKENOUTER
    if (st & (0x200000 | 0x100000)) sb_nc_paint(hwnd, dc);
    gdi_release_dc(dc);
}

u32 nc_hittest(u32 hwnd, i32 sx, i32 sy) {
    Wnd *w = wnd(hwnd);
    if (!w) return 0;
    i32 ox, oy;
    window_origin(hwnd, ox, oy);
    i32 x = sx - ox, y = sy - oy;
    if (x < 0 || y < 0 || x >= w->w || y >= w->h) return 0;   // HTNOWHERE
    NcBox n = nc_layout(*w);
    if (in(n.client, x, y)) return 1;                           // HTCLIENT
    if (in(n.close, x, y)) return 20;                           // HTCLOSE
    if (in(n.max, x, y)) return 9;                              // HTMAXBUTTON
    if (in(n.min, x, y)) return 8;                              // HTMINBUTTON
    if (in(n.caption, x, y)) return 2;                          // HTCAPTION
    if (w->menu && !(w->style & 0x40000000u) && y >= n.caption.b && y < n.client.t) return 5;   // HTMENU
    if (w->style & 0x200000 && x >= n.client.r) return 7;       // HTVSCROLL
    if (w->style & 0x100000 && y >= n.client.b) return 6;       // HTHSCROLL
    return 18;                                                  // HTBORDER
}

}  // namespace

STUB(ext_user32_dll_DefWindowProcA) {
    u32 hwnd = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    Wnd *w = wnd(hwnd);
    u32 r = 0;
    switch (msg) {
        case WM_NCCREATE:
            if (w && lp) { u32 t = rd32(lp + 36); if (t) w->text = gstr(t); }
            r = 1;
            break;
        case WM_PAINT: {
            if (w) w->invalid = false;
            break;
        }
        case WM_ERASEBKGND:
            if (w && w->cls && w->cls->brush) {
                i32 l, t, rr, b;
                nc_insets(*w, l, t, rr, b);
                Rect rc{0, 0, w->w - l - rr, w->h - t - b};
                u32 dc = wp ? wp : gdi_window_dc(hwnd, true);
                gdi_fill_rect(dc, rc, w->cls->brush);
                if (!wp) gdi_release_dc(dc);
                r = 1;
            }
            break;
        case 0x85: paint_nc(hwnd); break;                              // WM_NCPAINT
        case 0xA1:                                                     // WM_NCLBUTTONDOWN
            if (wp == 2 && w && !(w->style & 0x40000000u)) {           // HTCAPTION: arrastrar
                i32 ox, oy;
                window_origin(hwnd, ox, oy);
                g_drag = {hwnd, (i32)(i16)(lp & 0xFFFF) - ox, (i32)(i16)(lp >> 16) - oy};
            } else if (wp == 7 || wp == 6) sb_nc_click(hwnd, wp == 7 ? 1 : 0, (i32)(i16)(lp & 0xFFFF), (i32)(i16)(lp >> 16));   // HTVSCROLL / HTHSCROLL
            else if (wp == 5 && w && w->menu) {                         // HTMENU: abre la barra
                NcBox n = nc_layout(*w);
                i32 ox, oy;
                window_origin(hwnd, ox, oy);
                i32 mt = n.bd + (n.has_caption ? (w->exstyle & 0x80 ? 15 : CAPTION_H) : 0);
                menu_bar_click(hwnd, w->menu, Rect{ox + n.bd, oy + mt, ox + w->w - n.bd, oy + mt + MENU_H},
                               (i32)(i16)(lp & 0xFFFF), (i32)(i16)(lp >> 16));
            }
            else if (wp == 20) send(hwnd, WM_SYSCOMMAND, 0xF060, lp);   // HTCLOSE → SC_CLOSE
            else if (wp == 8) send(hwnd, WM_SYSCOMMAND, 0xF020, lp);      // SC_MINIMIZE
            else if (wp == 9) send(hwnd, WM_SYSCOMMAND, 0xF030, lp);      // SC_MAXIMIZE
            break;
        case 0xA3:                                                     // WM_NCLBUTTONDBLCLK
            break;
        case WM_CLOSE: destroy_window(hwnd); break;
        case WM_SETTEXT: if (w) { w->text = gstr(lp); if ((w->style & 0xC00000) == 0xC00000) paint_nc(hwnd); } r = 1; break;
        case WM_GETTEXT: r = w ? gstrcpy(lp, w->text, wp) : 0; break;
        case WM_GETTEXTLENGTH: r = w ? (u32)w->text.size() : 0; break;
        case WM_NCHITTEST: r = nc_hittest(hwnd, (i32)(i16)(lp & 0xFFFF), (i32)(i16)(lp >> 16)); break;
        case WM_SETCURSOR:
            if (w && w->cls && w->cls->cursor && (lp & 0xFFFF) == 1) { display().cursor = w->cls->cursor; r = 1; }
            break;
        case WM_MOUSEACTIVATE: r = 1; break;                           // MA_ACTIVATE
        case WM_WINDOWPOSCHANGED: send_size_move(hwnd); break;
        case WM_ACTIVATE: if ((wp & 0xFFFF) && w && !(w->style & 0x40000000u)) set_focus(hwnd); break;
        case WM_NCACTIVATE: if (w) { w->nc_dirty = true; } r = 1; break;
        case WM_SYSCOMMAND: if ((wp & 0xFFF0) == 0xF060) post(hwnd, WM_CLOSE, 0, 0); break;   // SC_CLOSE
        case 0x11: r = 1; break;                                       // WM_QUERYENDSESSION
        case WM_GETMINMAXINFO: break;
    }
    RET(r, 4);
}
STUB(ext_user32_dll_DefFrameProcA) { RET(0, 5); }
STUB(ext_user32_dll_CallWindowProcA) {
    u32 proc = arg(0);
    if (!proc) RET(0, 5);
    RET(guest_call(proc, {arg(1), arg(2), arg(3), arg(4)}), 5);
}

STUB(ext_user32_dll_SendMessageA) { RET(send(arg(0), arg(1), arg(2), arg(3)), 4); }
STUB(ext_user32_dll_PostMessageA) {
    u32 h = arg(0);
    if (h == 0xFFFFu) { for (u32 t : g_top) post(t, arg(1), arg(2), arg(3)); RET(1, 4); }   // HWND_BROADCAST
    post(h, arg(1), arg(2), arg(3));
    RET(1, 4);
}
STUB(ext_user32_dll_PostThreadMessageA) { post(0, arg(1), arg(2), arg(3)); RET(1, 4); }
STUB(ext_user32_dll_PostQuitMessage) { g_quit = true; g_quit_code = arg(0); RET(0, 1); }
STUB(ext_user32_dll_InSendMessage) { RET(0, 0); }

STUB(ext_user32_dll_GetMessageA) {
    u32 pmsg = arg(0), hwnd = arg(1), fmin = arg(2), fmax = arg(3);
    Msg m;
    while (!get_msg(m, hwnd, fmin, fmax, true)) pump_input(true, next_timer_wait());
    g_last = m;
    write_msg(pmsg, m);
    RET(m.msg != WM_QUIT, 4);
}
STUB(ext_user32_dll_PeekMessageA) {
    u32 pmsg = arg(0), hwnd = arg(1), fmin = arg(2), fmax = arg(3), flags = arg(4);
    Msg m;
    if (!get_msg(m, hwnd, fmin, fmax, flags & 1)) {
        // el juego sondea en bucle: ceder un poco de CPU
        static u32 idle = 0;
        if (other_threads()) thread_yield();
        else if (++idle % 64 == 0) pump_input(true, 1);
        RET(0, 5);
    }
    if (flags & 1) g_last = m;
    write_msg(pmsg, m);
    RET(1, 5);
}
STUB(ext_user32_dll_WaitMessage) { pump_input(true, next_timer_wait()); RET(1, 0); }
STUB(ext_user32_dll_MsgWaitForMultipleObjects) { pump_input(true, arg(3) == 0xFFFFFFFFu ? next_timer_wait() : std::min(arg(3), next_timer_wait())); RET(arg(0), 5); }
STUB(ext_user32_dll_TranslateMessage) { RET(0, 1); }   // el host ya genera WM_CHAR
STUB(ext_user32_dll_DispatchMessageA) {
    u32 p = arg(0);
    u32 hwnd = rd32(p), msg = rd32(p + 4), wp = rd32(p + 8), lp = rd32(p + 12);
    if (msg == WM_TIMER && lp) RET(guest_call(lp, {hwnd, msg, wp, ticks_ms()}), 1);
    RET(send(hwnd, msg, wp, lp), 1);
}
STUB(ext_user32_dll_GetMessagePos) { RET(((u32)(u16)g_last.y << 16) | (u16)g_last.x, 0); }
STUB(ext_user32_dll_GetMessageTime) { RET(g_last.time, 0); }
STUB(ext_user32_dll_RegisterWindowMessageA) {
    static std::map<std::string, u32> m;
    std::string s = gstr(arg(0));
    auto it = m.find(s);
    if (it == m.end()) it = m.emplace(s, 0xC000 + (u32)m.size()).first;
    RET(it->second, 1);
}
STUB(ext_user32_dll_RegisterClipboardFormatA) { RET(0xC100 + (gstrlen(arg(0)) & 0xFF), 1); }
STUB(ext_user32_dll_IsDialogMessageA) { RET(0, 2); }
STUB(ext_user32_dll_TranslateMDISysAccel) { RET(0, 2); }

// ---------------------------------------------------------------- ganchos (MFC: WH_CBT para subclasificar, WH_MSGFILTER)

STUB(ext_user32_dll_SetWindowsHookExA) {
    u32 id = g_next_hook++;
    g_hooks.push_back({id, arg(0), arg(1)});
    RET(id, 4);
}
STUB(ext_user32_dll_UnhookWindowsHookEx) {
    u32 id = arg(0);
    g_hooks.erase(std::remove_if(g_hooks.begin(), g_hooks.end(), [&](const Hook &h) { return h.id == id; }), g_hooks.end());
    RET(1, 1);
}
STUB(ext_user32_dll_CallNextHookEx) {
    u32 id = arg(0), code = arg(1), wp = arg(2), lp = arg(3);
    auto it = std::find_if(g_hooks.begin(), g_hooks.end(), [&](const Hook &h) { return h.id == id; });
    if (it != g_hooks.end()) {
        u32 type = it->type;
        for (auto j = std::make_reverse_iterator(it); j != g_hooks.rend(); ++j)
            if (j->type == type) RET(guest_call(j->proc, {code, wp, lp}), 4);
    }
    RET(0, 4);
}

// ---------------------------------------------------------------- atributos de ventana

STUB(ext_user32_dll_GetWindowLongA) {
    Wnd *w = wnd(arg(0));
    i32 i = (i32)arg(1);
    if (!w) RET(0, 2);
    switch (i) {
        case -4: RET(w->wndproc, 2);
        case -6: RET(w->hinst, 2);
        case -8: RET(w->owner ? w->owner : w->parent, 2);
        case -12: RET(w->id, 2);
        case -16: RET(w->style, 2);
        case -20: RET(w->exstyle, 2);
        case -21: RET(w->userdata, 2);
    }
    if (i >= 0 && (size_t)i + 4 <= w->extra.size()) { u32 v; std::memcpy(&v, &w->extra[(size_t)i], 4); RET(v, 2); }
    RET(0, 2);
}
STUB(ext_user32_dll_SetWindowLongA) {
    Wnd *w = wnd(arg(0));
    i32 i = (i32)arg(1);
    u32 v = arg(2), old = 0;
    if (!w) RET(0, 3);
    switch (i) {
        case -4: old = w->wndproc; w->wndproc = v; break;
        case -6: old = w->hinst; w->hinst = v; break;
        case -12: old = w->id; w->id = v; break;
        case -16: old = w->style; w->style = v; owners_changed(); w->nc_dirty = true; break;
        case -20: old = w->exstyle; w->exstyle = v; owners_changed(); w->nc_dirty = true; break;
        case -21: old = w->userdata; w->userdata = v; break;
        default:
            if (i >= 0 && (size_t)i + 4 <= w->extra.size()) { std::memcpy(&old, &w->extra[(size_t)i], 4); std::memcpy(&w->extra[(size_t)i], &v, 4); }
    }
    RET(old, 3);
}
STUB(ext_user32_dll_SetPropA) { Wnd *w = wnd(arg(0)); if (w) w->props[class_key(arg(1))] = arg(2); RET(w != nullptr, 3); }
STUB(ext_user32_dll_GetPropA) {
    Wnd *w = wnd(arg(0));
    if (!w) RET(0, 2);
    auto it = w->props.find(class_key(arg(1)));
    RET(it == w->props.end() ? 0 : it->second, 2);
}
STUB(ext_user32_dll_RemovePropA) {
    Wnd *w = wnd(arg(0));
    if (!w) RET(0, 2);
    auto it = w->props.find(class_key(arg(1)));
    if (it == w->props.end()) RET(0, 2);
    u32 v = it->second;
    w->props.erase(it);
    RET(v, 2);
}
// como Windows: el texto se pide y se cambia con mensajes (los controles los atienden)
STUB(ext_user32_dll_SetWindowTextA) { if (!wnd(arg(0))) RET(0, 2); u32 p = arg(1); RET(send(arg(0), WM_SETTEXT, 0, p) ? 1 : 0, 2); }
STUB(ext_user32_dll_GetWindowTextA) { if (!wnd(arg(0)) || !arg(2)) RET(0, 3); u32 h = arg(0), b = arg(1), n = arg(2); RET(send(h, WM_GETTEXT, n, b), 3); }
STUB(ext_user32_dll_GetWindowTextLengthA) { if (!wnd(arg(0))) RET(0, 1); RET(send(arg(0), WM_GETTEXTLENGTH, 0, 0), 1); }
STUB(ext_user32_dll_GetDlgCtrlID) { Wnd *w = wnd(arg(0)); RET(w ? w->id : 0, 1); }
u32 wm_dlg_item(u32 hwnd, u32 id) {
    Wnd *w = wnd(hwnd);
    if (w) for (u32 c : w->children) if (Wnd *k = wnd(c); k && k->id == id) return c;
    return 0;
}
STUB(ext_user32_dll_GetDlgItem) { RET(wm_dlg_item(arg(0), arg(1)), 2); }
STUB(ext_user32_dll_GetParent) {
    Wnd *w = wnd(arg(0));
    RET(w ? ((w->style & 0x40000000u) ? w->parent : w->owner) : 0, 1);
}
STUB(ext_user32_dll_SetParent) {
    Wnd *w = wnd(arg(0));
    if (!w) RET(0, 2);
    u32 old = w->parent, np = arg(1);
    if (Wnd *p = wnd(old)) p->children.erase(std::remove(p->children.begin(), p->children.end(), w->hwnd), p->children.end());
    else g_top.erase(std::remove(g_top.begin(), g_top.end(), w->hwnd), g_top.end());
    w->parent = np;
    if (Wnd *p = wnd(np)) p->children.push_back(w->hwnd); else g_top.push_back(w->hwnd);
    owners_changed();
    RET(old, 2);
}
STUB(ext_user32_dll_IsChild) {
    for (Wnd *w = wnd(arg(1)); w && (w->style & 0x40000000u); w = wnd(w->parent)) if (w->parent == arg(0)) RET(1, 2);
    RET(0, 2);
}
STUB(ext_user32_dll_GetWindow) {
    Wnd *w = wnd(arg(0));
    u32 cmd = arg(1);
    if (!w) RET(0, 2);
    const std::vector<u32> &sib = (w->style & 0x40000000u) && wnd(w->parent) ? wnd(w->parent)->children : g_top;
    auto it = std::find(sib.begin(), sib.end(), w->hwnd);
    switch (cmd) {
        case 0: RET(sib.empty() ? 0 : sib.back(), 2);                                  // GW_HWNDFIRST (arriba)
        case 1: RET(sib.empty() ? 0 : sib.front(), 2);                                 // GW_HWNDLAST
        case 2: RET(it != sib.end() && it != sib.begin() ? *(it - 1) : 0, 2);          // GW_HWNDNEXT (debajo)
        case 3: RET(it != sib.end() && it + 1 != sib.end() ? *(it + 1) : 0, 2);       // GW_HWNDPREV
        case 4: RET(w->owner, 2);                                                       // GW_OWNER
        case 5: RET(w->children.empty() ? 0 : w->children.back(), 2);                   // GW_CHILD
    }
    RET(0, 2);
}
STUB(ext_user32_dll_GetTopWindow) {
    Wnd *w = wnd(arg(0));
    if (!arg(0)) RET(g_top.empty() ? 0 : g_top.back(), 1);
    RET(w && !w->children.empty() ? w->children.back() : 0, 1);
}
STUB(ext_user32_dll_GetLastActivePopup) { RET(arg(0), 1); }
STUB(ext_user32_dll_GetDesktopWindow) { RET(DESKTOP, 0); }
STUB(ext_user32_dll_GetWindowThreadProcessId) { if (arg(1)) wr32(arg(1), 0x1000); RET(0x1004, 2); }
STUB(ext_user32_dll_FindWindowA) { RET(0, 2); }   // no hay otra instancia
STUB(ext_user32_dll_EnumThreadWindows) {   // un solo proceso: todas las ventanas de nivel superior
    std::vector<u32> tops = g_top;
    for (u32 t : tops) if (wnd(t) && !guest_call(arg(1), {t, arg(2)})) break;
    RET(1, 3);
}
STUB(ext_user32_dll_EnumWindows) {
    std::vector<u32> tops = g_top;
    for (u32 t : tops) if (wnd(t) && !guest_call(arg(0), {t, arg(1)})) break;
    RET(1, 2);
}
STUB(ext_user32_dll_EnumChildWindows) {
    Wnd *w = wnd(arg(0));
    if (w) { std::vector<u32> kids = w->children; for (u32 c : kids) if (!guest_call(arg(1), {c, arg(2)})) break; }
    RET(1, 3);
}

// ---------------------------------------------------------------- mostrar, mover, activar

STUB(ext_user32_dll_ShowWindow) {
    Wnd *w = wnd(arg(0));
    if (!w) RET(0, 2);
    bool was = w->style & 0x10000000u;
    u32 cmd = arg(1);
    bool show = cmd != 0 && cmd != 6 && cmd != 2 && cmd != 7;   // SW_HIDE, SW_MINIMIZE, SW_SHOWMINIMIZED, SW_SHOWMINNOACTIVE
    if (show != was) {
        Rect old = visible_chain(arg(0)) ? window_screen_rect(arg(0)) : Rect{};
        if (show) w->style |= 0x10000000u; else w->style &= ~0x10000000u;
        owners_changed();
        send(arg(0), WM_SHOWWINDOW, show, 0);
        if ((w = wnd(arg(0))) && show) expose(window_screen_rect(arg(0)));
        if (!show) expose(old);
    }
    if (show && (w = wnd(arg(0))) && !(w->style & 0x40000000u) && cmd != 4 && cmd != 8 && g_active != arg(0)) {   // SW_SHOWNOACTIVATE / SW_SHOWNA
        if (!g_active) send(arg(0), WM_ACTIVATEAPP, 1, 0);
        activate_window(arg(0));
    }
    RET(was, 2);
}
// pinta ya lo pendiente de una ventana (marco y cliente)
static void paint_now(u32 h) {
    Wnd *w = wnd(h);
    if (!w || !visible_chain(h)) return;
    if (w->nc_dirty) {
        w->nc_dirty = false;
        i32 l, t, r, b;
        nc_insets(*w, l, t, r, b);
        if (l || t || r || b) send(h, 0x85, 1, 0);   // WM_NCPAINT
    }
    if ((w = wnd(h)) && w->invalid) send(h, WM_PAINT, 0, 0);
}
STUB(ext_user32_dll_UpdateWindow) { paint_now(arg(0)); RET(1, 1); }
static void redraw(u32 h, const Rect *r, u32 flags) {
    Wnd *w = wnd(h);
    if (!w) return;
    if (flags & 1) invalidate(h, r, flags & 4, flags & 0x400);           // RDW_INVALIDATE, RDW_ERASE, RDW_FRAME
    if (flags & 8) { w->invalid = false; w->upd = {}; }                  // RDW_VALIDATE
    if (flags & 0x80) { std::vector<u32> kids = w->children; for (u32 c : kids) redraw(c, nullptr, flags & ~0x100u); }   // RDW_ALLCHILDREN
    if (flags & 0x100) {                                                 // RDW_UPDATENOW
        paint_now(h);
        if (flags & 0x80) if ((w = wnd(h))) { std::vector<u32> kids = w->children; for (u32 c : kids) paint_now(c); }
    }
}
STUB(ext_user32_dll_RedrawWindow) {   // (hwnd, rect, rgn, flags)
    Rect r;
    if (arg(1)) r = rd_rect(arg(1));
    if (!arg(0)) { for (u32 t : std::vector<u32>(g_top)) redraw(t, nullptr, arg(3)); RET(1, 4); }
    redraw(arg(0), arg(1) ? &r : nullptr, arg(3));
    RET(1, 4);
}
STUB(ext_user32_dll_InvalidateRect) {
    u32 h = arg(0);
    if (!h) { for (auto &kv : g_wnds) if (!kv.second.destroyed) invalidate(kv.first, nullptr, arg(2), true); RET(1, 3); }
    Rect r;
    if (arg(1)) r = rd_rect(arg(1));
    if (!wnd(h)) RET(0, 3);
    invalidate(h, arg(1) ? &r : nullptr, arg(2));
    RET(1, 3);
}
STUB(ext_user32_dll_InvalidateRgn) { if (wnd(arg(0))) invalidate(arg(0), nullptr, arg(2)); RET(1, 3); }
STUB(ext_user32_dll_ValidateRect) {
    Wnd *w = wnd(arg(0));
    if (w) {
        if (!arg(1)) { w->invalid = false; w->upd = {}; }
        else { Rect r = rd_rect(arg(1)); if (r.l <= w->upd.l && r.t <= w->upd.t && r.r >= w->upd.r && r.b >= w->upd.b) { w->invalid = false; w->upd = {}; } }
    }
    RET(1, 2);
}
STUB(ext_user32_dll_ValidateRgn) { Wnd *w = wnd(arg(0)); if (w) { w->invalid = false; w->upd = {}; } RET(1, 2); }
STUB(ext_user32_dll_GetUpdateRgn) { Wnd *w = wnd(arg(0)); RET(w && w->invalid ? 2 : 1, 3); }   // SIMPLEREGION / NULLREGION
STUB(ext_user32_dll_GetUpdateRect) {   // (hwnd, rect, erase)
    Wnd *w = wnd(arg(0));
    bool any = w && w->invalid && !rect_empty(w->upd);
    if (arg(1)) wr_rect(arg(1), any ? w->upd : Rect{});
    if (any && arg(2) && w->erase) {
        u32 dc = gdi_window_dc(arg(0), true);
        gdi_clip_to(dc, w->upd);
        if (send(arg(0), WM_ERASEBKGND, dc, 0)) if ((w = wnd(arg(0)))) w->erase = false;
        gdi_release_dc(dc);
    }
    RET(any, 3);
}
STUB(ext_user32_dll_LockWindowUpdate) { RET(1, 1); }

static void set_pos(u32 hwnd, i32 x, i32 y, i32 cw, i32 ch, u32 flags) {
    Wnd *w = wnd(hwnd);
    if (!w) return;
    Rect old = visible_chain(hwnd) ? window_screen_rect(hwnd) : Rect{};
    bool was_visible = w->style & 0x10000000u;
    bool moved = false;
    if (!(flags & 2) && (w->x != x || w->y != y)) { w->x = x; w->y = y; moved = true; }         // SWP_NOMOVE
    if (!(flags & 1) && (w->w != cw || w->h != ch)) { w->w = cw; w->h = ch; moved = true; }     // SWP_NOSIZE
    if (flags & 0x40) { if (!(w->style & 0x10000000u)) { w->style |= 0x10000000u; send(hwnd, WM_SHOWWINDOW, 1, 0); } }  // SWP_SHOWWINDOW
    if (flags & 0x80) { w = wnd(hwnd); if (w) { w->style &= ~0x10000000u; send(hwnd, WM_SHOWWINDOW, 0, 0); } }       // SWP_HIDEWINDOW
    if ((w = wnd(hwnd)) && (moved || was_visible != !!(w->style & 0x10000000u) || (flags & 0x20))) {   // SWP_FRAMECHANGED
        owners_changed();
        expose(old);
        if (visible_chain(hwnd)) expose(window_screen_rect(hwnd));
    }
    if (moved) send_size_move(hwnd);
}
STUB(ext_user32_dll_SetWindowPos) {
    set_pos(arg(0), (i32)arg(2), (i32)arg(3), (i32)arg(4), (i32)arg(5), arg(6));
    u32 after = arg(1);
    Wnd *w = wnd(arg(0));
    if (w && !(arg(6) & 4) && !(w->style & 0x40000000u) && (after == 0 || after == 0xFFFFFFFFu)) {   // HWND_TOP / HWND_TOPMOST
        g_top.erase(std::remove(g_top.begin(), g_top.end(), arg(0)), g_top.end());
        g_top.push_back(arg(0));
        owners_changed();
        if (visible_chain(arg(0))) expose(window_screen_rect(arg(0)));
    }
    RET(1, 7);
}
STUB(ext_user32_dll_MoveWindow) { set_pos(arg(0), (i32)arg(1), (i32)arg(2), (i32)arg(3), (i32)arg(4), 0); RET(1, 6); }
STUB(ext_user32_dll_BeginDeferWindowPos) { RET(0x7000, 1); }
STUB(ext_user32_dll_DeferWindowPos) { set_pos(arg(1), (i32)arg(3), (i32)arg(4), (i32)arg(5), (i32)arg(6), arg(7)); RET(arg(0), 8); }
STUB(ext_user32_dll_EndDeferWindowPos) { RET(1, 1); }
STUB(ext_user32_dll_BringWindowToTop) {
    g_top.erase(std::remove(g_top.begin(), g_top.end(), arg(0)), g_top.end());
    if (wnd(arg(0))) { g_top.push_back(arg(0)); owners_changed(); if (visible_chain(arg(0))) expose(window_screen_rect(arg(0))); }
    RET(1, 1);
}
STUB(ext_user32_dll_GetWindowRect) {
    Rect r;
    if (!wnd_screen_rect(arg(0), false, r)) RET(0, 2);
    wr_rect(arg(1), r);
    RET(1, 2);
}
STUB(ext_user32_dll_GetClientRect) {
    Wnd *w = wnd(arg(0));
    Rect r;
    if (w) { i32 l, t, rr, b; nc_insets(*w, l, t, rr, b); r = {0, 0, std::max(0, w->w - l - rr), std::max(0, w->h - t - b)}; }
    else if (arg(0) == DESKTOP) r = {0, 0, display().screen.w, display().screen.h};
    wr_rect(arg(1), r);
    RET(w != nullptr || arg(0) == DESKTOP, 2);
}
static void adjust_rect(u32 p, u32 style, bool menu, u32 ex) {
    Wnd tmp;
    tmp.style = style; tmp.exstyle = ex; tmp.menu = menu ? 1 : 0;
    i32 l, t, r, b;
    nc_insets(tmp, l, t, r, b);
    Rect rc = rd_rect(p);
    wr_rect(p, {rc.l - l, rc.t - t, rc.r + r, rc.b + b});
}
STUB(ext_user32_dll_AdjustWindowRectEx) { adjust_rect(arg(0), arg(1) & ~0x300000u, arg(2), arg(3)); RET(1, 4); }
STUB(ext_user32_dll_AdjustWindowRect) { adjust_rect(arg(0), arg(1) & ~0x300000u, arg(2), 0); RET(1, 3); }
STUB(ext_user32_dll_GetWindowPlacement) {
    Wnd *w = wnd(arg(0));
    u32 p = arg(1);
    if (!w) RET(0, 2);
    wr32(p, 44); wr32(p + 4, 0); wr32(p + 8, 1);
    wr_rect(p + 28, {w->x, w->y, w->x + w->w, w->y + w->h});
    RET(1, 2);
}
STUB(ext_user32_dll_SetWindowPlacement) { RET(1, 2); }
STUB(ext_user32_dll_ClientToScreen) {
    i32 ox, oy;
    screen_origin(arg(0), ox, oy);
    u32 p = arg(1);
    wr32(p, rd32(p) + (u32)ox); wr32(p + 4, rd32(p + 4) + (u32)oy);
    RET(1, 2);
}
STUB(ext_user32_dll_ScreenToClient) {
    i32 ox, oy;
    screen_origin(arg(0), ox, oy);
    u32 p = arg(1);
    wr32(p, rd32(p) - (u32)ox); wr32(p + 4, rd32(p + 4) - (u32)oy);
    RET(1, 2);
}
STUB(ext_user32_dll_MapWindowPoints) {
    i32 ax = 0, ay = 0, bx = 0, by = 0;
    if (arg(0) && arg(0) != DESKTOP) screen_origin(arg(0), ax, ay);
    if (arg(1) && arg(1) != DESKTOP) screen_origin(arg(1), bx, by);
    u32 p = arg(2);
    for (u32 i = 0; i < arg(3); i++) { wr32(p + 8 * i, rd32(p + 8 * i) + (u32)(ax - bx)); wr32(p + 8 * i + 4, rd32(p + 8 * i + 4) + (u32)(ay - by)); }
    RET((u32)(((u16)(ay - by) << 16) | (u16)(ax - bx)), 4);
}
STUB(ext_user32_dll_WindowFromPoint) { RET(window_from_point((i32)arg(0), (i32)arg(1)), 2); }
STUB(ext_user32_dll_IsWindowVisible) { RET(visible_chain(arg(0)), 1); }
STUB(ext_user32_dll_IsWindowEnabled) { Wnd *w = wnd(arg(0)); RET(w && !(w->style & 0x08000000u), 1); }
STUB(ext_user32_dll_EnableWindow) {
    Wnd *w = wnd(arg(0));
    if (!w) RET(0, 2);
    bool was_disabled = w->style & 0x08000000u;
    if (arg(1)) w->style &= ~0x08000000u; else w->style |= 0x08000000u;
    if (was_disabled == !!arg(1)) send(arg(0), WM_ENABLE, arg(1), 0);
    RET(was_disabled, 2);
}
STUB(ext_user32_dll_IsIconic) { RET(0, 1); }
STUB(ext_user32_dll_IsZoomed) { RET(0, 1); }
STUB(ext_user32_dll_ShowOwnedPopups) { RET(1, 2); }

STUB(ext_user32_dll_SetFocus) { u32 old = g_focus; set_focus(arg(0)); RET(old, 1); }
STUB(ext_user32_dll_GetFocus) { RET(g_focus, 0); }
STUB(ext_user32_dll_SetActiveWindow) { u32 old = g_active; if (wnd(arg(0))) activate_window(arg(0)); RET(old, 1); }
STUB(ext_user32_dll_GetActiveWindow) { RET(g_active, 0); }
STUB(ext_user32_dll_GetForegroundWindow) { RET(g_active, 0); }
STUB(ext_user32_dll_SetForegroundWindow) { if (wnd(arg(0))) activate_window(arg(0)); RET(1, 1); }
STUB(ext_user32_dll_SetCapture) { u32 old = g_capture; g_capture = arg(0); RET(old, 1); }
STUB(ext_user32_dll_ReleaseCapture) { g_capture = 0; RET(1, 0); }
STUB(ext_user32_dll_GetCapture) { RET(g_capture, 0); }

// ---------------------------------------------------------------- temporizadores

STUB(ext_user32_dll_SetTimer) {
    u32 hwnd = arg(0), id = arg(1), ms = arg(2), proc = arg(3);
    if (!hwnd && !id) { static u32 n = 0x100; id = n++; }
    for (auto &t : g_timers) if (t.hwnd == hwnd && t.id == id) { t.period = ms; t.proc = proc; t.next = ticks_ms() + ms; RET(id, 4); }
    g_timers.push_back({hwnd, id, ms, proc, ticks_ms() + ms});
    RET(id, 4);
}
STUB(ext_user32_dll_KillTimer) {
    u32 hwnd = arg(0), id = arg(1);
    size_t n = g_timers.size();
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](const Timer &t) { return t.hwnd == hwnd && t.id == id; }), g_timers.end());
    RET(n != g_timers.size(), 2);
}

// ---------------------------------------------------------------- teclado, ratón, cursor

STUB(ext_user32_dll_GetAsyncKeyState) {
    u32 vk = arg(0) & 0xFF;
    static bool tr = std::getenv("PCF7_TRACE_KEYS") != nullptr;
    if (tr) plog_once("gaks" + std::to_string(vk), "GetAsyncKeyState(0x%x) desde f_%x", vk, cur_fn);
    bool down;
    if (vk == 1) down = g_mouse_buttons & 1; else if (vk == 2) down = g_mouse_buttons & 2; else if (vk == 4) down = g_mouse_buttons & 0x10;
    else { std::lock_guard<std::mutex> lk(display().mx); down = display().keys[vk]; }
    RET(down ? 0x8000u : 0u, 1);
}
STUB(ext_user32_dll_GetKeyState) {
    u32 vk = arg(0) & 0xFF;
    bool down;
    { std::lock_guard<std::mutex> lk(display().mx); down = display().keys[vk]; }
    if (vk == 1) down = g_mouse_buttons & 1; else if (vk == 2) down = g_mouse_buttons & 2;
    RET(down ? 0xFF80u : 0u, 1);
}
STUB(ext_user32_dll_GetCursorPos) { wr32(arg(0), (u32)display().mouse_x); wr32(arg(0) + 4, (u32)display().mouse_y); RET(1, 1); }
STUB(ext_user32_dll_SetCursorPos) { display().mouse_x = (i32)arg(0); display().mouse_y = (i32)arg(1); RET(1, 2); }
STUB(ext_user32_dll_ClipCursor) { RET(1, 1); }
STUB(ext_user32_dll_SetCursor) {
    u32 old = display().cursor;
    if (std::getenv("PCF7_TRACE_CURSOR")) plog("SetCursor(0x%x)%s desde 0x%x (f_%x)", arg(0), arg(0) && !cursor_image(arg(0)) ? " SIN IMAGEN" : "", rd32(esp), cur_fn);
    display().cursor = arg(0);
    RET(old, 1);
}
STUB(ext_user32_dll_ShowCursor) {
    static i32 count = 0;
    count += arg(0) ? 1 : -1;
    if (std::getenv("PCF7_TRACE_CURSOR")) plog("ShowCursor(%d) -> %d desde 0x%x", arg(0), count, rd32(esp));
    display().cursor_visible = count >= 0;
    RET((u32)count, 1);
}
u32 load_cursor_file(const std::string &winpath);   // cursor.cpp
STUB(ext_user32_dll_LoadCursorA) { RET(0x30000u + (arg(1) & 0xFFFF), 2); }
STUB(ext_user32_dll_LoadCursorFromFileA) { RET(load_cursor_file(gstr(arg(0))), 1); }
STUB(ext_user32_dll_DestroyCursor) { RET(1, 1); }
// Iconos: identificadores propios (los de GDI empiezan en 0x40000). Aún no se dibujan.
constexpr u32 ICON_RES = 0x00F00000u, ICON_NEW = 0x00F10000u;
STUB(ext_user32_dll_LoadIconA) { RET(ICON_RES + (arg(1) & 0xFFFF), 2); }
STUB(ext_user32_dll_CreateIcon) { static u32 n = 0; RET(ICON_NEW + ++n, 7); }
STUB(ext_user32_dll_CreateIconIndirect) { static u32 n = 0x8000; RET(ICON_NEW + ++n, 1); }
STUB(ext_user32_dll_CopyIcon) { RET(arg(0), 1); }
STUB(ext_user32_dll_DestroyIcon) { RET(1, 1); }
STUB(ext_user32_dll_MessageBeep) { RET(1, 1); }


// ---------------------------------------------------------------- pantalla, sistema

STUB(ext_user32_dll_GetSystemMetrics) {
    Screen &s = display().screen;
    switch (arg(0)) {
        case 0: case 16: case 61: case 78: RET(s.w, 1);     // SM_CXSCREEN / FULLSCREEN / MAXIMIZED / VIRTUALSCREEN
        case 1: case 17: case 62: case 79: RET(s.h, 1);
        case 2: case 3: case 20: case 21: RET(16, 1);       // barras de desplazamiento
        case 4: RET(CAPTION_H, 1);                          // SM_CYCAPTION
        case 5: case 6: RET(1, 1);                          // SM_CXBORDER
        case 7: case 8: RET(3, 1);                          // SM_CXDLGFRAME
        case 11: case 12: case 13: case 14: RET(32, 1);     // iconos y cursor
        case 15: RET(MENU_H, 1);                            // SM_CYMENU
        case 19: RET(1, 1);                                 // SM_MOUSEPRESENT
        case 32: case 33: RET(4, 1);                        // SM_CXFRAME
        case 36: case 37: RET(4, 1);                        // SM_CXDOUBLECLK
        case 43: RET(3, 1);                                 // SM_CMOUSEBUTTONS
        case 45: case 46: RET(2, 1);                        // SM_CXEDGE
        case 49: case 50: RET(16, 1);                       // iconos pequeños
        case 80: RET(1, 1);                                 // SM_CMONITORS
    }
    RET(0, 1);
}
STUB(ext_user32_dll_GetSysColor) { RET(sys_color((int)arg(0)), 1); }
STUB(ext_user32_dll_GetSysColorBrush) { RET(gdi_sys_color_brush((int)arg(0)), 1); }
STUB(ext_user32_dll_SystemParametersInfoA) {
    u32 action = arg(0), p = arg(2);
    if (action == 0x30 && p) wr_rect(p, {0, 0, display().screen.w, display().screen.h});   // SPI_GETWORKAREA
    else if (p && action != 0x2A) wr32(p, 0);
    RET(1, 4);
}
STUB(ext_user32_dll_GetDialogBaseUnits) { RET((16u << 16) | 8u, 0); }

void set_display_mode(int w, int h, int bpp);   // gdi32.cpp
STUB(ext_user32_dll_ChangeDisplaySettingsA) {
    u32 dm = arg(0);
    if (!dm) { plog("ChangeDisplaySettingsA(NULL): modo por defecto"); RET(0, 2); }
    u32 fields = rd32(dm + 40);
    int bpp = (fields & 0x40000) ? (int)rd32(dm + 104) : 8;
    int w = (fields & 0x80000) ? (int)rd32(dm + 108) : display().screen.w;
    int h = (fields & 0x100000) ? (int)rd32(dm + 112) : display().screen.h;
    plog("ChangeDisplaySettingsA: %dx%dx%d (flags 0x%x)", w, h, bpp, arg(1));
    if (!(arg(1) & 2)) set_display_mode(w, h, bpp);   // CDS_TEST no cambia nada
    RET(0, 2);                                        // DISP_CHANGE_SUCCESSFUL
}
STUB(ext_user32_dll_ExitWindowsEx) { plog("ExitWindowsEx: ignorado"); RET(0, 2); }
STUB(ext_user32_dll_WinHelpA) { RET(1, 4); }

// ---------------------------------------------------------------- DC (gdi32.cpp hace el trabajo)

STUB(ext_user32_dll_GetDC) { RET(gdi_window_dc(arg(0), true), 1); }
STUB(ext_user32_dll_GetDCEx) { RET(gdi_window_dc(arg(0), true), 3); }
STUB(ext_user32_dll_GetWindowDC) { RET(gdi_window_dc(arg(0), false), 1); }
STUB(ext_user32_dll_ReleaseDC) { gdi_release_dc(arg(1)); RET(1, 2); }
STUB(ext_user32_dll_BeginPaint) {
    u32 hwnd = arg(0), ps = arg(1);
    Wnd *w = wnd(hwnd);
    u32 dc = gdi_window_dc(hwnd, true);
    std::memset(MEM + ps, 0, 64);
    wr32(ps, dc);
    if (w) {
        bool erase = w->erase;
        Rect upd = w->upd;
        if (rect_empty(upd)) { i32 l, t, r, b; nc_insets(*w, l, t, r, b); upd = {0, 0, w->w - l - r, w->h - t - b}; }
        w->invalid = w->erase = false;
        w->upd = {};
        gdi_clip_to(dc, upd);
        if (erase && send(hwnd, WM_ERASEBKGND, dc, 0) == 0) wr32(ps + 4, 1);   // fErase
        wr_rect(ps + 8, upd);
    }
    RET(dc, 2);
}
STUB(ext_user32_dll_EndPaint) { gdi_release_dc(rd32(arg(1))); RET(1, 2); }
STUB(ext_user32_dll_FillRect) { gdi_fill_rect(arg(0), rd_rect(arg(1)), arg(2)); RET(1, 3); }

// ---------------------------------------------------------------- rectángulos

STUB(ext_user32_dll_SetRect) { wr_rect(arg(0), {(i32)arg(1), (i32)arg(2), (i32)arg(3), (i32)arg(4)}); RET(1, 5); }
STUB(ext_user32_dll_SetRectEmpty) { wr_rect(arg(0), {}); RET(1, 1); }
STUB(ext_user32_dll_CopyRect) { wr_rect(arg(0), rd_rect(arg(1))); RET(1, 2); }
STUB(ext_user32_dll_IsRectEmpty) { Rect r = rd_rect(arg(0)); RET(r.r <= r.l || r.b <= r.t, 1); }
STUB(ext_user32_dll_EqualRect) { Rect a = rd_rect(arg(0)), b = rd_rect(arg(1)); RET(a.l == b.l && a.t == b.t && a.r == b.r && a.b == b.b, 2); }
STUB(ext_user32_dll_PtInRect) { Rect r = rd_rect(arg(0)); i32 x = (i32)arg(1), y = (i32)arg(2); RET(x >= r.l && x < r.r && y >= r.t && y < r.b, 3); }
STUB(ext_user32_dll_OffsetRect) { Rect r = rd_rect(arg(0)); i32 dx = (i32)arg(1), dy = (i32)arg(2); wr_rect(arg(0), {r.l + dx, r.t + dy, r.r + dx, r.b + dy}); RET(1, 3); }
STUB(ext_user32_dll_InflateRect) { Rect r = rd_rect(arg(0)); i32 dx = (i32)arg(1), dy = (i32)arg(2); wr_rect(arg(0), {r.l - dx, r.t - dy, r.r + dx, r.b + dy}); RET(1, 3); }
STUB(ext_user32_dll_IntersectRect) {
    Rect a = rd_rect(arg(1)), b = rd_rect(arg(2));
    Rect r{std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r), std::min(a.b, b.b)};
    if (r.l >= r.r || r.t >= r.b) { wr_rect(arg(0), {}); RET(0, 3); }
    wr_rect(arg(0), r);
    RET(1, 3);
}

// ---------------------------------------------------------------- cadenas

u8 cp1252_upper(u8 c);
u8 cp1252_lower(u8 c);
STUB(ext_user32_dll_CharUpperA) {
    u32 p = arg(0);
    if (p < 0x10000) RET(cp1252_upper((u8)p), 1);
    for (u32 i = p; rd8(i); i++) wr8(i, cp1252_upper((u8)rd8(i)));
    RET(p, 1);
}
STUB(ext_user32_dll_CharLowerA) {
    u32 p = arg(0);
    if (p < 0x10000) RET(cp1252_lower((u8)p), 1);
    for (u32 i = p; rd8(i); i++) wr8(i, cp1252_lower((u8)rd8(i)));
    RET(p, 1);
}
STUB(ext_user32_dll_CharNextA) { u32 p = arg(0); RET(rd8(p) ? p + 1 : p, 1); }
// OEM (850) ↔ ANSI (1252): el juego solo lo usa con nombres de fichero; se copia tal cual
STUB(ext_user32_dll_OemToCharBuffA) { std::memmove(MEM + arg(1), MEM + arg(0), arg(2)); RET(1, 3); }
STUB(ext_user32_dll_CharToOemBuffA) { std::memmove(MEM + arg(1), MEM + arg(0), arg(2)); RET(1, 3); }
STUB(ext_user32_dll_OemToCharA) { gstrcpy(arg(1), gstr(arg(0))); RET(1, 2); }
STUB(ext_user32_dll_CharToOemA) { gstrcpy(arg(1), gstr(arg(0))); RET(1, 2); }

// wsprintfA es cdecl (variádica): no limpia argumentos
static std::string format_win(u32 fmt, u32 argp) {
    std::string out, f = gstr(fmt, 1u << 16);
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%') { out.push_back(f[i]); continue; }
        if (++i >= f.size()) break;
        if (f[i] == '%') { out.push_back('%'); continue; }
        std::string spec = "%";
        while (i < f.size() && std::strchr("-+ #0", f[i])) spec.push_back(f[i++]);
        while (i < f.size() && std::isdigit((unsigned char)f[i])) spec.push_back(f[i++]);
        if (i < f.size() && f[i] == '.') { spec.push_back(f[i++]); while (i < f.size() && std::isdigit((unsigned char)f[i])) spec.push_back(f[i++]); }
        while (i < f.size() && (f[i] == 'l' || f[i] == 'h')) i++;
        if (i >= f.size()) break;
        char c = f[i];
        char buf[512];
        u32 v = rd32(argp);
        argp += 4;
        switch (c) {
            case 'd': case 'i': snprintf(buf, sizeof buf, (spec + "d").c_str(), (int)v); break;
            case 'u': snprintf(buf, sizeof buf, (spec + "u").c_str(), v); break;
            case 'x': case 'X': snprintf(buf, sizeof buf, (spec + c).c_str(), v); break;
            case 'c': case 'C': snprintf(buf, sizeof buf, (spec + "c").c_str(), (int)(v & 0xFF)); break;
            case 's': case 'S': snprintf(buf, sizeof buf, (spec + "s").c_str(), v ? gstr(v).c_str() : "(null)"); break;
            default: buf[0] = c; buf[1] = 0;
        }
        out += buf;
    }
    return out;
}
STUB(ext_user32_dll_wsprintfA) { std::string s = format_win(arg(1), esp + 12); RET(gstrcpy(arg(0), s, 1025), 0); }
STUB(ext_user32_dll_wvsprintfA) { std::string s = format_win(arg(1), arg(2)); RET(gstrcpy(arg(0), s, 1025), 3); }

u32 find_resource(u32 module, u32 name, u32 type);
u32 resource_module(u32 hrsrc);
STUB(ext_user32_dll_LoadStringA) {
    // tablas de cadenas: bloque (id/16 + 1) del tipo RT_STRING (6), 16 cadenas UTF-16 con longitud
    u32 inst = arg(0), id = arg(1), buf = arg(2), cap = arg(3);
    u32 r = find_resource(inst, id / 16 + 1, 6);
    if (!r) r = find_resource(0x02000000u, id / 16 + 1, 6);   // MFC42 lleva sus propias cadenas
    if (!r || !cap) { if (cap) wr8(buf, 0); RET(0, 4); }
    u32 p = resource_module(r) + rd32(r);
    for (u32 i = 0; i < id % 16; i++) p += 2 + 2 * rd16(p);
    u32 len = rd16(p), n = std::min(len, cap - 1);
    for (u32 i = 0; i < n; i++) { u32 w = rd16(p + 2 + 2 * i); wr8(buf + i, w < 0x100 ? w : '?'); }
    wr8(buf + n, 0);
    RET(n, 4);
}

// ---------------------------------------------------------------- menús (el juego no usa menús de Windows)

STUB(ext_user32_dll_GetMenu) { Wnd *w = wnd(arg(0)); RET(w && !(w->style & 0x40000000u) ? w->menu : 0, 1); }
STUB(ext_user32_dll_SetMenu) {   // la barra cambia el tamaño del área cliente
    Wnd *w = wnd(arg(0));
    if (!w || (w->style & 0x40000000u)) RET(0, 2);
    w->menu = arg(1);
    owners_changed();
    send_size_move(arg(0));
    expose(window_screen_rect(arg(0)));
    RET(1, 2);
}
STUB(ext_user32_dll_GetSystemMenu) { RET(0, 2); }
STUB(ext_user32_dll_DrawMenuBar) { if (Wnd *w = wnd(arg(0))) { w->nc_dirty = true; paint_nc(arg(0)); } RET(1, 1); }

// ---------------------------------------------------------------- DrawText (gdi32.cpp)

int gdi_draw_text(u32 hdc, const std::string &text, Rect &rc, u32 fmt);
void text_watch(const std::string &s);   // core.cpp (PCF7_TEXTWATCH)
STUB(ext_user32_dll_DrawTextA) {   // (hdc, str, n, rect, format)
    text_watch(gstr(arg(1)));
    u32 n = arg(2);
    std::string s = n == 0xFFFFFFFFu ? gstr(arg(1)) : gstr(arg(1), n);
    Rect r = rd_rect(arg(3));
    static const char *tw = std::getenv("PCF7_TEXTWATCH");
    if (tw && s.find(tw) != std::string::npos) plog("DrawText «%s» n=%d rect %d,%d-%d,%d fmt 0x%x", s.c_str(), (int)n, r.l, r.t, r.r, r.b, arg(4));
    int h = gdi_draw_text(arg(0), s, r, arg(4));
    if (arg(4) & 0x400) wr_rect(arg(3), r);   // DT_CALCRECT
    RET((u32)h, 5);
}

// ---------------------------------------------------------------- para los controles (controls.cpp)

void for_each_child(u32 parent, const std::function<void(u32)> &f) {
    Wnd *w = wnd(parent);
    if (!w) return;
    std::vector<u32> kids = w->children;
    for (u32 c : kids) if (wnd(c)) f(c);
}

extern "C" void ext_user32_dll_ShowWindow(void);

extern "C" void ext_user32_dll_CreateWindowExA(void);
u32 wm_send(u32 h, u32 m, u32 wp, u32 lp) { return send(h, m, wp, lp); }
void wm_post(u32 h, u32 m, u32 wp, u32 lp) { post(h, m, wp, lp); }
bool wm_exists(u32 h) { return wnd(h) != nullptr; }
u32 wm_style(u32 h) { Wnd *w = wnd(h); return w ? w->style : 0; }
void wm_set_style(u32 h, u32 s) { if (Wnd *w = wnd(h)) { w->style = s; owners_changed(); } }
u32 wm_exstyle(u32 h) { Wnd *w = wnd(h); return w ? w->exstyle : 0; }
u32 wm_parent(u32 h) { Wnd *w = wnd(h); return w ? ((w->style & 0x40000000u) ? w->parent : w->owner) : 0; }
u32 wm_id(u32 h) { Wnd *w = wnd(h); return w ? w->id : 0; }
std::string &wm_text(u32 h) { static std::string none; Wnd *w = wnd(h); if (!w) { none.clear(); return none; } return w->text; }
Rect wm_client(u32 h) {
    Wnd *w = wnd(h);
    if (!w) return {};
    i32 l, t, r, b;
    nc_insets(*w, l, t, r, b);
    return {0, 0, std::max(0, w->w - l - r), std::max(0, w->h - t - b)};
}
Rect wm_screen(u32 h, bool client) { Rect r; wnd_screen_rect(h, client, r); return r; }
void wm_invalidate(u32 h, bool erase) { invalidate(h, nullptr, erase); }
void wm_update(u32 h) { paint_now(h); }
u32 wm_capture() { return g_capture; }
void wm_set_capture(u32 h) { g_capture = h; }
u32 wm_focus() { return g_focus; }
void wm_set_focus(u32 h) { set_focus(h); }
bool wm_enabled(u32 h) { Wnd *w = wnd(h); return w && !(w->style & 0x08000000u); }
bool wm_visible(u32 h) { return visible_chain(h); }
u32 wm_create(u32 ex, const char *cls, const std::string &title, u32 style, i32 x, i32 y, i32 w, i32 h, u32 parent, u32 id) {
    u32 c = heap_alloc((u32)std::strlen(cls) + 1), t = heap_alloc((u32)title.size() + 1);
    gstrcpy(c, cls);
    gstrcpy(t, title);
    u32 r = call_stub(ext_user32_dll_CreateWindowExA, {ex, c, t, style, (u32)x, (u32)y, (u32)w, (u32)h, parent, id, 0x400000, 0});
    heap_free(c);
    heap_free(t);
    return r;
}
void wm_destroy(u32 h) { destroy_window(h); }
void wm_move(u32 h, i32 x, i32 y, i32 w, i32 hh) { set_pos(h, x, y, w, hh, 4); }   // SWP_NOZORDER
void wm_show(u32 h, bool show) {
    call_stub(ext_user32_dll_ShowWindow, {h, show ? 4u : 0u});   // SW_SHOWNOACTIVATE / SW_HIDE
}
void wm_raise(u32 h) {
    g_top.erase(std::remove(g_top.begin(), g_top.end(), h), g_top.end());
    if (wnd(h)) { g_top.push_back(h); owners_changed(); if (visible_chain(h)) expose(window_screen_rect(h)); }
}
u32 wm_set_timer(u32 hwnd, u32 id, u32 ms) {
    for (auto &t : g_timers) if (t.hwnd == hwnd && t.id == id) { t.period = ms; t.next = ticks_ms() + ms; return id; }
    g_timers.push_back({hwnd, id, ms, 0, ticks_ms() + ms});
    return id;
}
void wm_kill_timer(u32 hwnd, u32 id) {
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](const Timer &t) { return t.hwnd == hwnd && t.id == id; }), g_timers.end());
}
void wm_modal_loop(const std::function<bool()> &done) {
    while (!done()) {
        Msg m;
        if (!get_msg(m, 0, 0, 0, true)) { pump_input(true, next_timer_wait()); continue; }
        if (m.msg == WM_QUIT) { g_quit = true; g_quit_code = m.wp; break; }
        g_last = m;
        if (m.msg == WM_TIMER && m.lp) guest_call(m.lp, {m.hwnd, m.msg, m.wp, ticks_ms()});
        else send(m.hwnd, m.msg, m.wp, m.lp);
    }
}

// ---------------------------------------------------------------- MFC 6.0 que no está en la MFC42 4.21
// PCF7 Plus se compiló con Visual C++ 6 y usa dos funciones que la MFC42 del CD de PCF7 (4.21) no exporta. Con esa
// MFC y su MSVCRT (las del juego base, ya probadas) solo faltan estas: CWnd::ClientToScreen(LPRECT) y
// CWnd::ScreenToClient(LPRECT) (thiscall: ecx = CWnd, m_hWnd en +0x20; las dos esquinas del rectángulo).
STUB(ext_mfc42_dll_ord6605) {
    u32 hwnd = rd32(ecx + 0x20), r = arg(0);
    call_stub(ext_user32_dll_ClientToScreen, {hwnd, r});
    call_stub(ext_user32_dll_ClientToScreen, {hwnd, r + 8});
    RET(0, 1);
}
STUB(ext_mfc42_dll_ord6880) {
    u32 hwnd = rd32(ecx + 0x20), r = arg(0);
    call_stub(ext_user32_dll_ScreenToClient, {hwnd, r});
    call_stub(ext_user32_dll_ScreenToClient, {hwnd, r + 8});
    RET(0, 1);
}
