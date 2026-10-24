/* PCF7 — estado de la CPU x86 emulada y acceso a la memoria del juego.
 *
 * Lo comparten el código traducido por recomp/lift.py (C) y la capa de plataforma (C++). La memoria del
 * juego es un array plano MEM: una dirección x86 de 32 bits es un índice en MEM. Los registros son
 * globales (las funciones traducidas se llaman entre sí compartiendo la CPU). Los segmentos se ignoran:
 * fs:[x] lee MEM[x], así que el TEB vive en la dirección 0.
 */
#pragma once
#include <setjmp.h>
#ifdef _WIN32
/* el código generado usa _setjmp(jb)/_longjmp (sin máscara de señales); en mingw _setjmp lleva dos argumentos (y los
 * headers lo declaran así, y setjmp() se expande a él en x86_64) y no hay _longjmp: con un argumento es el setjmp de
 * la arquitectura (con __USE_MINGW_SETJMP_NON_SEH: sin desenrollar la pila, como en POSIX); con dos, la función real */
#define PCF_SJ_PICK(a, b, c, ...) c
#define PCF_SJ2(a, b) _setjmp(a, b)
#define PCF_SJ1(b) setjmp(b)
#define _setjmp(...) PCF_SJ_PICK(__VA_ARGS__, PCF_SJ2, PCF_SJ1, )(__VA_ARGS__)
#define _longjmp(b, v) longjmp(b, v)
#endif
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t i8; typedef int16_t i16; typedef int32_t i32; typedef int64_t i64;

#define MEMSZ (1u << 29)
extern u8 MEM[];

void js_bad(u32 a);                       /* acceso fuera de MEM */
void js_missing(u32 a);                   /* llamada indirecta a una dirección sin traducir */
void js_dbg(u32 tag, u32 a, u32 b);       /* sonda de depuración */
void js_abort(void);

#ifdef PCF_DOS
/* MS-DOS (juegos con «dos»: true): la ventana de vídeo 0xA0000-0xBFFFF es la tarjeta VGA (platform/src/vga.cpp:
   planos, modos de escritura, latches); el resto de MEM es memoria normal */
u32 vga_rd8(u32 a);
void vga_wr8(u32 a, u32 v);
#define VGA_EN(a) __builtin_expect((u32)((a) - 0xA0000u) < 0x20000u, 0)
#define VGA_RD(a, n) if (VGA_EN(a)) { u32 _v = 0; for (u32 _i = 0; _i < (n); _i++) _v |= vga_rd8((a) + _i) << (8 * _i); return _v; }
#define VGA_WR(a, v, n) if (VGA_EN(a)) { for (u32 _i = 0; _i < (n); _i++) vga_wr8((a) + _i, ((v) >> (8 * _i)) & 0xFF); return; }
#else
#define VGA_RD(a, n)
#define VGA_WR(a, v, n)
#endif

static inline u32 rd32(u32 a) { if (a > MEMSZ - 4) { js_bad(a); return 0; } VGA_RD(a, 4) u32 v; __builtin_memcpy(&v, MEM + a, 4); return v; }
#ifdef PCF7_WATCH_WRITES
/* diagnóstico (cmake -DPCF7_WATCH_WRITES=ON): escrituras de 32 bits en la palabra de PCF7_MEMWATCH */
extern u32 g_memwatch_w;
void memwatch_write(u32 a, u32 v);
#define WATCH_W(a, v) do { if (__builtin_expect((a) == g_memwatch_w, 0)) memwatch_write(a, v); } while (0)
#else
#define WATCH_W(a, v) do { } while (0)
#endif
static inline void wr32(u32 a, u32 v) { if (a > MEMSZ - 4) { js_bad(a); return; } VGA_WR(a, v, 4) WATCH_W(a, v); __builtin_memcpy(MEM + a, &v, 4); }
static inline u32 rd16(u32 a) { if (a > MEMSZ - 2) { js_bad(a); return 0; } VGA_RD(a, 2) u16 v; __builtin_memcpy(&v, MEM + a, 2); return v; }
static inline void wr16(u32 a, u32 v) { if (a > MEMSZ - 2) { js_bad(a); return; } VGA_WR(a, v, 2) u16 w = (u16)v; __builtin_memcpy(MEM + a, &w, 2); }
/* inmediatos vivos (lift.py): el valor se lee de los bytes de la propia instrucción en MEM, para que un EXE modificado
 * por un mod (imagen.cpp aplica sus diferencias sobre la imagen) cambie fechas, tamaños, identificadores… */
