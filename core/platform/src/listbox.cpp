// PCF7 — controles LISTBOX y COMBOBOX de Windows en el host (con la lista desplegable ComboLBox).
//
// Lista: elementos con texto y dato, orden (LBS_SORT), selección simple, múltiple y extendida, dibujo por el
// propietario (fijo y variable, con o sin cadenas), barra vertical, teclado y búsqueda por letra, y las
// notificaciones LBN_*. Combo: CBS_SIMPLE (edición + lista fija), CBS_DROPDOWN (edición + lista
// desplegable) y CBS_DROPDOWNLIST (solo lista desplegable); los mensajes CB_* se traducen a los LB_*.
#include "ctlutil.h"
#include "display.h"

#include <algorithm>
#include <map>
#include <strings.h>
#include <vector>

using namespace ctl;

namespace {

constexpr u32 LBS_NOTIFY = 1, LBS_SORT = 2, LBS_MULTIPLESEL = 8, LBS_OWNERDRAWFIXED = 0x10, LBS_OWNERDRAWVARIABLE = 0x20,
              LBS_HASSTRINGS = 0x40, LBS_NOINTEGRALHEIGHT = 0x100, LBS_WANTKEYBOARDINPUT = 0x400, LBS_EXTENDEDSEL = 0x800,
              LBS_DISABLENOSCROLL = 0x1000, LBS_NOSEL = 0x4000;
constexpr u32 LB_ERR = 0xFFFFFFFFu;

struct Item { std::string s; u32 data = 0; bool sel = false; i32 h = 0; };
struct Lb {
    u32 font = 0;
    std::vector<Item> items;
    i32 cur = -1;          // selección (simple) o elemento con el foco (múltiple)
    i32 top = 0, anchor = 0;
    i32 item_h = 0;
    bool focus = false, tracking = false, redraw = true;
    u32 combo = 0;         // lista de un combo: el combo
};
std::map<u32, Lb> g_lb;

bool owner_draw(u32 h) { return wm_style(h) & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE); }
bool has_strings(u32 h) { return !owner_draw(h) || (wm_style(h) & LBS_HASSTRINGS); }
bool multi(u32 h) { return wm_style(h) & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL); }
u32 ctl_type(u32 h) { return g_lb[h].combo ? 3 : 2; }   // ODT_COMBOBOX / ODT_LISTBOX
u32 notify_target(u32 h) { return g_lb[h].combo ? g_lb[h].combo : h; }

i32 item_height(u32 h, Lb &l) {
    if (!l.item_h) {
        DCx dc(h, l.font);
        l.item_h = gdi_dc_text_height(dc);
        if (owner_draw(h)) {   // WM_MEASUREITEM al padre (o al padre del combo)
            u32 mis = heap_alloc(24, true);
            wr32(mis, ctl_type(h)); wr32(mis + 4, wm_id(notify_target(h))); wr32(mis + 8, 0); wr32(mis + 16, (u32)l.item_h);
            u32 p = wm_parent(notify_target(h));
            if (p) wm_send(p, WM_MEASUREITEM, wm_id(notify_target(h)), mis);
            l.item_h = std::max<i32>(1, (i32)rd32(mis + 16));
            heap_free(mis);
        }
    }
    return l.item_h;
}
i32 page(u32 h, Lb &l) { return std::max(1, wm_client(h).b / item_height(h, l)); }

void update_sb(u32 h, Lb &l) {
    u32 st = wm_style(h);
    if (!(st & WS_VSCROLL) && !(st & LBS_DISABLENOSCROLL)) {
        // Windows muestra la barra solo si hace falta
        bool need = (i32)l.items.size() > page(h, l);
        if (!need) return;
    }
    SInfo s = sb_info(h, 1);
    s.min = 0; s.max = std::max(0, (i32)l.items.size() - 1); s.page = page(h, l); s.pos = l.top;
    sb_info(h, 1) = s;
    u32 si = heap_alloc(28, true);
    wr32(si, 28); wr32(si + 4, 1 | 2 | 4 | ((st & LBS_DISABLENOSCROLL) ? 8 : 0)); wr32(si + 8, 0); wr32(si + 12, (u32)s.max);
    wr32(si + 16, (u32)s.page); wr32(si + 20, (u32)l.top);
    call_stub(ext_user32_dll_SetScrollInfo, {h, 1, si, 1});
    heap_free(si);
}

void ensure_visible(u32 h, Lb &l, i32 i) {
    if (i < 0) return;
    i32 p = page(h, l);
    if (i < l.top) l.top = i;
    if (i >= l.top + p) l.top = i - p + 1;
    l.top = std::clamp(l.top, 0, std::max(0, (i32)l.items.size() - p));
}

void refresh(u32 h, Lb &l) { if (!l.redraw) return; update_sb(h, l); wm_invalidate(h, true); }

