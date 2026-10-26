// PCF7 — menús y tablas de aceleradores de USER32 implementados en el host.
//
// Un menú es una lista de elementos (texto, separador, submenú…) con su identificador y estado. La barra
// de menú de una ventana se pinta en su área no cliente (user32.cpp llama a menu_bar_paint) y al pulsarla
// se abre su desplegable; los desplegables (también los de TrackPopupMenu) son ventanas emergentes de la
// clase "#32768" que se siguen con un bucle modal, como en Windows.
#include "ctlutil.h"

#include <map>
#include <vector>

using namespace ctl;

extern "C" {
void ext_user32_dll_GetKeyState(void);
}
u32 find_resource(u32 module, u32 name, u32 type);   // kernel32.cpp
u32 resource_module(u32 hres);

namespace {

constexpr u32 MF_GRAYED = 0x1, MF_DISABLED = 0x2, MF_BITMAP = 0x4, MF_CHECKED = 0x8, MF_POPUP = 0x10,
              MF_MENUBARBREAK = 0x20, MF_MENUBREAK = 0x40, MF_HILITE = 0x80, MF_OWNERDRAW = 0x100,
              MF_BYPOSITION = 0x400, MF_SEPARATOR = 0x800, MF_DEFAULT = 0x1000, MF_RIGHTJUSTIFY = 0x4000,
              MFT_RADIOCHECK = 0x200;
constexpr u32 TYPE_BITS = MF_BITMAP | MF_MENUBARBREAK | MF_MENUBREAK | MF_OWNERDRAW | MF_SEPARATOR | MF_RIGHTJUSTIFY | MFT_RADIOCHECK;
constexpr u32 STATE_BITS = MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE | MF_DEFAULT;
constexpr u32 WM_INITMENU = 0x116, WM_INITMENUPOPUP = 0x117, WM_MENUSELECT = 0x11F, WM_ENTERMENULOOP = 0x211,
              WM_EXITMENULOOP = 0x212, WM_SYSCOMMAND = 0x112;

struct Item {
    u32 type = 0, state = 0, id = 0, sub = 0, data = 0, bitmap = 0, bmp_checked = 0, bmp_unchecked = 0;
    std::string text;
    bool separator() const { return type & MF_SEPARATOR; }
};
struct Menu { std::vector<Item> items; bool popup = false; };

std::map<u32, Menu> g_menus;
u32 g_next_menu = 0x00E00010u;

Menu *menu(u32 h) { auto it = g_menus.find(h); return it == g_menus.end() ? nullptr : &it->second; }

u32 new_menu(bool popup) {
    u32 h = g_next_menu;
    g_next_menu += 4;
    g_menus[h].popup = popup;
    return h;
}

void destroy_menu(u32 h) {
    Menu *m = menu(h);
    if (!m) return;
    std::vector<u32> subs;
    for (auto &it : m->items) if (it.sub) subs.push_back(it.sub);
    g_menus.erase(h);
    for (u32 s : subs) destroy_menu(s);
}

// elemento por posición o por identificador (en submenús también); devuelve el menú que lo contiene
Item *find_item(u32 h, u32 key, bool bypos, u32 *owner = nullptr, u32 *index = nullptr) {
    Menu *m = menu(h);
    if (!m) return nullptr;
    if (bypos) {
        if (key >= m->items.size()) return nullptr;
        if (owner) *owner = h;
        if (index) *index = key;
        return &m->items[key];
    }
    for (u32 i = 0; i < m->items.size(); i++) {
        Item &it = m->items[i];
        if (!it.sub && it.id == key) { if (owner) *owner = h; if (index) *index = i; return &it; }
    }
    for (auto &it : m->items)
        if (it.sub)
            if (Item *r = find_item(it.sub, key, false, owner, index)) return r;
    for (u32 i = 0; i < m->items.size(); i++)   // un submenú también se busca por su identificador (= HMENU)
        if (m->items[i].sub && m->items[i].sub == key) { if (owner) *owner = h; if (index) *index = i; return &m->items[i]; }
    return nullptr;
}

// elemento desde los parámetros de AppendMenu/InsertMenu/ModifyMenu
Item make_item(u32 flags, u32 id, u32 data) {
    Item it;
    it.type = flags & TYPE_BITS;
    it.state = flags & STATE_BITS;
    if (flags & MF_POPUP) { it.sub = id; it.id = id; } else it.id = id & 0xFFFF;
    if (it.type & MF_OWNERDRAW) it.data = data;
    else if (it.type & MF_BITMAP) it.bitmap = data;
    else if (!(it.type & MF_SEPARATOR) && data) it.text = gstr(data);
    return it;
}

u32 legacy_flags(const Item &it) {
    return it.type | it.state | (it.sub ? MF_POPUP : 0);
}

// ---------------------------------------------------------------- dibujo

u32 menu_font() { return stock(17); }   // DEFAULT_GUI_FONT

std::string label_of(const std::string &s) {   // el texto sin la parte del atajo (tras el tabulador)
    size_t t = s.find('\t');
    return t == std::string::npos ? s : s.substr(0, t);
}
std::string accel_of(const std::string &s) {
    size_t t = s.find('\t');
    return t == std::string::npos ? std::string() : s.substr(t + 1);
}
i32 text_w(u32 dc, const std::string &s) {   // sin contar los & de los atajos
    std::string v;
    for (size_t i = 0; i < s.size(); i++) { if (s[i] == '&' && i + 1 < s.size()) { i++; } v.push_back(s[i]); }
    return gdi_dc_text_width(dc, v);
}

constexpr i32 BAR_PAD = 6, POP_ITEM_H = 17, POP_SEP_H = 9, POP_LEFT = 17, POP_RIGHT = 17;

// rectángulos de los elementos de la barra (relativos a su esquina)
std::vector<Rect> bar_layout(u32 h, u32 dc, i32 width) {
    std::vector<Rect> out;
    Menu *m = menu(h);
    if (!m) return out;
    i32 x = 0;
    for (auto &it : m->items) {
        i32 w = it.separator() ? 6 : text_w(dc, label_of(it.text)) + 2 * BAR_PAD;
        out.push_back({x, 0, x + w, 19});
        x += w;
    }
    (void)width;
    return out;
}

struct PopSize { i32 w = 0, h = 0; std::vector<Rect> items; };
PopSize popup_layout(u32 h, u32 dc) {
    PopSize ps;
    Menu *m = menu(h);
    if (!m) return ps;
    i32 lw = 0, aw = 0;
    for (auto &it : m->items) {
        if (it.separator()) continue;
        lw = std::max(lw, text_w(dc, label_of(it.text)));
        aw = std::max(aw, text_w(dc, accel_of(it.text)));
    }
    i32 w = POP_LEFT + lw + (aw ? 16 + aw : 0) + POP_RIGHT + 6;
    i32 y = 3;
    for (auto &it : m->items) {
        i32 ih = it.separator() ? POP_SEP_H : POP_ITEM_H;
        ps.items.push_back({3, y, w - 3, y + ih});
        y += ih;
    }
    ps.w = w;
    ps.h = y + 3;
    return ps;
}

bool item_enabled(const Item &it) { return !(it.state & (MF_GRAYED | MF_DISABLED)); }

void draw_check(u32 dc, const Rect &r, bool radio, u32 color) {
    i32 cx = r.l + 8, cy = (r.t + r.b) / 2;
    if (radio) {
        for (i32 dy = -2; dy <= 2; dy++) gdi_hline(dc, cx - 2 + (dy == -2 || dy == 2), cx + 3 - (dy == -2 || dy == 2), cy + dy, color);
        return;
    }
    for (i32 k = 0; k < 3; k++) {   // la marca ✓ de 7×7
        gdi_vline(dc, cx - 3 + k, cy - 1 + k, cy + 2 + k, color);
        gdi_vline(dc, cx + k, cy + 1 - k, cy + 4 - k, color);
        gdi_vline(dc, cx + 3 + k - 0, cy - 2 - k + 2, cy + 1 - k + 2, color);
    }
}

void draw_popup(u32 h, u32 dc, const Rect &cl, i32 hot) {
    gdi_select(dc, menu_font());
    gdi_fill_color(dc, cl, C(4));   // COLOR_MENU
    gdi_draw_edge(dc, cl, 5, 0xF);  // EDGE_RAISED
    Menu *m = menu(h);
    if (!m) return;
    PopSize ps = popup_layout(h, dc);
    for (size_t i = 0; i < m->items.size(); i++) {
        const Item &it = m->items[i];
        Rect r = ps.items[i];
        if (it.separator()) {
            i32 y = (r.t + r.b) / 2 - 1;
            gdi_hline(dc, r.l + 1, r.r - 1, y, C(COLOR_BTNSHADOW));
            gdi_hline(dc, r.l + 1, r.r - 1, y + 1, C(COLOR_BTNHIGHLIGHT));
            continue;
        }
        bool sel = (i32)i == hot;
        bool en = item_enabled(it);
        if (sel) gdi_fill_color(dc, r, C(COLOR_HIGHLIGHT));
        u32 col = sel ? C(COLOR_HIGHLIGHTTEXT) : C(7);   // COLOR_MENUTEXT
        if (it.state & MF_CHECKED) draw_check(dc, r, it.type & MFT_RADIOCHECK, en ? col : C(COLOR_GRAYTEXT));
        Rect tr{r.l + POP_LEFT, r.t, r.r - POP_RIGHT, r.b};
        u32 fmt = 0x20 | 0x4;   // DT_SINGLELINE | DT_VCENTER
        if (en) {
            draw_label(dc, label_of(it.text), tr, fmt, true, col);
            if (!accel_of(it.text).empty()) draw_label(dc, accel_of(it.text), tr, fmt | 0x2, true, col);   // DT_RIGHT
        } else if (sel) {
            draw_label(dc, label_of(it.text), tr, fmt, true, C(COLOR_GRAYTEXT));
        } else {
            draw_label(dc, label_of(it.text), tr, fmt, false, 0);
            if (!accel_of(it.text).empty()) draw_label(dc, accel_of(it.text), tr, fmt | 0x2, false, 0);
        }
        if (it.sub) {   // flecha del submenú
            i32 ax = r.r - 9, ay = (r.t + r.b) / 2;
            for (i32 k = 0; k < 4; k++) gdi_vline(dc, ax + k, ay - 3 + k, ay + 4 - k, en ? col : C(COLOR_GRAYTEXT));
        }
    }
}

// ---------------------------------------------------------------- seguimiento de menús (bucle modal)

struct Level { u32 menu = 0, wnd = 0; i32 hot = -1; Rect screen; };
struct Tracker {
    u32 owner = 0, bar = 0, bar_wnd = 0;   // ventana dueña; barra (si se sigue desde ella)
    std::vector<Rect> bar_items;           // en coordenadas de pantalla
    i32 bar_hot = -1;
    std::vector<Level> levels;
    bool done = false;
    u32 result = 0;                        // identificador elegido (0: nada)
    bool system = false;
};
Tracker *g_tr = nullptr;

void open_level(u32 h, i32 x, i32 y);

void close_levels(size_t keep) {
    while (g_tr->levels.size() > keep) {
        wm_destroy(g_tr->levels.back().wnd);
        g_tr->levels.pop_back();
    }
}

void set_hot(size_t lvl, i32 i) {
    Level &L = g_tr->levels[lvl];
    if (L.hot == i) return;
    L.hot = i;
    wm_invalidate(L.wnd, true);
    Menu *m = menu(L.menu);
    if (m && i >= 0 && i < (i32)m->items.size()) {
        const Item &it = m->items[(size_t)i];
        wm_send(g_tr->owner, WM_MENUSELECT, (it.sub ? (u32)i : it.id) | ((legacy_flags(it) | MF_HILITE) << 16), L.menu);
    }
}

// abre el submenú del elemento caliente de un nivel (a su derecha)
void open_sub(size_t lvl) {
    close_levels(lvl + 1);
    Level &L = g_tr->levels[lvl];
    Menu *m = menu(L.menu);
    if (!m || L.hot < 0) return;
    const Item &it = m->items[(size_t)L.hot];
    if (!it.sub || !item_enabled(it)) return;
    DCx dc(L.wnd, menu_font());
    PopSize ps = popup_layout(L.menu, dc);
    Rect r = ps.items[(size_t)L.hot];
    open_level(it.sub, L.screen.l + r.r - 3, L.screen.t + r.t - 3);
}

void choose(const Item &it) {
    if (it.sub || it.separator() || !item_enabled(it)) return;
    g_tr->result = it.id;
    g_tr->done = true;
}

// punto de pantalla → (nivel, elemento); -1 si fuera de los desplegables
bool hit(i32 sx, i32 sy, size_t &lvl, i32 &item) {
    for (size_t k = g_tr->levels.size(); k-- > 0;) {
        Level &L = g_tr->levels[k];
        if (sx < L.screen.l || sy < L.screen.t || sx >= L.screen.r || sy >= L.screen.b) continue;
        lvl = k;
        item = -1;
        DCx dc(L.wnd, menu_font());
        PopSize ps = popup_layout(L.menu, dc);
        for (size_t i = 0; i < ps.items.size(); i++) {
            const Rect &r = ps.items[i];
            if (sy - L.screen.t >= r.t && sy - L.screen.t < r.b) { item = (i32)i; break; }
        }
        return true;
    }
    return false;
}

i32 bar_hit(i32 sx, i32 sy) {
    for (size_t i = 0; i < g_tr->bar_items.size(); i++) {
        const Rect &r = g_tr->bar_items[i];
        if (sx >= r.l && sx < r.r && sy >= r.t && sy < r.b) return (i32)i;
    }
    return -1;
}

void open_bar_item(i32 i);

void popup_mouse(u32 msg, i32 sx, i32 sy) {
    size_t lvl;
    i32 item;
    if (hit(sx, sy, lvl, item)) {
        Level &L = g_tr->levels[lvl];
        Menu *m = menu(L.menu);
        if (msg == WM_MOUSEMOVE) {
            if (item != L.hot) {
                close_levels(lvl + 1);
                set_hot(lvl, item);
                if (item >= 0 && m && m->items[(size_t)item].sub) open_sub(lvl);
            }
        } else if (msg == WM_LBUTTONUP && item >= 0 && m) {
            choose(m->items[(size_t)item]);
        }
        return;
    }
    if (g_tr->bar) {
        i32 b = bar_hit(sx, sy);
        if (b >= 0) {
            if (msg == WM_MOUSEMOVE && b != g_tr->bar_hot) open_bar_item(b);
            else if (msg == WM_LBUTTONDOWN && b == g_tr->bar_hot) g_tr->done = true;   // segunda pulsación: cierra
            return;
        }
    }
    if (msg == WM_LBUTTONDOWN) g_tr->done = true;   // fuera: se cancela
}

void popup_key(u32 vk) {
    if (g_tr->levels.empty()) return;
    size_t lvl = g_tr->levels.size() - 1;
    Level &L = g_tr->levels[lvl];
    Menu *m = menu(L.menu);
    if (!m) return;
    i32 n = (i32)m->items.size();
    auto step = [&](i32 d) {
        i32 i = L.hot;
        for (i32 k = 0; k < n; k++) {
            i = (i + d + n) % n;
            if (!m->items[(size_t)i].separator()) { set_hot(lvl, i); return; }
        }
    };
    switch (vk) {
    case 0x26: step(-1); break;   // VK_UP
    case 0x28: step(1); break;    // VK_DOWN
    case 0x27:                    // VK_RIGHT
        if (L.hot >= 0 && m->items[(size_t)L.hot].sub) { open_sub(lvl); if (g_tr->levels.size() > lvl + 1) set_hot(lvl + 1, 0); }
        else if (g_tr->bar && !g_tr->bar_items.empty()) open_bar_item((g_tr->bar_hot + 1) % (i32)g_tr->bar_items.size());
        break;
    case 0x25:                    // VK_LEFT
        if (lvl > 0) close_levels(lvl);
        else if (g_tr->bar && !g_tr->bar_items.empty()) open_bar_item((g_tr->bar_hot - 1 + (i32)g_tr->bar_items.size()) % (i32)g_tr->bar_items.size());
        break;
    case 0x1B:                    // VK_ESCAPE
        if (lvl > 0) close_levels(lvl); else g_tr->done = true;
        break;
    case 0x0D:                    // VK_RETURN
        if (L.hot >= 0) {
            if (m->items[(size_t)L.hot].sub) { open_sub(lvl); if (g_tr->levels.size() > lvl + 1) set_hot(lvl + 1, 0); }
            else choose(m->items[(size_t)L.hot]);
        }
        break;
    default:                      // letra del atajo (&X)
        for (i32 i = 0; i < n; i++) {
            const std::string &t = m->items[(size_t)i].text;
            size_t a = t.find('&');
            if (a != std::string::npos && a + 1 < t.size() && std::toupper((unsigned char)t[a + 1]) == (int)vk) {
                set_hot(lvl, i);
                if (m->items[(size_t)i].sub) open_sub(lvl); else choose(m->items[(size_t)i]);
                break;
            }
        }
    }
}

}  // namespace

