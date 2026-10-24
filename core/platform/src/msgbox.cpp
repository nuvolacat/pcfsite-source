// PCF7 — barra de estado (msctls_statusbar32 de COMCTL32) y MessageBox de Windows en el host.
//
// MessageBox es una ventana modal de verdad: título, icono, texto ajustado y los botones de su tipo,
// centrada en la pantalla; el programa espera a que se pulse uno (Intro y Escape como en Windows). En las
// pruebas sin ventana (PCF7_HEADLESS) contesta solo con el botón por defecto, salvo PCF7_MSGBOX=1.
#include "ctlutil.h"
#include "display.h"

#include <algorithm>
#include <map>
#include <vector>

extern "C" {
void ext_user32_dll_RegisterClassA(void);
void ext_user32_dll_CreateWindowExA(void);
void ext_user32_dll_ShowWindow(void);
}

using namespace ctl;

// ---------------------------------------------------------------- msctls_statusbar32

namespace {
struct Sb {
    u32 font = 0;
    std::vector<i32> parts{-1};
    std::vector<std::string> text{""};
    std::vector<u32> type{0};
    bool simple = false;
    std::string simple_text;
    i32 min_h = 0;
};
std::map<u32, Sb> g_sbar;

i32 sbar_height(u32 h, Sb &s) { DCx dc(h, s.font); return std::max(s.min_h, gdi_dc_text_height(dc) + 6); }

Rect part_rect(u32 h, Sb &s, int i) {
    Rect c = wm_client(h);
    i32 l = i == 0 ? 0 : s.parts[i - 1] + 2, r = s.parts[i] < 0 ? c.r : std::min(s.parts[i], c.r);
    if (i + 1 == (int)s.parts.size() && s.parts[i] < 0 && (wm_style(h) & 0x100)) r = c.r - 14;   // SBARS_SIZEGRIP
    return {l, 2, r, c.b};
}

void sbar_paint(u32 h, u32 dc) {
    Sb &s = g_sbar[h];
    Rect c = wm_client(h);
    gdi_fill_color(dc, c, C(COLOR_BTNFACE));
    gdi_set_text_color(dc, C(COLOR_BTNTEXT));
    gdi_set_bk(dc, 0, 1);
    auto draw = [&](Rect r, const std::string &t, u32 type, int idx) {
        if (!(type & 0x100)) gdi_draw_edge(dc, r, (type & 0x200) ? 4 : 2, 0xF);   // SBT_NOBORDERS / SBT_POPOUT
        Rect in{r.l + 1, r.t + 1, r.r - 1, r.b - 1};
        if (type & 0x1000) {   // SBT_OWNERDRAW: WM_DRAWITEM con el dato en itemData
            u32 dis = heap_alloc(48, true);
            wr32(dis, 0); wr32(dis + 4, wm_id(h)); wr32(dis + 8, (u32)idx); wr32(dis + 12, 1);
            wr32(dis + 20, h); wr32(dis + 24, dc); wr_rect(dis + 28, in); wr32(dis + 44, (u32)std::strtoul(t.c_str(), nullptr, 10));
            if (u32 p = wm_parent(h)) wm_send(p, WM_DRAWITEM, wm_id(h), dis);
            heap_free(dis);
            return;
        }
        // tabuladores: "izquierda\tcentro\tderecha"
        std::string a = t, b, cc;
        size_t t1 = t.find('\t');
        if (t1 != std::string::npos) {
            a = t.substr(0, t1);
            size_t t2 = t.find('\t', t1 + 1);
            b = t.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
            if (t2 != std::string::npos) cc = t.substr(t2 + 1);
        }
        Rect tr{in.l + 2, in.t, in.r - 2, in.b};
        gdi_draw_text(dc, a, tr, 0x20 | 0x4 | 0x800);
        if (!b.empty()) { Rect r2 = tr; gdi_draw_text(dc, b, r2, 0x20 | 0x4 | 0x1 | 0x800); }
        if (!cc.empty()) { Rect r3 = tr; gdi_draw_text(dc, cc, r3, 0x20 | 0x4 | 0x2 | 0x800); }
    };
    if (s.simple) draw({0, 2, c.r, c.b}, s.simple_text, 0x100, 255);
    else for (size_t i = 0; i < s.parts.size(); i++) draw(part_rect(h, s, (int)i), s.text[i], s.type[i], (int)i);
    if (wm_style(h) & 0x100) {   // agarradera de tamaño
        for (int k = 0; k < 3; k++)
            for (int d = 0; d < 12 - 4 * k; d++) {
                gdi_fill_color(dc, {c.r - 2 - d - 4 * k, c.b - 2 - (11 - 4 * k - d), c.r - 1 - d - 4 * k, c.b - 1 - (11 - 4 * k - d)}, C(COLOR_BTNSHADOW));
            }
    }
}

void sbar_reposition(u32 h) {   // pegada abajo en el padre, a todo lo ancho
    if (wm_style(h) & 0x3) return;   // CCS_TOP/NORESIZE…: la coloca el programa
    u32 p = wm_parent(h);
    if (!p) return;
    Rect pc = wm_client(p);
    i32 hh = sbar_height(h, g_sbar[h]);
    wm_move(h, 0, pc.b - hh, pc.r, hh);
}
}  // namespace