void draw_item(u32 h, Lb &l, u32 dc, i32 i, const Rect &r) {
    const Item &it = l.items[i];
    bool sel = multi(h) ? it.sel : i == l.cur;
    bool en = wm_enabled(h);
    if (owner_draw(h)) {
        u32 dis = heap_alloc(48, true);
        wr32(dis, ctl_type(h)); wr32(dis + 4, wm_id(notify_target(h))); wr32(dis + 8, (u32)i); wr32(dis + 12, 1);
        wr32(dis + 16, (sel ? 1u : 0u) | (en ? 0u : 4u) | (l.focus && i == l.cur ? 0x10u : 0u));
        wr32(dis + 20, h); wr32(dis + 24, dc); wr_rect(dis + 28, r); wr32(dis + 44, it.data);
        if (u32 p = wm_parent(notify_target(h))) wm_send(p, WM_DRAWITEM, wm_id(notify_target(h)), dis);
        heap_free(dis);
        return;
    }
    if (sel) gdi_fill_color(dc, r, C(COLOR_HIGHLIGHT));
    gdi_set_bk(dc, 0, 1);
    Rect tr{r.l + 2, r.t, r.r, r.b};
    if (sel) { gdi_set_text_color(dc, C(COLOR_HIGHLIGHTTEXT)); gdi_draw_text(dc, it.s, tr, 0x20 | 0x800 | 0x40); }
    else draw_label(dc, it.s, tr, 0x20 | 0x800 | 0x40, en, ~0u);
    if (l.focus && i == l.cur && !l.combo) gdi_focus_rect(dc, r);
}

void paint(u32 h, u32 dc) {
    Lb &l = g_lb[h];
    Rect r = wm_client(h);
    u32 br = ctl_color(l.combo ? l.combo : h, dc, WM_CTLCOLORLISTBOX);
    if (!br) { gdi_set_text_color(dc, C(COLOR_WINDOWTEXT)); }
    fill_bg(dc, r, br, wm_enabled(h) ? C(COLOR_WINDOW) : C(COLOR_BTNFACE));
    i32 ih = item_height(h, l);
    for (i32 i = l.top, y = 0; i < (i32)l.items.size() && y < r.b; i++, y += ih) {
        draw_item(h, l, dc, i, {0, y, r.r, y + ih});
        if (br) ctl_color(l.combo ? l.combo : h, dc, WM_CTLCOLORLISTBOX);   // restaura los colores del padre
        else gdi_set_text_color(dc, C(COLOR_WINDOWTEXT));
    }
    if (l.focus && l.items.empty() && !l.combo) gdi_focus_rect(dc, {0, 0, r.r, ih});
}

i32 find(Lb &l, const std::string &s, i32 start, bool exact) {
    i32 n = (i32)l.items.size();
    for (i32 k = 1; k <= n; k++) {
        i32 i = (start + k) % std::max(1, n);
        if (start < 0) i = k - 1;
        const std::string &t = l.items[i].s;
        if (exact ? !strcasecmp(t.c_str(), s.c_str()) : !strncasecmp(t.c_str(), s.c_str(), s.size())) return i;
    }
    return -1;
}

i32 insert(u32 h, Lb &l, i32 at, const std::string &s, u32 data) {
    Item it;
    it.s = has_strings(h) ? s : std::string();
    it.data = has_strings(h) ? 0 : data;
    if ((wm_style(h) & LBS_SORT) && at < 0) {
        at = 0;
        if (has_strings(h)) { while (at < (i32)l.items.size() && strcasecmp(l.items[at].s.c_str(), s.c_str()) <= 0) at++; }
        else at = (i32)l.items.size();
    } else if (at < 0 || at > (i32)l.items.size()) at = (i32)l.items.size();
    l.items.insert(l.items.begin() + at, it);
    if (l.cur >= at) l.cur++;
    refresh(h, l);
    return at;
}

void set_cur(u32 h, Lb &l, i32 i, bool notify_change) {
    i = std::clamp(i, -1, (i32)l.items.size() - 1);
    if (i == l.cur) return;
    l.cur = i;
    ensure_visible(h, l, i);
    refresh(h, l);
    if (notify_change && (wm_style(h) & LBS_NOTIFY || l.combo)) {
        if (l.combo) combo_list_changed(l.combo);
        else notify(h, 1);   // LBN_SELCHANGE
    }
}

i32 at_point(u32 h, Lb &l, i32 y) {
    i32 i = l.top + (y < 0 ? -1 : y / item_height(h, l));
    return i < (i32)l.items.size() ? i : (i32)l.items.size() - 1;
}

void click(u32 h, Lb &l, i32 i, bool ctrl, bool shift) {
    if (i < 0 || (wm_style(h) & LBS_NOSEL)) return;
    if (wm_style(h) & LBS_MULTIPLESEL) {
        l.items[i].sel = !l.items[i].sel;
        l.cur = i;
    } else if (wm_style(h) & LBS_EXTENDEDSEL) {
        if (!ctrl) for (auto &it : l.items) it.sel = false;
        if (shift) for (i32 k = std::min(l.anchor, i); k <= std::max(l.anchor, i); k++) l.items[k].sel = true;
        else { l.items[i].sel = ctrl ? !l.items[i].sel : true; l.anchor = i; }
        l.cur = i;
    } else {
        set_cur(h, l, i, true);
        return;
    }
    ensure_visible(h, l, i);
    refresh(h, l);
    if (wm_style(h) & LBS_NOTIFY) notify(h, 1);
}

