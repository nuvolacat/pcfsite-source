// PCF2000 — control WebBrowser falso (CLSID_WebBrowser) para el panel de ayuda (CHtmlView de la MFC 6.0 que aloja
// gfxwin en CGFXExplorer). Sin él, CHtmlView::Create falla, m_pBrowserApp queda a NULL y el juego llama a través de
// NULL en cada pantalla (ExecWB, Navigate…): la plataforma saltaba esas llamadas y la pila quedaba descuadrada hasta
// romper la cadena SEH. Aquí el control existe, se activa en una ventana hija propia (sin dibujar nada) y todos
// los métodos de IWebBrowser2 son inocuos. Lo que pide la COleControlSite de la MFC: IClassFactory, IOleObject,
// IOleInPlaceObject, IOleInPlaceActiveObject, IOleControl, IPersistStreamInit, IConnectionPointContainer e
// IDispatch/IWebBrowser2.
#include "com.h"
#include "ctlutil.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace {

constexpr u32 S_OK = 0, S_FALSE = 1, E_NOTIMPL = 0x80004001u, E_NOINTERFACE = 0x80004002u, E_FAIL = 0x80004005u;

const ComGuid CLSID_WebBrowser = {0x8856F961, 0x340A, 0x11D0, {0xA9, 0x6B, 0x00, 0xC0, 0x4F, 0xD7, 0x05, 0xA2}};
const ComGuid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IClassFactory = {0x00000001, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IOleInPlaceSite = {0x00000119, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IOleObject = {0x00000112, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IOleWindow = {0x00000114, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IOleInPlaceObject = {0x00000113, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IOleInPlaceActiveObject = {0x00000117, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IOleControl = {0xB196B288, 0xBAB4, 0x101A, {0xB6, 0x9C, 0x00, 0xAA, 0x00, 0x34, 0x1D, 0x07}};
const ComGuid IID_IPersist = {0x0000010C, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IPersistStreamInit = {0x7FD52380, 0x4E07, 0x101B, {0xAE, 0x2D, 0x08, 0x00, 0x2B, 0x2E, 0xC7, 0x13}};
const ComGuid IID_IConnectionPointContainer = {0xB196B284, 0xBAB4, 0x101A, {0xB6, 0x9C, 0x00, 0xAA, 0x00, 0x34, 0x1D, 0x07}};
const ComGuid IID_IConnectionPoint = {0xB196B286, 0xBAB4, 0x101A, {0xB6, 0x9C, 0x00, 0xAA, 0x00, 0x34, 0x1D, 0x07}};
const ComGuid IID_IDispatch = {0x00020400, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IWebBrowser = {0xEAB22AC1, 0x30C1, 0x11CF, {0xA7, 0xEB, 0x00, 0x00, 0xC0, 0x5B, 0xAE, 0x0B}};
const ComGuid IID_IWebBrowserApp = {0x0002DF05, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IWebBrowser2 = {0xD30C1661, 0xCDAF, 0x11D0, {0x8A, 0x3E, 0x00, 0xC0, 0x4F, 0xC9, 0xE2, 0x6E}};

// Un control = un registro con una "cara" (objeto COM) por interfaz: [vtable][refs][índice de cara][registro]
enum Face { F_OLEOBJECT, F_INPLACE, F_ACTIVE, F_CONTROL, F_PERSIST, F_CPC, F_BROWSER, F_N };
struct Run { std::string texto; bool negrita = false; };
struct Parrafo { std::vector<Run> runs; bool centrado = false, negrita = false; };   // negrita: todo el párrafo (títulos)
struct Browser { u32 obj[F_N] = {}; u32 refs = 1, site = 0, hwnd = 0, cp = 0; bool ip_activo = false, ui_activo = false; std::vector<Parrafo> pagina; int scroll = 0, alto_total = 0; u32 ext_w = 2646, ext_h = 2646; };
std::map<u32, Browser *> g_by_hwnd;
u32 g_font = 0, g_font_bold = 0;
std::map<u32, Browser *> g_by_obj;
u32 g_vt[F_N] = {}, g_vt_cf = 0, g_vt_cp = 0, g_cf = 0, g_wndclass = 0;

std::string wstr(u32 p) { std::string s; if (p) for (u32 i = 0; i < 400; i++) { u32 c = rd16(p + 2 * i); if (!c) break; s += c < 128 ? (char)c : '?'; } return s; }
Browser *self() { auto it = g_by_obj.find(arg(0)); return it == g_by_obj.end() ? nullptr : it->second; }
void write_guid(u32 p, const ComGuid &g) { wr32(p, g.a); wr16(p + 4, g.b); wr16(p + 6, g.c); std::memcpy(MEM + p + 8, g.d, 8); }

template <int N> void m_Ok() { RET(S_OK, N); }
template <int N> void m_NotImpl() { RET(E_NOTIMPL, N); }
template <int N> void m_Get0() { if (arg(1)) wr32(arg(1), 0); RET(S_OK, N); }   // get_X(out): 0 / NULL / VARIANT_FALSE

// ---------------------------------------------------------------- IUnknown de todas las caras
void b_QueryInterface() {
    Browser *b = self();
    u32 iid = arg(1), out = arg(2);
    static const std::pair<const ComGuid *, Face> map[] = {
        {&IID_IUnknown, F_OLEOBJECT}, {&IID_IOleObject, F_OLEOBJECT}, {&IID_IOleWindow, F_INPLACE},
        {&IID_IOleInPlaceObject, F_INPLACE}, {&IID_IOleInPlaceActiveObject, F_ACTIVE}, {&IID_IOleControl, F_CONTROL},
        {&IID_IPersist, F_PERSIST}, {&IID_IPersistStreamInit, F_PERSIST}, {&IID_IConnectionPointContainer, F_CPC},
        {&IID_IDispatch, F_BROWSER}, {&IID_IWebBrowser, F_BROWSER}, {&IID_IWebBrowserApp, F_BROWSER}, {&IID_IWebBrowser2, F_BROWSER}};
    if (b)
        for (auto &[g, f] : map)
            if (com_guid_eq(iid, *g)) { b->refs++; wr32(out, b->obj[f]); RET(S_OK, 3); }
    static std::set<std::string> dicho;
    char k[64]; snprintf(k, sizeof k, "%08x-%04x-%04x", rd32(iid), rd16(iid + 4), rd16(iid + 6));
    if (dicho.insert(k).second) plog("WebBrowser: QueryInterface(%s): no soportada", k);
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void b_AddRef() { Browser *b = self(); RET(b ? ++b->refs : 1, 1); }
void b_Release() { Browser *b = self(); RET(b && b->refs ? --b->refs : 0, 1); }   // nunca se destruye (uno por panel)

// ---------------------------------------------------------------- la página de ayuda: HTML (FrontPage, cp1252) → párrafos
std::string entidad(const std::string &e) {
    static const std::pair<const char *, const char *> tab[] = {{"nbsp", " "}, {"quot", "\""}, {"amp", "&"}, {"lt", "<"}, {"gt", ">"},
        {"aacute", "\xe1"}, {"eacute", "\xe9"}, {"iacute", "\xed"}, {"oacute", "\xf3"}, {"uacute", "\xfa"}, {"ntilde", "\xf1"},
        {"Aacute", "\xc1"}, {"Eacute", "\xc9"}, {"Iacute", "\xcd"}, {"Oacute", "\xd3"}, {"Uacute", "\xda"}, {"Ntilde", "\xd1"},
        {"uuml", "\xfc"}, {"iexcl", "\xa1"}, {"iquest", "\xbf"}, {"ordm", "\xba"}, {"ordf", "\xaa"}, {"copy", "\xa9"}};
    if (!e.empty() && e[0] == '#') { int n = std::atoi(e.c_str() + 1); return n > 0 && n < 256 ? std::string(1, (char)n) : ""; }
    for (auto &[k, v] : tab) if (e == k) return v;
    return "";
}
std::vector<Parrafo> parse_html(const std::string &h) {
    std::vector<Parrafo> out;
    Parrafo cur;
    bool in_body = false, bold = false, espacio = false;
    auto vacio = [&] { for (const Run &r : cur.runs) if (!r.texto.empty()) return false; return true; };
    auto add = [&](const std::string &t) {   // texto al último tramo si comparte negrita, si no tramo nuevo
        if (cur.runs.empty() || cur.runs.back().negrita != bold) cur.runs.push_back({"", bold});
        cur.runs.back().texto += t;
    };
    auto cerrar = [&] {
        while (!cur.runs.empty() && !cur.runs.back().texto.empty() && cur.runs.back().texto.back() == ' ') cur.runs.back().texto.pop_back();
        if (!vacio()) out.push_back(cur);
        cur = Parrafo{};
        espacio = false;
    };
    for (size_t i = 0; i < h.size();) {
        char c = h[i];
        if (c == '<') {
            size_t j = h.find('>', i);
            if (j == std::string::npos) break;
            std::string tag = h.substr(i + 1, j - i - 1), low;
            for (char t : tag) low += (char)std::tolower((unsigned char)t);
            std::string nombre = low.substr(0, low.find_first_of(" \t\r\n/"));
            if (nombre == "body") in_body = true;
            else if (nombre == "p" || nombre == "/p" || nombre == "h1" || nombre == "h2" || nombre == "h3" || nombre == "li" || nombre == "tr" || nombre == "/table") {
                cerrar();
                if (nombre == "p") { cur.centrado = low.find("center") != std::string::npos; cur.negrita = cur.centrado; }
                if (nombre == "li") { bool bb = bold; bold = false; add("\x95 "); bold = bb; }
                if (nombre[0] == 'h') cur.negrita = true;
            } else if (nombre == "br") add("\n");
            else if (nombre == "b" || nombre == "strong") bold = true;
            else if (nombre == "/b" || nombre == "/strong") bold = false;
            i = j + 1;
            continue;
        }
        if (!in_body) { i++; continue; }
        if (c == '&') {
            size_t j = h.find(';', i);
            if (j != std::string::npos && j - i < 10) { add(entidad(h.substr(i + 1, j - i - 1))); i = j + 1; espacio = false; continue; }
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!espacio && !vacio()) { add(" "); espacio = true; } }
        else { add(std::string(1, c)); espacio = false; }
        i++;
    }
    cerrar();
    return out;
}
void cargar_pagina(Browser &b, const std::string &winpath) {
    b.pagina.clear(); b.scroll = 0;
    std::ifstream in(host_path(winpath), std::ios::binary);
    if (!in) { plog("WebBrowser: no se encuentra %s", winpath.c_str()); return; }
    std::string h((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    b.pagina = parse_html(h);
    if (b.hwnd) wm_invalidate(b.hwnd, true);
}
STUB(ext_gdi32_dll_CreateFontIndirectA);
STUB(ext_user32_dll_MoveWindow);
u32 fuente(bool negrita) {
    u32 &f = negrita ? g_font_bold : g_font;
    if (!f) {
        u32 lf = heap_alloc(60, true);
        wr32(lf, (u32)-12); wr32(lf + 16, negrita ? 700 : 400);   // lfHeight, lfWeight
        gstrcpy(lf + 28, "Arial");
        f = call_stub(ext_gdi32_dll_CreateFontIndirectA, {lf});
        heap_free(lf);
    }
    return f;
}
// Un párrafo se compone palabra a palabra (cada una con su fuente, normal o negrita) con ajuste de línea; <br> corta línea.
struct Palabra { std::string t; bool negrita; int w, wsin; bool salto; };   // w con su espacio final, wsin sin él
void pintar(Browser &b) {
    // el control ocupa toda el área cliente de la vista CHtmlView (la MFC no nos avisa del tamaño)
    Rect cl = wm_client(b.hwnd);
    { static int n = 0; Rect sc = wm_screen(b.hwnd, true); if (n++ < 6) plog("WebBrowser: pinto %dx%d en %d,%d (padre 0x%x)", cl.w(), cl.h(), sc.l, sc.t, wm_parent(b.hwnd)); }
    ctl::Paint p(b.hwnd, 0);
    gdi_fill_color(p.dc, cl, 0xFFFFFF);
    gdi_set_bk(p.dc, 0xFFFFFF, 1);
    gdi_set_text_color(p.dc, 0x000000);
    const int margen = 8, ancho = cl.w() - 2 * margen;
    auto medir = [&](const std::string &t, bool negrita) {   // anchura de un texto de una línea
        gdi_select(p.dc, fuente(negrita));
        Rect r{0, 0, 10000, 100};
        gdi_draw_text(p.dc, t, r, 0x400 | 0x20);   // DT_CALCRECT | DT_SINGLELINE
        return r.w();
    };
    int alto_linea = 0;
    { gdi_select(p.dc, fuente(true)); Rect r{0, 0, 10000, 100}; gdi_draw_text(p.dc, "Ag", r, 0x400 | 0x20); alto_linea = r.h(); }
    int y = margen - b.scroll;
    for (const Parrafo &pa : b.pagina) {
        // palabras (con su espacio) en orden, cada una con su fuente
        std::vector<Palabra> pal;
        for (const Run &r : pa.runs) {
            bool neg = r.negrita || pa.negrita;
            size_t i = 0;
            while (i < r.texto.size()) {
                if (r.texto[i] == '\n') { pal.push_back({"", neg, 0, 0, true}); i++; continue; }
                size_t j = i;
                while (j < r.texto.size() && r.texto[j] != ' ' && r.texto[j] != '\n') j++;
                while (j < r.texto.size() && r.texto[j] == ' ') j++;
                std::string t = r.texto.substr(i, j - i), sin = t;
                while (!sin.empty() && sin.back() == ' ') sin.pop_back();
                int esp = medir("x x", neg) - medir("xx", neg);   // anchura de un espacio (DT_CALCRECT no cuenta los finales)
                int wsin = medir(sin, neg), w = wsin + (int)(t.size() - sin.size()) * esp;
                pal.push_back({t, neg, w, wsin, false});
                i = j;
            }
        }
        // líneas: se llenan hasta el ancho; la anchura de una línea no cuenta el espacio final de su última palabra
        size_t k = 0;
        while (k < pal.size()) {
            size_t fin = k;
            int w = 0;
            while (fin < pal.size() && !pal[fin].salto) {
                if (fin > k && w + pal[fin].wsin > ancho) break;
                w += pal[fin].w;
                fin++;
            }
            int wl = 0;
            for (size_t q = k; q < fin; q++) wl += q + 1 < fin ? pal[q].w : pal[q].wsin;
            int x = cl.l + margen + (pa.centrado ? std::max(0, (ancho - wl) / 2) : 0);
            if (y + alto_linea > cl.t && y < cl.b)
                for (size_t q = k; q < fin; q++) {
                    gdi_select(p.dc, fuente(pal[q].negrita));
                    Rect r{x, y, x + pal[q].w + 4, y + alto_linea};
                    gdi_draw_text(p.dc, pal[q].t, r, 0x20);   // DT_SINGLELINE
                    x += pal[q].w;
                }
            y += alto_linea;
            k = fin < pal.size() && pal[fin].salto ? fin + 1 : fin;
        }
        y += 6;
    }
    b.alto_total = y + b.scroll;
}
void wb_wndproc() {   // (hwnd, msg, wp, lp)
    u32 hwnd = arg(0), msg = arg(1), wp = arg(2);
    auto it = g_by_hwnd.find(hwnd);
    Browser *b = it == g_by_hwnd.end() ? nullptr : it->second;
    if (b) switch (msg) {
        case ctl::WM_PAINT: pintar(*b); RET(0, 4);
        case ctl::WM_ERASEBKGND: RET(1, 4);
        case ctl::WM_LBUTTONDOWN: case ctl::WM_MOUSEWHEEL: {   // avanzar/retroceder página
            Rect cl = wm_client(b->hwnd);
            int paso = std::max(40, cl.h() - 24);
            bool atras = msg == ctl::WM_MOUSEWHEEL ? (i32)(i16)(wp >> 16) > 0 : ctl::hi16(arg(3)) < cl.h() / 2 && b->scroll > 0 && false;
            int nuevo = atras ? b->scroll - paso : b->scroll + paso;
            if (msg == ctl::WM_LBUTTONDOWN && b->scroll + cl.h() >= b->alto_total) nuevo = 0;   // al final: vuelve al principio
            b->scroll = std::max(0, std::min(nuevo, std::max(0, b->alto_total - cl.h() + 8)));
            wm_invalidate(b->hwnd, true);
            RET(0, 4);
        }
        default: break;
    }
    ext_user32_dll_DefWindowProcA();
}

// ---------------------------------------------------------------- la ventana del control (hija, pinta la página)
STUB(ext_user32_dll_RegisterClassA);
STUB(ext_user32_dll_CreateWindowExA);
STUB(ext_user32_dll_DefWindowProcA);
STUB(ext_user32_dll_DestroyWindow);
STUB(ext_user32_dll_ShowWindow);
u32 create_control_window(u32 parent, u32 rect) {
    if (!g_wndclass) {
        u32 wc = heap_alloc(40 + 32, true);
        gstrcpy(wc + 40, "PCFWebBrowser");
        wr32(wc + 4, host_fn(wb_wndproc));   // lpfnWndProc
        wr32(wc + 36, wc + 40);                                  // lpszClassName
        g_wndclass = guest_call(host_fn(ext_user32_dll_RegisterClassA), {wc});
    }
    u32 cls = heap_alloc(32, true), title = cls + 16;
    gstrcpy(cls, "PCFWebBrowser");
    i32 x = 0, y = 0, w = 1, h = 1;
    if (rect) { x = (i32)rd32(rect); y = (i32)rd32(rect + 4); w = std::max(1, (i32)rd32(rect + 8) - x); h = std::max(1, (i32)rd32(rect + 12) - y); }
    u32 hwnd = guest_call(host_fn(ext_user32_dll_CreateWindowExA),
                          {0, cls, title, 0x40000000u | 0x10000000u, (u32)x, (u32)y, (u32)w, (u32)h, parent, 0, 0, 0});   // WS_CHILD | WS_VISIBLE
    heap_free(cls);
    plog("WebBrowser: ventana del control 0x%x (padre 0x%x, %d,%d %dx%d)", hwnd, parent, x, y, w, h);
    return hwnd;
}

// IOleInPlaceSite del sitio (con referencia; 0 si no lo da)
u32 site_ips(Browser &b) {
    if (!b.site) return 0;
    u32 tmp = heap_alloc(20, true);
    write_guid(tmp, IID_IOleInPlaceSite);
    u32 ips = guest_call(rd32(rd32(b.site)), {b.site, tmp, tmp + 16}) == 0 ? rd32(tmp + 16) : 0;
    heap_free(tmp);
    return ips;
}
// Desactivación como la hace el control de verdad: avisa al sitio (OnUIDeactivate / OnInPlaceDeactivate) para que
// la MFC suelte sus punteros al control (m_pSiteUIActive, subclasificación de la ventana)
void ui_desactivar(Browser &b) {
    if (!b.ui_activo) return;
    b.ui_activo = false;
    if (u32 ips = site_ips(b)) { guest_call(rd32(rd32(ips) + 40), {ips, 0}); guest_call(rd32(rd32(ips) + 8), {ips}); }   // OnUIDeactivate(FALSE)
}
void ip_desactivar(Browser &b) {
    ui_desactivar(b);
    if (!b.ip_activo) return;
    b.ip_activo = false;
    if (u32 ips = site_ips(b)) { guest_call(rd32(rd32(ips) + 44), {ips}); guest_call(rd32(rd32(ips) + 8), {ips}); }      // OnInPlaceDeactivate
}

// ---------------------------------------------------------------- IOleObject
void o_SetClientSite() { if (Browser *b = self()) { b->site = arg(1); if (b->site) guest_call(rd32(rd32(b->site) + 4), {b->site}); } RET(S_OK, 2); }
void o_GetClientSite() { Browser *b = self(); u32 s = b ? b->site : 0; if (s) guest_call(rd32(rd32(s) + 4), {s}); wr32(arg(1), s); RET(S_OK, 2); }
void o_DoVerb() {   // (verb, msg, site, index, hwndParent, rect)
    Browser *b = self();
    i32 verb = (i32)arg(1);
    u32 parent = arg(5), rect = arg(6);
    if (b && !b->hwnd) {
        // activación in-place como la haría IE: el sitio (IOleInPlaceSite) da la ventana contenedora y el rectángulo
        u32 ips = 0, tmp = heap_alloc(16 + 16 + 16 + 20 + 8, true), iid = tmp, pos = tmp + 16, clip = tmp + 32, info = tmp + 48, outs = tmp + 68;
        write_guid(iid, IID_IOleInPlaceSite);
        if (b->site && guest_call(rd32(rd32(b->site)), {b->site, iid, outs}) == 0) ips = rd32(outs);
        if (ips) {
            u32 vt = rd32(ips);
            wr32(outs, 0);
            if (guest_call(rd32(vt + 12), {ips, outs}) == 0 && rd32(outs)) parent = rd32(outs);   // GetWindow
            guest_call(rd32(vt + 20), {ips});                                                      // CanInPlaceActivate
            wr32(info, 20);
            u32 pf = tmp + 72, pd = tmp + 76;
            if (guest_call(rd32(vt + 32), {ips, pf, pd, pos, clip, info}) == 0 && rd32(pos + 8) > rd32(pos)) rect = pos;   // GetWindowContext
            if (rd32(pf)) guest_call(rd32(rd32(rd32(pf)) + 8), {rd32(pf)});
            if (rd32(pd)) guest_call(rd32(rd32(rd32(pd)) + 8), {rd32(pd)});
        }
        // la ventana antes de OnInPlaceActivate: ahí la MFC pide GetWindow y se engancha (subclasifica) a ella
        if (parent) { b->hwnd = create_control_window(parent, rect); if (b->hwnd) g_by_hwnd[b->hwnd] = b; }
        if (ips) {
            guest_call(rd32(rd32(ips) + 24), {ips});   // OnInPlaceActivate
            guest_call(rd32(rd32(ips) + 28), {ips});   // OnUIActivate
            guest_call(rd32(rd32(ips) + 8), {ips});    // Release
            b->ip_activo = b->ui_activo = true;
        }
        heap_free(tmp);
    } else if (b && b->hwnd && verb != -3 && b->ip_activo && !b->ui_activo) {
        // el panel vuelve a abrirse: el sitio tiene que volver a saber que somos el control activo (OnUIActivate)
        if (u32 ips = site_ips(*b)) { guest_call(rd32(rd32(ips) + 28), {ips}); guest_call(rd32(rd32(ips) + 8), {ips}); b->ui_activo = true; }
    }
    // OLEIVERB_HIDE (-3): el panel se cierra → la ventana se oculta (es hija de la ventana principal, no del diálogo) y
    // el sitio deja de tenernos como control activo (si no, la MFC guarda un puntero al control viejo en su
    // COleControlContainer::m_pSiteUIActive y, al activar el panel de la pantalla siguiente, llama a UIDeactivate
    // a través de la vista ya destruida: basura y salto a 0 en la segunda o tercera pantalla con ayuda);
    // SHOW (-1), OPEN (-2), INPLACEACTIVATE (-5) y UIACTIVATE (-4) la vuelven a enseñar
    if (b && verb == -3) ui_desactivar(*b);
    if (b && b->hwnd) call_stub(ext_user32_dll_ShowWindow, {b->hwnd, verb == -3 ? 0u : 5u});
    plog("WebBrowser: DoVerb(%d)", verb);
    RET(S_OK, 7);
}
void o_Close() {   // (flags): el control se destruye con su vista → fuera la ventana (avisando al sitio antes)
    Browser *b = self();
    if (b) ip_desactivar(*b);
    if (b && b->hwnd) { call_stub(ext_user32_dll_DestroyWindow, {b->hwnd}); g_by_hwnd.erase(b->hwnd); b->hwnd = 0; }
    RET(S_OK, 2);
}
void o_GetUserClassID() { write_guid(arg(1), CLSID_WebBrowser); RET(S_OK, 2); }
// la MFC fija el tamaño del control con SetExtent (HIMETRIC) y luego dimensiona su ventana con lo que devuelve GetExtent
void o_SetExtent() { Browser *b = self(); u32 p = arg(2); if (b && p) { b->ext_w = rd32(p); b->ext_h = rd32(p + 4); } RET(S_OK, 3); }
void o_GetExtent() { Browser *b = self(); u32 p = arg(2); if (p) { wr32(p, b ? b->ext_w : 2646); wr32(p + 4, b ? b->ext_h : 2646); } RET(S_OK, 3); }
void o_Advise() { if (arg(2)) wr32(arg(2), 1); RET(S_OK, 3); }
void o_GetMiscStatus() {   // RECOMPOSEONRESIZE | CANTLINKINSIDE | INSIDEOUT | ACTIVATEWHENVISIBLE | SETCLIENTSITEFIRST
    if (arg(2)) wr32(arg(2), 0x1 | 0x10 | 0x80 | 0x100 | 0x20000);
    RET(S_OK, 3);
}

// ---------------------------------------------------------------- IOleInPlaceObject / IOleInPlaceActiveObject
void ip_InPlaceDeactivate() { Browser *b = self(); if (b) { ip_desactivar(*b); if (b->hwnd) call_stub(ext_user32_dll_ShowWindow, {b->hwnd, 0}); } RET(S_OK, 1); }
void ip_UIDeactivate() { Browser *b = self(); if (b) ui_desactivar(*b); RET(S_OK, 1); }
void ip_SetObjectRects() {   // (pos, clip): la ventana del control sigue al área del panel
    Browser *b = self();
    u32 r = arg(1);
    if (b && b->hwnd && r) {
        Rect rc = rd_rect(r);
        call_stub(ext_user32_dll_MoveWindow, {b->hwnd, (u32)rc.l, (u32)rc.t, (u32)std::max(1, rc.w()), (u32)std::max(1, rc.h()), 1});
    }
    RET(S_OK, 3);
}
void ip_GetWindow() { Browser *b = self(); wr32(arg(1), b ? b->hwnd : 0); RET(b && b->hwnd ? S_OK : E_FAIL, 2); }

// ---------------------------------------------------------------- IOleControl
void c_GetControlInfo() { u32 p = arg(1); if (p) { wr32(p + 4, 0); wr32(p + 8, 0); wr32(p + 12, 0); } RET(S_OK, 2); }

// ---------------------------------------------------------------- IPersistStreamInit
void p_GetClassID() { write_guid(arg(1), CLSID_WebBrowser); RET(S_OK, 2); }
void p_IsDirty() { RET(S_FALSE, 1); }
void p_GetSizeMax() { if (arg(1)) { wr32(arg(1), 0); wr32(arg(1) + 4, 0); } RET(S_OK, 2); }

// ---------------------------------------------------------------- IConnectionPointContainer / IConnectionPoint
void cp_GetConnectionInterface() { write_guid(arg(1), IID_IDispatch); RET(S_OK, 2); }
void cp_GetContainer() { Browser *b = self(); wr32(arg(1), b ? b->obj[F_CPC] : 0); RET(S_OK, 2); }
void cp_Advise() { if (arg(2)) wr32(arg(2), 1); RET(S_OK, 3); }
void cpc_FindConnectionPoint() {   // (iid, out): un punto de conexión que acepta cualquier sumidero
    Browser *b = self();
    if (b && !b->cp) { b->cp = com_object(g_vt_cp, 8); g_by_obj[b->cp] = b; }
    wr32(arg(2), b ? b->cp : 0);
    RET(b ? S_OK : E_FAIL, 3);
}

// ---------------------------------------------------------------- IDispatch / IWebBrowser2
void d_GetTypeInfoCount() { if (arg(1)) wr32(arg(1), 0); RET(S_OK, 2); }
void d_GetIDsOfNames() { RET(0x80020006u, 6); }   // DISP_E_UNKNOWNNAME
void d_Invoke() {   // (dispid, riid, lcid, flags, params, result, excepinfo, argerr)
    if (arg(6)) std::memset(MEM + arg(6), 0, 16);   // VT_EMPTY
    RET(S_OK, 9);
}
void w_get_HWND() { Browser *b = self(); wr32(arg(1), b ? b->hwnd : 0); RET(S_OK, 2); }
void w_get_ReadyState() { if (arg(1)) wr32(arg(1), 4); RET(S_OK, 2); }   // READYSTATE_COMPLETE
void w_get_Visible() { if (arg(1)) wr16(arg(1), 0xFFFF); RET(S_OK, 2); }   // VARIANT_TRUE
void w_get_Type() { if (arg(1)) wr32(arg(1), 0); RET(S_OK, 2); }
void w_Navigate() { std::string u = wstr(arg(1)); plog("WebBrowser: Navigate(\"%s\")", u.c_str()); if (Browser *b = self()) cargar_pagina(*b, u); RET(S_OK, 6); }
void w_Navigate2() {   // (VARIANT *url, …): url VT_BSTR en +8
    static int n = 0;
    u32 v = arg(1);
    std::string u = v && rd16(v) == 8 && rd32(v + 8) ? wstr(rd32(v + 8)) : "";
    if (n++ < 40) plog("WebBrowser: Navigate2(\"%s\")", u.c_str());
    if (Browser *b = self()) cargar_pagina(*b, u);
    RET(S_OK, 6);
}
void w_QueryStatusWB() { if (arg(2)) wr32(arg(2), 1); RET(S_OK, 3); }   // OLECMDF_SUPPORTED
void w_ExecWB() { if (arg(4)) std::memset(MEM + arg(4), 0, 16); RET(S_OK, 5); }

void init() {
    if (g_vt_cf) return;
    g_vt_cf = com_vtbl({[] {   // IClassFactory::QueryInterface
                            u32 iid = arg(1), out = arg(2);
                            if (com_guid_eq(iid, IID_IClassFactory) || com_guid_eq(iid, IID_IUnknown)) { com_addref(arg(0)); wr32(out, arg(0)); RET(S_OK, 3); }
                            wr32(out, 0); RET(E_NOINTERFACE, 3);
                        },
                        [] { RET(com_addref(arg(0)), 1); }, [] { RET(com_release(arg(0)), 1); },
                        [] {   // CreateInstance(outer, iid, out): un control nuevo
                            u32 iid = arg(2), out = arg(3);
                            Browser *b = new Browser;
                            for (int f = 0; f < F_N; f++) { b->obj[f] = com_object(g_vt[f], 8); wr32(b->obj[f] + 8, (u32)f); g_by_obj[b->obj[f]] = b; }
                            b->refs = 0;
                            // la interfaz pedida
                            u32 saved = esp;
                            esp -= 16; wr32(esp + 4, b->obj[F_OLEOBJECT]); wr32(esp + 8, iid); wr32(esp + 12, out);
                            b_QueryInterface();
                            u32 hr = eax;
                            esp = saved;
                            plog("WebBrowser: control nuevo (IOleObject 0x%x, IWebBrowser2 0x%x) -> 0x%x", b->obj[F_OLEOBJECT], b->obj[F_BROWSER], hr);
                            RET(hr, 4);
                        },
                        m_Ok<2>});
    // IOleObject (24)
    g_vt[F_OLEOBJECT] = com_vtbl({b_QueryInterface, b_AddRef, b_Release, o_SetClientSite, o_GetClientSite, m_Ok<3> /*SetHostNames*/,
                                  o_Close, m_Ok<3> /*SetMoniker*/, m_NotImpl<4> /*GetMoniker*/, m_NotImpl<4> /*InitFromData*/,
                                  m_NotImpl<3> /*GetClipboardData*/, o_DoVerb, m_NotImpl<2> /*EnumVerbs*/, m_Ok<1> /*Update*/,
                                  m_Ok<1> /*IsUpToDate*/, o_GetUserClassID, m_NotImpl<3> /*GetUserType*/, o_SetExtent,
                                  o_GetExtent, o_Advise, m_Ok<2> /*Unadvise*/, m_NotImpl<2> /*EnumAdvise*/, o_GetMiscStatus,
                                  m_Ok<2> /*SetColorScheme*/});
    // IOleInPlaceObject (9)
    g_vt[F_INPLACE] = com_vtbl({b_QueryInterface, b_AddRef, b_Release, ip_GetWindow, m_NotImpl<2> /*ContextSensitiveHelp*/,
                                ip_InPlaceDeactivate, ip_UIDeactivate, ip_SetObjectRects, m_NotImpl<1> /*ReactivateAndUndo*/});
    // IOleInPlaceActiveObject (10)
    g_vt[F_ACTIVE] = com_vtbl({b_QueryInterface, b_AddRef, b_Release, ip_GetWindow, m_NotImpl<2>, m_NotImpl<2> /*TranslateAccelerator*/,
                               m_Ok<2> /*OnFrameWindowActivate*/, m_Ok<2> /*OnDocWindowActivate*/, m_Ok<4> /*ResizeBorder*/, m_Ok<2> /*EnableModeless*/});
    // IOleControl (7)
    g_vt[F_CONTROL] = com_vtbl({b_QueryInterface, b_AddRef, b_Release, c_GetControlInfo, m_Ok<2> /*OnMnemonic*/,
                                m_Ok<2> /*OnAmbientPropertyChange*/, m_Ok<2> /*FreezeEvents*/});
    // IPersistStreamInit (9)
    g_vt[F_PERSIST] = com_vtbl({b_QueryInterface, b_AddRef, b_Release, p_GetClassID, p_IsDirty, m_Ok<2> /*Load*/, m_Ok<3> /*Save*/,
                                p_GetSizeMax, m_Ok<1> /*InitNew*/});
    // IConnectionPointContainer (5) e IConnectionPoint (10)
    g_vt[F_CPC] = com_vtbl({b_QueryInterface, b_AddRef, b_Release, m_NotImpl<2> /*EnumConnectionPoints*/, cpc_FindConnectionPoint});
    g_vt_cp = com_vtbl({b_QueryInterface, b_AddRef, b_Release, cp_GetConnectionInterface, cp_GetContainer, cp_Advise,
                        m_Ok<2> /*Unadvise*/, m_NotImpl<2> /*EnumConnections*/});
    // IDispatch (7) + IWebBrowser (25) + IWebBrowserApp (20) + IWebBrowser2 (19)
    g_vt[F_BROWSER] = com_vtbl({b_QueryInterface, b_AddRef, b_Release, d_GetTypeInfoCount, m_NotImpl<4> /*GetTypeInfo*/, d_GetIDsOfNames, d_Invoke,
                                // IWebBrowser
                                m_Ok<1> /*GoBack*/, m_Ok<1> /*GoForward*/, m_Ok<1> /*GoHome*/, m_Ok<1> /*GoSearch*/, w_Navigate,
                                m_Ok<1> /*Refresh*/, m_Ok<2> /*Refresh2*/, m_Ok<1> /*Stop*/, m_Get0<2> /*get_Application*/,
                                m_Get0<2> /*get_Parent*/, m_Get0<2> /*get_Container*/, m_Get0<2> /*get_Document*/,
                                m_Get0<2> /*get_TopLevelContainer*/, w_get_Type, m_Get0<2> /*get_Left*/, m_Ok<2> /*put_Left*/,
                                m_Get0<2> /*get_Top*/, m_Ok<2>, m_Get0<2> /*get_Width*/, m_Ok<2>, m_Get0<2> /*get_Height*/, m_Ok<2>,
                                m_Get0<2> /*get_LocationName*/, m_Get0<2> /*get_LocationURL*/, m_Get0<2> /*get_Busy*/,
                                // IWebBrowserApp
                                m_Ok<1> /*Quit*/, m_Ok<3> /*ClientToWindow*/, m_Ok<6> /*PutProperty(BSTR, VARIANT por valor = 4)*/,
                                m_Get0<3> /*GetProperty(BSTR, VARIANT*)*/, m_Get0<2> /*get_Name*/, w_get_HWND, m_Get0<2> /*get_FullName*/,
                                m_Get0<2> /*get_Path*/, w_get_Visible, m_Ok<2> /*put_Visible*/, m_Get0<2> /*get_StatusBar*/, m_Ok<2>,
                                m_Get0<2> /*get_StatusText*/, m_Ok<2>, m_Get0<2> /*get_ToolBar*/, m_Ok<2>, m_Get0<2> /*get_MenuBar*/, m_Ok<2>,
                                m_Get0<2> /*get_FullScreen*/, m_Ok<2>,
                                // IWebBrowser2
                                w_Navigate2, w_QueryStatusWB, w_ExecWB, m_Ok<4> /*ShowBrowserBar*/, w_get_ReadyState,
                                m_Get0<2> /*get_Offline*/, m_Ok<2>, m_Get0<2> /*get_Silent*/, m_Ok<2>, m_Get0<2> /*get_RegisterAsBrowser*/, m_Ok<2>,
                                m_Get0<2> /*get_RegisterAsDropTarget*/, m_Ok<2>, m_Get0<2> /*get_TheaterMode*/, m_Ok<2>,
                                m_Get0<2> /*get_AddressBar*/, m_Ok<2>, m_Get0<2> /*get_Resizable*/, m_Ok<2>});
}

}  // namespace

// CoGetClassObject(CLSID_WebBrowser, IID_IClassFactory): la fábrica (0 si no es esa clase)
u32 webbrowser_class_object(u32 clsid, u32 iid) {
    if (!com_guid_eq(clsid, CLSID_WebBrowser)) return 0;
    init();
    if (!g_cf) g_cf = com_object(g_vt_cf); else com_addref(g_cf);
    if (!com_guid_eq(iid, IID_IClassFactory) && !com_guid_eq(iid, IID_IUnknown)) plog("WebBrowser: CoGetClassObject con otra interfaz");
    return g_cf;
}
