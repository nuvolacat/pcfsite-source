// PCF7 — KERNEL32: proceso, módulos, memoria, TLS, sincronización, tiempo, cadenas/locale, SEH, recursos.
// Los ficheros están en kfile.cpp.
#include "plat.h"
#include "handles.h"
#include "threads.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <strings.h>
#include <map>
#include <ctime>
#include <thread>
#include <vector>

extern "C" u32 gp_lookup(u32 namep, u32 modbase);
extern "C" u32 gp_lookup_sys(const char *dll, u32 namep);

static thread_local u32 g_last_error = 0;   // cada hilo del invitado es un hilo del host
void set_last_error(u32 e) { g_last_error = e; }

// ---------------------------------------------------------------- módulos

namespace {
struct ModName { const char *name; u32 base; const char *path; };
// Los módulos traducidos tienen su base real; los del sistema, una base ficticia (solo sirve como
// identificador para GetProcAddress/GetModuleFileName).
// Los módulos traducidos salen de MODS (modinfo.c): el EXE (su ruta la da target_winpath()) y sus DLL.
ModName MODNAMES[] = {
    {"kernel32.dll", 0x7E000000, "C:\\WINDOWS\\SYSTEM\\KERNEL32.DLL"},
    {"user32.dll", 0x7E100000, "C:\\WINDOWS\\SYSTEM\\USER32.DLL"},
    {"gdi32.dll", 0x7E200000, "C:\\WINDOWS\\SYSTEM\\GDI32.DLL"},
    {"advapi32.dll", 0x7E300000, "C:\\WINDOWS\\SYSTEM\\ADVAPI32.DLL"},
    {"winmm.dll", 0x7E400000, "C:\\WINDOWS\\SYSTEM\\WINMM.DLL"},
    {"ole32.dll", 0x7E500000, "C:\\WINDOWS\\SYSTEM\\OLE32.DLL"},
    {"comdlg32.dll", 0x7E600000, "C:\\WINDOWS\\SYSTEM\\COMDLG32.DLL"},
    {"ddraw.dll", 0x7E700000, "C:\\WINDOWS\\SYSTEM\\DDRAW.DLL"},
    {"dsound.dll", 0x7E800000, "C:\\WINDOWS\\SYSTEM\\DSOUND.DLL"},
    {"oleaut32.dll", 0x7E900000, "C:\\WINDOWS\\SYSTEM\\OLEAUT32.DLL"},   // MFC42 de PCF2000 la carga con LoadLibrary
};

std::string lower_base_name(std::string s) {
    size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(p + 1);
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    if (s.find('.') == std::string::npos) s += ".dll";
    return s;
}

struct MODINFO { const char *name; u32 base, size, entry, tls_dir; int is_dll, is_static; int deps[16]; };
extern "C" const MODINFO MODS[];
extern "C" const unsigned NMODS;
std::vector<ModName> &translated() {
    static std::vector<ModName> v = [] {
        std::vector<ModName> r;
        for (unsigned i = 0; i < NMODS; i++) {
            std::string n = MODS[i].name;
            for (auto &c : n) c = (char)std::tolower((unsigned char)c);
            std::string up = MODS[i].name;
            for (auto &c : up) c = (char)std::toupper((unsigned char)c);
            const char *path = i == 0 ? nullptr : strdup(("C:\\WINDOWS\\SYSTEM\\" + up).c_str());
            r.push_back({strdup(n.c_str()), MODS[i].base, path});
        }
        return r;
    }();
    return v;
}
const ModName *mod_by_name(const std::string &n) {
    std::string l = lower_base_name(n);
    for (auto &m : translated()) if (l == m.name) return &m;
    for (auto &m : MODNAMES) if (l == m.name) return &m;
    return nullptr;
}
const ModName *mod_by_base(u32 b) {
    for (auto &m : translated()) if (m.base == b) return &m;
    for (auto &m : MODNAMES) if (m.base == b) return &m;
    return nullptr;
}
}  // namespace

u32 module_base(const char *name) { auto m = mod_by_name(name); return m ? m->base : 0; }

// Funciones exportadas por "DLL del sistema" implementadas en el host y que el juego solo obtiene con
// GetProcAddress (no las importa): ddraw.dll, dsound.dll. Las registra cada subsistema.
// (se construye al primer uso: lo llaman constructores estáticos de otros ficheros)
static std::vector<std::pair<std::string, u32>> &dynexports() { static std::vector<std::pair<std::string, u32>> v; return v; }
void register_dynamic_export(const char *dll, const char *name, void (*f)(void)) {
    dynexports().emplace_back(std::string(dll) + "!" + name, host_fn(f));
}

STUB(ext_kernel32_dll_GetModuleHandleA) {
    u32 p = arg(0);
    if (!p) RET(thread_exe_base(), 1);
    auto m = mod_by_name(gstr(p));
    if (!m) set_last_error(126);
    RET(m ? m->base : 0, 1);
}
u32 load_translated(const std::string &lower_name);   // loader.cpp

STUB(ext_kernel32_dll_LoadLibraryA) {
    std::string n = gstr(arg(0));
    if (u32 b = load_translated(lower_base_name(n))) { plog("LoadLibraryA(\"%s\") -> 0x%x", n.c_str(), b); RET(b, 1); }
    auto m = mod_by_name(n);
    plog("LoadLibraryA(\"%s\") -> 0x%x", n.c_str(), m ? m->base : 0);
    if (!m) set_last_error(126);
    RET(m ? m->base : 0, 1);
}
STUB(ext_kernel32_dll_FreeLibrary) { RET(1, 1); }
STUB(ext_kernel32_dll_GetProcAddress) {
    u32 h = arg(0), n = arg(1);
    u32 r = 0;
    auto m = mod_by_base(h);
    if (n < 0x10000) {
        // exportaciones dinámicas registradas por ordinal ("#n": oleaut32 de la MFC42 de PCF2000)
        if (m) { std::string key = std::string(m->name) + "!#" + std::to_string(n); for (auto &e : dynexports()) if (e.first == key) r = e.second; }
        if (!r) plog_once("gpa-ord" + std::to_string(n), "GetProcAddress por ordinal %u en 0x%x: no soportado", n, h);
    } else if (h < 0x7E000000u) {
        r = gp_lookup(n, h);
    } else if (m) {
        r = gp_lookup_sys(m->name, n);
        if (!r) {
            std::string key = std::string(m->name) + "!" + gstr(n);
            for (auto &e : dynexports()) if (e.first == key) r = e.second;
        }
    }
    if (!r) plog("GetProcAddress(%s, \"%s\") -> no encontrada", m ? m->name : "?", n >= 0x10000 ? gstr(n).c_str() : "#");
    else if (std::getenv("PCF7_TRACE_GPA")) plog("GetProcAddress(%s, \"%s\") -> 0x%x", m ? m->name : "?", n >= 0x10000 ? gstr(n).c_str() : "#", r);
    RET(r, 2);
}
STUB(ext_kernel32_dll_GetModuleFileNameA) {
    u32 h = arg(0), buf = arg(1), cap = arg(2);
    auto m = mod_by_base(h ? h : thread_exe_base());
    RET(gstrcpy(buf, m && m->path ? m->path : target_winpath(), cap), 3);
}
STUB(ext_kernel32_dll_GetProcessVersion) { RET(0x00040000, 1); }

