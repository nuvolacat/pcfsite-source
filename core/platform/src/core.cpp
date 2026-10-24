// PCF7 — núcleo de la capa de plataforma: memoria y CPU del invitado, llamadas host→invitado,
// montículo, diagnóstico.
#include "plat.h"

#include <cstdarg>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <vector>

extern "C" {
u8 MEM[MEMSZ];
u32 eax, ecx, edx, ebx, esp, ebp, esi, edi, CF, ZF, SF, OF, PF, t, DF;
u32 cur_fn; u32 fnring[256]; u32 fnring_i;
u32 w_steps;
double ST[8]; u32 TOP, FPUCW = 0x037f, FPUSW; u64 MM[8];
}

// ---------------------------------------------------------------- diagnóstico

static std::mutex g_logmx;

// Además de stderr, todo se copia a $PCF7_LOG (por defecto <carpeta de trabajo>/<juego>.log) para poder ver
// qué pasó cuando el juego se lanza sin terminal.
static FILE *logfile() {
    static FILE *f = [] {
        const char *p = std::getenv("PCF7_LOG");
        std::string path = p && *p ? p : juego_trabajo() + "/" + JUEGO_ID + ".log";
        FILE *f = std::fopen(path.c_str(), std::getenv("PCF7_RELAUNCHED") ? "a" : "w");   // tras volver del lanzador se sigue el mismo registro
        if (f) setvbuf(f, nullptr, _IOLBF, 0);
        return f;
    }();
    return f;
}

static void logv(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    vfprintf(stderr, fmt, ap);
    if (FILE *f = logfile()) vfprintf(f, fmt, ap2);
    va_end(ap2);
}
static void logf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); logv(fmt, ap); va_end(ap); }

void plog(const char *fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logmx);
    va_list ap; va_start(ap, fmt);
    logv(fmt, ap);
    va_end(ap);
    logf("\n");
}

void plog_once(const std::string &key, const char *fmt, ...) {
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lk(g_logmx);
        if (!seen.insert(key).second) return;
    }
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plog("%s", buf);
}

u32 call_stub(void (*f)(void), std::initializer_list<u32> a) {
    u32 sp = esp;
    esp -= 4 * (u32)a.size();
    u32 i = 0;
    for (u32 v : a) wr32(esp + 4 * i++, v);
    esp -= 4;
    wr32(esp, 0);
    f();
    esp = sp;
    return eax;
}

u32 EFL_HI = 0;
// CPUID: Pentium II 266 ("GenuineIntel", familia 6 modelo 5) sin MMX, la CPU típica de 1998. El juego
// la mira para decidir la música y las transiciones por defecto.
void cpuid_emul() {
    switch (eax) {
    case 0: eax = 1; ebx = 0x756E6547u; edx = 0x49656E69u; ecx = 0x6C65746Eu; break;   // "GenuineIntel"
    case 1: eax = 0x652; ebx = 0; ecx = 0; edx = 0x813Fu; break;                          // FPU VME DE PSE TSC MSR CX8 CMOV
    default: eax = ebx = ecx = edx = 0; break;
    }
}

