// PCF7 — controles estándar de Windows en el host (1): BUTTON, STATIC y SCROLLBAR; las barras de
// desplazamiento de las ventanas (WS_HSCROLL/WS_VSCROLL) y sus API; el cursor de texto (caret) y las
// funciones de dibujo de USER32 (DrawEdge, DrawFrameControl, DrawFocusRect, FrameRect…).
//
// Los programas de Visual Basic (ProQuinielas) construyen sus controles intrínsecos subclasificando las
// clases del sistema, y los OCX y MFC también las usan. El aspecto y el comportamiento siguen a Windows 98:
// colores del sistema, bordes 3D, notificaciones WM_COMMAND, WM_CTLCOLOR* para que el padre elija colores
// y dibujo por el propietario (WM_DRAWITEM). EDIT, LISTBOX y COMBOBOX están en edit.cpp y listbox.cpp.
#include "ctlutil.h"
#include "display.h"

#include <algorithm>
#include <map>

u32 ticks_ms();
void set_last_error(u32 e);

using namespace ctl;

namespace {

struct Btn { u32 font = 0, check = 0, image = 0; bool pushed = false, tracking = false, focus = false; };
std::map<u32, Btn> g_btn;

// ---------------------------------------------------------------- BUTTON

u32 btype(u32 hwnd) { return wm_style(hwnd) & 0xF; }
bool is_check(u32 t) { return t == 2 || t == 3 || t == 5 || t == 6; }
bool is_radio(u32 t) { return t == 4 || t == 9; }

void check_mark(u32 dc, i32 x, i32 y, u32 color) {   // la marca de Windows: 7 columnas en un cuadro de 13
    static const int pts[][2] = {{0, 2}, {1, 3}, {2, 4}, {3, 3}, {4, 2}, {5, 1}, {6, 0}};
    for (auto &p : pts) gdi_fill_color(dc, {x + p[0], y + p[1], x + p[0] + 1, y + p[1] + 3}, color);
}
void radio_shape(u32 dc, i32 x, i32 y, bool checked, bool pressed, bool enabled) {   // círculo de 12 con relieve
    static const char *m[12] = {"    1111    ", "  11333311  ", " 13      42 ", " 13      42 ", "13        42", "13        42",
                                "13        42", "13        42", " 13      42 ", " 13      42 ", "  44444422  ", "    2222    "};
    u32 face = pressed || !enabled ? C(COLOR_BTNFACE) : C(COLOR_WINDOW);
    for (int yy = 0; yy < 12; yy++) {
        int first = -1, last = -1;
        for (int xx = 0; xx < 12; xx++) if (m[yy][xx] != ' ') { if (first < 0) first = xx; last = xx; }
        if (first >= 0 && last - first > 1) gdi_fill_color(dc, {x + first + 1, y + yy, x + last, y + yy + 1}, face);
        for (int xx = 0; xx < 12; xx++) {
            u32 c;
            switch (m[yy][xx]) {
            case '1': c = C(COLOR_BTNSHADOW); break;
            case '2': c = C(COLOR_BTNHIGHLIGHT); break;
            case '3': c = C(COLOR_3DDKSHADOW); break;
            case '4': c = C(COLOR_3DLIGHT); break;
            default: continue;
            }
            gdi_fill_color(dc, {x + xx, y + yy, x + xx + 1, y + yy + 1}, c);
        }
    }
    if (checked) {
        u32 dot = enabled ? C(COLOR_WINDOWTEXT) : C(COLOR_BTNSHADOW);
        gdi_fill_color(dc, {x + 5, y + 4, x + 7, y + 8}, dot);
        gdi_fill_color(dc, {x + 4, y + 5, x + 8, y + 7}, dot);
    }
}

void button_paint(u32 hwnd, u32 dc) {
    Btn &c = g_btn[hwnd];
    Rect r = wm_client(hwnd);
    u32 st = wm_style(hwnd), t = st & 0xF;
    bool en = wm_enabled(hwnd);
    const std::string text = wm_text(hwnd);
    if (t == 0xB) {   // BS_OWNERDRAW: WM_DRAWITEM al padre
        u32 dis = heap_alloc(48, true);
        wr32(dis, 4); wr32(dis + 4, wm_id(hwnd)); wr32(dis + 12, 1);
        wr32(dis + 16, (c.pushed ? 1u : 0u) | (en ? 0u : 4u) | (c.focus ? 0x10u : 0u));
        wr32(dis + 20, hwnd); wr32(dis + 24, dc); wr_rect(dis + 28, r);
        if (u32 p = wm_parent(hwnd)) wm_send(p, WM_DRAWITEM, wm_id(hwnd), dis);
        heap_free(dis);
        return;
    }
    if (t == 7) {   // BS_GROUPBOX
        u32 br = ctl_color(hwnd, dc, WM_CTLCOLORSTATIC);
        int th = gdi_dc_text_height(dc);
        Rect fr{r.l, r.t + th / 2, r.r, r.b};
        gdi_draw_edge(dc, fr, 6, 0xF);   // EDGE_ETCHED
        if (!text.empty()) {
            int tw = gdi_dc_text_width(dc, text);
            Rect tr{r.l + 8, r.t, r.l + 10 + tw, r.t + th};
            fill_bg(dc, tr, br, C(COLOR_BTNFACE));
            draw_label(dc, text, {tr.l + 1, tr.t, tr.r, tr.b}, 0x20, en, ~0u);
        }
        return;
    }
    if (is_check(t) || is_radio(t)) {
        u32 br = ctl_color(hwnd, dc, WM_CTLCOLORSTATIC);
        fill_bg(dc, r, br, C(COLOR_BTNFACE));
        bool left_text = st & 0x20;
        i32 by = r.t + (r.h() - 13) / 2, bx = left_text ? r.r - 13 : r.l;
        if (is_radio(t)) radio_shape(dc, bx, by, c.check == 1, c.pushed, en);
        else {
            gdi_draw_edge(dc, {bx, by, bx + 13, by + 13}, 10, 0xF);   // EDGE_SUNKEN
            u32 face = c.pushed || !en ? C(COLOR_BTNFACE) : c.check == 2 ? C(COLOR_3DLIGHT) : C(COLOR_WINDOW);
            gdi_fill_color(dc, {bx + 2, by + 2, bx + 11, by + 11}, face);
            if (c.check) check_mark(dc, bx + 3, by + 3, c.check == 2 || !en ? C(COLOR_BTNSHADOW) : C(COLOR_WINDOWTEXT));
        }
        Rect tr = left_text ? Rect{r.l, r.t, r.r - 17, r.b} : Rect{r.l + 17, r.t, r.r, r.b};
        u32 fmt = (st & 0x2000 ? 0x10u : 0x20u | 0x4u) | ((st & 0x300) == 0x200 ? 2u : (st & 0x300) == 0x300 ? 1u : 0u);
        draw_label(dc, text, tr, fmt, en, ~0u);
        if (c.focus && !text.empty()) {
            Rect calc = tr;
            gdi_draw_text(dc, text, calc, (fmt & ~0x4u) | 0x400);
            i32 y0 = tr.t + (tr.h() - calc.h()) / 2;
            gdi_focus_rect(dc, {tr.l - 1, y0 - 1, tr.l + calc.w() + 2, y0 + calc.h() + 1});
        }
        return;
    }
    // botón de pulsar (y BS_PUSHLIKE)
    ctl_color(hwnd, dc, WM_CTLCOLORBTN);
    gdi_set_text_color(dc, C(COLOR_BTNTEXT));
    bool down = c.pushed || ((st & 0x1000) && c.check);
    Rect b = r;
    if (t == 1 || (c.focus && t == 0)) { gdi_draw_edge(dc, b, 1, 0x800F); b = {b.l + 1, b.t + 1, b.r - 1, b.b - 1}; }   // marco del botón por defecto
    gdi_fill_color(dc, b, C(COLOR_BTNFACE));
    if (down) gdi_draw_edge(dc, b, 1, 0x4000 | 0xF);
    else gdi_draw_edge(dc, b, 5, 0xF);   // EDGE_RAISED
    Rect tr{b.l + 2, b.t + 2, b.r - 2, b.b - 2};
    if (down) tr = {tr.l + 1, tr.t + 1, tr.r + 1, tr.b + 1};
    if (c.image && (st & 0xC0)) draw_bitmap_center(dc, c.image, tr);
    else {
        u32 fmt = (st & 0x2000 ? 0x10u : 0x20u) | ((st & 0x300) == 0x100 ? 0u : (st & 0x300) == 0x200 ? 2u : 1u) |
                  ((st & 0xC00) == 0x400 ? 0u : (st & 0xC00) == 0x800 ? 8u : 4u);
        draw_label(dc, text, tr, fmt, en, ~0u);
    }
    if (c.focus) gdi_focus_rect(dc, {b.l + 3, b.t + 3, b.r - 3, b.b - 3});
}

void button_click(u32 hwnd) {
    u32 t = btype(hwnd);
    Btn &c = g_btn[hwnd];
    if (t == 3) c.check = c.check ? 0 : 1;            // BS_AUTOCHECKBOX
    else if (t == 6) c.check = (c.check + 1) % 3;     // BS_AUTO3STATE
    else if (t == 9) {                                // BS_AUTORADIOBUTTON: desmarca las demás del padre
        if (u32 p = wm_parent(hwnd))
            for_each_child(p, [&](u32 k) {
                if (k != hwnd && btype(k) == 9 && g_btn.count(k) && g_btn[k].check) { g_btn[k].check = 0; wm_invalidate(k, true); }
            });
        c.check = 1;
    }
    wm_invalidate(hwnd, true);
    notify(hwnd, 0);   // BN_CLICKED
}

void button_proc() {
    u32 hwnd = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    Btn &c = g_btn[hwnd];
    u32 t = btype(hwnd);
    switch (msg) {
    case WM_NCDESTROY: g_btn.erase(hwnd); break;
    case WM_PAINT: { Paint p(hwnd, c.font); button_paint(hwnd, p); RET(0, 4); }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_SETFONT: c.font = wp; if (lp) wm_invalidate(hwnd, true); RET(0, 4);
    case WM_GETFONT: RET(c.font, 4);
    case WM_SETTEXT: wm_text(hwnd) = gstr(lp); wm_invalidate(hwnd, true); RET(1, 4);
    case WM_ENABLE: wm_invalidate(hwnd, true); RET(0, 4);
    case WM_SETFOCUS: c.focus = true; wm_invalidate(hwnd, true); if (wm_style(hwnd) & 0x4000) notify(hwnd, 6); RET(0, 4);
    case WM_KILLFOCUS:
        c.focus = false;
        if (c.tracking) { c.pushed = c.tracking = false; if (wm_capture() == hwnd) wm_set_capture(0); }
        wm_invalidate(hwnd, true);
        if (wm_style(hwnd) & 0x4000) notify(hwnd, 7);
        RET(0, 4);
    case WM_GETDLGCODE:
        if (t == 7) RET(0x100, 4);
        if (is_radio(t)) RET(0x40 | 0x2000, 4);
        if (t == 1) RET(0x10 | 0x2000, 4);
        if (t == 0) RET(0x20 | 0x2000, 4);
        RET(0x2000, 4);
    case WM_NCHITTEST: if (t == 7) RET((u32)-1, 4); break;   // HTTRANSPARENT: el marco de grupo no recibe el ratón
    case WM_LBUTTONDBLCLK:
        if ((wm_style(hwnd) & 0x4000) || is_radio(t)) { notify(hwnd, 5); RET(0, 4); }   // BN_DOUBLECLICKED
        [[fallthrough]];
    case WM_LBUTTONDOWN:
        if (t == 7) RET(0, 4);
        wm_set_capture(hwnd);
        if (wm_focus() != hwnd) wm_set_focus(hwnd);
        c.pushed = c.tracking = true;
        wm_invalidate(hwnd, true);
        wm_update(hwnd);
        RET(0, 4);
    case WM_MOUSEMOVE:
        if (c.tracking) {
            Rect r = wm_client(hwnd);
            bool in = lo16(lp) >= 0 && lo16(lp) < r.r && hi16(lp) >= 0 && hi16(lp) < r.b;
            if (in != c.pushed) { c.pushed = in; wm_invalidate(hwnd, true); }
        }
        RET(0, 4);
    case WM_LBUTTONUP:
        if (c.tracking) {
            c.tracking = false;
            if (wm_capture() == hwnd) wm_set_capture(0);
            bool was = c.pushed;
            c.pushed = false;
            wm_invalidate(hwnd, true);
            if (was) button_click(hwnd);
        }
        RET(0, 4);
    case WM_KEYDOWN: {
        if (message_box_key(hwnd, wp)) RET(0, 4);
        if (wp == 0x20 && t != 7) { c.pushed = true; wm_invalidate(hwnd, true); RET(0, 4); }
        break;
    }
    case WM_KEYUP: if (wp == 0x20 && c.pushed) { c.pushed = false; wm_invalidate(hwnd, true); button_click(hwnd); RET(0, 4); } break;
    case WM_CHAR: if (wp == 0x20) RET(0, 4); break;
    case 0xF0: RET(c.check, 4);                                                               // BM_GETCHECK
    case 0xF1: if (c.check != wp) { c.check = wp; wm_invalidate(hwnd, true); } RET(0, 4);     // BM_SETCHECK
    case 0xF2: RET(c.check | (c.pushed ? 4u : 0u) | (c.focus ? 8u : 0u), 4);                  // BM_GETSTATE
    case 0xF3: if (c.pushed != !!wp) { c.pushed = wp != 0; wm_invalidate(hwnd, true); } RET(0, 4);   // BM_SETSTATE
    case 0xF4: wm_set_style(hwnd, (wm_style(hwnd) & ~0xFFFFu) | (wp & 0xFFFF)); if (lp) wm_invalidate(hwnd, true); RET(0, 4);
    case 0xF5: button_click(hwnd); RET(0, 4);                                                 // BM_CLICK
    case 0xF6: RET(c.image, 4);                                                               // BM_GETIMAGE
    case 0xF7: { u32 o = c.image; c.image = lp; wm_invalidate(hwnd, true); RET(o, 4); }        // BM_SETIMAGE
    }
    def_proc();
}

// ---------------------------------------------------------------- STATIC

struct Sta { u32 font = 0, image = 0; };
std::map<u32, Sta> g_sta;

void static_proc() {
    u32 hwnd = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    Sta &c = g_sta[hwnd];
    u32 st = wm_style(hwnd), t = st & 0x1F;
    switch (msg) {
    case WM_NCDESTROY: g_sta.erase(hwnd); break;
    case WM_PAINT: {
        Paint dc(hwnd, c.font);
        Rect r = wm_client(hwnd);
        switch (t) {
        case 4: gdi_fill_color(dc, r, C(COLOR_WINDOWTEXT)); break;    // SS_BLACKRECT
        case 5: gdi_fill_color(dc, r, C(COLOR_BTNSHADOW)); break;     // SS_GRAYRECT
        case 6: gdi_fill_color(dc, r, C(COLOR_BTNHIGHLIGHT)); break;  // SS_WHITERECT
        case 7: gdi_draw_edge(dc, r, 1, 0x800F); break;               // SS_BLACKFRAME
        case 8: gdi_draw_edge(dc, r, 2, 0x400F); break;               // SS_GRAYFRAME
        case 9: gdi_draw_edge(dc, r, 6, 0x800F); break;               // SS_WHITEFRAME
        case 0x10: gdi_draw_edge(dc, {r.l, r.t, r.r, r.t + 2}, 6, 0x2); break;   // SS_ETCHEDHORZ
        case 0x11: gdi_draw_edge(dc, {r.l, r.t, r.l + 2, r.b}, 6, 0x1); break;   // SS_ETCHEDVERT
        case 0x12: gdi_draw_edge(dc, r, 6, 0xF); break;               // SS_ETCHEDFRAME
        case 0xD: {                                                    // SS_OWNERDRAW
            u32 dis = heap_alloc(48, true);
            wr32(dis, 5); wr32(dis + 4, wm_id(hwnd)); wr32(dis + 12, 1);
            wr32(dis + 20, hwnd); wr32(dis + 24, dc); wr_rect(dis + 28, r);
            if (u32 p = wm_parent(hwnd)) wm_send(p, WM_DRAWITEM, wm_id(hwnd), dis);
            heap_free(dis);
            break;
        }
        case 3: case 0xE: {                                            // SS_ICON / SS_BITMAP
            u32 br = ctl_color(hwnd, dc, WM_CTLCOLORSTATIC);
            fill_bg(dc, r, br, C(COLOR_BTNFACE));
            if (c.image) draw_bitmap_center(dc, c.image, r);
            break;
        }
        default: {
            u32 br = ctl_color(hwnd, dc, WM_CTLCOLORSTATIC);
            fill_bg(dc, r, br, C(COLOR_BTNFACE));
            u32 fmt = (t == 1 ? 1u : t == 2 ? 2u : 0u) | (t == 0xB || t == 0xC ? 0x20u : 0x10u) | ((st & 0x80) ? 0x800u : 0u);
            if (st & 0x200) fmt = (fmt & ~0x10u) | 0x4 | 0x20;   // SS_CENTERIMAGE
            draw_label(dc, wm_text(hwnd), r, fmt, wm_enabled(hwnd), ~0u);
        }
        }
        if (st & 0x1000) gdi_draw_edge(dc, r, 2, 0xF);   // SS_SUNKEN
        RET(0, 4);
    }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_SETFONT: c.font = wp; if (lp) wm_invalidate(hwnd, true); RET(0, 4);
    case WM_GETFONT: RET(c.font, 4);
    case WM_SETTEXT: wm_text(hwnd) = gstr(lp); wm_invalidate(hwnd, true); RET(1, 4);
    case WM_ENABLE: wm_invalidate(hwnd, true); RET(0, 4);
    case WM_NCHITTEST: RET((st & 0x100) ? 1u : (u32)-1, 4);   // sin SS_NOTIFY es transparente al ratón
    case WM_LBUTTONDOWN: if (st & 0x100) notify(hwnd, 0); RET(0, 4);    // STN_CLICKED
    case WM_LBUTTONDBLCLK: if (st & 0x100) notify(hwnd, 1); RET(0, 4);  // STN_DBLCLK
    case WM_GETDLGCODE: RET(0x100, 4);
    case 0x170: case 0x172: { u32 o = c.image; c.image = msg == 0x170 ? wp : lp; wm_invalidate(hwnd, true); RET(o, 4); }   // STM_SETICON / STM_SETIMAGE
    case 0x171: case 0x173: RET(c.image, 4);
    }
    def_proc();
}

// ---------------------------------------------------------------- barras de desplazamiento

std::map<std::pair<u32, int>, SInfo> g_sb;   // (ventana, barra: 0 horizontal, 1 vertical, 2 control)
constexpr i32 SB_W = 16;

i32 sb_maxpos(const SInfo &s) { return std::max(s.min, s.max - std::max(0, s.page - 1)); }

struct SGeo { i32 arrow, track0, track1, thumb0, thumb1; };
SGeo sb_geo(const SInfo &s, i32 len, i32 thick) {
    SGeo g;
    g.arrow = std::min(thick, len / 2);
    g.track0 = g.arrow;
    g.track1 = len - g.arrow;
    i32 tl = g.track1 - g.track0, range = s.max - s.min + 1;
    i32 th = s.page ? std::max(8, (i32)((i64)tl * s.page / std::max(1, range))) : thick;
    if (th >= tl || s.max <= s.min || s.disabled) { g.thumb0 = g.thumb1 = -1; return g; }
    i32 span = sb_maxpos(s) - s.min;
    i32 off = span > 0 ? (i32)((i64)(tl - th) * (std::clamp(s.pos, s.min, sb_maxpos(s)) - s.min) / span) : 0;
    g.thumb0 = g.track0 + off;
    g.thumb1 = g.thumb0 + th;
    return g;
}

void arrow_glyph(u32 dc, const Rect &r, int dir, bool enabled) {   // 0 arriba, 1 abajo, 2 izquierda, 3 derecha
    i32 cx = (r.l + r.r) / 2, cy = (r.t + r.b) / 2;
    auto draw = [&](i32 o, u32 col) {
        for (int k = 0; k < 4; k++) {
            switch (dir) {
            case 0: gdi_fill_color(dc, {cx - 3 + k + o, cy - 2 + k + o, cx + 4 - k + o, cy - 1 + k + o}, col); break;
            case 1: gdi_fill_color(dc, {cx - 3 + k + o, cy + 1 - k + o, cx + 4 - k + o, cy + 2 - k + o}, col); break;
            case 2: gdi_fill_color(dc, {cx - 2 + k + o, cy - 3 + k + o, cx - 1 + k + o, cy + 4 - k + o}, col); break;
            default: gdi_fill_color(dc, {cx + 1 - k + o, cy - 3 + k + o, cx + 2 - k + o, cy + 4 - k + o}, col); break;
            }
        }
    };
    if (!enabled) draw(1, C(COLOR_BTNHIGHLIGHT));
    draw(0, enabled ? C(COLOR_BTNTEXT) : C(COLOR_BTNSHADOW));
}

int sb_hit(const SInfo &s, i32 len, i32 thick, i32 p) {   // 1/2 flechas, 3 caja, 4/5 página antes/después
    SGeo g = sb_geo(s, len, thick);
    if (p < 0 || p >= len) return 0;
    if (p < g.arrow) return 1;
    if (p >= len - g.arrow) return 2;
    if (g.thumb0 < 0) return 0;
    if (p >= g.thumb0 && p < g.thumb1) return 3;
    return p < g.thumb0 ? 4 : 5;
}

struct Track {
    bool on = false;
    u32 hwnd = 0, target = 0, lparam = 0;
    int bar = 0, part = 0;
    bool vert = false;
    Rect screen;
    i32 grab = 0;
    u32 next = 0;
    bool in = true;
} g_track;

void sb_send(u32 code, i32 pos = 0) {
    wm_send(g_track.target, g_track.vert ? WM_VSCROLL : WM_HSCROLL, ((u32)(u16)pos << 16) | code, g_track.lparam);
}

// dónde está la barra de una ventana en pantalla (bar 0 horizontal, 1 vertical)
bool nc_bar_rect(u32 hwnd, int bar, Rect &out) {
    u32 st = wm_style(hwnd);
    if (!(st & (bar ? WS_VSCROLL : WS_HSCROLL))) return false;
    Rect c = wm_screen(hwnd, true);
    out = bar ? Rect{c.r, c.t, c.r + SB_W, c.b} : Rect{c.l, c.b, c.r, c.b + SB_W};
    return true;
}
void sb_redraw_bar(u32 hwnd, int bar) {
    if (bar == 2) { wm_invalidate(hwnd, false); wm_update(hwnd); return; }
    u32 dc = gdi_window_dc(hwnd, false);
    sb_nc_paint(hwnd, dc);
    gdi_release_dc(dc);
}

// ---------------------------------------------------------------- SCROLLBAR (control)

void scrollbar_proc() {
    u32 hwnd = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    bool vert = wm_style(hwnd) & 1;
    SInfo &s = sb_info(hwnd, 2);
    switch (msg) {
    case WM_NCDESTROY: g_sb.erase({hwnd, 2}); break;
    case WM_PAINT: {
        Paint dc(hwnd, 0);
        ctl_color(hwnd, dc, WM_CTLCOLORSCROLLBAR);
        sb_draw(dc, wm_client(hwnd), vert, s, g_track.on && g_track.hwnd == hwnd && g_track.in ? (g_track.part <= 3 ? g_track.part : 0) : 0);
        RET(0, 4);
    }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_ENABLE: s.disabled = !wp; wm_invalidate(hwnd, false); RET(0, 4);
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        Rect sr = wm_screen(hwnd, true);
        sb_nc_click(hwnd, 2, sr.l + lo16(lp), sr.t + hi16(lp));
        RET(0, 4);
    }
    case WM_GETDLGCODE: RET(1 | 0x2000, 4);   // DLGC_WANTARROWS
    case 0xE0: { i32 o = s.pos; s.pos = std::clamp((i32)wp, s.min, sb_maxpos(s)); if (lp) sb_redraw_bar(hwnd, 2); RET((u32)o, 4); }   // SBM_SETPOS
    case 0xE1: RET((u32)s.pos, 4);                                                   // SBM_GETPOS
    case 0xE2: case 0xE6: s.min = (i32)wp; s.max = (i32)lp; s.pos = std::clamp(s.pos, s.min, sb_maxpos(s)); if (msg == 0xE6) sb_redraw_bar(hwnd, 2); RET(0, 4);
    case 0xE3: if (wp) wr32(wp, (u32)s.min); if (lp) wr32(lp, (u32)s.max); RET(0, 4); // SBM_GETRANGE
    case 0xE4: s.disabled = wp == 3; sb_redraw_bar(hwnd, 2); RET(1, 4);              // SBM_ENABLE_ARROWS
    case 0xE9: {                                                                      // SBM_SETSCROLLINFO
        u32 m = rd32(lp + 4);
        if (m & 1) { s.min = (i32)rd32(lp + 8); s.max = (i32)rd32(lp + 12); }
        if (m & 2) s.page = (i32)rd32(lp + 16);
        if (m & 4) s.pos = (i32)rd32(lp + 20);
        s.pos = std::clamp(s.pos, s.min, sb_maxpos(s));
        if (wp) sb_redraw_bar(hwnd, 2);
        RET((u32)s.pos, 4);
    }
    case 0xEA: {                                                                      // SBM_GETSCROLLINFO
        u32 m = rd32(lp + 4);
        if (m & 1) { wr32(lp + 8, (u32)s.min); wr32(lp + 12, (u32)s.max); }
        if (m & 2) wr32(lp + 16, (u32)s.page);
        if (m & 4) wr32(lp + 20, (u32)s.pos);
        if (m & 0x10) wr32(lp + 24, (u32)s.track);
        RET(1, 4);
    }
    }
    def_proc();
}