// ---------------------------------------------------------------- proceso y entorno

STUB(ext_kernel32_dll_GetVersion) { RET(0xC0000A04u, 0); }   // Windows 98 (4.10, bit alto = Win9x)
STUB(ext_kernel32_dll_GetVersionExA) {
    u32 p = arg(0);
    wr32(p + 4, 4); wr32(p + 8, 10); wr32(p + 12, 0x040A07CEu); wr32(p + 16, 1);   // VER_PLATFORM_WIN32_WINDOWS
    gstrcpy(p + 20, " A ", 128);
    RET(1, 1);
}

std::string g_guest_args;
std::string target_winpath() {
    std::string n = TARGET_EXE;
    for (auto &c : n) c = (char)std::toupper((unsigned char)c);
    std::string c = std::string("C:\\") + JUEGO_CARPETA + "\\";
    if (n == "PROQUINI.EXE") return c + "PROQUINI\\PROQUINI.EXE";   // se instalaba en su carpeta
    return c + n;
}

static u32 cmdline_ptr() {
    static u32 p = 0;
    if (!p) {
        std::string c = target_winpath() + (g_guest_args.empty() ? "" : " " + g_guest_args);
        p = heap_alloc((u32)c.size() + 1, true);
        gstrcpy(p, c);
    }
    return p;
}
STUB(ext_kernel32_dll_GetCommandLineA) { RET(thread_cmdline() ? thread_cmdline() : cmdline_ptr(), 0); }
// el entorno de un Windows 98 recién instalado
static const char *const ENTORNO[] = {"PATH=C:\\WINDOWS;C:\\WINDOWS\\COMMAND", "TEMP=C:\\WINDOWS\\TEMP",
                                      "TMP=C:\\WINDOWS\\TEMP", "WINDIR=C:\\WINDOWS", "COMSPEC=C:\\WINDOWS\\COMMAND.COM"};
STUB(ext_kernel32_dll_GetEnvironmentStrings) {
    static u32 p = 0;
    if (!p) {
        p = heap_alloc(512, true);
        u32 q = p;
        for (auto e : ENTORNO) q += gstrcpy(q, e) + 1;
        for (u32 i = 0; JUEGO_ENTORNO[i]; i++) q += gstrcpy(q, JUEGO_ENTORNO[i]) + 1;
        wr8(q, 0);
    }
    RET(p, 0);
}
// GetEnvironmentVariableA(nombre, búfer, tamaño): longitud copiada, o la necesaria (con el 0) si no cabe; 0 si no existe
STUB(ext_kernel32_dll_GetEnvironmentVariableA) {
    std::string n = gstr(arg(0));
    std::vector<const char *> todas(std::begin(ENTORNO), std::end(ENTORNO));
    for (u32 i = 0; JUEGO_ENTORNO[i]; i++) todas.push_back(JUEGO_ENTORNO[i]);
    for (auto e : todas) {
        const char *eq = std::strchr(e, '=');
        if ((size_t)(eq - e) != n.size() || strncasecmp(e, n.c_str(), n.size()) != 0) continue;
        u32 len = (u32)std::strlen(eq + 1);
        if (len + 1 > arg(2)) RET(len + 1, 3);
        gstrcpy(arg(1), eq + 1);
        RET(len, 3);
    }
    set_last_error(203);   // ERROR_ENVVAR_NOT_FOUND
    RET(0, 3);
}
STUB(ext_kernel32_dll_GetEnvironmentStringsW) { RET(0, 0); }   // Win9x: no existe → el CRT usa la A
STUB(ext_kernel32_dll_FreeEnvironmentStringsA) { RET(1, 1); }
STUB(ext_kernel32_dll_FreeEnvironmentStringsW) { RET(1, 1); }
STUB(ext_kernel32_dll_GetCommandLineW) { RET(0, 0); }
STUB(ext_kernel32_dll_SetEnvironmentVariableA) { RET(1, 2); }

STUB(ext_kernel32_dll_GetStartupInfoA) {
    u32 p = arg(0);
    std::memset(MEM + p, 0, 68);
    wr32(p, 68);
    RET(0, 1);
}
STUB(ext_kernel32_dll_GetStdHandle) { u32 n = arg(0); RET(n == 0xFFFFFFF6u ? 0x10u : n == 0xFFFFFFF5u ? 0x14u : 0x18u, 1); }
STUB(ext_kernel32_dll_SetStdHandle) { RET(1, 2); }
STUB(ext_kernel32_dll_SetHandleCount) { RET(arg(0), 1); }
STUB(ext_kernel32_dll_GetCurrentProcess) { RET(0xFFFFFFFFu, 0); }
STUB(ext_kernel32_dll_GetCurrentThread) { RET(0xFFFFFFFEu, 0); }
STUB(ext_kernel32_dll_GetCurrentProcessId) { RET(0x1000, 0); }
STUB(ext_kernel32_dll_ExitProcess) {
    if (thread_is_process()) { plog("ExitProcess(%u) de un programa en un hilo: termina el hilo", arg(0)); thread_exit(arg(0)); }
    process_exit(arg(0));
}
STUB(ext_kernel32_dll_TerminateProcess) { process_exit(arg(1)); }
STUB(ext_kernel32_dll_GetLastError) { RET(g_last_error, 0); }
STUB(ext_kernel32_dll_SetLastError) { g_last_error = arg(0); RET(0, 1); }
STUB(ext_kernel32_dll_SetErrorMode) { RET(0, 1); }
STUB(ext_kernel32_dll_OutputDebugStringA) { plog("[debug] %s", gstr(arg(0)).c_str()); RET(0, 1); }
STUB(ext_kernel32_dll_SetThreadPriority) { RET(1, 2); }
STUB(ext_kernel32_dll_GetSystemInfo) {
    u32 p = arg(0);
    std::memset(MEM + p, 0, 36);
    wr32(p + 4, 0x1000); wr32(p + 8, 0x10000); wr32(p + 12, 0x7FFEFFFF); wr32(p + 16, 1); wr32(p + 20, 1);
    wr32(p + 24, 586); wr32(p + 28, 0x10000); wr16(p + 32, 5);
    RET(0, 1);
}
STUB(ext_kernel32_dll_GlobalMemoryStatus) {
    u32 p = arg(0);
    wr32(p, 32); wr32(p + 4, 20); wr32(p + 8, 128u << 20); wr32(p + 12, 96u << 20);
    wr32(p + 16, 256u << 20); wr32(p + 20, 192u << 20); wr32(p + 24, 0x7FFE0000); wr32(p + 28, 0x60000000);
    RET(0, 1);
}