// PCF7_TRAZA=paso[:desde]: suma de control de cada entrada de función del hilo principal del juego (función y
// registros), anotada cada `paso` entradas a partir de la `desde`. Para comparar dos versiones (nativo/web) y
// encontrar la primera llamada en la que divergen. En la web se activa con ?traza=paso:desde (Module.pcf7Traza).
u32 g_traza_paso = 0, g_traza_desde = 0, g_traza_cuantas = 0;
static u64 g_traza_n = 0;
static u32 g_traza_h = 2166136261u;
u32 current_thread_id();
static void traza(u32 fn) {
    if (current_thread_id() != 0x1004) return;
    g_traza_n++;
    u32 v[8] = {fn, eax, ecx, edx, ebx, esi, edi, esp};
    for (u32 x : v) g_traza_h = (g_traza_h ^ x) * 16777619u;
    if (g_traza_n >= g_traza_desde && (g_traza_n - g_traza_desde) % g_traza_paso == 0 &&
        (!g_traza_cuantas || (g_traza_n - g_traza_desde) / g_traza_paso < g_traza_cuantas))
    {
        auto txt = [](u32 p) {   // la cadena a la que apunta un registro, si parece texto
            std::string r;
            if (p < 0x10000 || p >= MEMSZ - 40) return r;
            for (u32 i = 0; i < 40 && MEM[p + i]; i++) { u8 c = MEM[p + i]; if (c < 32 || c > 126) return std::string(); r += (char)c; }
            return r;
        };
        plog("traza %llu %08x f_%x eax=%x ecx=%x edx=%x esi=%x edi=%x esp=%x [%s|%s|%s]", (unsigned long long)g_traza_n, g_traza_h,
             fn, eax, ecx, edx, esi, edi, esp, txt(eax).c_str(), txt(edx).c_str(), txt(rd32(esp + 8)).c_str());
    }
}
int g_watch = [] {
    const char *w = std::getenv("PCF7_WATCH"), *m = std::getenv("PCF7_MEMWATCH"), *r = std::getenv("PCF7_TRACE_RANGE");
    if (const char *t = std::getenv("PCF7_TRAZA")) std::sscanf(t, "%u:%u:%u", &g_traza_paso, &g_traza_desde, &g_traza_cuantas);
    return (w && *w) || (m && *m) || (r && *r) || g_traza_paso || std::getenv("PCF7_SEHCHECK") || std::getenv("PCF7_MEMWATCH_REL") ? 1 : 0;
}();
// PCF7_TRACE_RANGE=inicio-fin (hex): anota cada entrada a una función de ese rango (hasta PCF7_TRACE_MAX, 400000), con su
// dirección de retorno y los dos primeros argumentos
u32 g_rango_ecx = 0;   // ecx de la última entrada a una función de PCF7_TRACE_RANGE (para PCF7_FATAL_LISTA=ecx:…)
static void range_trace(u32 fn) {
    static const char *e = std::getenv("PCF7_TRACE_RANGE");
    static u32 lo = e ? (u32)std::strtoul(e, nullptr, 16) : 0;
    static u32 hi = e && std::strchr(e, '-') ? (u32)std::strtoul(std::strchr(e, '-') + 1, nullptr, 16) : 0;
    static int n = 0;
    if (fn >= lo && fn < hi) g_rango_ecx = ecx;
    // PCF7_TRACE_LISTA=paso:desplazamiento — en cada entrada, la lista {puntero, cuenta} de ecx (TEMPORAL)
    if (fn >= lo && fn < hi) if (const char *li = std::getenv("PCF7_TRACE_LISTA")) {
        u32 paso = 0, off = 0;
        std::sscanf(li, "%x:%x", &paso, &off);
        u32 base = rd32(ecx), cnt = rd32(ecx + 4);
        plog("lista %x (<- %x): base %x, %u elementos", ecx, rd32(esp), base, cnt);
        for (u32 i = 0; i < cnt && i < 200; i++) {
            u32 e = base + i * paso;
            std::string t;
            for (u32 k = 0; k < 40 && e + k < MEMSZ && MEM[e + k] >= 32 && MEM[e + k] < 127; k++) t += (char)MEM[e + k];
            plog("  [%u] %x «%s» +%x=%x", i, e, t.c_str(), off, rd32(e + off));
        }
    }
    // PCF7_TRACE_IMG=off_buf:off_w:off_h — imagen del objeto ecx: ancho×alto frente al tamaño real del bloque (TEMPORAL)
    if (fn >= lo && fn < hi) if (const char *im = std::getenv("PCF7_TRACE_IMG")) {
        u32 ob = 0, ow = 0, oh = 0;
        std::sscanf(im, "%x:%x:%x", &ob, &ow, &oh);
        u32 b = rd32(ecx + ob), w = rd32(ecx + ow), h = rd32(ecx + oh);
        u32 hs = b ? heap_size(b) : 0;
        if ((u64)w * h > hs) plog("img: obj %x buf %x %ux%u = %u > bloque %u (f_%x <- %x)", ecx, b, w, h, w * h, hs, fn, rd32(esp));
    }
    // PCF7_TRACE_ARGMEM=k: además, las 4 palabras a las que apuntan los k primeros argumentos (TEMPORAL)
    if (fn >= lo && fn < hi) if (const char *am = std::getenv("PCF7_TRACE_ARGMEM")) {
        for (int k = 1; k <= std::atoi(am); k++) {
            u32 p = rd32(esp + 4 * k);
            if (p > 0x10000 && p < MEMSZ - 16) plog("  arg%d [%x] = %d %d %d %d", k, p, (int)rd32(p), (int)rd32(p + 4), (int)rd32(p + 8), (int)rd32(p + 12));
        }
    }
    static const int tope = std::getenv("PCF7_TRACE_MAX") ? std::atoi(std::getenv("PCF7_TRACE_MAX")) : 400000;   // entradas que se anotan
    if (fn >= lo && fn < hi && n < tope) {
        n++;
        static const u32 off = std::getenv("PCF7_TRACE_OFF") ? (u32)std::strtoul(std::getenv("PCF7_TRACE_OFF"), nullptr, 16) : 4;
        u32 o = ecx < MEMSZ - off - 4 ? rd32(ecx + off) : 0, vt = o && o < MEMSZ - 4 ? rd32(o) : 0;   // this->[off] (PCF7_TRACE_OFF, 4) y su vtable
        auto txt = [](u32 p) {   // la cadena a la que apunta un argumento, si parece texto
            std::string r;
            if (p < 0x10000 || p >= MEMSZ - 64) return r;
            for (u32 i = 0; i < 60 && MEM[p + i]; i++) { u8 c = MEM[p + i]; if (c < 32 || c > 126) return std::string(); r += (char)c; }
            return r;
        };
        plog("traza f_%x <- %x (%x, %x, %x) ecx=%x eax=%x esi=%x edi=%x [ecx+off]=%x vt=%x [%s|%s|%s]", fn, rd32(esp), rd32(esp + 4), rd32(esp + 8),
             rd32(esp + 12), ecx, eax, esi, edi, o, vt, txt(rd32(esp + 4)).c_str(), txt(rd32(esp + 8)).c_str(), txt(rd32(esp + 12)).c_str());
    }
}
// PCF7_MEMWATCH=dirección: en cada entrada de función, si la palabra ha cambiado se anota con la función
// anterior (la que la escribió, o una llamada desde ella)
// PCF7_MEMWATCH_REL=función:desplazamiento: la palabra vigilada es ecx+desplazamiento al entrar en esa función
static u32 g_memwatch_dyn = 0;
u32 memwatch_addr() {
    static const char *e = std::getenv("PCF7_MEMWATCH");
    static u32 addr = e && *e ? (u32)std::strtoul(e, nullptr, 16) : 0;
    return g_memwatch_dyn ? g_memwatch_dyn : addr;
}
extern "C" { u32 g_memwatch_w = memwatch_addr(); }
extern "C" void memwatch_write(u32 a, u32 v) {
    static int n = 0;
    // PCF7_MEMWATCH_VAL=valor: solo las escrituras de ese valor (sin límite de 50)
    static const char *fv = std::getenv("PCF7_MEMWATCH_VAL");
    static const u32 solo = fv ? (u32)std::strtoul(fv, nullptr, 16) : 0;
    // PCF7_MEMWATCH_DESDE=valor: todas las escrituras (hasta 3000) a partir de la primera de ese valor
    static const char *fd = std::getenv("PCF7_MEMWATCH_DESDE");
    static bool desde = false;
    if (fd) {
        if (!desde && v == (u32)std::strtoul(fd, nullptr, 16)) desde = true;
        if (!desde || n++ >= 3000) return;
    } else if (fv ? v != solo : n++ >= 50) return;
    std::string ring;
    for (int k = 8; k >= 1; k--) { char b[16]; snprintf(b, sizeof b, " %x", fnring[(fnring_i - k) & 255]); ring += b; }
    std::string ra;   // direcciones de retorno probables (detrás de un call)
    int m = 0;
    for (u32 p = esp; p < esp + 0x600 && m < 10; p += 4) {
        u32 r = rd32(p);
        if (r >= 0x401006 && r < 0x2800000 && (MEM[r - 5] == 0xE8 || MEM[r - 6] == 0xFF || MEM[r - 2] == 0xFF || MEM[r - 3] == 0xFF)) {
            char b[16]; snprintf(b, sizeof b, " %x", r); ra += b; m++;
        }
    }
    plog("memwatch: escritura [%x] = %x en f_%x (esp %x, ebp %x; anillo%s; retornos%s)", a, v, cur_fn, esp, ebp, ring.c_str(), ra.c_str());
}
static void mem_watch(u32 fn) {
    static const char *rel = std::getenv("PCF7_MEMWATCH_REL");
    static u32 rel_fn = rel ? (u32)std::strtoul(rel, nullptr, 16) : 0;
    static u32 rel_off = rel && std::strchr(rel, ':') ? (u32)std::strtoul(std::strchr(rel, ':') + 1, nullptr, 16) : 0;
    if (rel_fn && fn == rel_fn && !g_memwatch_dyn) { g_memwatch_dyn = ecx + rel_off; g_memwatch_w = g_memwatch_dyn; plog("memwatch: vigilo %x", g_memwatch_dyn); }
    u32 addr = memwatch_addr();
    static u32 last = 0, prev_fn = 0;
    static int n = 0;
    if (addr) {
        u32 v = rd32(addr);
        if (v != last && n < 200) {
            n++;
            std::string ring;
            for (int k = 12; k >= 1; k--) { char b[16]; snprintf(b, sizeof b, " %x", fnring[(fnring_i - k) & 255]); ring += b; }
            plog("memwatch [%x]: %x -> %x (antes de f_%x, tras f_%x; anillo%s; esp %x)", addr, last, v, fn, prev_fn, ring.c_str(), esp);
            last = v;
        }
    }
    prev_fn = fn;
}
// retorno de una llamada indirecta a una función del rango de PCF7_TRACE_RANGE (ftab.c): su eax
extern "C" void icall_ret(u32 fn) {
    static const char *e = std::getenv("PCF7_TRACE_RANGE");
    static u32 lo = e ? (u32)std::strtoul(e, nullptr, 16) : 0;
    static u32 hi = e && std::strchr(e, '-') ? (u32)std::strtoul(std::strchr(e, '-') + 1, nullptr, 16) : 0;
    static int n = 0;
    if (fn >= lo && fn < hi && n < 400000) { n++; plog("traza f_%x -> %x", fn, eax); }
}
// PCF7_SEHCHECK: avisa (una vez) cuando fs:[0] apunta por debajo de la pila actual al entrar en una función: alguna
// función ha vuelto sin quitar su registro SEH y el siguiente throw lo usaría
void dump_ring();
// PCF7_TEXTWATCH=texto: cuando se pinta un texto que lo contiene, dónde estaba el juego (diagnóstico)
// PCF7_MEMFIND=texto: al agotar el tiempo, dónde está ese texto en la memoria y quién apunta a cada copia
void mem_find(const char *t) {
    size_t n = std::strlen(t);
    for (u32 a = 0x10000; a + n < MEMSZ; a++) {
        if (MEM[a] != (u8)t[0] || std::memcmp(MEM + a, t, n) != 0) continue;
        u32 s = a;
        while (s > a - 200 && MEM[s - 1] >= 32) s--;
        plog("memfind: «%s» en %x (la cadena empieza en %x)", t, a, s);
        for (u32 p = 0x10000; p + 4 < MEMSZ; p += 4) if (rd32(p) >= s && rd32(p) <= a) plog("memfind:   puntero en %x -> %x", p, rd32(p));
    }
}
void text_watch(const std::string &s) {
    static const char *w = std::getenv("PCF7_TEXTWATCH");
    static int n = 0;
    if (!w || s.find(w) == std::string::npos || n++ >= 3) return;
    std::string ra;
    int m = 0;
    for (u32 a = esp; a < esp + 0x2000 && m < 24; a += 4) {
        u32 v = rd32(a);
        if (v >= 0x401006 && v < 0x2800000 && (MEM[v - 5] == 0xE8 || MEM[v - 6] == 0xFF || MEM[v - 2] == 0xFF || MEM[v - 3] == 0xFF)) { char b[16]; snprintf(b, sizeof b, " %x", v); ra += b; m++; }
    }
    plog("textwatch «%s»; retornos:%s", s.c_str(), ra.c_str());
    dump_ring();
}
static void seh_check(u32 fn) {
    static const bool on = std::getenv("PCF7_SEHCHECK") != nullptr;
    static bool dicho = false;
    if (!on || dicho) return;
    u32 f0 = rd32(0);
    if (f0 == 0xFFFFFFFFu || f0 >= esp || esp - f0 > 0x100000) return;
    dicho = true;
    plog("sehcheck: fs:[0]=%x por debajo de esp=%x al entrar en f_%x (registro {%x, %x})", f0, esp, fn, rd32(f0), rd32(f0 + 4));
    dump_ring();
}

