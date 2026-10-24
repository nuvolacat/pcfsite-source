// PCF7 — control EDIT de Windows en el host: una línea (con desplazamiento horizontal) y multilínea (con
// ajuste de línea y desplazamiento vertical), selección con ratón y teclado, cursor de texto, portapapeles
// interno, ES_PASSWORD/READONLY/NUMBER/UPPERCASE/LOWERCASE, límite de texto y las notificaciones EN_*.
#include "ctlutil.h"

#include <algorithm>
#include <map>
#include <vector>

using namespace ctl;

std::string g_clipboard_text;   // portapapeles de texto (también lo usan OpenClipboard/GetClipboardData)

namespace {

constexpr u32 ES_CENTER = 1, ES_RIGHT = 2, ES_MULTILINE = 4, ES_UPPERCASE = 8, ES_LOWERCASE = 0x10, ES_PASSWORD = 0x20,
              ES_AUTOHSCROLL = 0x80, ES_NOHIDESEL = 0x100, ES_READONLY = 0x800, ES_WANTRETURN = 0x1000, ES_NUMBER = 0x2000;
constexpr i32 MARGIN = 1;

struct Ed {
    u32 font = 0;
    std::string text;
    u32 anchor = 0, caret = 0;   // selección [min, max)
    i32 xoff = 0;                // desplazamiento horizontal (una línea)
    i32 top = 0;                 // primera línea visible (multilínea)
    u32 limit = 0x7FFFFFFE;
    bool modified = false, focus = false, selecting = false;
    char pass = '*';
    std::string undo;
    u32 undo_caret = 0;
    i32 lmargin = MARGIN, rmargin = MARGIN;
};
std::map<u32, Ed> g_ed;

bool multi(u32 h) { return wm_style(h) & ES_MULTILINE; }
u32 smin(const Ed &e) { return std::min(e.anchor, e.caret); }
u32 smax(const Ed &e) { return std::max(e.anchor, e.caret); }

std::string shown(u32 h, const Ed &e, const std::string &s) {   // lo que se ve (contraseña)
    if (!(wm_style(h) & ES_PASSWORD) || multi(h)) return s;
    return std::string(s.size(), e.pass);
}

// líneas (multilínea): [inicio, fin) de cada una, con ajuste de palabra si no hay ES_AUTOHSCROLL
struct Line { u32 a, b; };
std::vector<Line> layout(u32 h, Ed &e, u32 dc) {
    std::vector<Line> out;
    const std::string &t = e.text;
    bool wrap = multi(h) && !(wm_style(h) & ES_AUTOHSCROLL) && !(wm_style(h) & WS_HSCROLL);
    i32 width = wm_client(h).r - e.lmargin - e.rmargin;
    u32 i = 0;
    while (true) {
        u32 eol = (u32)t.find('\n', i);
        if (eol == (u32)std::string::npos) eol = (u32)t.size();
        u32 end = eol > i && t[eol - 1] == '\r' ? eol - 1 : eol;
        u32 a = i;
        if (wrap && width > 8) {
            while (a < end) {
                u32 b = a, last_space = 0;
                i32 w = 0;
                while (b < end) {
                    i32 cw = gdi_dc_text_width(dc, std::string(1, t[b]));
                    if (w + cw > width && b > a) break;
                    w += cw;
                    if (t[b] == ' ') last_space = b + 1;
                    b++;
                }
                if (b < end && last_space > a) b = last_space;
                out.push_back({a, b});
                a = b;
            }
            if (a == i) out.push_back({i, end});   // línea vacía
            else if (out.back().b != end) out.push_back({a, end});
        } else out.push_back({i, end});
        if (eol >= t.size()) break;
        i = eol + 1;
    }
    return out;
}
int line_of(const std::vector<Line> &ls, u32 pos) {
    for (int i = 0; i < (int)ls.size(); i++) if (pos >= ls[i].a && (pos <= ls[i].b || i + 1 == (int)ls.size() || pos < ls[i + 1].a)) {
        if (pos == ls[i].b && i + 1 < (int)ls.size() && ls[i + 1].a == pos) return i + 1;   // ajuste: el final de una es el principio de otra
        return i;
    }
    return (int)ls.size() - 1;
}

i32 text_x(u32 h, Ed &e, u32 dc, const std::string &line, u32 n) { return gdi_dc_text_width(dc, shown(h, e, line.substr(0, n))); }

// carácter más cercano a una x dentro de una línea
u32 hit_char(u32 h, Ed &e, u32 dc, const std::string &line, i32 x) {
    std::string s = shown(h, e, line);
    i32 acc = 0;
    for (u32 i = 0; i < s.size(); i++) {
        i32 w = gdi_dc_text_width(dc, std::string(1, s[i]));
        if (x < acc + w / 2) return i;
        acc += w;
    }
    return (u32)s.size();
}

i32 line_h(u32 dc) { return std::max(1, gdi_dc_text_height(dc)); }
i32 visible_lines(u32 h, u32 dc) { return std::max(1, wm_client(h).b / line_h(dc)); }

i32 align_x(u32 h, Ed &e, u32 dc, const std::string &line) {
    u32 st = wm_style(h);
    i32 w = wm_client(h).r - e.lmargin - e.rmargin, tw = gdi_dc_text_width(dc, shown(h, e, line));
    if ((st & 3) == ES_CENTER) return e.lmargin + std::max(0, (w - tw) / 2);
    if ((st & 3) == ES_RIGHT) return e.lmargin + std::max(0, w - tw);
    return e.lmargin;
}

void update_scrollbars(u32 h, Ed &e, u32 dc) {
    if (!(wm_style(h) & WS_VSCROLL) || !multi(h)) return;
    auto ls = layout(h, e, dc);
    SInfo s = sb_info(h, 1);
    s.min = 0; s.max = std::max(0, (i32)ls.size() - 1); s.page = visible_lines(h, dc); s.pos = e.top;
    sb_set(h, 1, s, true);
}

// el cursor siempre a la vista
void scroll_to_caret(u32 h, Ed &e, u32 dc) {
    if (multi(h)) {
        auto ls = layout(h, e, dc);
        int l = line_of(ls, e.caret), vis = visible_lines(h, dc);
        if (l < e.top) e.top = l;
        if (l >= e.top + vis) e.top = l - vis + 1;
        e.top = std::max(0, e.top);
        update_scrollbars(h, e, dc);
        return;
    }
    i32 w = wm_client(h).r - e.lmargin - e.rmargin;
    i32 cx = text_x(h, e, dc, e.text, e.caret);
    if (!(wm_style(h) & ES_AUTOHSCROLL) && !multi(h)) { e.xoff = 0; return; }
    if (cx - e.xoff > w) e.xoff = cx - w + w / 3;
    if (cx - e.xoff < 0) e.xoff = std::max(0, cx - w / 3);
}

void place_caret(u32 h, Ed &e) {
    if (!e.focus) return;
    DCx dc(h, e.font);
    i32 lh = line_h(dc);
    if (multi(h)) {
        auto ls = layout(h, e, dc);
        int l = line_of(ls, e.caret);
        std::string line = e.text.substr(ls[l].a, ls[l].b - ls[l].a);
        caret_pos(align_x(h, e, dc, line) + text_x(h, e, dc, line, e.caret - ls[l].a), (l - e.top) * lh);
    } else {
        i32 y = std::max(0, (wm_client(h).b - lh) / 2);
        if (!(wm_style(h) & ES_AUTOHSCROLL)) y = 0;
        caret_pos(align_x(h, e, dc, e.text) + text_x(h, e, dc, e.text, e.caret) - e.xoff, y);
    }
}

void paint(u32 h, u32 dc) {
    Ed &e = g_ed[h];
    Rect r = wm_client(h);
    u32 st = wm_style(h);
    bool en = wm_enabled(h), ro = st & ES_READONLY;
    u32 br = ctl_color(h, dc, (ro || !en) ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT);
    if (!br) { gdi_set_text_color(dc, C(COLOR_WINDOWTEXT)); gdi_set_bk(dc, ro || !en ? C(COLOR_BTNFACE) : C(COLOR_WINDOW), 2); }
    u32 bg = ro || !en ? C(COLOR_BTNFACE) : C(COLOR_WINDOW);
    fill_bg(dc, r, br, bg);
    i32 lh = line_h(dc);
    bool show_sel = e.focus || (st & ES_NOHIDESEL);
    u32 s0 = smin(e), s1 = smax(e);
    auto draw_line = [&](u32 a, u32 b, i32 y) {
        std::string line = e.text.substr(a, b - a);
        i32 x = align_x(h, e, dc, line) - (multi(h) ? 0 : e.xoff);
        std::string vis = shown(h, e, line);
        Rect tr{x, y, r.r, y + lh};
        if (!en) { draw_label(dc, vis, tr, 0x800 | 0x20, false, ~0u); return; }
        gdi_set_bk(dc, 0, 1);
        gdi_draw_text(dc, vis, tr, 0x800 | 0x20 | 0x100);
        if (show_sel && s1 > s0 && s1 > a && s0 < b) {
            u32 ia = std::max(s0, a) - a, ib = std::min(s1, b) - a;
            i32 xa = x + gdi_dc_text_width(dc, vis.substr(0, ia)), xb = x + gdi_dc_text_width(dc, vis.substr(0, ib));
            gdi_fill_color(dc, {xa, y, xb, y + lh}, C(COLOR_HIGHLIGHT));
            u32 old = 0; (void)old;
            gdi_set_text_color(dc, C(COLOR_HIGHLIGHTTEXT));
            Rect sr{xa, y, r.r, y + lh};
            gdi_draw_text(dc, vis.substr(ia, ib - ia), sr, 0x800 | 0x20 | 0x100);
            if (!br) gdi_set_text_color(dc, C(COLOR_WINDOWTEXT));
            else ctl_color(h, dc, (ro || !en) ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT);
        }
    };
    if (multi(h)) {
        auto ls = layout(h, e, dc);
        for (int i = e.top; i < (int)ls.size(); i++) {
            i32 y = (i - e.top) * lh;
            if (y >= r.b) break;
            draw_line(ls[i].a, ls[i].b, y);
        }
    } else {
        i32 y = (st & ES_AUTOHSCROLL) ? std::max(0, (r.b - lh) / 2) : 0;
        draw_line(0, (u32)e.text.size(), y);
    }
}

void changed(u32 h, Ed &e) {
    e.modified = true;
    wm_text(h) = e.text;
    DCx dc(h, e.font);
    scroll_to_caret(h, e, dc);
    wm_invalidate(h, true);
    place_caret(h, e);
    notify(h, 0x400);   // EN_UPDATE
    notify(h, 0x300);   // EN_CHANGE
}

void set_sel(u32 h, Ed &e, u32 a, u32 c) {
    u32 n = (u32)e.text.size();
    e.anchor = std::min(a, n);
    e.caret = std::min(c, n);
    DCx dc(h, e.font);
    scroll_to_caret(h, e, dc);
    wm_invalidate(h, true);
    place_caret(h, e);
}

// sustituye la selección por s (respeta el límite y los filtros del estilo); devuelve si cambió algo
bool replace_sel(u32 h, Ed &e, std::string s, bool user) {
    u32 st = wm_style(h);
    if (user && (st & ES_READONLY)) return false;
    if (!multi(h)) { s.erase(std::remove(s.begin(), s.end(), '\n'), s.end()); s.erase(std::remove(s.begin(), s.end(), '\r'), s.end()); }
    if (st & ES_UPPERCASE) for (auto &c : s) c = (char)std::toupper((unsigned char)c);
    if (st & ES_LOWERCASE) for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    if (user && (st & ES_NUMBER)) for (char c : s) if (c < '0' || c > '9') return false;
    u32 a = smin(e), b = smax(e);
    u32 room = e.limit > e.text.size() - (b - a) ? e.limit - (u32)(e.text.size() - (b - a)) : 0;
    if (s.size() > room) { s.resize(room); notify(h, 0x501); }   // EN_MAXTEXT
    if (s.empty() && a == b) return false;
    e.undo = e.text;
    e.undo_caret = e.caret;
    e.text.replace(a, b - a, s);
    e.anchor = e.caret = a + (u32)s.size();
    changed(h, e);
    return true;
}

u32 word_left(const std::string &t, u32 p) {
    while (p > 0 && t[p - 1] == ' ') p--;
    while (p > 0 && t[p - 1] != ' ' && t[p - 1] != '\n') p--;
    return p;
}
u32 word_right(const std::string &t, u32 p) {
    while (p < t.size() && t[p] != ' ' && t[p] != '\n') p++;
    while (p < t.size() && t[p] == ' ') p++;
    return p;
}

u32 pos_from_point(u32 h, Ed &e, i32 x, i32 y) {
    DCx dc(h, e.font);
    if (multi(h)) {
        auto ls = layout(h, e, dc);
        int l = std::clamp(e.top + y / line_h(dc), 0, (int)ls.size() - 1);
        std::string line = e.text.substr(ls[l].a, ls[l].b - ls[l].a);
        return ls[l].a + hit_char(h, e, dc, line, x - align_x(h, e, dc, line));
    }
    return hit_char(h, e, dc, e.text, x - align_x(h, e, dc, e.text) + e.xoff);
}

void key(u32 h, Ed &e, u32 vk) {
    bool shift = key_down(0x10), ctrl = key_down(0x11);
    u32 c = e.caret, n = (u32)e.text.size();
    auto move = [&](u32 to) { set_sel(h, e, shift ? e.anchor : to, to); };
    switch (vk) {
    case 0x25: move(ctrl ? word_left(e.text, c) : (!shift && smin(e) != smax(e) ? smin(e) : c > 0 ? c - 1 - (c >= 2 && e.text[c - 1] == '\n' && e.text[c - 2] == '\r') : 0)); break;
    case 0x27: move(ctrl ? word_right(e.text, c) : (!shift && smin(e) != smax(e) ? smax(e) : c < n ? c + 1 + (e.text[c] == '\r' && c + 1 < n && e.text[c + 1] == '\n') : n)); break;
    case 0x24: case 0x23: {   // inicio / fin
        if (!multi(h) || ctrl) { move(vk == 0x24 ? 0 : n); break; }
        DCx dc(h, e.font);
        auto ls = layout(h, e, dc);
        int l = line_of(ls, c);
        move(vk == 0x24 ? ls[l].a : ls[l].b);
        break;
    }
    case 0x26: case 0x28: case 0x21: case 0x22: {   // arriba, abajo, re pág, av pág
        if (!multi(h)) break;
        DCx dc(h, e.font);
        auto ls = layout(h, e, dc);
        int l = line_of(ls, c), step = (vk == 0x21 || vk == 0x22) ? visible_lines(h, dc) : 1;
        int nl = std::clamp(l + ((vk == 0x26 || vk == 0x21) ? -step : step), 0, (int)ls.size() - 1);
        std::string cur = e.text.substr(ls[l].a, ls[l].b - ls[l].a);
        i32 x = align_x(h, e, dc, cur) + text_x(h, e, dc, cur, c - ls[l].a);
        std::string tl = e.text.substr(ls[nl].a, ls[nl].b - ls[nl].a);
        move(ls[nl].a + hit_char(h, e, dc, tl, x - align_x(h, e, dc, tl)));
        break;
    }
    case 0x2E:   // suprimir
        if (wm_style(h) & ES_READONLY) break;
        if (shift && smin(e) != smax(e)) { wm_send(h, WM_CUT, 0, 0); break; }
        if (smin(e) == smax(e) && c < n) e.anchor = c + 1 + (e.text[c] == '\r' && c + 1 < n && e.text[c + 1] == '\n');
        replace_sel(h, e, "", true);
        break;
    case 0x2D:   // insertar: Ctrl copia, Mayús pega
        if (ctrl) wm_send(h, WM_COPY, 0, 0);
        else if (shift) wm_send(h, WM_PASTE, 0, 0);
        break;
    }
}

}  // namespace

