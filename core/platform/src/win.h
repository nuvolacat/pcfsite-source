// PCF7 — interfaz interna entre user32 (ventanas) y gdi32 (dibujo).
#pragma once
#include "plat.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

struct Rect { i32 l = 0, t = 0, r = 0, b = 0; i32 w() const { return r - l; } i32 h() const { return b - t; } };

inline Rect rd_rect(u32 p) { return {(i32)rd32(p), (i32)rd32(p + 4), (i32)rd32(p + 8), (i32)rd32(p + 12)}; }
inline void wr_rect(u32 p, const Rect &r) { wr32(p, (u32)r.l); wr32(p + 4, (u32)r.t); wr32(p + 8, (u32)r.r); wr32(p + 12, (u32)r.b); }

inline Rect rect_and(const Rect &a, const Rect &b) { return {std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r), std::min(a.b, b.b)}; }
inline bool rect_empty(const Rect &a) { return a.r <= a.l || a.b <= a.t; }
inline Rect rect_or(const Rect &a, const Rect &b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    return {std::min(a.l, b.l), std::min(a.t, b.t), std::max(a.r, b.r), std::max(a.b, b.b)};
}

// user32 → gdi32
u32 gdi_window_dc(u32 hwnd, bool client);       // DC de pantalla recortado a la ventana (y a lo que se ve de ella)
void gdi_release_dc(u32 hdc);
void gdi_fill_rect(u32 hdc, const Rect &r, u32 hbrush);
u32 gdi_sys_color_brush(int index);
void gdi_fill_color(u32 hdc, const Rect &r, u32 colorref);
void gdi_clip_to(u32 hdc, const Rect &r);                     // recorta el DC (coordenadas del DC)
// borde 3D (DrawEdge): bordes BDR_* y lados BF_*; si fill, rellena el interior con el color de los botones
void gdi_draw_edge(u32 hdc, Rect r, u32 edge, u32 flags);
void gdi_text(u32 hdc, i32 x, i32 y, const std::string &s, u32 colorref, bool bold, const Rect *clip);
int gdi_text_width(const std::string &s, bool bold);
void gdi_hline(u32 hdc, i32 x0, i32 x1, i32 y, u32 colorref);
void gdi_vline(u32 hdc, i32 x, i32 y0, i32 y1, u32 colorref);

// gdi32 para los controles: objetos y colores de un DC
u32 gdi_select(u32 hdc, u32 hobj);                            // SelectObject
void gdi_set_text_color(u32 hdc, u32 colorref);
void gdi_set_bk(u32 hdc, u32 colorref, int mode);             // mode: 1 TRANSPARENT, 2 OPAQUE
int gdi_draw_text(u32 hdc, const std::string &text, Rect &rc, u32 fmt);   // DrawText
int gdi_dc_text_width(u32 hdc, const std::string &s);         // con la fuente del DC
int gdi_dc_text_height(u32 hdc);
void gdi_invert(u32 hdc, const Rect &r);                      // InvertRect
void gdi_focus_rect(u32 hdc, const Rect &r);                  // DrawFocusRect (punteado)

// user32 para los controles (controls.cpp)
u32 wm_send(u32 hwnd, u32 msg, u32 wp, u32 lp);
void wm_post(u32 hwnd, u32 msg, u32 wp, u32 lp);
bool wm_exists(u32 hwnd);
u32 wm_style(u32 hwnd);
void wm_set_style(u32 hwnd, u32 style);
u32 wm_exstyle(u32 hwnd);
u32 wm_parent(u32 hwnd);            // padre (hijas) o propietario
u32 wm_id(u32 hwnd);
u32 wm_dlg_item(u32 hwnd, u32 id);  // hija con ese identificador (GetDlgItem)
std::string &wm_text(u32 hwnd);     // texto guardado de la ventana (lo que maneja DefWindowProc)
Rect wm_client(u32 hwnd);           // {0, 0, ancho, alto} del área cliente
Rect wm_screen(u32 hwnd, bool client);
void wm_invalidate(u32 hwnd, bool erase);
void wm_update(u32 hwnd);           // pinta ya lo pendiente
u32 wm_capture();
void wm_set_capture(u32 hwnd);
u32 wm_focus();
void wm_set_focus(u32 hwnd);
bool wm_enabled(u32 hwnd);
bool wm_visible(u32 hwnd);
u32 wm_create(u32 exstyle, const char *cls, const std::string &title, u32 style, i32 x, i32 y, i32 w, i32 h, u32 parent, u32 id);
void wm_destroy(u32 hwnd);
void wm_move(u32 hwnd, i32 x, i32 y, i32 w, i32 h);
void wm_show(u32 hwnd, bool show);
void wm_raise(u32 hwnd);
u32 wm_set_timer(u32 hwnd, u32 id, u32 ms);
void wm_kill_timer(u32 hwnd, u32 id);
// bucle modal: atiende mensajes hasta que done() sea cierto (WM_QUIT se vuelve a poner en la cola)
void wm_modal_loop(const std::function<bool()> &done);

// gdi32 → user32: qué ventana se ve en cada píxel de la pantalla (para recortar los DC de ventana)
const u32 *wm_owner_map(int &w, int &h);
void wm_dc_owners(u32 hwnd, std::vector<u32> &out);         // la ventana y, si no recorta a sus hijas, ellas

// gdi32 → user32
bool wnd_screen_rect(u32 hwnd, bool client, Rect &out);   // rectángulo de la ventana en coordenadas de pantalla
u32 sys_color(int index);