void fn_watch(u32 fn) {
    seh_check(fn);
    if (g_traza_paso) traza(fn);
    mem_watch(fn);
    range_trace(fn);
    if (!std::getenv("PCF7_WATCH")) return;
    static std::map<u32, int> *watch = [] {
        auto *m = new std::map<u32, int>;
        std::string w = std::getenv("PCF7_WATCH");
        for (size_t i = 0; i < w.size();) {
            size_t j = w.find(',', i);
            if (j == std::string::npos) j = w.size();
            (*m)[(u32)std::strtoul(w.substr(i, j - i).c_str(), nullptr, 16)] = 0;
            i = j + 1;
        }
        return m;
    }();
    auto it = watch->find(fn);
    if (it == watch->end() || it->second++ >= 5) return;
    std::string ra;
    int m = 0;
    for (u32 a = esp; a < esp + 0x400 && m < 8; a += 4) {
        u32 v = rd32(a);
        if (v >= 0x401006 && v < 0x2800000 && (MEM[v - 5] == 0xE8 || MEM[v - 6] == 0xFF || MEM[v - 2] == 0xFF || MEM[v - 3] == 0xFF)) { char b[16]; snprintf(b, sizeof b, " %x", v); ra += b; m++; }
    }
    plog("vigilada f_%x (ecx=%x, args %x %x %x) retornos:%s", fn, ecx, rd32(esp + 4), rd32(esp + 8), rd32(esp + 12), ra.c_str());
}