// procedimiento de los desplegables (#32768)
void menu_popup_proc() {
    u32 h = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    switch (msg) {
    case WM_PAINT: {
        Paint dc(h, menu_font());
        if (g_tr)
            for (auto &L : g_tr->levels)
                if (L.wnd == h) { draw_popup(L.menu, dc, wm_client(h), L.hot); break; }
        RET(0, 4);
    }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case 0x205: {
        if (!g_tr) RET(0, 4);
        Rect s = wm_screen(h, true);
        u32 m = msg == 0x205 ? WM_LBUTTONUP : msg == WM_RBUTTONDOWN ? WM_LBUTTONDOWN : msg;
        popup_mouse(m, s.l + lo16(lp), s.t + hi16(lp));
        RET(0, 4);
    }
    case WM_KEYDOWN: case 0x104:   // WM_SYSKEYDOWN
        if (g_tr) popup_key(wp);
        RET(0, 4);
    case WM_CHAR:
        if (g_tr && wp > 0x20) popup_key((u32)std::toupper((int)wp));
        RET(0, 4);
    case WM_SETCURSOR: RET(0, 4);
    }
    def_proc();
}

namespace {

void open_level(u32 h, i32 x, i32 y) {
    Menu *m = menu(h);
    if (!m) return;
    wm_send(g_tr->owner, WM_INITMENUPOPUP, h, (u32)g_tr->levels.size());   // la aplicación lo prepara
    u32 tmp = wm_create(0, "#32768", "", WS_POPUP, -1000, -1000, 10, 10, 0, 0);
    PopSize ps;
    { DCx dc(tmp, menu_font()); ps = popup_layout(h, dc); }
    int sw = 0, sh = 0;
    wm_owner_map(sw, sh);
    if (x + ps.w > sw) x = std::max(0, sw - ps.w);
    if (y + ps.h > sh) y = std::max(0, sh - ps.h);
    wm_move(tmp, x, y, ps.w, ps.h);
    Level L;
    L.menu = h;
    L.wnd = tmp;
    L.screen = {x, y, x + ps.w, y + ps.h};
    g_tr->levels.push_back(L);
    wm_show(tmp, true);
    wm_raise(tmp);
    wm_set_capture(tmp);
    wm_set_focus(tmp);
}

// el desplegable de un elemento de la barra
void open_bar_item(i32 i) {
    close_levels(0);
    g_tr->bar_hot = i;
    wm_send(g_tr->bar_wnd, 0x85, 1, 0);   // WM_NCPAINT: resalta el elemento
    Menu *b = menu(g_tr->bar);
    if (!b || i < 0 || i >= (i32)b->items.size()) return;
    const Item &it = b->items[(size_t)i];
    if (it.sub && item_enabled(it)) {
        const Rect &r = g_tr->bar_items[(size_t)i];
        open_level(it.sub, r.l, r.b);
    } else {
        // elemento de la barra sin desplegable: queda resaltado; la captura sigue en la ventana dueña
        wm_set_capture(g_tr->bar_wnd);
    }
}

// bucle modal común: la selección o 0
u32 run_tracker(Tracker &t) {
    Tracker *prev = g_tr;
    g_tr = &t;
    wm_send(t.owner, WM_ENTERMENULOOP, t.bar ? 0 : 1, 0);
    wm_modal_loop([&] { return t.done || (t.levels.empty() && !t.bar); });
    close_levels(0);
    if (wm_capture()) wm_set_capture(0);
    u32 r = t.result;
    if (t.bar_wnd) { t.bar_hot = -1; wm_send(t.bar_wnd, 0x85, 1, 0); }
    wm_send(t.owner, WM_MENUSELECT, 0xFFFF0000u, 0);   // menú cerrado
    wm_send(t.owner, WM_EXITMENULOOP, t.bar ? 0 : 1, 0);
    g_tr = prev;
    return r;
}

}  // namespace