#define IMM8(a) ((u32)MEM[(a)])
#define IMM16(a) ((u32)MEM[(a)] | ((u32)MEM[(a) + 1] << 8))
#define IMM32(a) ((u32)MEM[(a)] | ((u32)MEM[(a) + 1] << 8) | ((u32)MEM[(a) + 2] << 16) | ((u32)MEM[(a) + 3] << 24))
#define IMMS8(a) ((u32)(i32)(i8)MEM[(a)])
#define IMMS16(a) ((u32)(i32)(i16)IMM16(a))
static inline u32 rd8(u32 a) { if (a >= MEMSZ) { js_bad(a); return 0; } VGA_RD(a, 1) return MEM[a]; }
static inline void wr8(u32 a, u32 v) { if (a >= MEMSZ) { js_bad(a); return; } VGA_WR(a, v, 1) MEM[a] = (u8)v; }
static inline u64 rd64(u32 a) { return (u64)rd32(a) | ((u64)rd32(a + 4) << 32); }
static inline void wr64(u32 a, u64 v) { wr32(a, (u32)v); wr32(a + 4, (u32)(v >> 32)); }

static inline double F32(u32 b) { union { u32 i; float f; } u; u.i = b; return u.f; }
static inline double F64(u64 b) { union { u64 i; double d; } u; u.i = b; return u.d; }
static inline double F80(u32 a) {
    u64 m = rd64(a); u32 e = rd16(a + 8); u32 s = e >> 15; int ex = e & 0x7FFF; double v;
    if (ex == 0 && m == 0) v = 0.0; else v = (double)m * __builtin_exp2((double)(ex - 16383 - 63));
    return s ? -v : v;
}
static inline u32 B32(double d) { union { u32 i; float f; } u; u.f = (float)d; return u.i; }
static inline u64 B64(double d) { union { u64 i; double d2; } u; u.d2 = d; return u.i; }

extern u32 eax, ecx, edx, ebx, esp, ebp, esi, edi, CF, ZF, SF, OF, PF, t, DF;
extern u32 cur_fn; extern u32 fnring[256]; extern u32 fnring_i;
/* PCF7_WATCH="addr,addr…": registra las primeras entradas en esas funciones (diagnóstico) */
extern int g_watch; void fn_watch(u32 fn);
/* EFLAGS: bits altos (AC, ID) que el código puede alternar con pushfd/popfd; CPUID de un Pentium II */
extern u32 EFL_HI; void cpuid_emul(void);
extern u32 w_steps;
#ifdef PCF_DOS
/* MS-DOS: registros de segmento (es cs ss ds fs gs), bit IF, interrupciones de software (int n), puertos de E/S e
   IRQ del hardware emulado (temporizador, teclado…) que se atienden en los saltos hacia atrás (DOS_POLL) */
extern u32 g_ret_expected;   /* dirección de retorno que espera el marco actual (ret a otra = salto, int86 de Watcom) */
/* pila en sombra de las direcciones de retorno de las fronteras activas: un ret a una de ellas desenrolla hasta
   esa frontera (g_ret_jump); un ret a otra cosa es un salto (push dirección; ret) */
extern u32 g_sombra[4096];
extern u32 g_sombra_n;
extern u32 g_ret_jump;
static inline int dos_en_sombra(u32 a) { for (u32 i = g_sombra_n; i-- > 0;) if (g_sombra[i] == a) return 1; return 0; }
#define DOS_CALL_PRE(ra) do { if (g_sombra_n < 4096) g_sombra[g_sombra_n++] = (ra); g_ret_expected = (ra); } while (0)
#define DOS_CALL_POST(ra) do { if (g_sombra_n) g_sombra_n--; if (g_ret_jump) { if (g_ret_jump == (ra)) g_ret_jump = 0; else return; } } while (0)
extern u32 sreg[6];
extern u32 sbase[6];   /* base lineal de cada segmento (0 en el modelo plano) */
void seg_load(u32 i, u32 sel);
extern u32 g_if;
extern volatile u32 g_irq_pend;
void dos_int(u32 n);
u32 port_in(u32 port, u32 size);
void port_out(u32 port, u32 v, u32 size);
void irq_servir(void);
void call_regchg(u32 at, u32 s, u32 b, u32 d);   /* diagnóstico: esi/ebx/edi cambiados tras una llamada */
#define DOS_POLL() do { if (__builtin_expect(g_irq_pend != 0, 0) && g_if) irq_servir(); } while (0)
#endif
extern double ST[8]; extern u32 TOP, FPUCW, FPUSW; extern u64 MM[8];
/* conversión a entero como x86: fuera de rango (o NaN) da el "entero indefinido" 0x80…0 (en ARM64 el
   cast de C satura, y en C es comportamiento indefinido) */