// Diagnóstico: suma FNV-1a de cada bloque de 64 KB de la memoria del juego (para comparar nativo y web)
void mem_hashes() {
    std::string line;
    for (u32 b = 0; b < MEMSZ; b += 0x10000) {
        u32 h = 2166136261u;
        bool cero = true;
        for (u32 i = 0; i < 0x10000; i += 4) { u32 v; std::memcpy(&v, MEM + b + i, 4); if (v) cero = false; h = (h ^ v) * 16777619u; }
        if (cero) continue;
        char t[24];
        snprintf(t, sizeof t, " %x:%08x", b >> 16, h);
        line += t;
        if (line.size() > 900) { plog("hashes%s", line.c_str()); line.clear(); }
    }
    if (!line.empty()) plog("hashes%s", line.c_str());
}

// Diagnóstico: volcado hexadecimal de un rango de la memoria del juego (líneas de 64 bytes)
void mem_dump(u32 a, u32 n) {
    for (u32 o = 0; o < n && a + o + 64 <= MEMSZ; o += 64) {
        char t[200];
        int k = snprintf(t, sizeof t, "mem %08x ", a + o);
        for (u32 i = 0; i < 64; i++) k += snprintf(t + k, sizeof t - k, "%02x", MEM[a + o + i]);
        plog("%s", t);
    }
}