// ---------------------------------------------------------------- barra de menú (desde user32.cpp)

i32 g_bar_hot_wnd = 0;   // ventana cuya barra está resaltada y elemento

void menu_bar_paint(u32 hwnd, u32 hmenu, u32 window_dc, const Rect &bar) {
    gdi_select(window_dc, menu_font());
    gdi_fill_color(window_dc, bar, C(4));
    if (!menu(hmenu)) return;
    std::vector<Rect> items = bar_layout(hmenu, window_dc, bar.w());
    Menu *m = menu(hmenu);
    for (size_t i = 0; i < items.size(); i++) {
        const Item &it = m->items[i];
        if (it.separator()) continue;
        Rect r{bar.l + items[i].l, bar.t, bar.l + items[i].r, bar.b};
        bool hot = g_tr && g_tr->bar == hmenu && g_tr->bar_wnd == hwnd && g_tr->bar_hot == (i32)i;
        if (hot) gdi_draw_edge(window_dc, r, 0xA, 0xF);   // BDR_SUNKENOUTER|BDR_SUNKENINNER: pulsado
        Rect tr = r;
        if (hot) { tr.l++; tr.t++; tr.r++; tr.b++; }
        draw_label(window_dc, label_of(it.text), tr, 0x1 | 0x20 | 0x4, item_enabled(it), C(7));   // DT_CENTER
    }
}