// ---------------------------------------------------------------- memoria

STUB(ext_kernel32_dll_HeapCreate) { RET(0x00150000u, 3); }
STUB(ext_kernel32_dll_HeapDestroy) { RET(1, 1); }
STUB(ext_kernel32_dll_HeapAlloc) { RET(heap_alloc(arg(2), arg(1) & 8), 3); }               // HEAP_ZERO_MEMORY
STUB(ext_kernel32_dll_HeapReAlloc) {
    u32 flags = arg(1), p = arg(2), n = arg(3);
    if ((flags & 0x10) && n > heap_size(p)) RET(0, 4);                                    // HEAP_REALLOC_IN_PLACE_ONLY
    RET(heap_realloc(p, n, flags & 8), 4);
}
STUB(ext_kernel32_dll_HeapFree) { heap_free(arg(2)); RET(1, 3); }
STUB(ext_kernel32_dll_HeapSize) { RET(heap_size(arg(2)), 3); }
STUB(ext_kernel32_dll_HeapValidate) { RET(1, 3); }
STUB(ext_kernel32_dll_HeapCompact) { RET(0x100000, 2); }
STUB(ext_kernel32_dll_HeapWalk) { RET(0, 2); }

// VirtualAlloc: regiones alineadas a 64 KB (la granularidad de Windows). El sub-montículo de bloques
// pequeños de MSVCRT depende de ello (calcula la página con p & 0xfffff000); con bloques alineados solo
// a 16 B no reconoce sus propios bloques y los libera con HeapFree.
namespace { std::map<u32, u32> g_virt; }   // dirección alineada → bloque del montículo
STUB(ext_kernel32_dll_VirtualAlloc) {
    u32 addr = arg(0), n = arg(1), type = arg(2);
    static const bool traza = std::getenv("PCF7_TRACE_VMEM") != nullptr;
    if (traza) plog("VirtualAlloc(%x, %x, tipo %x, protección %x) desde f_%x", addr, n, type, arg(3), cur_fn);
    if (addr && !(type & 0x2000)) RET(addr, 4);     // MEM_COMMIT sobre una reserva previa: ya es memoria
    u32 sz = (n + 0xFFFu) & ~0xFFFu;
    u32 raw = heap_alloc(sz + 0x10000, true);
    if (!raw) RET(0, 4);
    u32 p = (raw + 0xFFFFu) & ~0xFFFFu;
    g_virt[p] = raw;
    RET(p, 4);
}
STUB(ext_kernel32_dll_VirtualFree) {
    u32 p = arg(0), type = arg(2);
    if (type & 0x8000) { auto it = g_virt.find(p); if (it != g_virt.end()) { heap_free(it->second); g_virt.erase(it); } }   // MEM_RELEASE
    RET(1, 3);
}
STUB(ext_kernel32_dll_VirtualProtect) { if (arg(3)) wr32(arg(3), 0x04); RET(1, 4); }

// Global*/Local*: la "handle" es el propio puntero (como GMEM_FIXED)
STUB(ext_kernel32_dll_GlobalAlloc) { RET(heap_alloc(arg(1), arg(0) & 0x40), 2); }            // GMEM_ZEROINIT
STUB(ext_kernel32_dll_GlobalReAlloc) { RET(heap_realloc(arg(0), arg(1), arg(2) & 0x40), 3); }
STUB(ext_kernel32_dll_GlobalFree) { heap_free(arg(0)); RET(0, 1); }
STUB(ext_kernel32_dll_GlobalLock) { RET(arg(0), 1); }
STUB(ext_kernel32_dll_GlobalUnlock) { RET(1, 1); }
STUB(ext_kernel32_dll_GlobalHandle) { RET(arg(0), 1); }
STUB(ext_kernel32_dll_GlobalSize) { RET(heap_size(arg(0)), 1); }
STUB(ext_kernel32_dll_GlobalFlags) { RET(0, 1); }
STUB(ext_kernel32_dll_LocalAlloc) { RET(heap_alloc(arg(1), arg(0) & 0x40), 2); }
STUB(ext_kernel32_dll_LocalReAlloc) { RET(heap_realloc(arg(0), arg(1), arg(2) & 0x40), 3); }
STUB(ext_kernel32_dll_LocalFree) { heap_free(arg(0)); RET(0, 1); }
STUB(ext_kernel32_dll_LocalLock) { RET(arg(0), 1); }
STUB(ext_kernel32_dll_LocalUnlock) { RET(1, 1); }

STUB(ext_kernel32_dll_IsBadReadPtr) { u32 p = arg(0), n = arg(1); RET(!p || (u64)p + n > MEMSZ, 2); }
STUB(ext_kernel32_dll_IsBadWritePtr) { u32 p = arg(0), n = arg(1); RET(!p || (u64)p + n > MEMSZ, 2); }
STUB(ext_kernel32_dll_IsBadCodePtr) { RET(arg(0) == 0, 1); }
STUB(ext_kernel32_dll_IsBadStringPtrA) { RET(arg(0) == 0, 2); }

// ---------------------------------------------------------------- TLS

namespace {
bool tls_used[64 + 1024];
}
u32 g_tls_estaticos = 1;   // huecos del TEB con bloques de TLS estático (threads.cpp los copia a cada hilo nuevo)
void tls_reserve_static(u32 n) { for (u32 i = 0; i < n; i++) tls_used[i] = true; g_tls_estaticos = n; }

// Los 64 primeros índices viven en el TEB (TlsSlots, fs:[0xE10]) como en Windows: el código del CRT
// puede leerlos directamente.
STUB(ext_kernel32_dll_TlsAlloc) {
    for (u32 i = 0; i < 64; i++) if (!tls_used[i]) { tls_used[i] = true; wr32(mm::TEB + 0xE10 + 4 * i, 0); RET(i, 0); }
    RET(0xFFFFFFFFu, 0);
}
STUB(ext_kernel32_dll_TlsFree) { u32 i = arg(0); if (i < 64) tls_used[i] = false; RET(1, 1); }
STUB(ext_kernel32_dll_TlsGetValue) { u32 i = arg(0); set_last_error(0); RET(i < 64 ? rd32(mm::TEB + 0xE10 + 4 * i) : 0, 1); }
STUB(ext_kernel32_dll_TlsSetValue) { u32 i = arg(0); if (i < 64) wr32(mm::TEB + 0xE10 + 4 * i, arg(1)); RET(i < 64, 2); }