// ---------------------------------------------------------------- caret

struct Caret { u32 hwnd = 0; i32 x = 0, y = 0, w = 1, h = 16; int hide = 1; } g_caret;
void caret_publish() {
    Display &d = display();
    std::lock_guard<std::mutex> lk(d.mx);
    if (!g_caret.hwnd || g_caret.hide > 0 || !wm_exists(g_caret.hwnd)) { d.caret.on = false; d.screen.frame++; return; }
    Rect c = wm_screen(g_caret.hwnd, true);
    d.caret = {true, c.l + g_caret.x, c.t + g_caret.y, std::max(1, g_caret.w), std::max(1, g_caret.h)};
    d.screen.frame++;
}

}  // namespace

// ---------------------------------------------------------------- API compartida (ctlutil.h)

SInfo &sb_info(u32 hwnd, int bar) {
    auto k = std::make_pair(hwnd, bar);
    auto it = g_sb.find(k);
    if (it == g_sb.end()) it = g_sb.emplace(k, SInfo{}).first;
    return it->second;
}
void sb_set(u32 hwnd, int bar, const SInfo &s, bool redraw) { sb_info(hwnd, bar) = s; if (redraw) sb_redraw_bar(hwnd, bar); }

void sb_draw(u32 dc, const Rect &r, bool vert, const SInfo &s, int pressed) {
    i32 len = vert ? r.h() : r.w(), thick = vert ? r.w() : r.h();
    if (len <= 0 || thick <= 0) return;
    SGeo g = sb_geo(s, len, thick);
    auto seg = [&](i32 a, i32 b) { return vert ? Rect{r.l, r.t + a, r.r, r.t + b} : Rect{r.l + a, r.t, r.l + b, r.b}; };
    bool en = !s.disabled && s.max > s.min;
    u32 f = C(COLOR_BTNFACE), w = C(COLOR_BTNHIGHLIGHT);   // guía: mezcla de BTNFACE y blanco (la trama de Windows)
    u32 mid = 0;
    for (int sh = 0; sh < 24; sh += 8) mid |= ((((f >> sh) & 0xFF) + ((w >> sh) & 0xFF)) / 2) << sh;
    gdi_fill_color(dc, seg(g.track0, g.track1), mid);
    for (int k = 0; k < 2; k++) {
        Rect a = k == 0 ? seg(0, g.arrow) : seg(len - g.arrow, len);
        gdi_fill_color(dc, a, C(COLOR_BTNFACE));
        if (pressed == k + 1) gdi_draw_edge(dc, a, 1, 0x4000 | 0xF);
        else gdi_draw_edge(dc, a, 5, 0xF);
        Rect ga = pressed == k + 1 ? Rect{a.l + 1, a.t + 1, a.r + 1, a.b + 1} : a;
        arrow_glyph(dc, ga, vert ? k : k + 2, en);
    }
    if (g.thumb0 >= 0) {
        Rect t = seg(g.thumb0, g.thumb1);
        gdi_fill_color(dc, t, C(COLOR_BTNFACE));
        gdi_draw_edge(dc, t, 5, 0xF);
    }
}

