// PCF7 — capa de plataforma: utilidades comunes a todos los módulos (kernel32, user32, gdi32…).
#pragma once
#include "rt.h"
#include "layout.h"   // mapa de memoria de este programa (generado por recomp/build.py)
#include "compat.h"   // lo que le falta a Windows (mingw) de POSIX

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// Define un stub del sistema. recomp/build.py busca `STUB(ext_…)` en platform/src para no generar su
// versión por defecto.
#define STUB(name) extern "C" void name(void)

// Mapa de memoria del invitado (direcciones x86 = índices en MEM)
namespace mm {
constexpr u32 TEB = 0x00000000;          // fs:[x] lee MEM[x] (el lifter ignora los segmentos)
constexpr u32 PEB = 0x00001000;
constexpr u32 SCRATCH = 0x00002000;      // cadenas fijas del proceso (línea de órdenes, entorno…)
#ifdef PCF7_STACK_LO
constexpr u32 STACK_LO = PCF7_STACK_LO;  // pila del hilo principal (el juego la sube si su programa es grande)
#else
constexpr u32 STACK_LO = 0x01000000;     // pila del hilo principal: 15 MB
#endif
constexpr u32 STACK_HI = 0x01F00000;
constexpr u32 HEAP_LO = PCF7_HEAP_LO;    // por encima de los módulos y los trampolines
constexpr u32 HEAP_HI = 0x1C000000;      // 448 MB
constexpr u32 MAP_LO = 0x1C000000;       // vistas de ficheros mapeados (MapViewOfFile): 64 MB
constexpr u32 MAP_HI = MEMSZ;
}  // namespace mm

// Llama a una función del juego con la convención stdcall/cdecl desde el host (WndProc, DllMain…).
// Devuelve EAX. Restaura esp al valor previo pase lo que pase (cdecl no limpia los argumentos).
u32 guest_call(u32 fn, std::initializer_list<u32> args);

// Llama a un stub del sistema desde el host como si lo llamara el invitado (apila args y retorno)
u32 call_stub(void (*f)(void), std::initializer_list<u32> args);

// Registra una función del host y devuelve una dirección de invitado que la llama (para punteros a
// función que el juego invoca: GetProcAddress de ddraw/dsound, métodos de objetos COM…).
u32 host_fn(void (*f)(void));

// Cadenas del invitado
std::string gstr(u32 p, size_t max = 4096);
u32 gstrcpy(u32 dst, const std::string &s, u32 cap = 0xFFFFFFFFu);  // devuelve longitud copiada
inline u32 gstrlen(u32 p) { u32 n = 0; while (rd8(p + n)) n++; return n; }

// Montículo del invitado (HeapAlloc, GlobalAlloc, LocalAlloc, VirtualAlloc…)
u32 heap_alloc(u32 n, bool zero = false);
void heap_free(u32 p);
u32 heap_size(u32 p);
u32 heap_realloc(u32 p, u32 n, bool zero = false);

// Registro de diagnóstico (stderr). `once` evita repetir el mismo aviso.
void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void plog_once(const std::string &key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Rutas: traduce una ruta de Windows del juego a una ruta del host (insensible a mayúsculas).
std::string host_path(const std::string &winpath);
std::string host_path_w(const std::string &winpath);              // para escribir (en la web, copia al escribible)
// Partidas en la web (web.cpp): lo que el juego escribe se guarda fuera (navegador y cuenta). web_sucio = ese fichero
// ha cambiado; web_abierto = hay (+1) / ya no hay (-1) un handle escribiendo en él; web_persistir, desde el hilo principal.
#ifdef __EMSCRIPTEN__
void web_sucio(const std::string &hostpath);
void web_abierto(const std::string &hostpath, int d);
void web_persistir(bool todo = false);   // todo: también lo recién cambiado (al acabar el juego)
#else
inline void web_sucio(const std::string &) {}
inline void web_abierto(const std::string &, int) {}
#endif
std::vector<std::string> host_dirs(const std::string &winpath);  // carpetas del host de una carpeta del juego
extern std::string g_cwd;   // directorio actual (Windows, p. ej. "C:\\FUTBOL7")

// Programa traducido (modinfo.c): "manager.exe", "dbasewin.exe"…; su ruta de Windows y sus argumentos
extern "C" const char TARGET_EXE[];
// El juego (juego.c, generado desde juegos/<id>/juego.json): identificador ("pcf7"), nombre para la ventana,
// carpeta de instalación (C:\<carpeta>), carpeta de trabajo bajo $HOME ("pcf7-work"), programa principal y dirección
// donde el menú deja el apartado elegido para el lanzador (0: el juego no tiene lanzador)
extern "C" const char JUEGO_ID[], JUEGO_NOMBRE[], JUEGO_CARPETA[], JUEGO_TRABAJO[], JUEGO_PRINCIPAL[];
extern "C" const unsigned JUEGO_SALIDA_MENU;
struct JuegoLanza { unsigned apartado; const char *programa, *arg; };
extern "C" const JuegoLanza JUEGO_LANZADOR[];   // termina en {0, nullptr, nullptr}
extern "C" const char *const JUEGO_ENTORNO[];   // "NOMBRE=valor" propias del juego; termina en nullptr
std::string juego_trabajo();             // $HOME/<JUEGO_TRABAJO>
void dinput_key(u32 dik, bool down);     // dinput.cpp: tecla pulsada/soltada (código DIK = scan | 0x80 si extendida)
std::string target_winpath();            // p. ej. "C:\\FUTBOL7\\MANAGER.EXE"
extern std::string g_guest_args;         // argumentos de la línea de órdenes del invitado

// Fin del proceso (ExitProcess): vuelve al bucle del host
[[noreturn]] void process_exit(u32 code);