// ---------------------------------------------------------------- sincronización
// Secciones críticas, hilos y esperas que ceden el turno: threads.cpp.

STUB(ext_kernel32_dll_InterlockedIncrement) { u32 p = arg(0); u32 v = rd32(p) + 1; wr32(p, v); RET(v, 1); }
STUB(ext_kernel32_dll_InterlockedDecrement) { u32 p = arg(0); u32 v = rd32(p) - 1; wr32(p, v); RET(v, 1); }

STUB(ext_kernel32_dll_CreateEventA) {
    auto e = std::make_shared<KEvent>();
    e->manual = arg(1); e->signaled = arg(2);
    if (arg(3)) e->name = gstr(arg(3));
    RET(handles().add(e), 4);
}
STUB(ext_kernel32_dll_OpenEventA) { set_last_error(2); RET(0, 3); }   // no hay otra instancia del juego
STUB(ext_kernel32_dll_SetEvent) { if (auto e = handles().get<KEvent>(arg(0))) e->signaled = true; RET(1, 1); }
STUB(ext_kernel32_dll_PulseEvent) { if (auto e = handles().get<KEvent>(arg(0))) e->signaled = false; RET(1, 1); }
STUB(ext_kernel32_dll_CreateMutexA) { RET(handles().add(std::make_shared<KEvent>()), 3); }
STUB(ext_kernel32_dll_ReleaseMutex) { RET(1, 1); }
STUB(ext_kernel32_dll_CreateSemaphoreA) { RET(handles().add(std::make_shared<KEvent>()), 4); }
STUB(ext_kernel32_dll_ReleaseSemaphore) { RET(1, 3); }
STUB(ext_kernel32_dll_WaitForSingleObject) {
    u32 h = arg(0), timeout = arg(1);
    u32 until = ticks_ms() + timeout;
    for (;;) {
        bool ready;
        if (is_thread_handle(h)) ready = thread_done(h, nullptr);
        else if (auto e = handles().get<KEvent>(h)) { ready = e->signaled; if (ready && !e->manual) e->signaled = false; }
        else ready = true;
        if (ready) RET(0, 2);                                                   // WAIT_OBJECT_0
        if (timeout != 0xFFFFFFFFu && (i32)(until - ticks_ms()) <= 0) RET(0x102, 2);   // WAIT_TIMEOUT
        cooperative_sleep(timeout == 0 ? 0 : 1);
    }
}
// (n, HANDLE*, esperar a todos, ms): como WaitForSingleObject sobre varios; devuelve WAIT_OBJECT_0 + índice del
// primero listo (o 0 si se esperaba a todos). El renderer de DirectShow del PCF2001 espera así su evento de
// pintado o el de cambio de estado: con un 0 incondicional creía que el estado había cambiado y no pintaba.
STUB(ext_kernel32_dll_WaitForMultipleObjects) {
    u32 n = arg(0), ph = arg(1), all = arg(2), timeout = arg(3);
    if (!n || n > 64) RET(0xFFFFFFFFu, 4);
    u32 until = ticks_ms() + timeout;
    auto ready = [](u32 h, bool consume) {
        if (is_thread_handle(h)) return thread_done(h, nullptr);
        if (auto e = handles().get<KEvent>(h)) { bool r = e->signaled; if (r && consume && !e->manual) e->signaled = false; return r; }
        return true;
    };
    for (;;) {
        if (all) {
            bool todos = true;
            for (u32 i = 0; i < n && todos; i++) todos = ready(rd32(ph + 4 * i), false);
            if (todos) { for (u32 i = 0; i < n; i++) ready(rd32(ph + 4 * i), true); RET(0, 4); }
        } else {
            for (u32 i = 0; i < n; i++) if (ready(rd32(ph + 4 * i), true)) RET(i, 4);   // WAIT_OBJECT_0 + i
        }
        if (timeout != 0xFFFFFFFFu && (i32)(until - ticks_ms()) <= 0) RET(0x102, 4);   // WAIT_TIMEOUT
        cooperative_sleep(timeout == 0 ? 0 : 1);
    }
}
STUB(ext_kernel32_dll_CloseHandle) { if (std::getenv("PCF7_TRACE_KFILE")) plog("CloseHandle(%x)", arg(0)); handles().close(arg(0)); RET(1, 1); }
STUB(ext_kernel32_dll_DuplicateHandle) { if (arg(3)) wr32(arg(3), arg(1)); RET(1, 7); }

// ---------------------------------------------------------------- tiempo

static const auto g_t0 = std::chrono::steady_clock::now();
// Reloj virtual (diagnóstico: PCF7_RELOJ_VIRTUAL, o ?reloj en la web): el tiempo lo marca el propio juego (una
// milésima cada 5000 entradas de función, más lo que duermen los Sleep) y la fecha es fija. Así dos versiones
// (nativa y web) ven exactamente lo mismo y sus trazas se pueden comparar llamada a llamada.
bool g_reloj_virtual = std::getenv("PCF7_RELOJ_VIRTUAL") != nullptr;
std::atomic<u32> g_virt_extra{0};
u32 ticks_ms() {
    if (g_reloj_virtual) return fnring_i / 5000 + g_virt_extra.load();
    return (u32)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_t0).count();
}

STUB(ext_kernel32_dll_GetTickCount) { RET(ticks_ms(), 0); }
STUB(ext_kernel32_dll_Sleep) { u32 ms = arg(0); cooperative_sleep(ms); RET(0, 1); }

static void put_systemtime(u32 p, const std::tm &t, int ms) {
    wr16(p, (u32)(t.tm_year + 1900)); wr16(p + 2, (u32)(t.tm_mon + 1)); wr16(p + 4, (u32)t.tm_wday);
    wr16(p + 6, (u32)t.tm_mday); wr16(p + 8, (u32)t.tm_hour); wr16(p + 10, (u32)t.tm_min);
    wr16(p + 12, (u32)t.tm_sec); wr16(p + 14, (u32)ms);
}
// PCF7_FECHA=AAAA-MM-DD: el reloj del juego va en ese día (a la hora de ahora); para probar con fechas de la época
static i64 fecha_desfase() {
    static const i64 d = [] {
        const char *f = std::getenv("PCF7_FECHA");
        int a, m, dd;
        if (!f || std::sscanf(f, "%d-%d-%d", &a, &m, &dd) != 3) return (i64)0;
        std::time_t ahora = std::time(nullptr);
        std::tm hoy; gmtime_r(&ahora, &hoy);
        std::tm t{}; t.tm_year = a - 1900; t.tm_mon = m - 1; t.tm_mday = dd;
        std::tm h0{}; h0.tm_year = hoy.tm_year; h0.tm_mon = hoy.tm_mon; h0.tm_mday = hoy.tm_mday;
        return (i64)(timegm(&t) - timegm(&h0));
    }();
    return d;
}
static void now_tm(bool local, std::tm &t, int &ms) {
    if (g_reloj_virtual) {   // 19-oct-2026 12:00:00 más el reloj virtual
        u32 v = ticks_ms();
        std::time_t s = (std::time_t)1792411200 + v / 1000;
        ms = (int)(v % 1000);
        gmtime_r(&s, &t);
        return;
    }
    auto now = std::chrono::system_clock::now();
    std::time_t s = std::chrono::system_clock::to_time_t(now);
    ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    s += fecha_desfase();
    if (local) localtime_r(&s, &t); else gmtime_r(&s, &t);
}
STUB(ext_kernel32_dll_GetLocalTime) { std::tm t; int ms; now_tm(true, t, ms); put_systemtime(arg(0), t, ms); RET(0, 1); }
STUB(ext_kernel32_dll_GetSystemTime) { std::tm t; int ms; now_tm(false, t, ms); put_systemtime(arg(0), t, ms); RET(0, 1); }
STUB(ext_kernel32_dll_SetLocalTime) { RET(0, 1); }
STUB(ext_kernel32_dll_GetTimeZoneInformation) { std::memset(MEM + arg(0), 0, 172); RET(0, 1); }   // TIME_ZONE_ID_UNKNOWN