void sb_nc_paint(u32 hwnd, u32 wdc) {
    Rect win = wm_screen(hwnd, false);
    for (int bar = 0; bar < 2; bar++) {
        Rect r;
        if (!nc_bar_rect(hwnd, bar, r)) continue;
        Rect local{r.l - win.l, r.t - win.t, r.r - win.l, r.b - win.t};
        int pressed = g_track.on && g_track.hwnd == hwnd && g_track.bar == bar && g_track.in && g_track.part <= 3 ? g_track.part : 0;
        sb_draw(wdc, local, bar == 1, sb_info(hwnd, bar), pressed);
    }
    if ((wm_style(hwnd) & (WS_VSCROLL | WS_HSCROLL)) == (WS_VSCROLL | WS_HSCROLL)) {   // cuadro de la esquina
        Rect c = wm_screen(hwnd, true);
        gdi_fill_color(wdc, {c.r - win.l, c.b - win.t, c.r - win.l + SB_W, c.b - win.t + SB_W}, C(COLOR_BTNFACE));
    }
}

void sb_nc_click(u32 hwnd, int bar, i32 sx, i32 sy) {
    Rect r;
    if (bar == 2) { r = wm_screen(hwnd, true); g_track.vert = wm_style(hwnd) & 1; g_track.target = wm_parent(hwnd); g_track.lparam = hwnd; }
    else { if (!nc_bar_rect(hwnd, bar, r)) return; g_track.vert = bar == 1; g_track.target = hwnd; g_track.lparam = 0; }
    SInfo &s = sb_info(hwnd, bar);
    i32 len = g_track.vert ? r.h() : r.w(), thick = g_track.vert ? r.w() : r.h();
    i32 p = g_track.vert ? sy - r.t : sx - r.l;
    int part = sb_hit(s, len, thick, p);
    if (!part || s.disabled) return;
    g_track.on = true; g_track.hwnd = hwnd; g_track.bar = bar; g_track.part = part; g_track.screen = r; g_track.in = true;
    if (part == 3) {
        SGeo g = sb_geo(s, len, thick);
        g_track.grab = p - g.thumb0;
        s.track = s.pos;
    } else {
        static const u32 code[] = {0, 0, 1, 0, 2, 3};   // SB_LINEUP/LINEDOWN/PAGEUP/PAGEDOWN
        sb_send(code[part]);
        g_track.next = ticks_ms() + 350;
    }
    sb_redraw_bar(hwnd, bar);
}

