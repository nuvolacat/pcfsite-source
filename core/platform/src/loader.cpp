// PCF7 — cargador: pone la imagen de los módulos en MEM, prepara TEB/PEB/TLS/pila, ejecuta los DllMain
// (MSVCRT, MFC42) y salta al punto de entrada del EXE (WinMainCRTStartup).
#include "display.h"
#include "plat.h"
#include "threads.h"

#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <vector>

struct MODINFO { const char *name; u32 base, size, entry, tls_dir; int is_dll, is_static; int deps[16]; };
extern "C" const MODINFO MODS[];
extern "C" const unsigned NMODS;

namespace {
std::jmp_buf g_exit_jb;
u32 g_exit_code = 0;
}

void process_exit(u32 code) {
    g_exit_code = code;
    std::longjmp(g_exit_jb, 1);
}

u32 module_base(const char *name);   // kernel32.cpp

// DllMain(hinst, DLL_PROCESS_ATTACH, 0) de un módulo traducido, después de los de sus dependencias
static bool g_inited[64];
bool init_module(unsigned i) {
    if (i >= NMODS || g_inited[i]) return true;
    g_inited[i] = true;
    for (int d : MODS[i].deps) {
        if (d < 0) break;
        if (!init_module((unsigned)d)) return false;
    }
    if (!MODS[i].is_dll || !MODS[i].entry) return true;
    u32 r = guest_call(MODS[i].entry, {MODS[i].base, 1, 0});
    plog("DllMain %s -> %u", MODS[i].name, r);
    return r != 0;
}

// LoadLibrary de un módulo traducido: su base (inicializándolo si hace falta) o 0
u32 load_translated(const std::string &lower_name) {
    for (unsigned i = 0; i < NMODS; i++) {
        std::string n = MODS[i].name;
        for (auto &c : n) c = (char)std::tolower((unsigned char)c);
        if (n != lower_name) continue;
        if (!init_module(i)) plog("DllMain de %s ha fallado", MODS[i].name);
        return MODS[i].base;
    }
    return 0;
}

bool imagen_construir(u8 *dst);   // imagen.cpp: la imagen a partir de los ficheros del juego (receta de imagen.c)
u32 imagen_fin();
extern std::string g_imagen_error;

// La imagen inicial: mem.bin si existe (desarrollo: build/<gen>/mem.bin o PCF7_MEMBIN) y, si no, se reconstruye con
// los ficheros del juego (la app). PCF7_MEMCHECK=1: con mem.bin, además se reconstruye aparte y se compara.
static bool load_image(const char *path) {
    FILE *f = path && *path ? std::fopen(path, "rb") : nullptr;
    if (!f) {
        plog("sin %s: la imagen se construye con los ficheros del juego", path && *path ? path : "mem.bin");
        return imagen_construir(MEM);
    }
    size_t n = std::fread(MEM, 1, MEMSZ, f);
    std::fclose(f);
    plog("mem.bin: %zu B", n);
    if (std::getenv("PCF7_MEMCHECK")) {
        std::vector<u8> otra(imagen_fin());
        if (!imagen_construir(otra.data())) plog("MEMCHECK: no se ha podido construir la imagen: %s", g_imagen_error.c_str());
        else {
            size_t dif = 0, primera = 0;
            for (size_t i = 0; i < otra.size(); i++) if (otra[i] != MEM[i]) { if (!dif) primera = i; dif++; }
            if (dif || otra.size() != n) plog("MEMCHECK: la imagen reconstruida DIFIERE de mem.bin: %zu bytes (el primero en 0x%zx), tamaños %zu/%zu", dif, primera, otra.size(), n);
            else plog("MEMCHECK: la imagen reconstruida es idéntica a mem.bin (%zu B)", n);
        }
    }
    return n > 0;
}

// TLS estático de los módulos (IMAGE_TLS_DIRECTORY32): copia cada plantilla a un bloque y lo pone en su slot (0, 1…)
// del array de TLS del TEB (fs:[0x2c]); devuelve cuántos hay.
static u32 setup_static_tls(u32 tls_array) {
    u32 slot = 0;
    for (unsigned i = 0; i < NMODS; i++) {
        u32 d = MODS[i].tls_dir;
        if (!d) continue;
        u32 start = rd32(d), end = rd32(d + 4), pindex = rd32(d + 8), zerofill = rd32(d + 16);
        u32 n = end - start;
        u32 blk = heap_alloc(n + zerofill + 16, true);
        std::memcpy(MEM + blk, MEM + start, n);
        wr32(pindex, slot);
        wr32(tls_array + 4 * slot, blk);
        plog("TLS de %s: %u B + %u a cero, índice %u", MODS[i].name, n, zerofill, slot);
        slot++;
    }
    return slot;
}