bool key_state(u32 vk) { return key_down(vk); }

void key(u32 h, Lb &l, u32 vk) {
    if (l.items.empty()) return;
    i32 n = (i32)l.items.size(), c = std::max(0, l.cur), p = page(h, l), to = c;
    switch (vk) {
    case 0x26: to = c - 1; break;
    case 0x28: to = l.cur < 0 ? 0 : c + 1; break;
    case 0x21: to = c - p + 1; break;
    case 0x22: to = c + p - 1; break;
    case 0x24: to = 0; break;
    case 0x23: to = n - 1; break;
    case 0x20: if (multi(h)) click(h, l, c, true, false); return;
    default: return;
    }
    to = std::clamp(to, 0, n - 1);
    if (multi(h)) click(h, l, to, false, key_state(0x10));
    else set_cur(h, l, to, true);
}

// mensajes LB_* (comunes a la lista sola y a la de un combo)
bool lb_message(u32 h, Lb &l, u32 msg, u32 wp, u32 lp, u32 &r) {
    i32 n = (i32)l.items.size();
    auto text_of = [&](i32 i) { return i >= 0 && i < n ? l.items[i].s : std::string(); };
    switch (msg) {
    case 0x180: r = (u32)insert(h, l, -1, has_strings(h) ? gstr(lp) : "", lp); return true;                // LB_ADDSTRING
    case 0x181: r = (u32)insert(h, l, (i32)wp == -1 ? n : (i32)wp, has_strings(h) ? gstr(lp) : "", lp); return true;   // LB_INSERTSTRING
    case 0x182:                                                                                             // LB_DELETESTRING
        if ((i32)wp < 0 || (i32)wp >= n) { r = LB_ERR; return true; }
        if (owner_draw(h)) {
            u32 di = heap_alloc(20, true);
            wr32(di, ctl_type(h)); wr32(di + 4, wm_id(notify_target(h))); wr32(di + 8, wp); wr32(di + 12, h); wr32(di + 16, l.items[wp].data);
            if (u32 p = wm_parent(notify_target(h))) wm_send(p, WM_DELETEITEM, wm_id(notify_target(h)), di);
            heap_free(di);
        }
        l.items.erase(l.items.begin() + wp);
        if (l.cur == (i32)wp) l.cur = -1; else if (l.cur > (i32)wp) l.cur--;
        l.top = std::clamp(l.top, 0, std::max(0, (i32)l.items.size() - 1));
        refresh(h, l);
        r = (u32)l.items.size();
        return true;
    case 0x184: l.items.clear(); l.cur = -1; l.top = 0; refresh(h, l); r = 0; return true;                 // LB_RESETCONTENT
    case 0x185:                                                                                             // LB_SETSEL
        if (!multi(h)) { r = LB_ERR; return true; }
        if ((i32)lp == -1) for (auto &it : l.items) it.sel = wp != 0;
        else if ((i32)lp < n) l.items[lp].sel = wp != 0;
        refresh(h, l);
        r = 0;
        return true;
    case 0x186:                                                                                             // LB_SETCURSEL
        if (multi(h)) { r = LB_ERR; return true; }
        set_cur(h, l, (i32)wp, false);
        r = (i32)wp < n ? wp : LB_ERR;
        return true;
    case 0x187: r = (i32)wp >= 0 && (i32)wp < n ? (multi(h) ? l.items[wp].sel : (i32)wp == l.cur) : LB_ERR; return true;   // LB_GETSEL
    case 0x188: r = (u32)l.cur; return true;                                                                // LB_GETCURSEL
    case 0x189: {                                                                                           // LB_GETTEXT
        if ((i32)wp < 0 || (i32)wp >= n) { r = LB_ERR; return true; }
        if (!has_strings(h)) { wr32(lp, l.items[wp].data); r = 4; return true; }
        r = gstrcpy(lp, l.items[wp].s);
        return true;
    }
    case 0x18A: r = (i32)wp >= 0 && (i32)wp < n ? (has_strings(h) ? (u32)l.items[wp].s.size() : 4u) : LB_ERR; return true;   // LB_GETTEXTLEN
    case 0x18B: r = (u32)n; return true;                                                                    // LB_GETCOUNT
    case 0x18C: {                                                                                           // LB_SELECTSTRING
        i32 i = find(l, gstr(lp), (i32)wp, false);
        if (i >= 0 && !multi(h)) set_cur(h, l, i, false);
        r = i >= 0 ? (u32)i : LB_ERR;
        return true;
    }
    case 0x18E: r = (u32)l.top; return true;                                                                // LB_GETTOPINDEX
    case 0x18F: { i32 i = find(l, gstr(lp), (i32)wp, false); r = i >= 0 ? (u32)i : LB_ERR; return true; }    // LB_FINDSTRING
    case 0x1A2: { i32 i = find(l, gstr(lp), (i32)wp, true); r = i >= 0 ? (u32)i : LB_ERR; return true; }     // LB_FINDSTRINGEXACT
    case 0x190: { u32 c = 0; for (auto &it : l.items) c += it.sel; r = multi(h) ? c : LB_ERR; return true; }  // LB_GETSELCOUNT
    case 0x191: { u32 c = 0; for (i32 i = 0; i < n && c < wp; i++) if (l.items[i].sel) wr32(lp + 4 * c++, (u32)i); r = c; return true; }   // LB_GETSELITEMS
    case 0x192: case 0x194: case 0x195: case 0x1A5: case 0x1A8: r = 1; return true;                          // tabulaciones, extensión, columnas, locale
    case 0x193: r = 0; return true;
    case 0x197: l.top = std::clamp((i32)wp, 0, std::max(0, n - 1)); refresh(h, l); r = 0; return true;     // LB_SETTOPINDEX
    case 0x198: {                                                                                           // LB_GETITEMRECT
        i32 ih = item_height(h, l);
        wr_rect(lp, {0, ((i32)wp - l.top) * ih, wm_client(h).r, ((i32)wp - l.top + 1) * ih});
        r = 1;
        return true;
    }
    case 0x199: r = (i32)wp >= 0 && (i32)wp < n ? l.items[wp].data : LB_ERR; return true;                   // LB_GETITEMDATA
    case 0x19A: if ((i32)wp >= 0 && (i32)wp < n) l.items[wp].data = lp; r = (i32)wp < n ? 1 : LB_ERR; return true;   // LB_SETITEMDATA
    case 0x19B: for (i32 i = std::min(lo16(lp), hi16(lp)); i <= std::max(lo16(lp), hi16(lp)) && i < n; i++) if (i >= 0) l.items[i].sel = wp != 0; refresh(h, l); r = 0; return true;   // LB_SELITEMRANGE
    case 0x183: for (i32 i = std::min((i32)wp, (i32)lp); i <= std::max((i32)wp, (i32)lp) && i < n; i++) if (i >= 0) l.items[i].sel = true; refresh(h, l); r = 0; return true;   // LB_SELITEMRANGEEX
    case 0x19C: l.anchor = (i32)wp; r = 0; return true;
    case 0x19D: r = (u32)l.anchor; return true;
    case 0x19E: l.cur = (i32)wp; ensure_visible(h, l, l.cur); refresh(h, l); r = 0; return true;            // LB_SETCARETINDEX
    case 0x19F: r = (u32)l.cur; return true;
    case 0x1A0: l.item_h = std::max(1, lo16(lp)); refresh(h, l); r = 0; return true;                       // LB_SETITEMHEIGHT
    case 0x1A1: r = (u32)item_height(h, l); return true;                                                    // LB_GETITEMHEIGHT
    case 0x1A6: r = 0x0C0A; return true;                                                                    // LB_GETLOCALE
    case 0x1A7: l.items.resize(wp); refresh(h, l); r = 0; return true;                                      // LB_SETCOUNT
    case 0x1A9: {                                                                                           // LB_ITEMFROMPOINT
        i32 i = at_point(h, l, hi16(lp));
        Rect c = wm_client(h);
        bool out = lo16(lp) < 0 || lo16(lp) >= c.r || hi16(lp) < 0 || hi16(lp) >= c.b;
        r = ((u32)out << 16) | (u32)(u16)std::max(0, i);
        return true;
    }
    }
    (void)text_of;
    return false;
}

}  // namespace