bool sb_track_event(int kind, i32 sx, i32 sy) {
    if (!g_track.on) return false;
    if (!wm_exists(g_track.hwnd)) { g_track.on = false; return false; }
    SInfo &s = sb_info(g_track.hwnd, g_track.bar);
    i32 len = g_track.vert ? g_track.screen.h() : g_track.screen.w(), thick = g_track.vert ? g_track.screen.w() : g_track.screen.h();
    i32 p = g_track.vert ? sy - g_track.screen.t : sx - g_track.screen.l;
    if (kind == 1) {   // soltar
        if (g_track.part == 3) sb_send(4, s.track);   // SB_THUMBPOSITION
        sb_send(8);                                   // SB_ENDSCROLL
        g_track.on = false;
        sb_redraw_bar(g_track.hwnd, g_track.bar);
        return true;
    }
    if (g_track.part == 3) {
        if (kind == 0) {
            SGeo g = sb_geo(s, len, thick);
            i32 th = g.thumb1 - g.thumb0, tl = g.track1 - g.track0 - th, span = sb_maxpos(s) - s.min;
            i32 np = tl > 0 ? s.min + (i32)(((i64)(p - g_track.grab - g.track0) * span + tl / 2) / tl) : s.min;
            np = std::clamp(np, s.min, sb_maxpos(s));
            if (np != s.track) { s.track = np; sb_send(5, np); }   // SB_THUMBTRACK
        }
        return true;
    }
    if (kind == 0) {
        bool in = sb_hit(s, len, thick, p) == g_track.part || (g_track.part >= 4 && sb_hit(s, len, thick, p) >= 4);
        if (in != g_track.in) { g_track.in = in; sb_redraw_bar(g_track.hwnd, g_track.bar); }
        return true;
    }
    if (g_track.in && (i32)(ticks_ms() - g_track.next) >= 0) {   // repetición mientras se mantiene pulsado
        int now_part = sb_hit(s, len, thick, p);
        if (g_track.part < 4 || now_part == g_track.part) {
            static const u32 code[] = {0, 0, 1, 0, 2, 3};
            sb_send(code[g_track.part]);
        }
        g_track.next = ticks_ms() + 50;
    }
    return true;
}