// FILETIME: centenas de ns desde 1601
constexpr u64 EPOCH_DIFF = 116444736000000000ull;
u64 unix_to_filetime(i64 secs) { return (u64)secs * 10000000ull + EPOCH_DIFF; }
STUB(ext_kernel32_dll_SystemTimeToFileTime) {
    u32 st = arg(0), ft = arg(1);
    std::tm t{};
    t.tm_year = (int)rd16(st) - 1900; t.tm_mon = (int)rd16(st + 2) - 1; t.tm_mday = (int)rd16(st + 6);
    t.tm_hour = (int)rd16(st + 8); t.tm_min = (int)rd16(st + 10); t.tm_sec = (int)rd16(st + 12);
    u64 v = unix_to_filetime(timegm(&t)) + (u64)rd16(st + 14) * 10000ull;
    wr64(ft, v);
    RET(1, 2);
}
STUB(ext_kernel32_dll_GetSystemTimeAsFileTime) {
    std::tm t; int ms; now_tm(false, t, ms);
    wr64(arg(0), unix_to_filetime(timegm(&t)) + (u64)ms * 10000ull);
    RET(0, 1);
}
STUB(ext_kernel32_dll_FileTimeToSystemTime) {
    u64 v = rd64(arg(0));
    std::time_t s = (std::time_t)((v - EPOCH_DIFF) / 10000000ull);
    std::tm t; gmtime_r(&s, &t);
    put_systemtime(arg(1), t, (int)((v / 10000ull) % 1000));
    RET(1, 2);
}
STUB(ext_kernel32_dll_FileTimeToLocalFileTime) { wr64(arg(1), rd64(arg(0))); RET(1, 2); }
STUB(ext_kernel32_dll_LocalFileTimeToFileTime) { wr64(arg(1), rd64(arg(0))); RET(1, 2); }
STUB(ext_kernel32_dll_FileTimeToDosDateTime) {
    u64 v = rd64(arg(0));
    std::time_t s = (std::time_t)((v - EPOCH_DIFF) / 10000000ull);
    std::tm t; gmtime_r(&s, &t);
    if (arg(1)) wr16(arg(1), (u32)(((t.tm_year - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday));
    if (arg(2)) wr16(arg(2), (u32)((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2)));
    RET(1, 3);
}

// ---------------------------------------------------------------- cadenas, páginas de códigos, locale
// Página de códigos del juego: Windows-1252 (español).

static const u16 CP1252_HI[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
static u16 cp_to_u(u8 c) { return c >= 0x80 && c < 0xA0 ? CP1252_HI[c - 0x80] : c; }
static u8 u_to_cp(u16 w) {
    if (w < 0x80 || (w >= 0xA0 && w < 0x100)) return (u8)w;
    for (int i = 0; i < 32; i++) if (CP1252_HI[i] == w) return (u8)(0x80 + i);
    return '?';
}

STUB(ext_kernel32_dll_GetACP) { RET(1252, 0); }
STUB(ext_kernel32_dll_GetOEMCP) { RET(850, 0); }
STUB(ext_kernel32_dll_IsValidCodePage) { u32 c = arg(0); RET(c == 1252 || c == 850 || c == 437, 1); }
STUB(ext_kernel32_dll_GetCPInfo) {
    u32 p = arg(1);
    std::memset(MEM + p, 0, 20);
    wr32(p, 1); wr8(p + 4, '?');
    RET(1, 2);
}
STUB(ext_kernel32_dll_IsDBCSLeadByte) { RET(0, 1); }
STUB(ext_kernel32_dll_MultiByteToWideChar) {
    u32 src = arg(2), dst = arg(4), cap = arg(5);
    i32 n = (i32)arg(3);
    if (n < 0) n = (i32)gstrlen(src) + 1;
    if (!cap) RET((u32)n, 6);
    i32 m = n < (i32)cap ? n : (i32)cap;
    for (i32 i = 0; i < m; i++) wr16(dst + 2 * (u32)i, cp_to_u((u8)rd8(src + (u32)i)));
    RET((u32)m, 6);
}
STUB(ext_kernel32_dll_WideCharToMultiByte) {
    u32 src = arg(2), dst = arg(4), cap = arg(5);
    i32 n = (i32)arg(3);
    if (n < 0) { n = 0; while (rd16(src + 2 * (u32)n)) n++; n++; }
    if (!cap) RET((u32)n, 8);
    i32 m = n < (i32)cap ? n : (i32)cap;
    for (i32 i = 0; i < m; i++) wr8(dst + (u32)i, u_to_cp((u16)rd16(src + 2 * (u32)i)));
    RET((u32)m, 8);
}

static u8 cp_upper(u8 c) {
    if (c >= 'a' && c <= 'z') return (u8)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (u8)(c - 32);
    if (c == 0x9A || c == 0x9C || c == 0x9E) return (u8)(c - 16);
    if (c == 0xFF) return 0x9F;
    return c;
}
static u8 cp_lower(u8 c) {
    if (c >= 'A' && c <= 'Z') return (u8)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (u8)(c + 32);
    if (c == 0x8A || c == 0x8C || c == 0x8E) return (u8)(c + 16);
    if (c == 0x9F) return 0xFF;
    return c;
}
u8 cp1252_upper(u8 c) { return cp_upper(c); }
u8 cp1252_lower(u8 c) { return cp_lower(c); }

// tipo de carácter (CT_CTYPE1) en Windows-1252
static u16 ctype1(u16 w) {
    u8 c = w < 0x100 ? (u8)w : u_to_cp(w);
    u16 r = 0;
    if (cp_upper(c) == c && cp_lower(c) != c) r |= 0x001 | 0x100;           // C1_UPPER | C1_ALPHA
    if (cp_lower(c) == c && cp_upper(c) != c) r |= 0x002 | 0x100;           // C1_LOWER | C1_ALPHA
    if (c == 0xAA || c == 0xBA || c == 0xDF) r |= 0x002 | 0x100;
    if (c >= '0' && c <= '9') r |= 0x004;                                    // C1_DIGIT
    if (c == ' ' || (c >= 9 && c <= 13) || c == 0xA0) r |= 0x008;            // C1_SPACE
    if ((c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
        (c >= 0x7B && c <= 0x7E) || (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7) r |= 0x010;  // C1_PUNCT
    if (c < 0x20 || c == 0x7F) r |= 0x020;                                   // C1_CNTRL
    if (c == ' ' || c == '\t') r |= 0x040;                                   // C1_BLANK
    if ((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')) r |= 0x080;   // C1_XDIGIT
    return r;
}
STUB(ext_kernel32_dll_GetStringTypeA) {
    u32 type = arg(1), src = arg(2), dst = arg(4);
    i32 n = (i32)arg(3);
    if (n < 0) n = (i32)gstrlen(src) + 1;
    for (i32 i = 0; i < n; i++) wr16(dst + 2 * (u32)i, type == 1 ? ctype1((u8)rd8(src + (u32)i)) : 0);
    RET(1, 5);
}
STUB(ext_kernel32_dll_GetStringTypeExA) {
    u32 type = arg(1), src = arg(2), dst = arg(4);
    i32 n = (i32)arg(3);
    if (n < 0) n = (i32)gstrlen(src) + 1;
    for (i32 i = 0; i < n; i++) wr16(dst + 2 * (u32)i, type == 1 ? ctype1((u8)rd8(src + (u32)i)) : 0);
    RET(1, 5);
}
STUB(ext_kernel32_dll_GetStringTypeW) {
    u32 type = arg(0), src = arg(1), dst = arg(3);
    i32 n = (i32)arg(2);
    if (n < 0) { n = 0; while (rd16(src + 2 * (u32)n)) n++; n++; }
    for (i32 i = 0; i < n; i++) wr16(dst + 2 * (u32)i, type == 1 ? ctype1((u16)rd16(src + 2 * (u32)i)) : 0);
    RET(1, 4);
}
STUB(ext_kernel32_dll_LCMapStringA) {
    u32 flags = arg(1), src = arg(2), dst = arg(4), cap = arg(5);
    i32 n = (i32)arg(3);
    if (n < 0) n = (i32)gstrlen(src) + 1;
    if (!cap) RET((u32)n, 6);
    i32 m = n < (i32)cap ? n : (i32)cap;
    for (i32 i = 0; i < m; i++) {
        u8 c = (u8)rd8(src + (u32)i);
        if (flags & 0x200) c = cp_upper(c); else if (flags & 0x100) c = cp_lower(c);   // LCMAP_UPPERCASE / LOWERCASE
        wr8(dst + (u32)i, c);
    }
    RET((u32)m, 6);
}
STUB(ext_kernel32_dll_LCMapStringW) { RET(0, 6); }   // Win9x: no implementada (el CRT usa la A)
STUB(ext_kernel32_dll_CompareStringA) {
    u32 flags = arg(1), a = arg(2), b = arg(4);
    i32 na = (i32)arg(3), nb = (i32)arg(5);
    if (na < 0) na = (i32)gstrlen(a);
    if (nb < 0) nb = (i32)gstrlen(b);
    for (i32 i = 0;; i++) {
        if (i == na || i == nb) RET(na == nb ? 2 : na < nb ? 1 : 3, 6);
        u8 x = (u8)rd8(a + (u32)i), y = (u8)rd8(b + (u32)i);
        if (flags & 1) { x = cp_lower(x); y = cp_lower(y); }   // NORM_IGNORECASE
        if (x != y) RET(x < y ? 1 : 3, 6);
    }
}
STUB(ext_kernel32_dll_CompareStringW) { RET(0, 6); }
STUB(ext_kernel32_dll_GetUserDefaultLCID) { RET(0x0C0A, 0); }   // español (alfabetización moderna)
STUB(ext_kernel32_dll_GetThreadLocale) { RET(0x0C0A, 0); }
STUB(ext_kernel32_dll_IsValidLocale) { RET(1, 2); }
STUB(ext_kernel32_dll_GetLocaleInfoA) {
    u32 type = arg(1) & 0xFFFF, buf = arg(2), cap = arg(3);
    const char *v = "";
    switch (type) {
        case 0x0E: v = ","; break;          // LOCALE_SDECIMAL
        case 0x0F: v = "."; break;          // LOCALE_STHOUSAND
        case 0x14: v = "Pts"; break;        // LOCALE_SCURRENCY
        case 0x1004: v = "1252"; break;     // LOCALE_IDEFAULTANSICODEPAGE
        case 0x0B: v = "850"; break;        // LOCALE_IDEFAULTCODEPAGE
        case 0x03: v = "ESN"; break;        // LOCALE_SABBREVLANGNAME
        case 0x07: v = "ESP"; break;        // LOCALE_SABBREVCTRYNAME
        case 0x01: v = "0c0a"; break;       // LOCALE_ILANGUAGE
        default: plog_once("locinfo" + std::to_string(type), "GetLocaleInfoA(0x%x) sin valor", type);
    }
    u32 n = (u32)std::strlen(v) + 1;
    if (!cap) RET(n, 4);
    gstrcpy(buf, v, cap);
    RET(n <= cap ? n : cap, 4);
}
STUB(ext_kernel32_dll_EnumSystemLocalesA) { RET(1, 2); }

STUB(ext_kernel32_dll_lstrlenA) { RET(arg(0) ? gstrlen(arg(0)) : 0, 1); }
STUB(ext_kernel32_dll_lstrcpyA) { gstrcpy(arg(0), gstr(arg(1), 1u << 20)); RET(arg(0), 2); }
STUB(ext_kernel32_dll_lstrcpynA) { gstrcpy(arg(0), gstr(arg(1), 1u << 20), arg(2)); RET(arg(0), 3); }
STUB(ext_kernel32_dll_lstrcatA) { u32 d = arg(0); gstrcpy(d + gstrlen(d), gstr(arg(1), 1u << 20)); RET(d, 2); }
STUB(ext_kernel32_dll_lstrcmpA) { int r = std::strcmp(gstr(arg(0)).c_str(), gstr(arg(1)).c_str()); RET(r < 0 ? -1 : r > 0, 2); }
STUB(ext_kernel32_dll_lstrcmpiA) {
    std::string a = gstr(arg(0)), b = gstr(arg(1));
    for (auto &c : a) c = (char)cp_lower((u8)c);
    for (auto &c : b) c = (char)cp_lower((u8)c);
    int r = std::strcmp(a.c_str(), b.c_str());
    RET(r < 0 ? -1 : r > 0, 2);
}
STUB(ext_kernel32_dll_lstrlenW) { u32 p = arg(0), n = 0; if (p) while (rd16(p + 2 * n)) n++; RET(n, 1); }
STUB(ext_kernel32_dll_MulDiv) {
    i64 r = (i64)(i32)arg(0) * (i32)arg(1);
    i32 d = (i32)arg(2);
    if (!d) RET(0xFFFFFFFFu, 3);
    i64 q = (r >= 0) == (d > 0) ? (r + d / 2) / d : (r - d / 2) / d;   // redondeo como Windows
    RET((u32)(i32)q, 3);
}
STUB(ext_kernel32_dll_FormatMessageA) {
    u32 buf = arg(4), cap = arg(5);
    std::string s = "Error " + std::to_string(arg(2));
    if (arg(0) & 0x100) {                         // FORMAT_MESSAGE_ALLOCATE_BUFFER
        u32 p = heap_alloc((u32)s.size() + 1);
        gstrcpy(p, s);
        wr32(buf, p);
    } else {
        gstrcpy(buf, s, cap);
    }
    RET((u32)s.size(), 7);
}

// Átomos globales (los usa MFC para nombres de propiedades de ventana)
namespace {
std::vector<std::string> g_atoms;
}
STUB(ext_kernel32_dll_GlobalAddAtomA) {
    u32 p = arg(0);
    if (p < 0x10000) RET(p, 1);
    std::string s = gstr(p);
    for (size_t i = 0; i < g_atoms.size(); i++) if (g_atoms[i] == s) RET(0xC000 + i, 1);
    g_atoms.push_back(s);
    RET(0xC000 + g_atoms.size() - 1, 1);
}
STUB(ext_kernel32_dll_GlobalDeleteAtom) { RET(0, 1); }
STUB(ext_kernel32_dll_GlobalGetAtomNameA) {
    u32 a = arg(0) - 0xC000;
    if (a >= g_atoms.size()) RET(0, 3);
    RET(gstrcpy(arg(1), g_atoms[a], arg(2)), 3);
}

// ---------------------------------------------------------------- SEH (excepciones estructuradas)
//
// La cadena de registros está en fs:[0] = MEM[0]: cada registro es {siguiente, manejador}. Para lanzar
// una excepción se llama a cada manejador como en Windows:
//   manejador(EXCEPTION_RECORD*, registro, CONTEXT*, DispatcherContext*) → disposición
// 0 = ExceptionContinueExecution, 1 = ExceptionContinueSearch.

static u32 make_context() {
    u32 ctx = heap_alloc(0x2CC, true);
    wr32(ctx, 0x10007);   // CONTEXT_FULL
    wr32(ctx + 0x9C, edi); wr32(ctx + 0xA0, esi); wr32(ctx + 0xA4, ebx); wr32(ctx + 0xA8, edx);
    wr32(ctx + 0xAC, ecx); wr32(ctx + 0xB0, eax); wr32(ctx + 0xB4, ebp); wr32(ctx + 0xC4, esp);
    return ctx;
}

// Como RtlpExecuteHandler2 de Windows, cada manejador se llama con un registro del despachador encima de
// la cadena: {siguiente, manejador, marco que se está atendiendo}. Si durante el manejador se lanza otra
// excepción (p. ej. un `throw;` dentro de un `catch` de C++), al llegar a este registro se notifica
// ExceptionNestedException; si se desenrolla durante un desenrollado, ExceptionCollidedUnwind. El runtime
// de C++ (_UnwindNestedFrames) cuenta con encontrar este registro en fs:[0] y lo vuelve a enlazar.
constexpr u32 EH_UNWINDING = 2, EH_EXIT_UNWIND = 4, EH_NESTED_CALL = 0x10;

static void seh_exception_guard() {   // RtlpExceptionHandler
    u32 rec = arg(0), frame = arg(1), dctx = arg(3);
    if (rd32(rec + 4) & (EH_UNWINDING | EH_EXIT_UNWIND)) RET(1, 4);
    if (dctx) wr32(dctx, rd32(frame + 8));
    RET(2, 4);
}
static void seh_unwind_guard() {      // RtlpUnwindHandler
    u32 rec = arg(0), frame = arg(1), dctx = arg(3);
    if (!(rd32(rec + 4) & (EH_UNWINDING | EH_EXIT_UNWIND))) RET(1, 4);
    if (dctx) wr32(dctx, rd32(frame + 8));
    RET(3, 4);
}

// llama a un manejador con el registro del despachador puesto; *dctx = DispatcherContext al volver
static u32 seh_call(u32 handler, u32 rec, u32 frame, u32 ctx, void (*guard)(), u32 &dctx_out) {
    u32 saved_esp = esp, saved_top = rd32(mm::TEB);
    esp -= 16;
    u32 reg = esp;
    wr32(reg, saved_top); wr32(reg + 4, host_fn(guard)); wr32(reg + 8, frame); wr32(reg + 12, 0);
    wr32(mm::TEB, reg);
    u32 disp = guest_call(handler, {rec, frame, ctx, reg + 12});
    dctx_out = rd32(reg + 12);
    wr32(mm::TEB, rd32(reg));   // como Windows: desapila su registro (_UnwindNestedFrames pudo reenlazarlo)
    esp = saved_esp;
    return disp;
}

STUB(ext_kernel32_dll_RaiseException) {
    u32 code = arg(0), flags = arg(1), nargs = arg(2), pargs = arg(3);
    u32 rec = heap_alloc(0x50, true);
    wr32(rec, code); wr32(rec + 4, flags & 1); wr32(rec + 12, rd32(esp));   // ExceptionAddress = retorno
    if (nargs > 15) nargs = 15;
    wr32(rec + 16, nargs);
    for (u32 i = 0; i < nargs; i++) wr32(rec + 20 + 4 * i, rd32(pargs + 4 * i));
    u32 ctx = make_context();
    plog("RaiseException(0x%08x, %u args) desde f_%x [%x %x %x] obj %x", code, nargs, cur_fn, rd32(rec + 20), rd32(rec + 24), rd32(rec + 28), rd32(rec + 24) ? rd32(rd32(rec + 24)) : 0);
    static bool tr = std::getenv("PCF7_TRACE_SEH") != nullptr;
    u32 frame = rd32(mm::TEB), nested = 0;
    for (int depth = 0; frame && frame != 0xFFFFFFFFu && depth < 1000; depth++) {
        u32 handler = rd32(frame + 4);
        if (handler < 0x400000 || handler >= PCF7_HEAP_LO) {   // marco pisado o colgante (PCF2000): se avisa y se salta
            plog("SEH: marco 0x%x con manejador 0x%x fuera del código (anterior 0x%x): se salta", frame, handler, rd32(frame));
            frame = rd32(frame);
            continue;
        }
        if (frame == nested) { wr32(rec + 4, rd32(rec + 4) & ~EH_NESTED_CALL); nested = 0; }
        if (tr) { u32 st = rd32(frame + 8), tl = rd32(frame + 12);
            plog("  SEH: marco 0x%x manejador 0x%x (estado %d, nivel %d)", frame, handler, (int)st, (int)tl);
            if (st > 0x400000 && st < 0x10000000 && tl < 16)
                plog("        ámbito %d: %x %x %x", tl, rd32(st + 12 * tl), rd32(st + 12 * tl + 4), rd32(st + 12 * tl + 8)); }
        u32 dctx = 0;
        u32 disp = seh_call(handler, rec, frame, ctx, seh_exception_guard, dctx);
        if (tr) plog("  SEH: -> %u", disp);
        if (disp == 0) { heap_free(ctx); heap_free(rec); RET(0, 4); }   // continuar donde se lanzó
        if (disp == 2) {                                                  // excepción anidada
            wr32(rec + 4, rd32(rec + 4) | EH_NESTED_CALL);
            if (dctx > nested) nested = dctx;
        }
        frame = rd32(frame);
    }
    fatal("excepción 0x%08x no capturada", code);
}

STUB(ext_kernel32_dll_RtlUnwind) {
    u32 target = arg(0), rec = arg(2), retval = arg(3);
    bool own = !rec;
    if (own) { rec = heap_alloc(0x50, true); wr32(rec, 0xC0000027u); }   // STATUS_UNWIND
    wr32(rec + 4, rd32(rec + 4) | EH_UNWINDING);
    if (!target) wr32(rec + 4, rd32(rec + 4) | EH_EXIT_UNWIND);
    u32 ctx = make_context();
    u32 frame = rd32(mm::TEB);
    while (frame && frame != 0xFFFFFFFFu && frame != target) {
        u32 handler = rd32(frame + 4);
        if (handler < 0x400000 || handler >= PCF7_HEAP_LO) {   // ídem al desenrollar
            plog("RtlUnwind: marco 0x%x con manejador 0x%x fuera del código: se salta", frame, handler);
            u32 next = rd32(frame);
            frame = next;
            wr32(mm::TEB, frame);
            continue;
        }
        u32 dctx = 0;
        u32 disp = seh_call(handler, rec, frame, ctx, seh_unwind_guard, dctx);
        if (disp == 3 && dctx) frame = dctx;   // otro desenrollado en curso: seguir tras su marco
        u32 next = rd32(frame);
        frame = next;
        wr32(mm::TEB, frame);
    }
    heap_free(ctx);
    if (own) heap_free(rec);
    RET(retval, 4);
}

STUB(ext_kernel32_dll_SetUnhandledExceptionFilter) { RET(0, 1); }
STUB(ext_kernel32_dll_UnhandledExceptionFilter) {
    u32 ep = arg(0);
    u32 rec = ep ? rd32(ep) : 0;
    fatal("excepción no manejada 0x%08x en 0x%08x", rec ? rd32(rec) : 0, rec ? rd32(rec + 12) : 0);
}

// ---------------------------------------------------------------- recursos (directorio .rsrc de la imagen)

static u32 rsrc_root(u32 base) {
    u32 pe = base + rd32(base + 0x3C);
    u32 rva = rd32(pe + 24 + 96 + 2 * 8);
    return rva ? base + rva : 0;
}

// Busca en un nivel del directorio una entrada por id (< 0x10000) o por nombre (cadena del invitado,
// "#123" equivale al id 123). id == 0xFFFFFFFF = la primera.
static u32 rsrc_find(u32 root, u32 dir, u32 key) {
    u32 nnamed = rd16(dir + 12), nid = rd16(dir + 14);
    std::string skey;
    if (key >= 0x10000 && key != 0xFFFFFFFFu) {
        skey = gstr(key);
        if (!skey.empty() && skey[0] == '#') { key = (u32)std::strtoul(skey.c_str() + 1, nullptr, 10); skey.clear(); }
        for (auto &c : skey) c = (char)std::toupper((unsigned char)c);
    }
    for (u32 i = 0; i < nnamed + nid; i++) {
        u32 e = dir + 16 + 8 * i;
        u32 name = rd32(e), off = rd32(e + 4);
        bool match;
        if (key == 0xFFFFFFFFu) match = true;
        else if (name & 0x80000000u) {
            if (skey.empty()) continue;
            u32 s = root + (name & 0x7FFFFFFFu);
            u32 len = rd16(s);
            std::string n;
            for (u32 k = 0; k < len; k++) n.push_back((char)std::toupper((int)rd16(s + 2 + 2 * k)));
            match = n == skey;
        } else {
            match = skey.empty() && name == key;
        }
        if (match) return off;
    }
    return 0xFFFFFFFFu;
}

u32 find_resource(u32 module, u32 name, u32 type) {
    if (!module) module = 0x00400000;
    if (module >= 0x7E000000u) return 0;
    u32 root = rsrc_root(module);
    if (!root) return 0;
    u32 o = rsrc_find(root, root, type);
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return 0;
    o = rsrc_find(root, root + (o & 0x7FFFFFFFu), name);
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return 0;
    o = rsrc_find(root, root + (o & 0x7FFFFFFFu), 0xFFFFFFFFu);
    if (o == 0xFFFFFFFFu || (o & 0x80000000u)) return 0;
    return root + o;   // IMAGE_RESOURCE_DATA_ENTRY
}
u32 resource_module(u32 hrsrc) {   // el módulo que contiene el recurso
    for (unsigned i = 0; i < NMODS; i++)
        if (hrsrc >= MODS[i].base && hrsrc < MODS[i].base + MODS[i].size) return MODS[i].base;
    return 0x00400000u;
}

STUB(ext_kernel32_dll_FindResourceA) {
    u32 r = find_resource(arg(0), arg(1), arg(2));
    if (!r) set_last_error(1814);   // ERROR_RESOURCE_NAME_NOT_FOUND
    RET(r, 3);
}
STUB(ext_kernel32_dll_LoadResource) { u32 h = arg(1); RET(h ? resource_module(h) + rd32(h) : 0, 2); }
STUB(ext_kernel32_dll_LockResource) { RET(arg(0), 1); }
STUB(ext_kernel32_dll_SizeofResource) { u32 h = arg(1); RET(h ? rd32(h + 4) : 0, 2); }

u8 unicode_to_cp1252(u32 cp) { return cp > 0xFFFF ? (u8)'?' : u_to_cp((u16)cp); }