void statusbar_proc() {
    u32 h = arg(0), msg = arg(1), wp = arg(2), lp = arg(3);
    Sb &s = g_sbar[h];
    switch (msg) {
    case WM_CREATE: sbar_reposition(h); RET(0, 4);
    case WM_NCDESTROY: g_sbar.erase(h); break;
    case WM_SIZE: sbar_reposition(h); wm_invalidate(h, true); RET(0, 4);
    case WM_PAINT: { Paint dc(h, s.font); sbar_paint(h, dc); RET(0, 4); }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_SETFONT: s.font = wp; sbar_reposition(h); if (lp) wm_invalidate(h, true); RET(0, 4);
    case WM_GETFONT: RET(s.font, 4);
    case WM_SETTEXT: s.text[0] = lp ? gstr(lp) : ""; wm_invalidate(h, true); RET(1, 4);
    case 0x401: {   // SB_SETTEXTA: wParam = parte | SBT_*
        u32 i = wp & 0xFF;
        std::string t = (wp & 0x1000) ? std::to_string(lp) : (lp ? gstr(lp) : "");
        if (i == 255) s.simple_text = t;
        else if (i < s.parts.size()) { s.text[i] = t; s.type[i] = wp & 0xFF00; }
        wm_invalidate(h, true);
        RET(1, 4);
    }
    case 0x402: {   // SB_GETTEXTA
        u32 i = wp & 0xFF;
        if (i >= s.parts.size()) RET(0, 4);
        if (lp) gstrcpy(lp, s.text[i]);
        RET(((u32)s.type[i] << 16) | (u32)s.text[i].size(), 4);
    }
    case 0x403: { u32 i = wp & 0xFF; RET(i < s.parts.size() ? ((u32)s.type[i] << 16) | (u32)s.text[i].size() : 0, 4); }   // SB_GETTEXTLENGTHA
    case 0x404: {   // SB_SETPARTS
        u32 n = std::min<u32>(wp, 256);
        s.parts.assign(n, -1);
        for (u32 i = 0; i < n; i++) s.parts[i] = (i32)rd32(lp + 4 * i);
        s.text.resize(n); s.type.resize(n);
        wm_invalidate(h, true);
        RET(1, 4);
    }
    case 0x406: { u32 n = std::min<u32>(wp, (u32)s.parts.size()); for (u32 i = 0; i < n && lp; i++) wr32(lp + 4 * i, (u32)s.parts[i]); RET((u32)s.parts.size(), 4); }   // SB_GETPARTS
    case 0x407: if (lp) { wr32(lp, 0); wr32(lp + 4, 2); wr32(lp + 8, 2); } RET(1, 4);   // SB_GETBORDERS
    case 0x408: s.min_h = (i32)wp; sbar_reposition(h); RET(0, 4);                       // SB_SETMINHEIGHT
    case 0x409: s.simple = wp != 0; wm_invalidate(h, true); RET(0, 4);                   // SB_SIMPLE
    case 0x40A: { u32 i = wp; if (i >= s.parts.size()) RET(0, 4); wr_rect(lp, part_rect(h, s, (int)i)); RET(1, 4); }   // SB_GETRECT
    case 0x40E: RET(s.simple, 4);                                                        // SB_ISSIMPLE
    case 0x40F: case 0x410: case 0x411: case 0x412: case 0x413: RET(0, 4);              // iconos y ayudas
    case 0x2001: RET(0, 4);                                                              // SB_SETBKCOLOR
    case WM_NCHITTEST: {
        Rect sr = wm_screen(h, true);
        if ((wm_style(h) & 0x100) && lo16(lp) >= sr.r - 14 && hi16(lp) >= sr.b - 14) RET(17, 4);   // HTBOTTOMRIGHT
        break;
    }
    }
    def_proc();
}