void caret_create(u32 hwnd, i32 w, i32 h) { g_caret = {hwnd, 0, 0, w ? w : 1, h ? h : 16, 1}; caret_publish(); }
void caret_destroy() { g_caret.hwnd = 0; caret_publish(); }
void caret_pos(i32 x, i32 y) { g_caret.x = x; g_caret.y = y; caret_publish(); }
void caret_show(bool show) { g_caret.hide += show ? -1 : 1; if (g_caret.hide < 0) g_caret.hide = 0; caret_publish(); }

// ---------------------------------------------------------------- registro de las clases

u32 ctl_proc(const std::string &cls) {   // procedimiento del host de una clase del sistema (0 si no la hay)
    void edit_proc(); void listbox_proc(); void combobox_proc(); void combolbox_proc(); void statusbar_proc();
    void menu_popup_proc();
    static std::map<std::string, u32> procs = {
        {"button", host_fn(button_proc)}, {"static", host_fn(static_proc)}, {"scrollbar", host_fn(scrollbar_proc)},
        {"edit", host_fn(edit_proc)}, {"listbox", host_fn(listbox_proc)}, {"combobox", host_fn(combobox_proc)},
        {"combolbox", host_fn(combolbox_proc)}, {"msctls_statusbar32", host_fn(statusbar_proc)},
        {"#32768", host_fn(menu_popup_proc)},
    };
    auto it = procs.find(cls);
    return it == procs.end() ? 0 : it->second;
}