void listbox_proc() {
    u32 h = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    Lb &l = g_lb[h];
    u32 r;
    if (lb_message(h, l, msg, wp, lp, r)) RET(r, 4);
    switch (msg) {
    case WM_NCDESTROY: g_lb.erase(h); break;
    case WM_CREATE: update_sb(h, l); RET(0, 4);
    case WM_PAINT: { Paint dc(h, l.font); paint(h, dc); RET(0, 4); }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_SETFONT: l.font = wp; l.item_h = 0; if (lp) refresh(h, l); RET(0, 4);
    case WM_GETFONT: RET(l.font, 4);
    case WM_SETREDRAW: l.redraw = wp != 0; if (wp) refresh(h, l); RET(0, 4);
    case WM_SIZE: refresh(h, l); RET(0, 4);
    case WM_ENABLE: wm_invalidate(h, true); RET(0, 4);
    case WM_SETFOCUS: l.focus = true; wm_invalidate(h, true); if (wm_style(h) & LBS_NOTIFY) notify(h, 4); RET(0, 4);
    case WM_KILLFOCUS: l.focus = false; wm_invalidate(h, true); if (wm_style(h) & LBS_NOTIFY) notify(h, 5); RET(0, 4);
    case WM_GETDLGCODE: RET(1 | 0x80, 4);   // DLGC_WANTARROWS | DLGC_WANTCHARS
    case WM_LBUTTONDOWN: {
        if (wm_focus() != h && !l.combo) wm_set_focus(h);
        click(h, l, at_point(h, l, hi16(lp)), wp & 8, wp & 4);   // MK_CONTROL, MK_SHIFT
        l.tracking = true;
        wm_set_capture(h);
        RET(0, 4);
    }
    case WM_MOUSEMOVE:
        if (l.tracking && !multi(h)) {
            i32 i = at_point(h, l, hi16(lp));
            if (hi16(lp) < 0 && l.top > 0) l.top--;
            if (i >= 0) set_cur(h, l, i, true);
        }
        RET(0, 4);
    case WM_LBUTTONUP:
        if (l.tracking) { l.tracking = false; if (wm_capture() == h) wm_set_capture(0); }
        RET(0, 4);
    case WM_LBUTTONDBLCLK:
        if (wm_style(h) & LBS_NOTIFY) notify(h, 2);   // LBN_DBLCLK
        RET(0, 4);
    case WM_KEYDOWN: {
        if (wm_style(h) & LBS_WANTKEYBOARDINPUT) {
            u32 p = wm_parent(h);
            u32 res = p ? wm_send(p, 0x2E, (wp & 0xFFFF) | ((u32)(u16)l.cur << 16), h) : 0xFFFFFFFFu;   // WM_VKEYTOITEM
            if (res == 0xFFFFFFFEu) RET(0, 4);
            if ((i32)res >= 0) { set_cur(h, l, (i32)res, true); RET(0, 4); }
        }
        key(h, l, wp);
        RET(0, 4);
    }
    case WM_CHAR: {   // salto al siguiente elemento que empieza por esa letra
        if (!has_strings(h) || wp < 32) RET(0, 4);
        i32 i = find(l, std::string(1, (char)wp), l.cur, false);
        if (i >= 0) { if (multi(h)) click(h, l, i, false, false); else set_cur(h, l, i, true); }
        RET(0, 4);
    }
    case WM_VSCROLL: {
        i32 p = page(h, l), maxtop = std::max(0, (i32)l.items.size() - p);
        switch (wp & 0xFFFF) {
        case 0: l.top--; break;
        case 1: l.top++; break;
        case 2: l.top -= p; break;
        case 3: l.top += p; break;
        case 4: case 5: l.top = (i32)(wp >> 16); break;
        case 6: l.top = 0; break;
        case 7: l.top = maxtop; break;
        }
        l.top = std::clamp(l.top, 0, maxtop);
        refresh(h, l);
        RET(0, 4);
    }
    case WM_MOUSEWHEEL: for (int i = 0; i < 3; i++) wm_send(h, WM_VSCROLL, hi16(wp) > 0 ? 0 : 1, 0); RET(0, 4);
    }
    def_proc();
}