void dump_ring() {
    logf("  últimas funciones (de la más antigua a la más reciente):\n   ");
    for (u32 i = 0; i < 32; i++) logf(" %x", fnring[(fnring_i - 32 + i) & 255]);
    logf("\n  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            eax, ecx, edx, ebx, esp, ebp, esi, edi);

    // posibles direcciones de retorno en la pila (valor en el EXE precedido de un `call`)
    logf("  pila (retornos probables):");
    int n = 0;
    for (u32 a = esp; a < esp + 0x4000 && a + 4 < MEMSZ && n < 24; a += 4) {
        u32 v = rd32(a);
        if (v < 0x401006 || v >= 0x2800000) continue;
        if (MEM[v - 5] == 0xE8 || MEM[v - 6] == 0xFF || MEM[v - 3] == 0xFF || MEM[v - 2] == 0xFF) { logf(" %x", v); n++; }
    }
    logf("\n");
}

// Detectores de pila (rt.h): solo informan con PCF7_STACKCHECK=1 (hay casos legítimos: __chkstk, colas).
static bool stackcheck() { static bool on = std::getenv("PCF7_STACKCHECK") != nullptr; return on; }

extern "C" void call_mismatch(u32 at, u32 c0) {
    if (!stackcheck()) return;
    static std::set<u32> seen;
    {
        std::lock_guard<std::mutex> lk(g_logmx);
        if (!seen.insert(at).second) return;
    }
    plog("llamada descuadra la pila: call en 0x%x, esp %+d; últimas: %x %x %x %x %x %x", at, (int)(esp - c0),
         fnring[(fnring_i - 6) & 255], fnring[(fnring_i - 5) & 255], fnring[(fnring_i - 4) & 255],
         fnring[(fnring_i - 3) & 255], fnring[(fnring_i - 2) & 255], fnring[(fnring_i - 1) & 255]);
}

extern "C" void call_mismatch_i(u32 at, u32 c0, u32 tg) {
    if (!stackcheck()) return;
    plog_once("cm" + std::to_string(at), "llamada indirecta a 0x%x descuadra la pila: call en 0x%x, esp %+d", tg, at, (int)(esp - c0));
}

extern "C" void esp_mismatch(u32 fn, u32 at, u32 e0) {
    if (!stackcheck()) return;
    static std::set<u32> seen;
    {
        std::lock_guard<std::mutex> lk(g_logmx);
        if (!seen.insert(fn).second) return;
    }
    plog("pila desequilibrada: f_%x ret en 0x%x con esp %+d", fn, at, (int)(esp - e0));
}

void fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf("FATAL: %s (en f_%x)\n", buf, cur_fn);
    dump_ring();
    // PCF7_FATAL_LISTA=dirección:paso:desplazamiento — una lista {puntero, cuenta} del juego: el texto y una palabra de cada elemento
    if (const char *l = std::getenv("PCF7_FATAL_LISTA")) {
        u32 a = 0, paso = 0, off = 0;
        if (std::strncmp(l, "ecx:", 4) == 0) { a = g_rango_ecx; std::sscanf(l + 4, "%x:%x", &paso, &off); }
        else std::sscanf(l, "%x:%x:%x", &a, &paso, &off);
        static bool una = false;
        if (una) std::_Exit(70);
        una = true;
        u32 base = rd32(a), n = rd32(a + 4);
        logf("lista en %x: base %x, %u elementos de 0x%x\n", a, base, n, paso);
        for (u32 i = 0; i < n && i < 400; i++) {
            u32 e = base + i * paso;
            std::string t;
            for (u32 k = 0; k < 40 && e + k < MEMSZ && MEM[e + k] >= 32 && MEM[e + k] < 127; k++) t += (char)MEM[e + k];
            logf("  [%u] %x «%s» +%x=%x\n", i, e, t.c_str(), off, rd32(e + off));
        }
    }
    std::exit(70);
}