// ---------------------------------------------------------------- MessageBox

namespace {
struct Box {
    u32 hwnd = 0, result = 0;
    std::string text;
    u32 type = 0;
    std::vector<std::pair<u32, std::string>> buttons;   // (IDOK…, texto)
    u32 def = 0;
    Rect text_rect;
    bool done = false;
};
Box *g_box = nullptr;
std::map<u32, Box *> g_boxes;

void box_icon(u32 dc, i32 x, i32 y, u32 kind) {   // MB_ICONHAND 0x10, QUESTION 0x20, EXCLAMATION 0x30, ASTERISK 0x40
    if (kind == 0x10) {   // círculo rojo con una X
        for (int yy = 0; yy < 32; yy++) for (int xx = 0; xx < 32; xx++) {
            int dx = xx - 15, dy = yy - 15;
            if (dx * dx + dy * dy <= 15 * 15) gdi_fill_color(dc, {x + xx, y + yy, x + xx + 1, y + yy + 1}, 0x0000FF);
        }
        for (int k = -7; k <= 7; k++) { gdi_fill_color(dc, {x + 15 + k, y + 15 + k, x + 17 + k, y + 17 + k}, 0xFFFFFF); gdi_fill_color(dc, {x + 15 + k, y + 15 - k, x + 17 + k, y + 17 - k}, 0xFFFFFF); }
    } else if (kind == 0x30) {   // triángulo amarillo con !
        for (int yy = 0; yy < 30; yy++) { int half = yy / 2; gdi_fill_color(dc, {x + 15 - half, y + 1 + yy, x + 17 + half, y + 2 + yy}, 0x00FFFF); }
        gdi_fill_color(dc, {x + 15, y + 9, x + 17, y + 22}, 0); gdi_fill_color(dc, {x + 15, y + 24, x + 17, y + 27}, 0);
    } else {   // círculo (blanco con i azul, o con ?)
        for (int yy = 0; yy < 32; yy++) for (int xx = 0; xx < 32; xx++) {
            int dx = xx - 15, dy = yy - 15, d = dx * dx + dy * dy;
            if (d <= 15 * 15) gdi_fill_color(dc, {x + xx, y + yy, x + xx + 1, y + yy + 1}, d >= 14 * 14 ? 0 : 0xFFFFFF);
        }
        u32 blue = 0xFF0000;
        Rect r{x, y + 4, x + 32, y + 28};
        gdi_set_text_color(dc, blue);
        gdi_set_bk(dc, 0, 1);
        gdi_draw_text(dc, kind == 0x20 ? "?" : "i", r, 0x1 | 0x4 | 0x20);
    }
}

void box_proc() {
    u32 h = arg(0), msg = arg(1), wp = arg(2);
    Box *b = g_boxes.count(h) ? g_boxes[h] : nullptr;
    switch (msg) {
    case WM_PAINT: {
        if (!b) break;
        Paint dc(h, stock(17));
        Rect c = wm_client(h);
        gdi_fill_color(dc, c, C(COLOR_BTNFACE));
        if (b->type & 0xF0) box_icon(dc, 12, 12, b->type & 0xF0);
        gdi_set_text_color(dc, C(COLOR_WINDOWTEXT));
        gdi_set_bk(dc, 0, 1);
        Rect tr = b->text_rect;
        gdi_draw_text(dc, b->text, tr, 0x10 | 0x800);
        RET(0, 4);
    }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_COMMAND:
        if (b && (wp & 0xFFFF) >= 1 && (wp & 0xFFFF) <= 7) { b->result = wp & 0xFFFF; b->done = true; }
        RET(0, 4);
    case WM_KEYDOWN:
        if (!b) break;
        if (wp == 0x0D) { b->result = b->def; b->done = true; RET(0, 4); }
        if (wp == 0x1B) {   // Escape: Cancelar (o el único botón)
            for (auto &bt : b->buttons) if (bt.first == 2) { b->result = 2; b->done = true; }
            if (b->buttons.size() == 1) { b->result = b->buttons[0].first; b->done = true; }
            RET(0, 4);
        }
        break;
    case WM_CLOSE:
        if (b) { b->result = b->buttons.size() == 1 ? b->buttons[0].first : 2; for (auto &bt : b->buttons) if (bt.first == 2) b->result = 2; b->done = true; }
        RET(0, 4);
    }
    def_proc();
}

u32 box_class() {
    static bool done = false;
    if (!done) {
        done = true;
        u32 wc = heap_alloc(40 + 32, true), name = wc + 40;
        gstrcpy(name, "PCF7MessageBox");
        wr32(wc, 0); wr32(wc + 4, host_fn(box_proc)); wr32(wc + 16, 0x400000); wr32(wc + 24, 0x37F00); wr32(wc + 36, name);
        call_stub(ext_user32_dll_RegisterClassA, {wc});
    }
    return 0;
}

}  // namespace