// pulsación en la barra (coordenadas de pantalla); bar = rectángulo de la barra en pantalla
void menu_bar_click(u32 hwnd, u32 hmenu, const Rect &bar, i32 sx, i32 sy) {
    if (!menu(hmenu)) return;
    Tracker t;
    t.owner = hwnd;
    t.bar = hmenu;
    t.bar_wnd = hwnd;
    {
        DCx dc(hwnd, menu_font());
        for (const Rect &r : bar_layout(hmenu, dc, bar.w())) t.bar_items.push_back({bar.l + r.l, bar.t, bar.l + r.r, bar.b});
    }
    g_tr = &t;
    i32 i = bar_hit(sx, sy);
    g_tr = nullptr;
    if (i < 0) return;
    wm_send(hwnd, WM_INITMENU, hmenu, 0);
    Tracker *prev = g_tr;
    g_tr = &t;
    open_bar_item(i);
    g_tr = prev;
    Menu *m = menu(hmenu);
    if (m && !m->items[(size_t)i].sub) {   // elemento directo de la barra: se elige al soltar
        if (item_enabled(m->items[(size_t)i])) { t.result = m->items[(size_t)i].id; t.done = true; }
    }
    u32 r = run_tracker(t);
    if (r) wm_post(hwnd, WM_COMMAND, r & 0xFFFF, 0);
}