void edit_proc() {
    u32 h = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    Ed &e = g_ed[h];
    switch (msg) {
    case WM_NCCREATE: {
        u32 t = rd32(lp + 36);
        e.text = t ? gstr(t) : "";
        if (!multi(h)) { e.text.erase(std::remove(e.text.begin(), e.text.end(), '\n'), e.text.end()); e.text.erase(std::remove(e.text.begin(), e.text.end(), '\r'), e.text.end()); }
        wm_text(h) = e.text;
        RET(1, 4);
    }
    case WM_NCDESTROY: if (e.focus) caret_destroy(); g_ed.erase(h); break;
    case WM_PAINT: { Paint dc(h, e.font); paint(h, dc); RET(0, 4); }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_SETFONT: e.font = wp; if (lp) wm_invalidate(h, true); if (e.focus) { DCx dc(h, e.font); caret_create(h, 1, line_h(dc)); caret_show(true); place_caret(h, e); } RET(0, 4);
    case WM_GETFONT: RET(e.font, 4);
    case WM_SETTEXT: {
        e.text = lp ? gstr(lp) : "";
        if (!multi(h)) { e.text.erase(std::remove(e.text.begin(), e.text.end(), '\n'), e.text.end()); e.text.erase(std::remove(e.text.begin(), e.text.end(), '\r'), e.text.end()); }
        if (e.text.size() > e.limit) e.text.resize(e.limit);
        wm_text(h) = e.text;
        e.anchor = e.caret = 0;
        e.xoff = 0; e.top = 0;
        e.modified = false;
        { DCx dc(h, e.font); update_scrollbars(h, e, dc); }
        wm_invalidate(h, true);
        place_caret(h, e);
        notify(h, 0x400);
        notify(h, 0x300);
        RET(1, 4);
    }
    case WM_GETTEXT: { u32 n = wp ? gstrcpy(lp, e.text, wp) : 0; RET(n, 4); }
    case WM_GETTEXTLENGTH: RET((u32)e.text.size(), 4);
    case WM_ENABLE: wm_invalidate(h, true); RET(0, 4);
    case WM_SIZE: { DCx dc(h, e.font); scroll_to_caret(h, e, dc); wm_invalidate(h, true); RET(0, 4); }
    case WM_SETFOCUS: {
        e.focus = true;
        DCx dc(h, e.font);
        caret_create(h, 1, line_h(dc));
        caret_show(true);
        place_caret(h, e);
        wm_invalidate(h, true);
        notify(h, 0x100);   // EN_SETFOCUS
        RET(0, 4);
    }
    case WM_KILLFOCUS:
        e.focus = false;
        caret_destroy();
        wm_invalidate(h, true);
        notify(h, 0x200);   // EN_KILLFOCUS
        RET(0, 4);
    case WM_GETDLGCODE: {
        u32 r = 1 | 2 | 0x80;   // DLGC_WANTARROWS | DLGC_WANTTAB(?) no: WANTCHARS
        r = 1 | 0x80 | 0x8;     // WANTARROWS | WANTCHARS | HASSETSEL
        if (multi(h)) r |= 4;   // DLGC_WANTALLKEYS
        RET(r, 4);
    }
    case WM_LBUTTONDOWN: {
        if (wm_focus() != h) wm_set_focus(h);
        u32 p = pos_from_point(h, e, lo16(lp), hi16(lp));
        bool shift = key_down(0x10);
        set_sel(h, e, shift ? e.anchor : p, p);
        e.selecting = true;
        wm_set_capture(h);
        RET(0, 4);
    }
    case WM_MOUSEMOVE:
        if (e.selecting) set_sel(h, e, e.anchor, pos_from_point(h, e, lo16(lp), hi16(lp)));
        RET(0, 4);
    case WM_LBUTTONUP:
        if (e.selecting) { e.selecting = false; if (wm_capture() == h) wm_set_capture(0); }
        RET(0, 4);
    case WM_LBUTTONDBLCLK: {   // selecciona la palabra
        u32 p = pos_from_point(h, e, lo16(lp), hi16(lp));
        u32 a = p, b = p;
        while (a > 0 && e.text[a - 1] != ' ' && e.text[a - 1] != '\n') a--;
        while (b < e.text.size() && e.text[b] != ' ' && e.text[b] != '\n' && e.text[b] != '\r') b++;
        set_sel(h, e, a, b);
        RET(0, 4);
    }
    case WM_KEYDOWN: key(h, e, wp); RET(0, 4);
    case WM_CHAR: {
        u32 c = wp & 0xFF;
        bool ctrl = key_down(0x11);
        if (c == 3) { wm_send(h, WM_COPY, 0, 0); RET(0, 4); }    // Ctrl+C
        if (c == 22) { wm_send(h, WM_PASTE, 0, 0); RET(0, 4); }  // Ctrl+V
        if (c == 24) { wm_send(h, WM_CUT, 0, 0); RET(0, 4); }    // Ctrl+X
        if (c == 26) { wm_send(h, WM_UNDO, 0, 0); RET(0, 4); }   // Ctrl+Z
        if (c == 1) { set_sel(h, e, 0, (u32)e.text.size()); RET(0, 4); }   // Ctrl+A
        if (c == 8) {   // retroceso
            if (wm_style(h) & ES_READONLY) RET(0, 4);
            if (smin(e) == smax(e) && e.caret > 0) {
                u32 k = ctrl ? word_left(e.text, e.caret) : e.caret - 1 - (e.caret >= 2 && e.text[e.caret - 1] == '\n' && e.text[e.caret - 2] == '\r');
                e.anchor = k;
            }
            replace_sel(h, e, "", true);
            RET(0, 4);
        }
        if (c == 13) {
            if (multi(h) && ((wm_style(h) & ES_WANTRETURN) || true)) replace_sel(h, e, "\r\n", true);
            RET(0, 4);
        }
        if (c == 9 || c == 27 || c < 32) RET(0, 4);
        replace_sel(h, e, std::string(1, (char)c), true);
        RET(0, 4);
    }
    case WM_VSCROLL: {
        if (!multi(h)) RET(0, 4);
        DCx dc(h, e.font);
        auto ls = layout(h, e, dc);
        int vis = visible_lines(h, dc), maxtop = std::max(0, (int)ls.size() - vis);
        switch (wp & 0xFFFF) {
        case 0: e.top--; break;
        case 1: e.top++; break;
        case 2: e.top -= vis; break;
        case 3: e.top += vis; break;
        case 4: case 5: e.top = hi16(wp) & 0xFFFF; break;
        case 6: e.top = 0; break;
        case 7: e.top = maxtop; break;
        }
        e.top = std::clamp(e.top, 0, maxtop);
        update_scrollbars(h, e, dc);
        wm_invalidate(h, true);
        place_caret(h, e);
        RET(0, 4);
    }
    case WM_MOUSEWHEEL: { u32 code = hi16(wp) > 0 ? 0 : 1; for (int i = 0; i < 3; i++) wm_send(h, WM_VSCROLL, code, 0); RET(0, 4); }
    case WM_COPY:
        if (smax(e) > smin(e) && !(wm_style(h) & ES_PASSWORD)) g_clipboard_text = e.text.substr(smin(e), smax(e) - smin(e));
        RET(0, 4);
    case WM_CUT: if (smax(e) > smin(e) && !(wm_style(h) & ES_PASSWORD)) { g_clipboard_text = e.text.substr(smin(e), smax(e) - smin(e)); replace_sel(h, e, "", true); } RET(0, 4);
    case WM_PASTE: replace_sel(h, e, g_clipboard_text, true); RET(0, 4);
    case WM_CLEAR: replace_sel(h, e, "", true); RET(0, 4);
    case WM_UNDO: case 0xC7: {   // EM_UNDO
        std::string t = e.undo;
        e.undo = e.text;
        e.text = t;
        e.anchor = e.caret = std::min(e.undo_caret, (u32)e.text.size());
        changed(h, e);
        RET(1, 4);
    }
    // ---- EM_*
    case 0xB0: {   // EM_GETSEL
        if (wp) wr32(wp, smin(e));
        if (lp) wr32(lp, smax(e));
        RET((smax(e) << 16) | (smin(e) & 0xFFFF), 4);
    }
    case 0xB1: {   // EM_SETSEL (-1 inicio: quitar selección)
        i32 a = (i32)wp, b = (i32)lp;
        u32 n = (u32)e.text.size();
        if (a == -1) { set_sel(h, e, e.caret, e.caret); RET(0, 4); }
        u32 ua = a < 0 ? n : std::min((u32)a, n), ub = b < 0 ? n : std::min((u32)b, n);
        set_sel(h, e, ua, ub);
        RET(0, 4);
    }
    case 0xB2: case 0xB3: case 0xB4: if (msg == 0xB2) wr_rect(lp, wm_client(h)); RET(0, 4);   // EM_GETRECT / SETRECT(NP)
    case 0xB5: { wm_send(h, WM_VSCROLL, wp, 0); RET(1, 4); }                                    // EM_SCROLL
    case 0xB6: {                                                                                 // EM_LINESCROLL
        DCx dc(h, e.font);
        auto ls = layout(h, e, dc);
        e.top = std::clamp(e.top + (i32)lp, 0, std::max(0, (int)ls.size() - 1));
        update_scrollbars(h, e, dc);
        wm_invalidate(h, true);
        RET(1, 4);
    }
    case 0xB7: { DCx dc(h, e.font); scroll_to_caret(h, e, dc); wm_invalidate(h, true); RET(1, 4); }   // EM_SCROLLCARET
    case 0xB8: RET(e.modified, 4);                                                               // EM_GETMODIFY
    case 0xB9: e.modified = wp != 0; RET(0, 4);                                                  // EM_SETMODIFY
    case 0xBA: { if (!multi(h)) RET(1, 4); DCx dc(h, e.font); RET((u32)layout(h, e, dc).size(), 4); }   // EM_GETLINECOUNT
    case 0xBB: {                                                                                 // EM_LINEINDEX
        if (!multi(h)) RET(0, 4);
        DCx dc(h, e.font);
        auto ls = layout(h, e, dc);
        i32 l = (i32)wp == -1 ? line_of(ls, e.caret) : (i32)wp;
        RET(l >= 0 && l < (i32)ls.size() ? ls[l].a : (u32)-1, 4);
    }
    case 0xC1: {                                                                                 // EM_LINELENGTH
        if (!multi(h)) RET((u32)e.text.size(), 4);
        DCx dc(h, e.font);
        auto ls = layout(h, e, dc);
        int l = (i32)wp == -1 ? line_of(ls, e.caret) : line_of(ls, wp);
        RET(ls[l].b - ls[l].a, 4);
    }
    case 0xC2: replace_sel(h, e, gstr(lp), false); RET(0, 4);                                    // EM_REPLACESEL
    case 0xC4: {                                                                                 // EM_GETLINE
        DCx dc(h, e.font);
        auto ls = layout(h, e, dc);
        u32 l = multi(h) ? wp : 0;
        if (l >= ls.size()) RET(0, 4);
        u32 cap = rd16(lp);
        std::string s = e.text.substr(ls[l].a, ls[l].b - ls[l].a);
        u32 n = std::min<u32>(cap, (u32)s.size());
        for (u32 i = 0; i < n; i++) wr8(lp + i, (u8)s[i]);
        RET(n, 4);
    }
    case 0xC5: e.limit = wp ? wp : 0x7FFFFFFE; RET(0, 4);                                       // EM_LIMITTEXT
    case 0xD5: RET(e.limit, 4);                                                                  // EM_GETLIMITTEXT
    case 0xC6: RET(!e.undo.empty() || e.modified, 4);                                            // EM_CANUNDO
    case 0xCD: e.undo.clear(); RET(0, 4);                                                        // EM_EMPTYUNDOBUFFER
    case 0xC9: {                                                                                 // EM_LINEFROMCHAR
        if (!multi(h)) RET(0, 4);
        DCx dc(h, e.font);
        auto ls = layout(h, e, dc);
        RET((u32)line_of(ls, (i32)wp == -1 ? e.caret : wp), 4);
    }
    case 0xCB: RET(1, 4);                                                                        // EM_SETTABSTOPS
    case 0xCC: e.pass = wp ? (char)wp : '*'; wm_set_style(h, wp ? wm_style(h) | ES_PASSWORD : wm_style(h) & ~ES_PASSWORD); wm_invalidate(h, true); RET(0, 4);   // EM_SETPASSWORDCHAR
    case 0xD2: RET((wm_style(h) & ES_PASSWORD) ? (u8)e.pass : 0, 4);                            // EM_GETPASSWORDCHAR
    case 0xCE: RET((u32)(multi(h) ? e.top : 0), 4);                                              // EM_GETFIRSTVISIBLELINE
    case 0xCF: wm_set_style(h, wp ? wm_style(h) | ES_READONLY : wm_style(h) & ~ES_READONLY); wm_invalidate(h, true); RET(1, 4);   // EM_SETREADONLY
    case 0xD3: if (wp & 1) e.lmargin = lo16(lp) < 0 ? MARGIN : lo16(lp); if (wp & 2) e.rmargin = hi16(lp) < 0 ? MARGIN : hi16(lp); wm_invalidate(h, true); RET(0, 4);   // EM_SETMARGINS
    case 0xD4: RET(make_lp(e.lmargin, e.rmargin), 4);                                            // EM_GETMARGINS
    case 0xD6: {                                                                                 // EM_POSFROMCHAR
        DCx dc(h, e.font);
        u32 p = std::min<u32>(wp, (u32)e.text.size());
        if (multi(h)) {
            auto ls = layout(h, e, dc);
            int l = line_of(ls, p);
            std::string line = e.text.substr(ls[l].a, ls[l].b - ls[l].a);
            RET(make_lp(align_x(h, e, dc, line) + text_x(h, e, dc, line, p - ls[l].a), (l - e.top) * line_h(dc)), 4);
        }
        RET(make_lp(align_x(h, e, dc, e.text) + text_x(h, e, dc, e.text, p) - e.xoff, 0), 4);
    }
    case 0xD7: {                                                                                 // EM_CHARFROMPOS
        u32 p = pos_from_point(h, e, lo16(lp), hi16(lp));
        DCx dc(h, e.font);
        u32 line = multi(h) ? (u32)line_of(layout(h, e, dc), p) : 0;
        RET((line << 16) | (p & 0xFFFF), 4);
    }
    case 0xBC: case 0xBD: RET(0, 4);   // EM_SETHANDLE / GETHANDLE: no hay memoria local de 16 bits
    }
    def_proc();
}