// ---------------------------------------------------------------- COMBOBOX

namespace {
constexpr u32 CBS_SIMPLE = 1, CBS_DROPDOWN = 2, CBS_DROPDOWNLIST = 3, CBS_OWNERDRAWFIXED = 0x10, CBS_OWNERDRAWVARIABLE = 0x20,
              CBS_SORT = 0x100, CBS_HASSTRINGS = 0x200;
constexpr i32 BTN_W = 16;

struct Cb {
    u32 font = 0, edit = 0, list = 0;
    bool dropped = false, focus = false, pressed = false;
    i32 drop_h = 100, field_h = 0, drop_w = 0;
};
std::map<u32, Cb> g_cb;
u32 kind(u32 h) { return wm_style(h) & 3; }

i32 field_height(u32 h, Cb &c) {
    if (!c.field_h) { DCx dc(h, c.font); c.field_h = gdi_dc_text_height(dc) + 8; }
    return c.field_h;
}
Rect btn_rect(u32 h, Cb &c) { Rect r = wm_client(h); return {r.r - BTN_W - 2, 2, r.r - 2, field_height(h, c) - 2}; }

void layout(u32 h, Cb &c) {
    Rect r = wm_client(h);
    i32 fh = field_height(h, c);
    if (c.edit) wm_move(c.edit, 3, 3, (kind(h) == CBS_SIMPLE ? r.r - 6 : r.r - BTN_W - 6), fh - 6);
    if (kind(h) == CBS_SIMPLE && c.list) wm_move(c.list, 0, fh, r.r, std::max(0, r.b - fh));
}

std::string sel_text(Cb &c) {
    Lb &l = g_lb[c.list];
    return l.cur >= 0 && l.cur < (i32)l.items.size() ? l.items[l.cur].s : std::string();
}

void close_up(u32 h, Cb &c, bool ok) {
    if (!c.dropped) return;
    c.dropped = false;
    wm_show(c.list, false);
    if (wm_capture() == c.list) wm_set_capture(0);
    notify(h, ok ? 9 : 10);   // CBN_SELENDOK / CBN_SELENDCANCEL
    notify(h, 8);             // CBN_CLOSEUP
    wm_invalidate(h, true);
}
void drop_down(u32 h, Cb &c) {
    if (c.dropped || kind(h) == CBS_SIMPLE) return;
    notify(h, 7);   // CBN_DROPDOWN
    Rect s = wm_screen(h, false);
    Lb &l = g_lb[c.list];
    i32 ih = item_height(c.list, l);
    i32 n = std::max<i32>(1, (i32)l.items.size());
    i32 hh = std::min(std::max(c.drop_h - field_height(h, c), ih + 2), n * ih + 2);
    i32 y = s.t + field_height(h, c);
    if (y + hh > display().screen.h && s.t - hh >= 0) y = s.t - hh;
    wm_move(c.list, s.l, y, std::max(c.drop_w, s.w()), hh);
    ensure_visible(c.list, l, l.cur);
    c.dropped = true;
    wm_raise(c.list);
    wm_show(c.list, true);
    wm_set_capture(c.list);
    wm_invalidate(h, true);
}

void cb_paint(u32 h, u32 dc) {
    Cb &c = g_cb[h];
    Rect r = wm_client(h);
    i32 fh = field_height(h, c);
    bool en = wm_enabled(h);
    gdi_fill_color(dc, {0, 0, r.r, fh}, C(COLOR_BTNFACE));
    gdi_draw_edge(dc, {0, 0, r.r, fh}, 10, 0xF);   // EDGE_SUNKEN
    if (kind(h) == CBS_SIMPLE) return;
    Rect b = btn_rect(h, c);
    gdi_fill_color(dc, b, C(COLOR_BTNFACE));
    if (c.pressed || c.dropped) gdi_draw_edge(dc, b, 1, 0x4000 | 0xF); else gdi_draw_edge(dc, b, 5, 0xF);
    {   // flecha
        i32 cx = (b.l + b.r) / 2 + (c.dropped ? 1 : 0), cy = (b.t + b.b) / 2 + (c.dropped ? 1 : 0);
        for (int k = 0; k < 4; k++) gdi_fill_color(dc, {cx - 3 + k, cy - 1 + k, cx + 4 - k, cy + k}, en ? C(COLOR_BTNTEXT) : C(COLOR_BTNSHADOW));
    }
    if (kind(h) != CBS_DROPDOWNLIST) return;
    Rect f{2, 2, b.l, fh - 2};
    u32 br = ctl_color(h, dc, WM_CTLCOLOREDIT);
    fill_bg(dc, f, br, en ? C(COLOR_WINDOW) : C(COLOR_BTNFACE));
    Lb &l = g_lb[c.list];
    if (wm_style(h) & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) {
        u32 dis = heap_alloc(48, true);
        wr32(dis, 3); wr32(dis + 4, wm_id(h)); wr32(dis + 8, (u32)l.cur); wr32(dis + 12, 1);
        wr32(dis + 16, (c.focus ? 1u | 0x10u : 0u) | (en ? 0u : 4u) | 0x1000u);   // ODS_COMBOBOXEDIT
        wr32(dis + 20, h); wr32(dis + 24, dc); wr_rect(dis + 28, {f.l + 1, f.t + 1, f.r - 1, f.b - 1});
        wr32(dis + 44, l.cur >= 0 && l.cur < (i32)l.items.size() ? l.items[l.cur].data : 0);
        if (u32 p = wm_parent(h)) wm_send(p, WM_DRAWITEM, wm_id(h), dis);
        heap_free(dis);
        return;
    }
    Rect t{f.l + 1, f.t + 1, f.r - 1, f.b - 1};
    if (c.focus && !c.dropped) { gdi_fill_color(dc, t, C(COLOR_HIGHLIGHT)); gdi_set_text_color(dc, C(COLOR_HIGHLIGHTTEXT)); }
    else if (!br) gdi_set_text_color(dc, C(COLOR_WINDOWTEXT));
    gdi_set_bk(dc, 0, 1);
    Rect tt{t.l + 2, t.t, t.r, t.b};
    if (en) gdi_draw_text(dc, sel_text(c), tt, 0x20 | 0x4 | 0x800 | 0x40);
    else draw_label(dc, sel_text(c), tt, 0x20 | 0x4 | 0x800 | 0x40, false, ~0u);
    if (c.focus && !c.dropped) gdi_focus_rect(dc, t);
}

}  // namespace