// el ratón cuando la barra tiene la captura (elemento sin desplegable): user32 lo reenvía aquí
bool menu_tracking() { return g_tr != nullptr; }

// ---------------------------------------------------------------- API: menús

STUB(ext_user32_dll_CreateMenu) { RET(new_menu(false), 0); }
STUB(ext_user32_dll_CreatePopupMenu) { RET(new_menu(true), 0); }
STUB(ext_user32_dll_DestroyMenu) { bool ok = menu(arg(0)) != nullptr; destroy_menu(arg(0)); RET(ok, 1); }
STUB(ext_user32_dll_IsMenu) { RET(menu(arg(0)) != nullptr, 1); }

STUB(ext_user32_dll_AppendMenuA) {   // (hmenu, flags, id, item)
    Menu *m = menu(arg(0));
    if (!m) RET(0, 4);
    m->items.push_back(make_item(arg(1), arg(2), arg(3)));
    RET(1, 4);
}
STUB(ext_user32_dll_InsertMenuA) {   // (hmenu, pos, flags, id, item)
    Menu *m = menu(arg(0));
    if (!m) RET(0, 5);
    u32 pos = arg(1), flags = arg(2);
    Item it = make_item(flags, arg(3), arg(4));
    if (pos == 0xFFFFFFFFu) { m->items.push_back(it); RET(1, 5); }
    u32 owner = 0, idx = 0;
    if (flags & MF_BYPOSITION) { owner = arg(0); idx = std::min<u32>(pos, (u32)m->items.size()); }
    else if (!find_item(arg(0), pos, false, &owner, &idx)) { m->items.push_back(it); RET(1, 5); }
    Menu *o = menu(owner);
    o->items.insert(o->items.begin() + idx, it);
    RET(1, 5);
}
STUB(ext_user32_dll_ModifyMenuA) {   // (hmenu, pos, flags, id, item)
    u32 flags = arg(2);
    Item *it = find_item(arg(0), arg(1), flags & MF_BYPOSITION);
    if (!it) RET(0, 5);
    Item n = make_item(flags, arg(3), arg(4));
    if (it->sub && it->sub != n.sub) destroy_menu(it->sub);
    n.bmp_checked = it->bmp_checked;
    n.bmp_unchecked = it->bmp_unchecked;
    *it = n;
    RET(1, 5);
}
static u32 remove_item(u32 h, u32 pos, u32 flags, bool destroy) {
    u32 owner = 0, idx = 0;
    Item *it = find_item(h, pos, flags & MF_BYPOSITION, &owner, &idx);
    if (!it) return 0;
    u32 sub = it->sub;
    Menu *o = menu(owner);
    o->items.erase(o->items.begin() + idx);
    if (destroy && sub) destroy_menu(sub);
    return 1;
}
STUB(ext_user32_dll_DeleteMenu) { RET(remove_item(arg(0), arg(1), arg(2), true), 3); }
STUB(ext_user32_dll_RemoveMenu) { RET(remove_item(arg(0), arg(1), arg(2), false), 3); }

