// compat_win.cpp — las funciones de compat.h que necesitan la API de Windows (ver compat.h). Solo en Windows.
#ifdef _WIN32
#include "compat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int setenv(const char *k, const char *v, int) {
    SetEnvironmentVariableA(k, v);
    return _putenv_s(k, v);
}
int unsetenv(const char *k) {
    SetEnvironmentVariableA(k, nullptr);
    return _putenv_s(k, "");
}

ssize_t pread(int fd, void *buf, size_t n, int64_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    LARGE_INTEGER cero{}, antes{};
    SetFilePointerEx(h, cero, &antes, FILE_CURRENT);
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD r = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)n, &r, &ov);
    DWORD e = ok ? 0 : GetLastError();
    SetFilePointerEx(h, antes, nullptr, FILE_BEGIN);
    if (!ok) { if (e == ERROR_HANDLE_EOF) return 0; errno = EIO; return -1; }
    return (ssize_t)r;
}
ssize_t pwrite(int fd, const void *buf, size_t n, int64_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    LARGE_INTEGER cero{}, antes{};
    SetFilePointerEx(h, cero, &antes, FILE_CURRENT);
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD w = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)n, &w, &ov);
    SetFilePointerEx(h, antes, nullptr, FILE_BEGIN);
    if (!ok) { errno = EIO; return -1; }
    return (ssize_t)w;
}
int futimens(int fd, const struct timespec ts[2]) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    auto ft = [](const struct timespec &t) {
        uint64_t v = (uint64_t)t.tv_sec * 10000000ull + (uint64_t)t.tv_nsec / 100 + 116444736000000000ull;
        FILETIME f;
        f.dwLowDateTime = (DWORD)v;
        f.dwHighDateTime = (DWORD)(v >> 32);
        return f;
    };
    FILETIME acceso = ft(ts[0]), escritura = ft(ts[1]);
    if (!SetFileTime(h, nullptr, &acceso, &escritura)) { errno = EACCES; return -1; }
    return 0;
}
int pcf_rename(const char *a, const char *b) {
    if (MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    DWORD e = GetLastError();
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
    return -1;
}

bool plat_exe_path(char *buf, size_t n) {
    DWORD r = GetModuleFileNameA(nullptr, buf, (DWORD)n);
    return r > 0 && r < n;
}

namespace {
struct Arranque { void *(*fn)(void *); void *arg; };
DWORD WINAPI arranque(LPVOID p) {
    auto *a = (Arranque *)p;
    auto fn = a->fn;
    void *arg = a->arg;
    delete a;
    fn(arg);
    return 0;
}
}  // namespace

// la pila solo se reserva (STACK_SIZE_PARAM_IS_A_RESERVATION): se compromete página a página según se usa
bool plat_thread_start(plat_thread_t *th, size_t pila, void *(*fn)(void *), void *arg) {
    auto *a = new Arranque{fn, arg};
    HANDLE h = CreateThread(nullptr, pila, arranque, a, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) { delete a; return false; }
    *th = h;
    return true;
}
void plat_thread_join(plat_thread_t th) { WaitForSingleObject((HANDLE)th, INFINITE); CloseHandle((HANDLE)th); }
void plat_thread_detach(plat_thread_t th) { CloseHandle((HANDLE)th); }
void plat_thread_exit() { ExitThread(0); }
#endif