static inline u32 fpu_i32(double v) { return (v >= -2147483648.0 && v < 2147483648.0) ? (u32)(i32)v : 0x80000000u; }
static inline u32 fpu_i16(double v) { return (v >= -32768.0 && v < 32768.0) ? (u32)(u16)(i16)v : 0x8000u; }
static inline u64 fpu_i64(double v) { return (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? (u64)(i64)v : 0x8000000000000000ull; }
/* redondeo de la FPU según el control (fist/fistp/frndint): 0 al par más cercano, 1 abajo, 2 arriba, 3 truncar */
static inline double fpu_rnd(double v) {
    switch ((FPUCW >> 10) & 3) {
    case 0: return __builtin_nearbyint(v);
    case 1: return __builtin_floor(v);
    case 2: return __builtin_ceil(v);
    default: return __builtin_trunc(v);
    }
}

void icall(u32 a);
u32 icall_try(u32 a);

/* Saltos no locales del runtime de C++ y del SEH (`mov esp, …; jmp eax` a la continuación de un `catch`
 * o al cuerpo de un `__except`): en C no se puede saltar a mitad de una función que está más arriba en la
 * pila del host. Las funciones que tienen esas continuaciones se envuelven con una pista de aterrizaje
 * (setjmp); ijmp() hace longjmp a la invocación correspondiente, que sigue en la continuación (traducida
 * como entrada propia) y vuelve como si la función hubiera terminado. Sin pista, es una llamada normal. */
typedef struct pcf7_land { jmp_buf jb; u32 lo, hi, esp0, target; struct pcf7_land *prev; } pcf7_land;
#ifdef __cplusplus
extern thread_local pcf7_land *g_land;
extern thread_local u32 g_land_now;
#else
extern _Thread_local pcf7_land *g_land;
extern _Thread_local u32 g_land_now;   /* la pista entra en esta continuación: no volver a saltar */
#endif
void ijmp(u32 a);
extern u32 g_thunk_base;   /* copia de una plantilla de código que se está ejecutando (ver recomp/lift.py) */
void land_to(u32 a);

// Detector de desequilibrio de pila: en cada `ret`, esp debe valer lo mismo que al entrar.
void esp_mismatch(u32 fn, u32 at, u32 e0);
#define ESPCHK_B() u32 _e0 = esp
// Tras cada `call`: esp vuelve a como estaba antes de empujar el retorno, más los argumentos que el
// callee limpie (stdcall, 0..64 B). Fuera de ese rango, la llamada ha descuadrado la pila.
void call_mismatch(u32 at, u32 c0);
#define CALLCHK(at) do { if ((u32)(esp - _c0) > 0x200u) call_mismatch(at, _c0); } while (0)
void call_mismatch_i(u32 at, u32 c0, u32 tg);
#define CALLCHK_I(tg, at) do { if ((u32)(esp - _c0) > 0x200u) call_mismatch_i(at, _c0, tg); } while (0)
#ifdef PCF7_WEB   /* en el navegador, `f_x(); return;` debe ser una llamada de cola real (ver CMakeLists.txt) */
#define ESPCHK_T(fn) do { } while (0)
#else
#define ESPCHK_T(fn) do { if ((i32)(esp - _e0) < 4 || (i32)(esp - _e0) > 64) esp_mismatch(fn, 0, _e0); } while (0)
#endif
#define ESPCHK_E(fn, at) do { if (esp != _e0) esp_mismatch(fn, at, _e0); } while (0)

/* ---- para los stubs del sistema (convención de Windows: argumentos en la pila emulada) ---- */
static inline u32 arg(u32 i) { return rd32(esp + 4 + 4 * i); }
/* devuelve v en EAX y limpia la dirección de retorno + n dwords de argumentos (stdcall) */
#define RET(v, n) do { eax = (u32)(v); esp += 4 + 4 * (n); return; } while (0)

#ifdef __cplusplus
}
#endif