STUB(ext_user32_dll_GetSubMenu) {
    Menu *m = menu(arg(0));
    u32 i = arg(1);
    RET(m && i < m->items.size() ? m->items[i].sub : 0, 2);
}
STUB(ext_user32_dll_GetMenuItemID) {
    Menu *m = menu(arg(0));
    u32 i = arg(1);
    if (!m || i >= m->items.size()) RET(0xFFFFFFFFu, 2);
    RET(m->items[i].sub ? 0xFFFFFFFFu : m->items[i].id, 2);
}
STUB(ext_user32_dll_GetMenuItemCount) { Menu *m = menu(arg(0)); RET(m ? (u32)m->items.size() : 0xFFFFFFFFu, 1); }
STUB(ext_user32_dll_GetMenuState) {   // (hmenu, id, flags)
    Item *it = find_item(arg(0), arg(1), arg(2) & MF_BYPOSITION);
    if (!it) RET(0xFFFFFFFFu, 3);
    u32 f = legacy_flags(*it);
    if (it->sub) { Menu *s = menu(it->sub); f = (f & 0xFF) | ((s ? (u32)s->items.size() : 0) << 8); }
    RET(f, 3);
}
STUB(ext_user32_dll_GetMenuStringA) {   // (hmenu, id, buf, max, flags)
    Item *it = find_item(arg(0), arg(1), arg(4) & MF_BYPOSITION);
    if (!it) RET(0, 5);
    if (!arg(2)) RET((u32)it->text.size(), 5);
    RET(gstrcpy(arg(2), it->text, arg(3)), 5);
}
STUB(ext_user32_dll_EnableMenuItem) {   // (hmenu, id, flags): estado anterior
    Item *it = find_item(arg(0), arg(1), arg(2) & MF_BYPOSITION);
    if (!it) RET(0xFFFFFFFFu, 3);
    u32 prev = it->state & (MF_GRAYED | MF_DISABLED);
    it->state = (it->state & ~(MF_GRAYED | MF_DISABLED)) | (arg(2) & (MF_GRAYED | MF_DISABLED));
    RET(prev, 3);
}
STUB(ext_user32_dll_CheckMenuItem) {
    Item *it = find_item(arg(0), arg(1), arg(2) & MF_BYPOSITION);
    if (!it) RET(0xFFFFFFFFu, 3);
    u32 prev = it->state & MF_CHECKED;
    it->state = (it->state & ~MF_CHECKED) | (arg(2) & MF_CHECKED);
    RET(prev, 3);
}
STUB(ext_user32_dll_CheckMenuRadioItem) {   // (hmenu, first, last, check, flags)
    bool bypos = arg(4) & MF_BYPOSITION;
    u32 owner = 0, i0 = 0, i1 = 0;
    if (!find_item(arg(0), arg(1), bypos, &owner, &i0) || !find_item(arg(0), arg(2), bypos, nullptr, &i1)) RET(0, 5);
    Menu *m = menu(owner);
    u32 chk = 0;
    find_item(arg(0), arg(3), bypos, nullptr, &chk);
    for (u32 i = std::min(i0, i1); i <= std::max(i0, i1) && i < m->items.size(); i++) {
        m->items[i].state &= ~MF_CHECKED;
        if (i == chk) { m->items[i].state |= MF_CHECKED; m->items[i].type |= MFT_RADIOCHECK; }
    }
    RET(1, 5);
}
STUB(ext_user32_dll_HiliteMenuItem) {   // (hwnd, hmenu, id, flags)
    Item *it = find_item(arg(1), arg(2), arg(3) & MF_BYPOSITION);
    if (!it) RET(0, 4);
    it->state = (it->state & ~MF_HILITE) | (arg(3) & MF_HILITE);
    RET(1, 4);
}

