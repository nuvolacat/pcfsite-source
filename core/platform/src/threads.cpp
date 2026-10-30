// PCF7 — hilos del juego (CreateThread) con multitarea COOPERATIVA.
//
// El código traducido usa registros globales (rt.h): dos hilos del invitado no pueden ejecutarse a la
// vez. Cada hilo del invitado es un hilo del host, pero solo uno tiene "el testigo" y ejecuta código del
// juego; el testigo se pasa en las llamadas que esperan (Sleep, WaitFor*, EnterCriticalSection ocupada,
// GetMessage/PeekMessage sin mensajes). Al cambiar de hilo se guarda y restaura la CPU completa y el TEB
// (que vive en la dirección 0 y lleva la cadena SEH y los huecos de TLS de cada hilo).
#include "handles.h"
#include "threads.h"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <vector>

u32 ticks_ms();
extern u32 g_tls_estaticos;   // kernel32.cpp: huecos de TLS estático

namespace {

constexpr u32 TEB_BYTES = 0x1000;

struct Cpu {
    u32 r[15];     // eax ecx edx ebx esp ebp esi edi CF ZF SF OF PF t DF
    double st[8];
    u32 top, fpucw, fpusw, cur;
    u64 mm[8];
};

struct GThread {
    u32 id = 0, handle = 0;
    u32 start = 0, param = 0;
    u32 stack_lo = 0, stack_hi = 0;
    bool done = false, can_run = false;
    u32 exit_code = 0x103;           // STILL_ACTIVE
    u32 exe_base = 0, cmdline = 0;   // ≠ 0: el hilo hace de otro proceso (ver thread_spawn_process)
    u32 wake = 0;                    // ≠ 0: duerme (Sleep) hasta ese ticks_ms(); mientras, no se le pasa el testigo
    Cpu cpu{};
    std::vector<u8> teb;
    std::condition_variable cv;
};

std::mutex g_mx;                      // protege la lista y el testigo
std::vector<GThread *> g_threads;
GThread *g_cur = nullptr;
u32 g_next_id = 0x1004;

void save_cpu(Cpu &c) {
    u32 *regs[15] = {&eax, &ecx, &edx, &ebx, &esp, &ebp, &esi, &edi, &CF, &ZF, &SF, &OF, &PF, &t, &DF};
    for (int i = 0; i < 15; i++) c.r[i] = *regs[i];
    std::memcpy(c.st, ST, sizeof ST);
    std::memcpy(c.mm, MM, sizeof MM);
    c.top = TOP; c.fpucw = FPUCW; c.fpusw = FPUSW; c.cur = cur_fn;
}
void load_cpu(const Cpu &c) {
    u32 *regs[15] = {&eax, &ecx, &edx, &ebx, &esp, &ebp, &esi, &edi, &CF, &ZF, &SF, &OF, &PF, &t, &DF};
    for (int i = 0; i < 15; i++) *regs[i] = c.r[i];
    std::memcpy(ST, c.st, sizeof ST);
    std::memcpy(MM, c.mm, sizeof MM);
    TOP = c.top; FPUCW = c.fpucw; FPUSW = c.fpusw; cur_fn = c.cur;
}

// Pasa el testigo al siguiente hilo vivo que no esté dormido (en orden circular) y espera a recuperarlo. Si todos
// los demás duermen, sigue este (si no duerme o despierta antes) o el que despierte antes. Con g_mx tomado.
void switch_from(GThread *me, std::unique_lock<std::mutex> &lk) {
    size_t n = g_threads.size(), i = 0;
    while (i < n && g_threads[i] != me) i++;
    u32 now = ticks_ms();
    GThread *next = nullptr, *pronto = nullptr;
    for (size_t k = 1; k <= n; k++) {
        GThread *c = g_threads[(i + k) % n];
        if (c->done || c == me) continue;
        if (!c->wake || (i32)(c->wake - now) <= 0) { next = c; break; }
        if (!pronto || (i32)(c->wake - pronto->wake) < 0) pronto = c;
    }
    if (!next) {
        if (!me->done && (!me->wake || (pronto && (i32)(me->wake - pronto->wake) <= 0))) return;
        next = pronto;
    }
    if (!next) return;
    save_cpu(me->cpu);
    me->teb.assign(MEM, MEM + TEB_BYTES);
    me->can_run = false;
    next->can_run = true;
    g_cur = next;
    next->cv.notify_one();
    if (me->done) return;              // el hilo termina: no espera
    me->cv.wait(lk, [&] { return me->can_run; });
    std::memcpy(MEM, me->teb.data(), TEB_BYTES);
    load_cpu(me->cpu);
}

void *thread_main(void *p) {
    GThread *t = (GThread *)p;
    {
        std::unique_lock<std::mutex> lk(g_mx);
        t->cv.wait(lk, [&] { return t->can_run; });
        std::memcpy(MEM, t->teb.data(), TEB_BYTES);
    }
    esp = t->stack_hi - 16;
    ebp = 0;
    DF = 0;
    FPUCW = 0x027F;
    TOP = 0;
    u32 r = guest_call(t->start, {t->param});
    thread_exit(r);
    return nullptr;
}

}  // namespace