extern "C" void js_bad(u32 a) {
    static u32 n = 0;
    if (n++ < 8) { plog("acceso fuera de memoria: 0x%08x (en f_%x)", a, cur_fn); if (n == 1) dump_ring(); }
    if (n > 100000) fatal("demasiados accesos fuera de memoria");
}

extern "C" void js_missing(u32 a) {
    static u32 n = 0;
    if (n++ < 16) {
        plog("llamada indirecta a 0x%08x sin traducir (en f_%x, vuelve a 0x%x; arg0 0x%x -> 0x%x)", a, cur_fn,
             rd32(esp), rd32(esp + 4), rd32(esp + 4) < MEMSZ - 4 ? rd32(rd32(esp + 4)) : 0);
        if (a >= mm::HEAP_LO && a < MEMSZ - 32) {   // código generado en tiempo de ejecución: sus bytes
            std::string b;
            for (u32 i = 0; i < 32; i++) { char t[4]; snprintf(t, sizeof t, "%02x", MEM[a + i]); b += t; }
            plog("  bytes: %s", b.c_str());
        }
    }
    // PCF7_UNRESOLVED=juegos/<juego>/unresolved-<programa>.txt: se anotan para traducirlas en el siguiente build
    static const char *unr = std::getenv("PCF7_UNRESOLVED");
    static std::set<u32> seen;
    if (unr && a >= 0x400000 && seen.insert(a).second)
        if (FILE *f = std::fopen(unr, "a")) { std::fprintf(f, "%x\n", a); std::fclose(f); }
    if (a < 0x1000 || n > 100000) fatal("llamada indirecta a 0x%08x", a);
}

extern "C" void js_dbg(u32 tag, u32 a, u32 b) {
    if (tag == 0xD7F9u) return;   // latido de bucles largos
    if (tag == 0xBAD0u) fatal("valor 0x%x fuera de la tabla de saltos en 0x%x", a, b);
    plog("dbg %x: %x %x (en f_%x)", tag, a, b, cur_fn);
}

extern "C" void js_abort(void) { fatal("abortado"); }