// MENUITEMINFOA: cbSize, fMask, fType, fState, wID, hSubMenu, hbmpChecked, hbmpUnchecked, dwItemData,
// dwTypeData, cch
static void get_info(const Item &it, u32 p) {
    u32 mask = rd32(p + 4);
    if (mask & 0x10) {   // MIIM_TYPE
        wr32(p + 8, it.type);
        if (!(it.type & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) {
            u32 buf = rd32(p + 36), cch = rd32(p + 40);
            if (buf && cch) wr32(p + 40, gstrcpy(buf, it.text, cch));
            else wr32(p + 40, (u32)it.text.size());
        } else {
            wr32(p + 36, it.type & MF_BITMAP ? it.bitmap : it.data);
        }
    }
    if (mask & 0x1) wr32(p + 12, it.state);
    if (mask & 0x2) wr32(p + 16, it.id);
    if (mask & 0x4) wr32(p + 20, it.sub);
    if (mask & 0x8) { wr32(p + 24, it.bmp_checked); wr32(p + 28, it.bmp_unchecked); }
    if (mask & 0x20) wr32(p + 32, it.data);
}
static void set_info(Item &it, u32 p) {
    u32 mask = rd32(p + 4);
    if (mask & 0x10) {
        it.type = rd32(p + 8);
        u32 d = rd32(p + 36);
        if (it.type & MF_BITMAP) it.bitmap = d;
        else if (!(it.type & (MF_OWNERDRAW | MF_SEPARATOR))) it.text = d ? gstr(d) : "";
    }
    if (mask & 0x1) it.state = rd32(p + 12);
    if (mask & 0x2) it.id = rd32(p + 16);
    if (mask & 0x4) it.sub = rd32(p + 20);
    if (mask & 0x8) { it.bmp_checked = rd32(p + 24); it.bmp_unchecked = rd32(p + 28); }
    if (mask & 0x20) it.data = rd32(p + 32);
}
STUB(ext_user32_dll_GetMenuItemInfoA) {   // (hmenu, item, bypos, info)
    Item *it = find_item(arg(0), arg(1), arg(2) != 0);
    if (!it) RET(0, 4);
    get_info(*it, arg(3));
    RET(1, 4);
}
STUB(ext_user32_dll_SetMenuItemInfoA) {
    Item *it = find_item(arg(0), arg(1), arg(2) != 0);
    if (!it) RET(0, 4);
    set_info(*it, arg(3));
    RET(1, 4);
}
STUB(ext_user32_dll_InsertMenuItemA) {
    Menu *m = menu(arg(0));
    if (!m) RET(0, 4);
    Item it;
    set_info(it, arg(3));
    u32 owner = arg(0), idx = (u32)m->items.size();
    if (arg(2)) idx = std::min<u32>(arg(1), (u32)m->items.size());
    else if (!find_item(arg(0), arg(1), false, &owner, &idx)) { owner = arg(0); idx = (u32)m->items.size(); }
    Menu *o = menu(owner);
    o->items.insert(o->items.begin() + idx, it);
    RET(1, 4);
}
STUB(ext_user32_dll_SetMenuDefaultItem) {   // (hmenu, item, bypos)
    Menu *m = menu(arg(0));
    if (!m) RET(0, 3);
    for (auto &it : m->items) it.state &= ~MF_DEFAULT;
    if (arg(1) == 0xFFFFFFFFu) RET(1, 3);
    Item *it = find_item(arg(0), arg(1), arg(2) != 0);
    if (!it) RET(0, 3);
    it->state |= MF_DEFAULT;
    RET(1, 3);
}
STUB(ext_user32_dll_GetMenuDefaultItem) {   // (hmenu, bypos, flags)
    Menu *m = menu(arg(0));
    if (m)
        for (u32 i = 0; i < m->items.size(); i++)
            if (m->items[i].state & MF_DEFAULT) RET(arg(1) ? i : m->items[i].id, 3);
    RET(0xFFFFFFFFu, 3);
}
STUB(ext_user32_dll_SetMenuItemBitmaps) {   // (hmenu, pos, flags, unchecked, checked)
    Item *it = find_item(arg(0), arg(1), arg(2) & MF_BYPOSITION);
    if (!it) RET(0, 5);
    it->bmp_unchecked = arg(3);
    it->bmp_checked = arg(4);
    RET(1, 5);
}
STUB(ext_user32_dll_GetMenuCheckMarkDimensions) { RET(make_lp(13, 13), 0); }

// plantilla MENU de los recursos: {versión, desplazamiento} y elementos {flags, [id], texto UTF-16}
static u32 parse_menu(u32 &p, u32 h) {
    for (;;) {
        u16 flags = (u16)rd16(p);
        p += 2;
        u32 id = 0;
        if (!(flags & MF_POPUP)) { id = rd16(p); p += 2; }
        std::string text;
        for (;; p += 2) { u16 c = (u16)rd16(p); if (!c) { p += 2; break; } text.push_back(c < 256 ? (char)c : '?'); }
        Item it;
        it.type = flags & TYPE_BITS;
        it.state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED);
        it.text = text;
        if (flags & MF_POPUP) { it.sub = new_menu(true); it.id = it.sub; parse_menu(p, it.sub); }
        else { it.id = id; if (text.empty() && !id) it.type |= MF_SEPARATOR; }
        menu(h)->items.push_back(it);
        if (flags & 0x80) break;   // MF_END
    }
    return h;
}
static u32 load_menu_template(u32 p) {
    u16 version = (u16)rd16(p), off = (u16)rd16(p + 2);
    if (version != 0) { plog("LoadMenu: plantilla MENUEX no soportada"); return 0; }
    p += 4 + off;
    u32 h = new_menu(false);
    return parse_menu(p, h);
}
STUB(ext_user32_dll_LoadMenuA) {
    u32 res = find_resource(arg(0), arg(1), 4);   // RT_MENU
    if (!res) RET(0, 2);
    RET(load_menu_template(resource_module(res) + rd32(res)), 2);
}
STUB(ext_user32_dll_LoadMenuIndirectA) { RET(load_menu_template(arg(0)), 1); }