// la lista del combo cambió de elemento (ratón o teclado)
void combo_list_changed(u32 h) {
    auto it = g_cb.find(h);
    if (it == g_cb.end()) return;
    Cb &c = it->second;
    if (c.edit) { wm_send(c.edit, WM_SETTEXT, 0, 0); std::string s = sel_text(c); u32 p = heap_alloc((u32)s.size() + 1); gstrcpy(p, s); wm_send(c.edit, WM_SETTEXT, 0, p); heap_free(p); wm_send(c.edit, 0xB1, 0, (u32)-1); }
    wm_invalidate(h, true);
    notify(h, 1);   // CBN_SELCHANGE
}

void combobox_proc() {
    u32 h = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    Cb &c = g_cb[h];
    // CB_* → LB_* sobre la lista
    static const std::map<u32, u32> cb2lb = {
        {0x143, 0x180}, {0x14A, 0x181}, {0x144, 0x182}, {0x14B, 0x184}, {0x146, 0x18B}, {0x147, 0x188}, {0x148, 0x189},
        {0x149, 0x18A}, {0x14C, 0x18F}, {0x158, 0x1A2}, {0x150, 0x199}, {0x151, 0x19A}, {0x15B, 0x18E}, {0x15C, 0x197},
        {0x15A, 0x1A6}, {0x159, 0x1A5}, {0x161, 0x1A8}};
    if (auto it = cb2lb.find(msg); it != cb2lb.end() && c.list) {
        u32 r = wm_send(c.list, it->second, wp, lp);
        if (msg == 0x14B && c.edit) wm_send(c.edit, WM_SETTEXT, 0, 0);
        if (msg == 0x14B || msg == 0x144) wm_invalidate(h, true);
        RET(r, 4);
    }
    switch (msg) {
    case WM_CREATE: {
        u32 st = wm_style(h);
        Rect r = wm_client(h);
        c.drop_h = r.b;
        u32 lbs = (st & CBS_SORT ? LBS_SORT : 0) | (st & CBS_HASSTRINGS ? LBS_HASSTRINGS : 0) |
                  (st & CBS_OWNERDRAWFIXED ? LBS_OWNERDRAWFIXED : 0) | (st & CBS_OWNERDRAWVARIABLE ? LBS_OWNERDRAWVARIABLE : 0) |
                  LBS_NOTIFY | WS_VSCROLL;
        if (kind(h) == CBS_SIMPLE) c.list = wm_create(0x200, "ComboLBox", "", lbs | WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 0, 0, h, 1000);
        else c.list = wm_create(0x8 | 0x80, "ComboLBox", "", lbs | WS_POPUP | WS_BORDER, 0, 0, 0, 0, h, 1000);   // WS_EX_TOPMOST | TOOLWINDOW
        g_lb[c.list].combo = h;
        if (kind(h) != CBS_DROPDOWNLIST) {
            c.edit = wm_create(0, "Edit", "", WS_CHILD | WS_VISIBLE | 0x80 | (st & 0x2000 ? 8u : 0u) | (st & 0x4000 ? 0x10u : 0u), 0, 0, 0, 0, h, 1001);
        }
        // altura del campo: Windows ajusta la del combo cerrado
        if (kind(h) != CBS_SIMPLE) { Rect s = wm_screen(h, false); u32 p = wm_parent(h); Rect ps = p ? wm_screen(p, true) : Rect{}; wm_move(h, s.l - ps.l, s.t - ps.t, s.w(), field_height(h, c)); }
        layout(h, c);
        RET(0, 4);
    }
    case WM_NCDESTROY: if (c.list) wm_destroy(c.list); g_cb.erase(h); break;
    case WM_SIZE: layout(h, c); wm_invalidate(h, true); RET(0, 4);
    case WM_PAINT: { Paint dc(h, c.font); cb_paint(h, dc); RET(0, 4); }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_SETFONT:
        c.font = wp; c.field_h = 0;
        if (c.edit) wm_send(c.edit, WM_SETFONT, wp, lp);
        if (c.list) { g_lb[c.list].font = wp; g_lb[c.list].item_h = 0; }
        layout(h, c);
        if (lp) wm_invalidate(h, true);
        RET(0, 4);
    case WM_GETFONT: RET(c.font, 4);
    case WM_ENABLE: if (c.edit) wm_send(c.edit, WM_ENABLE, wp, 0); wm_invalidate(h, true); RET(0, 4);
    case WM_SETTEXT: if (c.edit) RET(wm_send(c.edit, WM_SETTEXT, wp, lp), 4); { i32 i = (i32)wm_send(c.list, 0x1A2, (u32)-1, lp); if (i >= 0) wm_send(c.list, 0x186, (u32)i, 0); wm_invalidate(h, true); } RET(1, 4);
    case WM_GETTEXT: if (c.edit) RET(wm_send(c.edit, WM_GETTEXT, wp, lp), 4); RET(wp ? gstrcpy(lp, sel_text(c), wp) : 0, 4);
    case WM_GETTEXTLENGTH: if (c.edit) RET(wm_send(c.edit, WM_GETTEXTLENGTH, 0, 0), 4); RET((u32)sel_text(c).size(), 4);
    case WM_SETFOCUS: c.focus = true; if (c.edit) wm_set_focus(c.edit); wm_invalidate(h, true); notify(h, 3); RET(0, 4);
    case WM_KILLFOCUS: if (wp == c.edit || wp == c.list) RET(0, 4); c.focus = false; close_up(h, c, false); wm_invalidate(h, true); notify(h, 4); RET(0, 4);
    case WM_COMMAND:   // del cuadro de edición
        if (lp == c.edit && (wp >> 16) == 0x300) notify(h, 5);   // EN_CHANGE → CBN_EDITCHANGE
        if (lp == c.edit && (wp >> 16) == 0x400) notify(h, 6);   // EN_UPDATE → CBN_EDITUPDATE
        if (lp == c.edit && (wp >> 16) == 0x100) { c.focus = true; notify(h, 3); }
        if (lp == c.edit && (wp >> 16) == 0x200 && wm_focus() != h && wm_focus() != c.list) { c.focus = false; notify(h, 4); }
        RET(0, 4);
    case WM_GETDLGCODE: RET(1 | 0x80, 4);
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (wm_focus() != h && (!c.edit || wm_focus() != c.edit)) wm_set_focus(h);
        if (kind(h) == CBS_SIMPLE) RET(0, 4);
        if (c.dropped) close_up(h, c, true); else drop_down(h, c);
        RET(0, 4);
    case WM_KEYDOWN:
        if (wp == 0x73 || ((wp == 0x26 || wp == 0x28) && key_state(0x12))) { if (c.dropped) close_up(h, c, true); else drop_down(h, c); RET(0, 4); }   // F4 / Alt+flecha
        if (c.dropped && (wp == 0x0D || wp == 0x1B)) { close_up(h, c, wp == 0x0D); RET(0, 4); }
        wm_send(c.list, WM_KEYDOWN, wp, lp);
        RET(0, 4);
    case WM_CHAR: if (!c.edit) wm_send(c.list, WM_CHAR, wp, lp); RET(0, 4);
    case WM_MOUSEWHEEL: wm_send(c.list, WM_KEYDOWN, hi16(wp) > 0 ? 0x26 : 0x28, 0); RET(0, 4);
    case 0x14E: { u32 r = wm_send(c.list, 0x186, wp, 0); if (c.edit) { std::string s = sel_text(c); u32 p = heap_alloc((u32)s.size() + 1); gstrcpy(p, s); wm_send(c.edit, WM_SETTEXT, 0, p); heap_free(p); } wm_invalidate(h, true); RET(r, 4); }   // CB_SETCURSEL
    case 0x14D: { u32 r = wm_send(c.list, 0x18C, wp, lp); if (c.edit && r != LB_ERR) { std::string s = sel_text(c); u32 p = heap_alloc((u32)s.size() + 1); gstrcpy(p, s); wm_send(c.edit, WM_SETTEXT, 0, p); heap_free(p); } wm_invalidate(h, true); RET(r, 4); }   // CB_SELECTSTRING
    case 0x14F: if (wp) drop_down(h, c); else close_up(h, c, false); RET(1, 4);   // CB_SHOWDROPDOWN
    case 0x157: RET(c.dropped, 4);                                                // CB_GETDROPPEDSTATE
    case 0x152: { Rect s = wm_screen(h, false); wr_rect(lp, {s.l, s.t, s.r, s.t + c.drop_h}); RET(1, 4); }   // CB_GETDROPPEDCONTROLRECT
    case 0x153: if ((i32)wp == -1) { c.field_h = lo16(lp) + 6; layout(h, c); } else wm_send(c.list, 0x1A0, 0, lp); RET(0, 4);   // CB_SETITEMHEIGHT
    case 0x154: RET((i32)wp == -1 ? (u32)field_height(h, c) - 6 : wm_send(c.list, 0x1A1, 0, 0), 4);
    case 0x140: { u32 r = c.edit ? wm_send(c.edit, 0xB0, wp, lp) : 0; RET(r, 4); }                    // CB_GETEDITSEL
    case 0x142: if (c.edit) wm_send(c.edit, 0xB1, (u32)lo16(lp), (u32)hi16(lp)); RET(1, 4);           // CB_SETEDITSEL
    case 0x141: if (c.edit) wm_send(c.edit, 0xC5, wp, 0); RET(1, 4);                                   // CB_LIMITTEXT
    case 0x155: case 0x156: RET(0, 4);                                                                 // extended UI
    case 0x15F: RET((u32)std::max(c.drop_w, wm_client(h).r), 4);                                       // CB_GETDROPPEDWIDTH
    case 0x160: c.drop_w = (i32)wp; RET(wp, 4);
    }
    def_proc();
}

