// PCF7 — utilidades comunes de los controles estándar de Windows implementados en el host
// (controls.cpp, edit.cpp, listbox.cpp, msgbox.cpp).
#pragma once
#include "win.h"

#include <string>

extern "C" {
void ext_user32_dll_DefWindowProcA(void);
void ext_user32_dll_BeginPaint(void);
void ext_user32_dll_EndPaint(void);
void ext_gdi32_dll_GetStockObject(void);
void ext_user32_dll_GetKeyState(void);
void ext_user32_dll_SetScrollInfo(void);
}

namespace ctl {

constexpr u32 WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_MOVE = 0x03, WM_SIZE = 0x05, WM_SETFOCUS = 0x07, WM_KILLFOCUS = 0x08,
              WM_ENABLE = 0x0A, WM_SETREDRAW = 0x0B, WM_SETTEXT = 0x0C, WM_GETTEXT = 0x0D, WM_GETTEXTLENGTH = 0x0E,
              WM_PAINT = 0x0F, WM_CLOSE = 0x10, WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_SETCURSOR = 0x20,
              WM_DRAWITEM = 0x2B, WM_MEASUREITEM = 0x2C, WM_DELETEITEM = 0x2D, WM_SETFONT = 0x30, WM_GETFONT = 0x31,
              WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCHITTEST = 0x84, WM_GETDLGCODE = 0x87, WM_KEYDOWN = 0x100,
              WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_COMMAND = 0x111, WM_TIMER = 0x113, WM_HSCROLL = 0x114,
              WM_VSCROLL = 0x115, WM_CTLCOLOREDIT = 0x133, WM_CTLCOLORLISTBOX = 0x134, WM_CTLCOLORBTN = 0x135,
              WM_CTLCOLORDLG = 0x136, WM_CTLCOLORSCROLLBAR = 0x137, WM_CTLCOLORSTATIC = 0x138, WM_MOUSEMOVE = 0x200,
              WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203, WM_RBUTTONDOWN = 0x204,
              WM_MOUSEWHEEL = 0x20A, WM_CUT = 0x300, WM_COPY = 0x301, WM_PASTE = 0x302, WM_CLEAR = 0x303, WM_UNDO = 0x304;
constexpr u32 WS_CHILD = 0x40000000u, WS_VISIBLE = 0x10000000u, WS_DISABLED = 0x08000000u, WS_VSCROLL = 0x200000u,
              WS_HSCROLL = 0x100000u, WS_BORDER = 0x800000u, WS_POPUP = 0x80000000u, WS_TABSTOP = 0x10000u;
constexpr int COLOR_SCROLLBAR = 0, COLOR_WINDOW = 5, COLOR_WINDOWTEXT = 8, COLOR_HIGHLIGHT = 13, COLOR_HIGHLIGHTTEXT = 14,
              COLOR_BTNFACE = 15, COLOR_BTNSHADOW = 16, COLOR_GRAYTEXT = 17, COLOR_BTNTEXT = 18, COLOR_BTNHIGHLIGHT = 20,
              COLOR_3DDKSHADOW = 21, COLOR_3DLIGHT = 22;

inline u32 C(int i) { return sys_color(i); }
inline i32 lo16(u32 v) { return (i32)(i16)(v & 0xFFFF); }
inline i32 hi16(u32 v) { return (i32)(i16)(v >> 16); }
inline u32 make_lp(i32 a, i32 b) { return ((u32)(u16)b << 16) | (u16)a; }
inline u32 stock(u32 i) { return call_stub(ext_gdi32_dll_GetStockObject, {i}); }

// WM_COMMAND al padre con el código de notificación
inline void notify(u32 hwnd, u32 code) {
    u32 p = wm_parent(hwnd);
    if (p) wm_send(p, WM_COMMAND, (code << 16) | (wm_id(hwnd) & 0xFFFF), hwnd);
}

// DC del área cliente con la fuente del control (o la del sistema)
struct DCx {
    u32 dc;
    DCx(u32 hwnd, u32 font) : dc(gdi_window_dc(hwnd, true)) { gdi_select(dc, font ? font : stock(13)); }
    ~DCx() { gdi_release_dc(dc); }
    operator u32() const { return dc; }
};

// BeginPaint/EndPaint (valida la zona pendiente y recorta el DC a ella)
struct Paint {
    u32 hwnd, ps, dc;
    Paint(u32 h, u32 font) : hwnd(h), ps(heap_alloc(64, true)) {
        dc = call_stub(ext_user32_dll_BeginPaint, {hwnd, ps});
        gdi_select(dc, font ? font : stock(13));
    }
    ~Paint() { call_stub(ext_user32_dll_EndPaint, {hwnd, ps}); heap_free(ps); }
    operator u32() const { return dc; }
};

// el padre elige colores y fondo con WM_CTLCOLOR*: devuelve su brocha (0 si no contesta)
inline u32 ctl_color(u32 hwnd, u32 dc, u32 msg) {
    u32 p = wm_parent(hwnd);
    return p ? wm_send(p, msg, dc, hwnd) : 0;
}
inline void fill_bg(u32 dc, const Rect &r, u32 brush, u32 fallback) {
    if (brush) gdi_fill_rect(dc, r, brush);
    else gdi_fill_color(dc, r, fallback);
}
// texto (con & de atajo) en gris grabado si está deshabilitado; si color es ~0u usa el que ya tenga el DC
inline void draw_label(u32 dc, const std::string &text, Rect r, u32 fmt, bool enabled, u32 color) {
    gdi_set_bk(dc, 0, 1);
    if (enabled) { if (color != ~0u) gdi_set_text_color(dc, color); gdi_draw_text(dc, text, r, fmt); return; }
    Rect r1{r.l + 1, r.t + 1, r.r + 1, r.b + 1};
    gdi_set_text_color(dc, C(COLOR_BTNHIGHLIGHT));
    gdi_draw_text(dc, text, r1, fmt);
    gdi_set_text_color(dc, C(COLOR_BTNSHADOW));
    gdi_draw_text(dc, text, r, fmt);
}

inline bool key_down(u32 vk) { return call_stub(ext_user32_dll_GetKeyState, {vk}) & 0x8000; }

// el resto del mensaje lo atiende DefWindowProc (los argumentos siguen en la pila)
inline void def_proc() { ext_user32_dll_DefWindowProcA(); }

}  // namespace ctl