u32 message_box(u32 owner, const std::string &text, const std::string &caption, u32 type) {
    static bool headless = std::getenv("PCF7_HEADLESS") && !std::getenv("PCF7_MSGBOX");
    plog("MessageBox [%s]: %s (tipo 0x%x)", caption.c_str(), text.c_str(), type);
    if (std::getenv("PCF7_TRACE_MSGBOX")) {   // diagnóstico: las 256 últimas funciones antes de la caja
        std::string ring;
        for (u32 i = 0; i < 256; i++) { char b[16]; snprintf(b, sizeof b, " %x", fnring[(fnring_i + i) & 255]); ring += b; }
        plog("  anillo:%s", ring.c_str());
    }
    static const char *labels[] = {"", "Aceptar", "Cancelar", "&Anular", "&Reintentar", "&Omitir", "&Sí", "&No"};
    std::vector<u32> ids;
    switch (type & 0xF) {
    case 1: ids = {1, 2}; break;          // MB_OKCANCEL
    case 2: ids = {3, 4, 5}; break;       // MB_ABORTRETRYIGNORE
    case 3: ids = {6, 7, 2}; break;       // MB_YESNOCANCEL
    case 4: ids = {6, 7}; break;          // MB_YESNO
    case 5: ids = {4, 2}; break;          // MB_RETRYCANCEL
    default: ids = {1}; break;            // MB_OK
    }
    u32 defi = (type >> 8) & 0xF;         // MB_DEFBUTTON1..3 (0x000, 0x100, 0x200)
    u32 def = ids[std::min<u32>(defi, (u32)ids.size() - 1)];
    if (headless) return def;
    box_class();
    Box b;
    b.text = text; b.type = type; b.def = def;
    for (u32 id : ids) b.buttons.push_back({id, labels[id]});
    // medidas
    u32 tmpdc = gdi_window_dc(0, true);
    gdi_select(tmpdc, stock(17));
    i32 sw = display().screen.w, sh = display().screen.h;
    Rect tr{0, 0, std::max(120, sw * 3 / 5), 0};
    gdi_draw_text(tmpdc, text, tr, 0x10 | 0x400 | 0x800);
    gdi_release_dc(tmpdc);
    i32 icon = (type & 0xF0) ? 44 : 0;
    i32 bw = 75, bh = 23, gap = 6;
    i32 btns = (i32)ids.size() * bw + ((i32)ids.size() - 1) * gap;
    i32 cw = std::max(tr.w() + icon + 30, btns + 24);
    i32 th = std::max(tr.h(), icon ? 32 : 0);
    i32 ch = 12 + th + 16 + bh + 12;
    b.text_rect = {12 + icon, 12 + std::max(0, (32 - tr.h()) / 2 * (icon ? 1 : 0)), 12 + icon + tr.w() + 4, 12 + th + 4};
    // ventana con marco de diálogo y título
    i32 ww = cw + 6, wh = ch + 6 + 19;
    u32 cls = heap_alloc(16), cap = heap_alloc((u32)caption.size() + 1);
    gstrcpy(cls, "PCF7MessageBox");
    gstrcpy(cap, caption.empty() ? "Error" : caption);
    u32 h = call_stub(ext_user32_dll_CreateWindowExA, {1 /*WS_EX_DLGMODALFRAME*/ | 8 /*TOPMOST*/, cls, cap, 0x80000000u | 0xC00000u | 0x80000u,
                                                       (u32)((sw - ww) / 2), (u32)((sh - wh) / 2), (u32)ww, (u32)wh, owner, 0, 0x400000, 0});
    heap_free(cls);
    heap_free(cap);
    if (!h) return def;
    b.hwnd = h;
    g_boxes[h] = &b;
    i32 bx = (cw - btns) / 2, by = ch - 12 - bh;
    u32 focus_btn = 0;
    for (u32 id : ids) {
        u32 bt = wm_create(0, "Button", labels[id], WS_CHILD | WS_VISIBLE | WS_TABSTOP | (id == def ? 1u : 0u), bx, by, bw, bh, h, id);
        wm_send(bt, WM_SETFONT, stock(17), 0);
        if (id == def) focus_btn = bt;
        bx += bw + gap;
    }
    // modal: el propietario queda deshabilitado mientras tanto
    bool owner_enabled = owner && wm_enabled(owner);
    if (owner_enabled) wm_set_style(owner, wm_style(owner) | WS_DISABLED);
    u32 old_capture = wm_capture();
    wm_set_capture(0);
    call_stub(ext_user32_dll_ShowWindow, {h, 5});
    u32 old_focus = wm_focus();
    if (focus_btn) wm_set_focus(focus_btn);
    // Intro/Escape llegan al botón con el foco: se reenvían a la caja
    wm_modal_loop([&] { return b.done || !wm_exists(h); });
    if (owner_enabled) wm_set_style(owner, wm_style(owner) & ~WS_DISABLED);
    g_boxes.erase(h);
    if (wm_exists(h)) wm_destroy(h);
    if (old_focus && wm_exists(old_focus)) wm_set_focus(old_focus);
    (void)old_capture;
    u32 r = b.result ? b.result : def;
    plog("MessageBox -> %u", r);
    return r;
}