extern "C" void plat_unimplemented(const char *name, u32 nargs) {
    plog_once(name, "API sin implementar: %s (%u args) llamada desde f_%x", name + 4, nargs, cur_fn);
}

// ---------------------------------------------------------------- llamadas host → invitado

u32 guest_call(u32 fn, std::initializer_list<u32> args) {
    u32 saved = esp;
    std::vector<u32> v(args);
    for (size_t i = v.size(); i-- > 0;) { esp -= 4; wr32(esp, v[i]); }
    esp -= 4; wr32(esp, 0xC0DEF00Du);   // dirección de retorno ficticia: el `ret` traducido es un `return` de C
#ifdef PCF_DOS
    u32 saved_ret = g_ret_expected, saved_n = g_sombra_n;
    DOS_CALL_PRE(0xC0DEF00Du);
#endif
    if (!icall_try(fn)) fatal("guest_call: 0x%08x no es una función traducida", fn);
#ifdef PCF_DOS
    g_ret_expected = saved_ret;
    g_sombra_n = saved_n;
    if (g_ret_jump == 0xC0DEF00Du) g_ret_jump = 0;
#endif
    esp = saved;
    return eax;
}

extern "C" void (*HOSTFN[4096])(void);

u32 host_fn(void (*f)(void)) {
    static u32 n = 0;
    for (u32 i = 0; i < n; i++) if (HOSTFN[i] == f) return PCF7_HOSTFN_BASE + 16 * i;
    if (n >= 4096) fatal("sin huecos para funciones del host");
    HOSTFN[n] = f;
    return PCF7_HOSTFN_BASE + 16 * n++;
}

// ---------------------------------------------------------------- cadenas

std::string gstr(u32 p, size_t max) {
    std::string s;
    if (!p) return s;
    for (size_t i = 0; i < max; i++) { u8 c = (u8)rd8(p + (u32)i); if (!c) break; s.push_back((char)c); }
    return s;
}

u32 gstrcpy(u32 dst, const std::string &s, u32 cap) {
    if (!cap) return 0;
    u32 n = (u32)s.size();
    if (n > cap - 1) n = cap - 1;
    for (u32 i = 0; i < n; i++) wr8(dst + i, (u8)s[i]);
    wr8(dst + n, 0);
    return n;
}

// ---------------------------------------------------------------- montículo
//
// Cabecera de 16 B delante de cada bloque: [capacidad][tamaño pedido][marca][-]. Listas libres por
// clase de tamaño (potencias de dos hasta 1 MB); los bloques mayores se reutilizan por primer ajuste.

namespace {
constexpr u32 HDR = 16, MAGIC = 0x48454150u;  // "HEAP"
std::mutex hmx;
u32 htop = mm::HEAP_LO;
std::vector<u32> freelist[21];   // clases 16 B .. 16 MB
std::vector<u32> bigfree;

int cls(u32 cap) { int c = 0; u32 s = 16; while (s < cap && c < 20) { s <<= 1; c++; } return c; }
u32 cls_size(int c) { return 16u << c; }
}  // namespace

