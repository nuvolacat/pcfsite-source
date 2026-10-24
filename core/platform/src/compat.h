// compat.h — lo que le falta a Windows (llvm-mingw) de las llamadas POSIX que usan la plataforma y el lanzador, para
// que el mismo código compile en macOS, Linux y Windows. Lo incluyen plat.h y app/lanzador/util.h.
//
//   pread/pwrite      ReadFile/WriteFile con desplazamiento (sin mover el puntero del fichero)
//   futimens          SetFileTime
//   setenv/unsetenv   el entorno del proceso Y el del CRT (los hijos heredan el primero, getenv lee el segundo)
//   mkdir(ruta, modo) _mkdir; lstat → stat; timegm → _mkgmtime; strcasecmp → _stricmp
//   O_BINARY          0 fuera de Windows (en Windows open() abre en modo texto si no se pide)
//   pcf_rename        rename que sustituye el destino si existe (en Windows rename() falla si existe)
//   plat_thread_*     hilos con pila grande: en Windows CreateThread con STACK_SIZE_PARAM_IS_A_RESERVATION (pthread_create
//                     de winpthreads *compromete* toda la pila: 1 GB de memoria de verdad)
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
// (sin windows.h: la plataforma define sus propios nombres de Win32 —S_OK, E_NOINTERFACE…— para el invitado; lo que
// necesita la API de Windows de verdad está en compat_win.cpp)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>

#ifndef timegm
#define timegm _mkgmtime
#endif
#define lstat stat
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
static inline int pcf_mkdir(const char *p, int) { return _mkdir(p); }
#define mkdir pcf_mkdir

int setenv(const char *k, const char *v, int sobrescribir);
int unsetenv(const char *k);
ssize_t pread(int fd, void *buf, size_t n, int64_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, int64_t off);
int futimens(int fd, const struct timespec ts[2]);
int pcf_rename(const char *a, const char *b);

bool plat_exe_path(char *buf, size_t n);   // la ruta del ejecutable (GetModuleFileName)

typedef void *plat_thread_t;   // HANDLE
bool plat_thread_start(plat_thread_t *th, size_t pila, void *(*fn)(void *), void *arg);
void plat_thread_join(plat_thread_t th);
void plat_thread_detach(plat_thread_t th);
[[noreturn]] void plat_thread_exit();

#else   // POSIX

#include <pthread.h>
#include <cstdio>
#ifndef O_BINARY
#define O_BINARY 0
#endif
static inline int pcf_rename(const char *a, const char *b) { return std::rename(a, b); }

typedef pthread_t plat_thread_t;
static inline bool plat_thread_start(plat_thread_t *th, size_t pila, void *(*fn)(void *), void *arg) {
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, pila);
    return pthread_create(th, &at, fn, arg) == 0;
}
static inline void plat_thread_join(plat_thread_t th) { pthread_join(th, nullptr); }
static inline void plat_thread_detach(plat_thread_t th) { pthread_detach(th); }
[[noreturn]] static inline void plat_thread_exit() { pthread_exit(nullptr); }

#endif