void threads_init_main() {
    std::lock_guard<std::mutex> lk(g_mx);
    GThread *m = new GThread;
    m->id = 0x1004;
    m->handle = 0;
    m->can_run = true;
    g_threads.push_back(m);
    g_cur = m;
}

// diagnóstico (sin sincronizar): dónde está cada hilo que no tiene el testigo
void threads_dump() {
    for (auto *t : g_threads) {
        if (t->done) continue;
        if (t == g_cur) { plog("  hilo 0x%x: tiene el testigo (pila %08x-%08x)", t->id, t->stack_lo, t->stack_hi); continue; }
        u32 sp = t->cpu.r[4];
        std::string ra;
        int m = 0;
        for (u32 a = sp; a && a < sp + 0x1000 && m < 12; a += 4) {
            u32 v = rd32(a);
            if (v >= 0x401006 && v < 0x800000 && (MEM[v - 5] == 0xE8 || MEM[v - 6] == 0xFF || MEM[v - 2] == 0xFF)) { char b[16]; snprintf(b, sizeof b, " %x", v); ra += b; m++; }
        }
        plog("  hilo 0x%x: en f_%x, esp=%08x, retornos:%s", t->id, t->cpu.cur, sp, ra.c_str());
    }
}

u32 current_thread_id() { return g_cur ? g_cur->id : 0x1004; }
u32 thread_exe_base() { return g_cur && g_cur->exe_base ? g_cur->exe_base : 0x00400000u; }
u32 thread_cmdline() { return g_cur ? g_cur->cmdline : 0; }
bool thread_is_process() { return g_cur && g_cur->exe_base; }
bool other_threads() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto *t : g_threads) if (t != g_cur && !t->done) return true;
    return false;
}

void thread_yield() {
    std::unique_lock<std::mutex> lk(g_mx);
    if (g_cur) switch_from(g_cur, lk);
}

extern bool g_reloj_virtual;   // kernel32.cpp
extern std::atomic<u32> g_virt_extra;

// Sleep(ms): el hilo queda dormido y el testigo pasa a los que pueden correr (antes se lo quedaba esperando 0,5 ms
// en cada vuelta: con varios hilos dormidos, el que trabajaba casi no avanzaba; grabar una liga tardaba 7 minutos).
// Si recupera el testigo antes de tiempo es que todos duermen: entonces se espera de verdad, a ratos cortos.
void cooperative_sleep(u32 ms) {
    if (g_reloj_virtual) { g_virt_extra += ms; thread_yield(); return; }   // el tiempo salta: ver kernel32.cpp
    if (!ms) { thread_yield(); return; }
    u32 until = ticks_ms() + ms;
    { std::lock_guard<std::mutex> lk(g_mx); if (g_cur) g_cur->wake = until ? until : 1; }
    for (;;) {
        thread_yield();
        i32 left = (i32)(until - ticks_ms());
        if (left <= 0) break;
        std::this_thread::sleep_for(std::chrono::microseconds(std::min(left, 2) * 500));
    }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_cur) g_cur->wake = 0;
}

void thread_exit(u32 code) {
    std::unique_lock<std::mutex> lk(g_mx);
    GThread *me = g_cur;
    me->done = true;
    me->exit_code = code;
    plog("hilo 0x%x terminado (%u)", me->id, code);
    switch_from(me, lk);
    lk.unlock();
    plat_thread_exit();
}

bool thread_done(u32 handle, u32 *exit_code) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto *t : g_threads)
        if (t->handle == handle) { if (exit_code) *exit_code = t->exit_code; return t->done; }
    return true;
}
bool is_thread_handle(u32 handle) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto *t : g_threads) if (t->handle && t->handle == handle) return true;
    return false;
}

// ---------------------------------------------------------------- KERNEL32

struct KThread : KObj { u32 id; };

STUB(ext_kernel32_dll_CreateThread) {
    u32 stack = arg(1), start = arg(2), param = arg(3), flags = arg(4), pid = arg(5);
    if (flags & 4) plog("CreateThread: CREATE_SUSPENDED no soportado (se arranca)");
    GThread *t = new GThread;
    t->start = start;
    t->param = param;
    u32 sz = stack > (1u << 20) ? stack : (1u << 20);
    t->stack_lo = heap_alloc(sz, true);
    t->stack_hi = t->stack_lo + sz;
    // TEB propio: copia del actual con cadena SEH vacía, pila propia y TLS a cero (salvo el estático)
    t->teb.assign(MEM, MEM + TEB_BYTES);
    auto put = [&](u32 off, u32 v) { std::memcpy(&t->teb[off], &v, 4); };
    put(0x00, 0xFFFFFFFFu);
    put(0x04, t->stack_hi);
    put(0x08, t->stack_lo);
    std::memset(&t->teb[0xE10 + 4 * g_tls_estaticos], 0, (64 - g_tls_estaticos) * 4);
    for (u32 i = 0; i < g_tls_estaticos; i++) {   // bloques de TLS estático propios (copia de los actuales)
        u32 static_tls;
        std::memcpy(&static_tls, &t->teb[0xE10 + 4 * i], 4);
        if (!static_tls) continue;
        u32 n = heap_size(static_tls);
        u32 copy = heap_alloc(n ? n : 16, true);
        std::memcpy(MEM + copy, MEM + static_tls, n);
        put(0xE10 + 4 * i, copy);
    }
    auto kt = std::make_shared<KThread>();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        t->id = (g_next_id += 4);
        kt->id = t->id;
        t->handle = handles().add(kt);
        put(0x24, t->id);
        g_threads.push_back(t);
    }