void tls_reserve_static(u32 n);   // kernel32.cpp: los índices de TlsAlloc empiezan detrás

// WinExec de un programa que también está traducido en este ejecutable (GSW32.EXE): se arranca en un hilo
bool spawn_translated(const std::string &cmdline) {
    std::string exe = cmdline.substr(0, cmdline.find(' '));
    size_t sl = exe.find_last_of("\\/");
    if (sl != std::string::npos) exe = exe.substr(sl + 1);
    for (auto &c : exe) c = (char)std::tolower((unsigned char)c);
    if (exe.size() < 4 || exe.compare(exe.size() - 4, 4, ".exe")) exe += ".exe";
    for (unsigned i = 1; i < NMODS; i++) {
        std::string n = MODS[i].name;
        for (auto &c : n) c = (char)std::tolower((unsigned char)c);
        if (n != exe || MODS[i].is_dll || !MODS[i].entry) continue;
        static std::vector<std::string> running;   // una sola instancia de cada uno, como hacen ellos
        for (auto &r : running) if (r == n) return true;
        running.push_back(n);
        std::string args = cmdline.find(' ') == std::string::npos ? "" : cmdline.substr(cmdline.find(' '));
        std::string up = n;
        for (auto &c : up) c = (char)std::toupper((unsigned char)c);
        u32 h = thread_spawn_process(MODS[i].entry, MODS[i].base, "C:\\WINDOWS\\SYSTEM\\" + up + args);
        plog("WinExec(\"%s\"): %s en el hilo 0x%x", cmdline.c_str(), MODS[i].name, h);
        return true;
    }
    return false;
}
std::string reg_query_default(u32 root, const std::string &path, bool *found);   // registry.cpp
void reg_set_default(u32 root, const std::string &path, const std::string &value);
extern "C" u32 gp_lookup(u32 namep, u32 modbase);   // gp.c

// Los controles OCX (ProQuinielas) los registraba el instalador con DllRegisterServer, y OLEAUT32/OLEPRO32
// (StdFont, StdPicture, STDOLE2.TLB) venían registradas con Windows: se hace la primera vez que arranca el
// programa (el registro persiste en registro.txt) y se anota para no repetirlo.
static void register_ocx_once() {
    constexpr u32 HKLM = 0x80000002u, HKCR = 0x80000000u;
    // lo que Windows traía ya registrado: StdFont y StdPicture y los proxies de automatización (OLEAUT32 los
    // da por hechos al registrarse). Esta OLEAUT32 (2.20, 1997) ya contiene StdFont/StdPicture.
    static const char *std_keys[][2] = {
        {"CLSID\\{0BE35203-8F91-11CE-9DE3-00AA004BB851}", "Standard Font"},
        {"CLSID\\{0BE35203-8F91-11CE-9DE3-00AA004BB851}\\InprocServer32", "OLEAUT32.DLL"},
        {"CLSID\\{0BE35204-8F91-11CE-9DE3-00AA004BB851}", "Standard Picture"},
        {"CLSID\\{0BE35204-8F91-11CE-9DE3-00AA004BB851}\\InprocServer32", "OLEAUT32.DLL"},
        {"CLSID\\{00020420-0000-0000-C000-000000000046}\\InprocServer32", "OLEAUT32.DLL"},   // PSDispatch
        {"CLSID\\{00020421-0000-0000-C000-000000000046}\\InprocServer32", "OLEAUT32.DLL"},   // PSEnumVARIANT
        {"CLSID\\{00020422-0000-0000-C000-000000000046}\\InprocServer32", "OLEAUT32.DLL"},   // PSTypeInfo
        {"CLSID\\{00020423-0000-0000-C000-000000000046}\\InprocServer32", "OLEAUT32.DLL"},   // PSTypeLib
        {"CLSID\\{00020424-0000-0000-C000-000000000046}\\InprocServer32", "OLEAUT32.DLL"},   // PSOAInterface
        {"CLSID\\{00020425-0000-0000-C000-000000000046}\\InprocServer32", "OLEAUT32.DLL"},   // PSTypeComp
        {"Licenses", ""},
    };
    for (auto &k : std_keys) {
        bool found = false;
        std::string v = reg_query_default(HKCR, k[0], &found);
        if (!found || (std::string(k[1]) == "OLEAUT32.DLL" && v != k[1])) reg_set_default(HKCR, k[0], k[1]);
    }
    for (unsigned i = 0; i < NMODS; i++) {
        std::string n = MODS[i].name;
        for (auto &c : n) c = (char)std::tolower((unsigned char)c);
        bool ocx = n.size() > 4 && !n.compare(n.size() - 4, 4, ".ocx");
        if (!ocx && n != "oleaut32.dll" && n != "olepro32.dll") continue;
        std::string mark = "Software\\PCF7\\OCX registrados\\" + n;
        bool found = false;
        reg_query_default(HKLM, mark, &found);
        if (found) continue;
        u32 base = load_translated(n);
        u32 name = heap_alloc(32);
        gstrcpy(name, "DllRegisterServer", 32);
        u32 fn = base ? gp_lookup(name, base) : 0;
        heap_free(name);
        if (!fn) { plog("%s: sin DllRegisterServer", MODS[i].name); continue; }
        u32 hr = guest_call(fn, {});
        plog("DllRegisterServer de %s -> 0x%x", MODS[i].name, hr);
        if (hr == 0) reg_set_default(HKLM, mark, "1");
    }
}