STUB(ext_user32_dll_TrackPopupMenu) {   // (hmenu, flags, x, y, reserved, hwnd, rect)
    u32 h = arg(0), flags = arg(1), owner = arg(5);
    if (!menu(h)) RET(0, 7);
    Tracker t;
    t.owner = owner;
    Tracker *prev = g_tr;
    g_tr = &t;
    i32 x = (i32)arg(2), y = (i32)arg(3);
    open_level(h, x, y);
    if (!t.levels.empty()) {   // TPM_CENTERALIGN / TPM_RIGHTALIGN
        Level &L = t.levels[0];
        i32 w = L.screen.w();
        if (flags & 0x8) x -= w; else if (flags & 0x4) x -= w / 2;
        if (x != L.screen.l) { wm_move(L.wnd, x, L.screen.t, w, L.screen.h()); L.screen = {x, L.screen.t, x + w, L.screen.b}; }
    }
    g_tr = prev;
    u32 r = run_tracker(t);
    if (flags & 0x100) RET(r, 7);   // TPM_RETURNCMD
    if (r) wm_post(owner, WM_COMMAND, r & 0xFFFF, 0);
    RET(r != 0, 7);
}
STUB(ext_user32_dll_TrackPopupMenuEx) {   // (hmenu, flags, x, y, hwnd, params)
    u32 a0 = arg(0), a1 = arg(1), a2 = arg(2), a3 = arg(3), a4 = arg(4);
    u32 saved = esp;
    esp -= 32;   // reutiliza TrackPopupMenu con sus 7 argumentos
    wr32(esp, rd32(saved));
    wr32(esp + 4, a0); wr32(esp + 8, a1); wr32(esp + 12, a2); wr32(esp + 16, a3); wr32(esp + 20, 0); wr32(esp + 24, a4); wr32(esp + 28, 0);
    ext_user32_dll_TrackPopupMenu();
    esp = saved;
    RET(eax, 6);
}
STUB(ext_user32_dll_EndMenu) { if (g_tr) g_tr->done = true; RET(1, 0); }

// ---------------------------------------------------------------- aceleradores

namespace {
struct Accel { u8 virt; u16 key, cmd; };
std::map<u32, std::vector<Accel>> g_accels;
u32 g_next_accel = 0x00E80010u;
u32 new_accel(std::vector<Accel> v) { u32 h = g_next_accel; g_next_accel += 4; g_accels[h] = std::move(v); return h; }
}

STUB(ext_user32_dll_CreateAcceleratorTableA) {   // (ACCEL*, n): ACCEL = {BYTE fVirt; WORD key; WORD cmd} (6 B)
    std::vector<Accel> v;
    for (u32 i = 0; i < arg(1); i++) {
        u32 p = arg(0) + 6 * i;
        v.push_back({(u8)rd8(p), (u16)rd16(p + 2), (u16)rd16(p + 4)});
    }
    RET(new_accel(std::move(v)), 2);
}
STUB(ext_user32_dll_DestroyAcceleratorTable) { RET(g_accels.erase(arg(0)) ? 1 : 0, 1); }
STUB(ext_user32_dll_CopyAcceleratorTableA) {   // (h, dst, n)
    auto it = g_accels.find(arg(0));
    if (it == g_accels.end()) RET(0, 3);
    if (!arg(1)) RET((u32)it->second.size(), 3);
    u32 n = std::min<u32>(arg(2), (u32)it->second.size());
    for (u32 i = 0; i < n; i++) {
        u32 p = arg(1) + 6 * i;
        wr8(p, it->second[i].virt); wr8(p + 1, 0); wr16(p + 2, it->second[i].key); wr16(p + 4, it->second[i].cmd);
    }
    RET(n, 3);
}
STUB(ext_user32_dll_LoadAcceleratorsA) {   // recurso RT_ACCELERATOR: {WORD fFlags, key, cmd, relleno}, el último con 0x80
    u32 res = find_resource(arg(0), arg(1), 9);
    if (!res) RET(0, 2);
    u32 p = resource_module(res) + rd32(res);
    std::vector<Accel> v;
    for (u32 i = 0; i < 1024; i++, p += 8) {
        u16 f = (u16)rd16(p);
        v.push_back({(u8)(f & 0x7F), (u16)rd16(p + 2), (u16)rd16(p + 4)});
        if (f & 0x80) break;
    }
    RET(new_accel(std::move(v)), 2);
}
STUB(ext_user32_dll_TranslateAcceleratorA) {   // (hwnd, haccel, MSG*)
    u32 hwnd = arg(0), msgp = arg(2);
    auto it = g_accels.find(arg(1));
    if (!hwnd || it == g_accels.end() || !msgp) RET(0, 3);
    u32 msg = rd32(msgp + 4), wp = rd32(msgp + 8);
    bool keydown = msg == WM_KEYDOWN || msg == 0x104, chr = msg == WM_CHAR || msg == 0x106;
    if (!keydown && !chr) RET(0, 3);
    bool shift = key_down(0x10), ctrl = key_down(0x11), alt = msg == 0x104 || msg == 0x106 || key_down(0x12);
    for (const Accel &a : it->second) {
        bool virt = a.virt & 1;   // FVIRTKEY
        if (virt != keydown) continue;
        if (virt) {
            if (a.key != (wp & 0xFFFF)) continue;
            if (bool(a.virt & 4) != shift || bool(a.virt & 8) != ctrl || bool(a.virt & 0x10) != alt) continue;   // FSHIFT FCONTROL FALT
        } else {
            if (a.key != (wp & 0xFFFF) || bool(a.virt & 0x10) != alt) continue;
        }
        wm_send(hwnd, WM_COMMAND, (1u << 16) | a.cmd, 0);
        RET(1, 3);
    }
    RET(0, 3);
}