u32 heap_alloc(u32 n, bool zero) {
    std::lock_guard<std::mutex> lk(hmx);
    if (!n) n = 1;
    static u32 trace_n = std::getenv("PCF7_HEAPTRACE") ? (u32)std::strtoul(std::getenv("PCF7_HEAPTRACE"), nullptr, 0) : 0;
    if (trace_n == n) {   // diagnóstico: quién pide bloques de este tamaño
        static int k = 0;
        if (k++ < 4) {
            std::string ra;
            int m = 0;
            for (u32 a = esp; a < esp + 0x800 && m < 10; a += 4) {
                u32 v = rd32(a);
                if (v >= 0x401006 && v < 0x2800000 && (MEM[v - 5] == 0xE8 || MEM[v - 6] == 0xFF || MEM[v - 2] == 0xFF || MEM[v - 3] == 0xFF)) { char b[16]; snprintf(b, sizeof b, " %x", v); ra += b; m++; }
            }
            plog("heap_alloc(%u) en f_%x; retornos:%s", n, cur_fn, ra.c_str());
        }
    }
    u32 cap;
    u32 p = 0;
    if (n <= (1u << 20)) {
        int c = cls(n);
        cap = cls_size(c);
        if (!freelist[c].empty()) { p = freelist[c].back(); freelist[c].pop_back(); }
    } else {
        cap = (n + 0xFFFFu) & ~0xFFFFu;
        for (size_t i = 0; i < bigfree.size(); i++) {
            u32 q = bigfree[i];
            if (rd32(q - HDR) >= cap) { p = q; cap = rd32(q - HDR); bigfree.erase(bigfree.begin() + (long)i); break; }
        }
    }
    if (!p) {
        u32 h = (htop + 15u) & ~15u;
        if ((u64)h + HDR + cap > mm::HEAP_HI) {
            plog("montículo agotado (%u B)", n);
            static bool once = false;
            if (!once) {   // qué ocupa el montículo: bloques vivos por tamaño pedido
                once = true;
                std::map<u32, std::pair<u32, u64>> by;
                for (u32 q = (mm::HEAP_LO + 15u) & ~15u; q + HDR < htop;) {
                    u32 c = rd32(q), req = rd32(q + 4);
                    if (rd32(q + 8) == MAGIC) { auto &e = by[req]; e.first++; e.second += c; }
                    if (!c) break;
                    q = (q + HDR + c + 15u) & ~15u;
                }
                std::vector<std::pair<u64, u32>> top;
                for (auto &[req, e] : by) top.push_back({e.second, req});
                std::sort(top.rbegin(), top.rend());
                for (size_t i = 0; i < top.size() && i < 12; i++)
                    plog("  vivos de %u B: %u bloques, %llu KB", top[i].second, by[top[i].second].first, (unsigned long long)(top[i].first >> 10));
            }
            return 0;
        }
        p = h + HDR;
        htop = p + cap;
    }
    wr32(p - HDR, cap); wr32(p - HDR + 4, n); wr32(p - HDR + 8, MAGIC); wr32(p - HDR + 12, 0);
    // En Windows las reservas grandes del montón son páginas nuevas, a cero, y hay código del juego que lo da por
    // hecho (imágenes que solo escriben los píxeles opacos). Un bloque grande reutilizado se entrega limpio: si no,
    // lo que se ve depende del orden de reservas de cada ejecución (en el navegador salía ruido en el menú).
    if (zero || n >= 0x10000) std::memset(MEM + p, 0, cap);
    if (p == memwatch_addr()) plog("memwatch: heap_alloc(%u) -> %x (en f_%x)", n, p, cur_fn);
    return p;
}

static bool valid_block(u32 p) {
    return p >= mm::HEAP_LO + HDR && p < mm::HEAP_HI && rd32(p - HDR + 8) == MAGIC;
}

void heap_free(u32 p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(hmx);
    if (!valid_block(p)) {
        bool freed = p >= mm::HEAP_LO + HDR && p < mm::HEAP_HI && rd32(p - HDR + 8) == 0 && rd32(p - HDR) >= 16;
        plog_once("hfree" + std::to_string(p), "heap_free %s: 0x%x (en f_%x; anillo %x %x %x %x)", freed ? "DOBLE" : "de un puntero ajeno",
                  p, cur_fn, fnring[(fnring_i - 5) & 255], fnring[(fnring_i - 4) & 255], fnring[(fnring_i - 3) & 255], fnring[(fnring_i - 2) & 255]);
        return;
    }
    u32 cap = rd32(p - HDR);
    if (p == memwatch_addr()) plog("memwatch: heap_free(%x) (en f_%x; anillo %x %x %x %x)", p, cur_fn, fnring[(fnring_i - 5) & 255],
                                   fnring[(fnring_i - 4) & 255], fnring[(fnring_i - 3) & 255], fnring[(fnring_i - 2) & 255]);
    wr32(p - HDR + 8, 0);
    if (cap <= (1u << 20)) freelist[cls(cap)].push_back(p);
    else bigfree.push_back(p);
}

u32 heap_size(u32 p) { return valid_block(p) ? rd32(p - HDR + 4) : 0; }

u32 heap_realloc(u32 p, u32 n, bool zero) {
    if (!p) return heap_alloc(n, zero);
    if (!valid_block(p)) return 0;
    u32 cap = rd32(p - HDR), old = rd32(p - HDR + 4);
    if (n <= cap) {
        if (zero && n > old) std::memset(MEM + p + old, 0, n - old);
        wr32(p - HDR + 4, n);
        return p;
    }
    u32 q = heap_alloc(n, zero);
    if (!q) return 0;
    std::memcpy(MEM + q, MEM + p, old);
    heap_free(p);
    return q;
}