#ifdef PCF_DOS
int run_game_dos();   // dos.cpp
#endif

int run_game(const char *membin) {
    if (!load_image(membin)) { plog("no hay imagen de memoria: %s", g_imagen_error.empty() ? membin : g_imagen_error.c_str()); return 1; }
#ifdef PCF_DOS
    // MS-DOS: sin TEB ni DLL; el programa termina con int 21h/4Ch (process_exit)
    if (setjmp(g_exit_jb)) { plog("fin del programa de DOS (%u)", g_exit_code); return (int)g_exit_code; }
    return run_game_dos();
#endif

    // TEB en la dirección 0 (fs:[x] = MEM[x])
    wr32(mm::TEB + 0x00, 0xFFFFFFFFu);      // ExceptionList: fin de la cadena SEH
    wr32(mm::TEB + 0x04, mm::STACK_HI);     // StackBase
    wr32(mm::TEB + 0x08, mm::STACK_LO);     // StackLimit
    wr32(mm::TEB + 0x18, mm::TEB);          // Self
    wr32(mm::TEB + 0x20, 0x1000);           // ClientId.UniqueProcess
    wr32(mm::TEB + 0x24, 0x1004);           // ClientId.UniqueThread
    u32 tls_array = mm::TEB + 0xE10;        // TlsSlots (64 entradas) del TEB real
    wr32(mm::TEB + 0x2C, tls_array);        // ThreadLocalStoragePointer
    wr32(mm::TEB + 0x30, mm::PEB);          // ProcessEnvironmentBlock
    wr32(mm::PEB + 0x08, 0x00400000);       // ImageBaseAddress
    // los índices de TlsAlloc van detrás de TODOS los estáticos (el 2001 tiene dos: manager.exe y gfxwin.dll; si
    // TlsAlloc diera el 1, el TLS de gfxwin acabaría encima de los datos por hilo de MSVCRT)
    tls_reserve_static(std::max(1u, setup_static_tls(tls_array)));

    threads_init_main();
    set_display_mode(640, 480, 8);   // escritorio inicial; el juego cambia de modo con DirectDraw
    esp = mm::STACK_HI - 0x100;
    ebp = 0;

    if (setjmp(g_exit_jb)) {
        if (JUEGO_SALIDA_MENU) plog("ExitProcess(%u), apartado pedido al lanzador: %u", g_exit_code, rd32(JUEGO_SALIDA_MENU));
        else plog("ExitProcess(%u)", g_exit_code);
        return (int)g_exit_code;
    }

    // DllMain de las DLL que carga Windows con el EXE (sus importaciones), dependencias primero
    for (unsigned i = 0; i < NMODS; i++)
        if (MODS[i].is_dll && MODS[i].is_static && !init_module(i)) { plog("DllMain de %s ha fallado", MODS[i].name); return 1; }
    register_ocx_once();
    if (const char *rtp = std::getenv("PCF7_RTPATCH")) {   // el parcheador RTPatch (juegos/pcfpatch): solo la DLL, sin el programa
        extern int rtpatch_aplicar(const char *);
        return rtpatch_aplicar(rtp);
    }
    for (unsigned i = 0; i < NMODS; i++) {
        if (MODS[i].is_dll) continue;
        plog("entrada de %s: 0x%x", MODS[i].name, MODS[i].entry);
        guest_call(MODS[i].entry, {});
        plog("el punto de entrada ha vuelto (eax=%u)", eax);
        return (int)eax;
    }
    return 1;
}