// Intro y Escape dentro de una caja de mensaje (los pulsa el botón con el foco)
bool message_box_key(u32 hwnd, u32 vk) {
    for (auto &[h, b] : g_boxes) {
        if (hwnd != h && wm_parent(hwnd) != h) continue;
        if (vk == 0x0D) { u32 id = wm_parent(hwnd) == h ? wm_id(hwnd) : b->def; if (id >= 1 && id <= 7) { b->result = id; b->done = true; } return true; }
        if (vk == 0x1B) { for (auto &bt : b->buttons) if (bt.first == 2) { b->result = 2; b->done = true; } if (b->buttons.size() == 1) { b->result = b->buttons[0].first; b->done = true; } return true; }
        if (vk == 0x25 || vk == 0x27 || vk == 0x09) {   // flechas y tabulador: cambiar de botón
            std::vector<u32> btns;
            for_each_child(h, [&](u32 k) { btns.push_back(k); });
            auto it = std::find(btns.begin(), btns.end(), hwnd);
            if (it != btns.end() && !btns.empty()) {
                size_t i = (size_t)(it - btns.begin());
                i = vk == 0x25 ? (i + btns.size() - 1) % btns.size() : (i + 1) % btns.size();
                wm_set_focus(btns[i]);
            }
            return true;
        }
    }
    return false;
}

STUB(ext_user32_dll_MessageBoxA) { RET(message_box(arg(0), gstr(arg(1)), gstr(arg(2)), arg(3)), 4); }
STUB(ext_user32_dll_MessageBoxExA) { RET(message_box(arg(0), gstr(arg(1)), gstr(arg(2)), arg(3)), 5); }
STUB(ext_user32_dll_MessageBoxIndirectA) {   // MSGBOXPARAMS: cbSize, owner, hinst, text, caption, style…
    u32 p = arg(0);
    RET(message_box(rd32(p + 4), gstr(rd32(p + 12)), gstr(rd32(p + 16)), rd32(p + 20)), 1);
}