// ---------------------------------------------------------------- portapapeles (solo texto, en el proceso)

namespace {
bool g_clip_open = false;
u32 g_clip_handle = 0;
}
STUB(ext_user32_dll_OpenClipboard) { g_clip_open = true; RET(1, 1); }
STUB(ext_user32_dll_CloseClipboard) { g_clip_open = false; RET(1, 0); }
STUB(ext_user32_dll_EmptyClipboard) { g_clipboard_text.clear(); RET(1, 0); }
STUB(ext_user32_dll_IsClipboardFormatAvailable) { RET((arg(0) == 1 || arg(0) == 7) && !g_clipboard_text.empty(), 1); }   // CF_TEXT / CF_OEMTEXT
STUB(ext_user32_dll_CountClipboardFormats) { RET(g_clipboard_text.empty() ? 0 : 1, 0); }
STUB(ext_user32_dll_EnumClipboardFormats) { RET(arg(0) == 0 && !g_clipboard_text.empty() ? 1 : 0, 1); }
STUB(ext_user32_dll_GetClipboardFormatNameA) { RET(0, 3); }
STUB(ext_user32_dll_SetClipboardData) {   // (format, HGLOBAL): el texto se copia
    if (arg(0) == 1 && arg(1)) g_clipboard_text = gstr(arg(1));
    RET(arg(1), 2);
}
STUB(ext_user32_dll_GetClipboardData) {
    if (arg(0) != 1 || g_clipboard_text.empty()) RET(0, 1);
    if (g_clip_handle) heap_free(g_clip_handle);
    g_clip_handle = heap_alloc((u32)g_clipboard_text.size() + 1);
    gstrcpy(g_clip_handle, g_clipboard_text);
    RET(g_clip_handle, 1);
}