// ---------------------------------------------------------------- API de barras de desplazamiento (USER32)

static u32 sb_get_info(u32 hwnd, int bar, u32 p) {
    if (bar == 2) return wm_send(hwnd, 0xEA, 0, p);
    SInfo &s = sb_info(hwnd, bar);
    u32 m = rd32(p + 4);
    if (m & 1) { wr32(p + 8, (u32)s.min); wr32(p + 12, (u32)s.max); }
    if (m & 2) wr32(p + 16, (u32)s.page);
    if (m & 4) wr32(p + 20, (u32)s.pos);
    if (m & 0x10) wr32(p + 24, (u32)s.track);
    return 1;
}
// Windows muestra u oculta la barra de una ventana según el rango (salvo SIF_DISABLENOSCROLL)
static void sb_autoshow(u32 hwnd, int bar, bool disable_noscroll) {
    SInfo &s = sb_info(hwnd, bar);
    bool needed = s.max > s.min && (s.page == 0 || s.page <= s.max - s.min);
    u32 flag = bar ? WS_VSCROLL : WS_HSCROLL, st = wm_style(hwnd);
    if (disable_noscroll) { s.disabled = !needed; if (!(st & flag)) { wm_set_style(hwnd, st | flag); wm_invalidate(hwnd, true); } return; }
    s.disabled = false;
    if (needed != !!(st & flag)) {
        wm_set_style(hwnd, needed ? st | flag : st & ~flag);
        Rect w = wm_screen(hwnd, false);
        wm_send(hwnd, WM_SIZE, 0, make_lp(wm_client(hwnd).r, wm_client(hwnd).b));
        wm_invalidate(hwnd, true);
        (void)w;
    }
}
static u32 sb_set_info(u32 hwnd, int bar, u32 p, bool redraw) {
    if (bar == 2) return wm_send(hwnd, 0xE9, redraw, p);
    SInfo &s = sb_info(hwnd, bar);
    u32 m = rd32(p + 4);
    if (m & 1) { s.min = (i32)rd32(p + 8); s.max = (i32)rd32(p + 12); }
    if (m & 2) s.page = (i32)rd32(p + 16);
    if (m & 4) s.pos = (i32)rd32(p + 20);
    s.pos = std::clamp(s.pos, s.min, sb_maxpos(s));
    if (m & 3) sb_autoshow(hwnd, bar, m & 8);
    if (redraw && wm_visible(hwnd)) sb_redraw_bar(hwnd, bar);
    return (u32)s.pos;
}