#ifdef __EMSCRIPTEN__
    size_t pila = 16u << 20;    // va en la memoria de WebAssembly (2 GB en total)
#else
    size_t pila = 256u << 20;
#endif
    plat_thread_t th;
    if (!plat_thread_start(&th, pila, thread_main, t)) {
        // sin hilo del host, el del juego nunca correría y se llevaría el testigo para siempre
        plog("CreateThread: no se puede crear el hilo del host");
        std::lock_guard<std::mutex> lk(g_mx);
        t->done = true;
        t->exit_code = 0;
        RET(0, 6);
    }
    plat_thread_detach(th);
    if (pid) wr32(pid, t->id);
    plog("CreateThread(inicio 0x%x, param 0x%x) -> hilo 0x%x", start, param, t->id);
    RET(t->handle, 6);
}

// Otro programa traducido en el mismo proceso (GSW32.EXE, el servidor de gráficos de GRAPH32): un hilo
// que empieza en su punto de entrada y que ve su propio módulo principal y su línea de órdenes.
u32 thread_spawn_process(u32 entry, u32 exe_base, const std::string &cmdline) {
    u32 cl = heap_alloc((u32)cmdline.size() + 1, true);
    gstrcpy(cl, cmdline);
    u32 saved = esp;
    esp -= 28;
    wr32(esp, 0); wr32(esp + 4, 0); wr32(esp + 8, 0); wr32(esp + 12, entry); wr32(esp + 16, 0); wr32(esp + 20, 0); wr32(esp + 24, 0);
    ext_kernel32_dll_CreateThread();   // desapila la dirección de retorno ficticia y sus 6 argumentos
    esp = saved;
    u32 h = eax;
    std::lock_guard<std::mutex> lk(g_mx);
    for (GThread *t : g_threads)
        if (t->handle == h) { t->exe_base = exe_base; t->cmdline = cl; }
    return h;
}
STUB(ext_kernel32_dll_ExitThread) { thread_exit(arg(0)); }
STUB(ext_kernel32_dll_GetExitCodeThread) {
    u32 code = 0;
    thread_done(arg(0), &code);
    wr32(arg(1), code);
    RET(1, 2);
}
STUB(ext_kernel32_dll_ResumeThread) { RET(0, 1); }
STUB(ext_kernel32_dll_SuspendThread) { RET(0, 1); }
STUB(ext_kernel32_dll_GetCurrentThreadId) { RET(current_thread_id(), 0); }

// Secciones críticas: CRITICAL_SECTION { DebugInfo, LockCount, RecursionCount, OwningThread, … }
STUB(ext_kernel32_dll_InitializeCriticalSection) { std::memset(MEM + arg(0), 0, 24); wr32(arg(0) + 4, 0xFFFFFFFFu); RET(0, 1); }
STUB(ext_kernel32_dll_DeleteCriticalSection) { RET(0, 1); }
STUB(ext_kernel32_dll_EnterCriticalSection) {
    u32 cs = arg(0), me = current_thread_id();
    // Un hilo que sondea en bucle (entrar, mirar un estado, salir) esperando a otro no cedería nunca
    // el testigo: de vez en cuando se cede igualmente. Como mucho una vez por milisegundo: MSVCRT entra en una
    // sección crítica en cada putc y el juego graba las partidas byte a byte (ceder cada 32 entradas hacía que
    // grabar una liga de 5,8 MB tardase 7 minutos).
    static u32 spins = 0, ultimo = 0;
    if ((++spins & 31) == 0 && other_threads()) {
        u32 t = ticks_ms();
        if (t != ultimo) { ultimo = t; thread_yield(); }
    }
    while (rd32(cs + 12) && rd32(cs + 12) != me) cooperative_sleep(0);
    wr32(cs + 12, me);
    wr32(cs + 8, rd32(cs + 8) + 1);
    RET(0, 1);
}
STUB(ext_kernel32_dll_LeaveCriticalSection) {
    u32 cs = arg(0);
    u32 n = rd32(cs + 8);
    if (n) n--;
    wr32(cs + 8, n);
    if (!n) wr32(cs + 12, 0);
    RET(0, 1);
}