// lista desplegable de un combo
void combolbox_proc() {
    u32 h = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    u32 combo = g_lb.count(h) ? g_lb[h].combo : 0;
    bool popup = combo && !(wm_style(h) & WS_CHILD);
    if (popup && combo && g_cb.count(combo)) {
        Cb &c = g_cb[combo];
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
            Rect cl = wm_client(h);
            if (lo16(lp) < 0 || hi16(lp) < 0 || lo16(lp) >= cl.r || hi16(lp) >= cl.b) {   // fuera: se cierra
                close_up(combo, c, false);
                RET(0, 4);
            }
            break;
        }
        case WM_LBUTTONUP: {
            Rect cl = wm_client(h);
            if (lo16(lp) >= 0 && hi16(lp) >= 0 && lo16(lp) < cl.r && hi16(lp) < cl.b) {
                Lb &l = g_lb[h];
                l.tracking = false;
                close_up(combo, c, true);
                RET(0, 4);
            }
            wm_set_capture(h);   // el clic que abrió la lista: se sigue con ella abierta
            RET(0, 4);
        }
        case WM_MOUSEMOVE: {   // la selección sigue al ratón aunque no esté pulsado
            Lb &l = g_lb[h];
            Rect cl = wm_client(h);
            if (lo16(lp) >= 0 && hi16(lp) >= 0 && lo16(lp) < cl.r && hi16(lp) < cl.b) {
                i32 i = at_point(h, l, hi16(lp));
                if (i >= 0 && i != l.cur) { l.cur = i; wm_invalidate(h, true); combo_list_changed(combo); }
            }
            RET(0, 4);
        }
        case WM_KEYDOWN:
            if (wp == 0x0D || wp == 0x1B) { close_up(combo, c, wp == 0x0D); RET(0, 4); }
            break;
        case WM_KILLFOCUS: close_up(combo, c, false); break;
        }
    }
    listbox_proc();
}