STUB(ext_user32_dll_GetScrollInfo) { RET(sb_get_info(arg(0), (int)arg(1), arg(2)), 3); }
STUB(ext_user32_dll_SetScrollInfo) { RET(sb_set_info(arg(0), (int)arg(1), arg(2), arg(3)), 4); }
STUB(ext_user32_dll_GetScrollPos) { if (arg(1) == 2) RET(wm_send(arg(0), 0xE1, 0, 0), 2); RET((u32)sb_info(arg(0), (int)arg(1)).pos, 2); }
STUB(ext_user32_dll_SetScrollPos) {
    if (arg(1) == 2) RET(wm_send(arg(0), 0xE0, arg(2), arg(3)), 4);
    SInfo &s = sb_info(arg(0), (int)arg(1));
    i32 old = s.pos;
    s.pos = std::clamp((i32)arg(2), s.min, sb_maxpos(s));
    if (arg(3) && wm_visible(arg(0))) sb_redraw_bar(arg(0), (int)arg(1));
    RET((u32)old, 4);
}
STUB(ext_user32_dll_GetScrollRange) {
    if (arg(1) == 2) { wm_send(arg(0), 0xE3, arg(2), arg(3)); RET(1, 4); }
    SInfo &s = sb_info(arg(0), (int)arg(1));
    wr32(arg(2), (u32)s.min); wr32(arg(3), (u32)s.max);
    RET(1, 4);
}
STUB(ext_user32_dll_SetScrollRange) {
    if (arg(1) == 2) { wm_send(arg(0), arg(4) ? 0xE6 : 0xE2, arg(2), arg(3)); RET(1, 5); }
    SInfo &s = sb_info(arg(0), (int)arg(1));
    s.min = (i32)arg(2); s.max = (i32)arg(3);
    s.pos = std::clamp(s.pos, s.min, sb_maxpos(s));
    sb_autoshow(arg(0), (int)arg(1), false);
    if (arg(4) && wm_visible(arg(0))) sb_redraw_bar(arg(0), (int)arg(1));
    RET(1, 5);
}
STUB(ext_user32_dll_ShowScrollBar) {   // (hwnd, SB_HORZ 0 / VERT 1 / CTL 2 / BOTH 3, show)
    u32 h = arg(0), bar = arg(1), show = arg(2);
    if (bar == 2) { wm_show(h, show != 0); RET(1, 3); }
    u32 st = wm_style(h), flags = bar == 0 ? WS_HSCROLL : bar == 1 ? WS_VSCROLL : WS_HSCROLL | WS_VSCROLL;
    u32 ns = show ? st | flags : st & ~flags;
    if (ns != st) {
        wm_set_style(h, ns);
        Rect c = wm_client(h);
        wm_send(h, WM_SIZE, 0, make_lp(c.r, c.b));
        wm_invalidate(h, true);
    }
    RET(1, 3);
}
STUB(ext_user32_dll_EnableScrollBar) {   // (hwnd, bar, ESB_ENABLE_BOTH 0 / DISABLE_BOTH 3)
    u32 h = arg(0), bar = arg(1);
    for (int b = 0; b < 3; b++) if ((u32)b == bar || (bar == 3 && b < 2)) { sb_info(h, b).disabled = arg(2) == 3; sb_redraw_bar(h, b); }
    RET(1, 3);
}
STUB(ext_user32_dll_ScrollWindow) {   // (hwnd, dx, dy, rect, clip): se repinta todo (no se mueven píxeles)
    wm_invalidate(arg(0), true);
    RET(1, 5);
}
STUB(ext_user32_dll_ScrollWindowEx) {   // (hwnd, dx, dy, scroll, clip, rgn, update, flags)
    if (arg(6)) wr_rect(arg(6), wm_client(arg(0)));
    if (arg(7) & 2) wm_invalidate(arg(0), arg(7) & 4);   // SW_INVALIDATE, SW_ERASE
    RET(2, 8);   // SIMPLEREGION
}
STUB(ext_user32_dll_ScrollDC) { if (arg(6)) wr_rect(arg(6), {}); RET(1, 7); }

// ---------------------------------------------------------------- caret (API)

STUB(ext_user32_dll_CreateCaret) {   // (hwnd, bitmap, w, h)
    i32 w = (i32)arg(2), h = (i32)arg(3);
    caret_create(arg(0), w ? w : 1, h ? h : 16);
    RET(1, 4);
}
STUB(ext_user32_dll_DestroyCaret) { caret_destroy(); RET(1, 0); }
STUB(ext_user32_dll_SetCaretPos) { caret_pos((i32)arg(0), (i32)arg(1)); RET(1, 2); }
STUB(ext_user32_dll_GetCaretPos) { wr32(arg(0), (u32)g_caret.x); wr32(arg(0) + 4, (u32)g_caret.y); RET(1, 1); }
STUB(ext_user32_dll_ShowCaret) { caret_show(true); RET(1, 1); }
STUB(ext_user32_dll_HideCaret) { caret_show(false); RET(1, 1); }
STUB(ext_user32_dll_GetCaretBlinkTime) { RET(530, 0); }
STUB(ext_user32_dll_SetCaretBlinkTime) { RET(1, 1); }

// ---------------------------------------------------------------- dibujo de USER32