// controls.cpp: barras de desplazamiento compartidas por SCROLLBAR, EDIT y LISTBOX
struct SInfo { i32 min = 0, max = 100, page = 0, pos = 0, track = 0; bool disabled = false; };
SInfo &sb_info(u32 hwnd, int bar);                    // bar: 0 SB_HORZ, 1 SB_VERT, 2 SB_CTL
void sb_set(u32 hwnd, int bar, const SInfo &s, bool redraw);
void sb_draw(u32 dc, const Rect &r, bool vert, const SInfo &s, int pressed);
void sb_nc_paint(u32 hwnd, u32 window_dc);            // barras WS_HSCROLL/WS_VSCROLL de una ventana
void sb_nc_click(u32 hwnd, int bar, i32 sx, i32 sy);  // pulsación en una de ellas (coordenadas de pantalla)
bool sb_track_event(int kind, i32 sx, i32 sy);        // ratón mientras se sigue una barra (0 mover, 1 soltar, 2 reloj)
void for_each_child(u32 parent, const std::function<void(u32)> &f);
void draw_bitmap_center(u32 dc, u32 hbm, const Rect &r);
// caret
void caret_create(u32 hwnd, i32 w, i32 h);
void caret_destroy();
void caret_pos(i32 x, i32 y);
void caret_show(bool show);
bool message_box_key(u32 hwnd, u32 vk);              // msgbox.cpp: Intro/Escape/flechas en una caja de mensaje
void combo_list_changed(u32 combo);                  // listbox.cpp