STUB(ext_user32_dll_DrawEdge) {   // (hdc, rect, edge, flags)
    Rect r = rd_rect(arg(1));
    gdi_draw_edge(arg(0), r, arg(2), arg(3));
    if (arg(3) & 0x2000) {   // BF_ADJUST: el rectángulo pasa a ser el interior
        int n = ((arg(2) & 3) ? 1 : 0) + ((arg(2) & 12) ? 1 : 0);
        u32 f = arg(3);
        wr_rect(arg(1), {r.l + ((f & 1) ? n : 0), r.t + ((f & 2) ? n : 0), r.r - ((f & 4) ? n : 0), r.b - ((f & 8) ? n : 0)});
    }
    RET(1, 4);
}
STUB(ext_user32_dll_DrawFocusRect) { gdi_focus_rect(arg(0), rd_rect(arg(1))); RET(1, 2); }
STUB(ext_user32_dll_FrameRect) {   // (hdc, rect, brush): marco de 1 píxel
    Rect r = rd_rect(arg(1));
    u32 b = arg(2);
    gdi_fill_rect(arg(0), {r.l, r.t, r.r, r.t + 1}, b); gdi_fill_rect(arg(0), {r.l, r.b - 1, r.r, r.b}, b);
    gdi_fill_rect(arg(0), {r.l, r.t, r.l + 1, r.b}, b); gdi_fill_rect(arg(0), {r.r - 1, r.t, r.r, r.b}, b);
    RET(1, 3);
}
STUB(ext_user32_dll_InvertRect) { gdi_invert(arg(0), rd_rect(arg(1))); RET(1, 2); }
STUB(ext_user32_dll_DrawFrameControl) {   // (hdc, rect, type, state)
    u32 dc = arg(0), type = arg(2), state = arg(3);
    Rect r = rd_rect(arg(1));
    bool pushed = state & 0x200, inactive = state & 0x100, checked = state & 0x400;
    switch (type) {
    case 4: {   // DFC_BUTTON
        u32 kind = state & 0xF;
        if (kind == 0x10 || (state & 0x10)) {   // DFCS_BUTTONPUSH
            gdi_fill_color(dc, r, C(COLOR_BTNFACE));
            gdi_draw_edge(dc, r, pushed ? 10 : 5, 0xF);
        } else if (kind == 0 || kind == 8) {   // CHECK / 3STATE
            i32 s = std::min(r.w(), r.h()), x = r.l, y = r.t + (r.h() - s) / 2;
            gdi_draw_edge(dc, {x, y, x + s, y + s}, 10, 0xF);
            gdi_fill_color(dc, {x + 2, y + 2, x + s - 2, y + s - 2}, pushed || inactive ? C(COLOR_BTNFACE) : C(COLOR_WINDOW));
            if (checked) check_mark(dc, x + (s - 7) / 2, y + (s - 7) / 2, inactive ? C(COLOR_BTNSHADOW) : C(COLOR_WINDOWTEXT));
        } else radio_shape(dc, r.l + (r.w() - 12) / 2, r.t + (r.h() - 12) / 2, checked, pushed, !inactive);
        break;
    }
    case 3: {   // DFC_SCROLL: flechas (DFCS_SCROLLUP 0, DOWN 1, LEFT 2, RIGHT 3, COMBOBOX 5)
        gdi_fill_color(dc, r, C(COLOR_BTNFACE));
        if (pushed) gdi_draw_edge(dc, r, 1, 0x4000 | 0xF); else gdi_draw_edge(dc, r, 5, 0xF);
        u32 k = state & 0xF;
        Rect g = pushed ? Rect{r.l + 1, r.t + 1, r.r + 1, r.b + 1} : r;
        arrow_glyph(dc, g, k == 5 ? 1 : (int)std::min<u32>(k, 3), !inactive);
        break;
    }
    case 1: {   // DFC_CAPTION: cerrar 0, minimizar 1, maximizar 2, restaurar 3
        gdi_fill_color(dc, r, C(COLOR_BTNFACE));
        gdi_draw_edge(dc, r, pushed ? 10 : 5, 0xF);
        break;
    }
    default:
        gdi_fill_color(dc, r, C(COLOR_BTNFACE));
    }
    RET(1, 4);
}
STUB(ext_user32_dll_DrawIcon) { RET(1, 4); }      // los iconos de los recursos no se cargan
STUB(ext_user32_dll_DrawIconEx) { RET(1, 9); }
STUB(ext_user32_dll_DrawTextExA) {   // (hdc, str, n, rect, fmt, params)
    u32 n = arg(2);
    std::string s = n == 0xFFFFFFFFu ? gstr(arg(1)) : gstr(arg(1), n);
    Rect r = rd_rect(arg(3));
    int h = gdi_draw_text(arg(0), s, r, arg(4));
    if (arg(4) & 0x400) wr_rect(arg(3), r);
    RET((u32)h, 6);
}
STUB(ext_user32_dll_GrayStringA) {   // (hdc, brush, proc, data, n, x, y, w, h): texto gris sin función propia
    if (arg(2)) RET(guest_call(arg(2), {arg(0), arg(3), arg(4)}) ? 1 : 0, 9);
    std::string s = gstr(arg(3), arg(4) ? arg(4) : 4096);
    Rect r{(i32)arg(5), (i32)arg(6), (i32)arg(5) + 4000, (i32)arg(6) + 400};
    gdi_set_text_color(arg(0), C(COLOR_GRAYTEXT));
    gdi_set_bk(arg(0), 0, 1);
    gdi_draw_text(arg(0), s, r, 0x20 | 0x100);
    RET(1, 9);
}
static i32 tab_width(u32 hdc, const std::string &s, u32 ntabs, u32 tabs, i32 origin, i32 *out_x = nullptr) {
    i32 x = 0;
    i32 deftab = 8 * gdi_dc_text_width(hdc, "x");
    std::string run;
    u32 k = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '\t') {
            if (out_x && !run.empty()) { (void)origin; }
            x += gdi_dc_text_width(hdc, run);
            run.clear();
            if (i == s.size()) break;
            i32 stop;
            if (ntabs && tabs) {
                stop = 0;
                while (k < ntabs && (stop = (i32)rd32(tabs + 4 * k)) <= x) k++;
                if (k >= ntabs) { i32 last = (i32)rd32(tabs + 4 * (ntabs - 1)); stop = ntabs == 1 ? (x / last + 1) * last : x + deftab; }
            } else stop = (x / deftab + 1) * deftab;
            x = stop;
        } else run.push_back(s[i]);
    }
    return x;
}
STUB(ext_user32_dll_TabbedTextOutA) {   // (hdc, x, y, str, n, ntabs, tabs, origin)
    u32 hdc = arg(0);
    std::string s = gstr(arg(3), arg(4));
    i32 x0 = (i32)arg(1), y = (i32)arg(2), x = 0;
    i32 deftab = 8 * gdi_dc_text_width(hdc, "x");
    u32 ntabs = arg(5), tabs = arg(6), k = 0;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('\t', i);
        std::string run = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        Rect r{x0 + x, y, x0 + x + 4000, y + 400};
        gdi_draw_text(hdc, run, r, 0x20 | 0x100 | 0x800);
        x += gdi_dc_text_width(hdc, run);
        if (j == std::string::npos) break;
        i32 stop;
        if (ntabs && tabs) {
            while (k < ntabs && (i32)rd32(tabs + 4 * k) <= x) k++;
            stop = k < ntabs ? (i32)rd32(tabs + 4 * k) : x + deftab;
        } else stop = (x / deftab + 1) * deftab;
        x = stop;
        i = j + 1;
    }
    RET(((u32)gdi_dc_text_height(hdc) << 16) | (u16)x, 8);
}
STUB(ext_user32_dll_GetTabbedTextExtentA) {   // (hdc, str, n, ntabs, tabs)
    std::string s = gstr(arg(1), arg(2));
    i32 w = tab_width(arg(0), s, arg(3), arg(4), 0);
    RET(((u32)gdi_dc_text_height(arg(0)) << 16) | (u16)w, 5);
}
STUB(ext_user32_dll_DrawCaption) { RET(1, 4); }
