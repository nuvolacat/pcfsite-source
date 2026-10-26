// PCF7 — APIs de Windows sencillas que no necesitan el estado de las ventanas ni de GDI: variantes
// Unicode (W) de ficheros y cadenas (convierten y reutilizan la versión A), ficheros de 16 bits
// (_lopen…), fechas, perfiles (.INI), teclado, rectángulos y caracteres, VERSION y SHELL32. Las usan
// sobre todo el runtime de Visual Basic y los controles de ProQuinielas.
#include "handles.h"
#include "win.h"

#include <algorithm>
#include <ctime>
#include <strings.h>
#include <fstream>
#include <map>
#include <vector>

void set_last_error(u32 e);

extern "C" {
void ext_kernel32_dll_CreateFileA(void);
void ext_kernel32_dll_DeleteFileA(void);
void ext_kernel32_dll_CreateDirectoryA(void);
void ext_kernel32_dll_RemoveDirectoryA(void);
void ext_kernel32_dll_MoveFileA(void);
void ext_kernel32_dll_GetFileAttributesA(void);
void ext_kernel32_dll_SetFileAttributesA(void);
void ext_kernel32_dll_SetCurrentDirectoryA(void);
void ext_kernel32_dll_GetCurrentDirectoryA(void);
void ext_kernel32_dll_GetFullPathNameA(void);
void ext_kernel32_dll_GetModuleFileNameA(void);
void ext_kernel32_dll_FindFirstFileA(void);
void ext_kernel32_dll_FindNextFileA(void);
void ext_kernel32_dll_GetLocaleInfoA(void);
void ext_kernel32_dll_SetEnvironmentVariableA(void);
void ext_kernel32_dll_ReadFile(void);
void ext_kernel32_dll_WriteFile(void);
void ext_kernel32_dll_SetFilePointer(void);
void ext_kernel32_dll_CloseHandle(void);
void ext_kernel32_dll_GetPrivateProfileStringA(void);
void ext_kernel32_dll_LoadLibraryA(void);
void ext_kernel32_dll_FindResourceA(void);
void ext_kernel32_dll_LoadResource(void);
void ext_gdi32_dll_CreateDIBitmap(void);
void ext_user32_dll_LoadIconA(void);
void ext_user32_dll_LoadCursorA(void);
void ext_user32_dll_LoadCursorFromFileA(void);
}

namespace {

std::string wide(u32 p) {
    std::string s;
    for (u32 i = 0; p && i < 32768; i++) { u32 c = rd16(p + 2 * i); if (!c) break; s.push_back(c < 256 ? (char)c : '?'); }
    return s;
}
u32 narrow_copy(u32 wp) {   // cadena W del invitado → copia ANSI en el montículo (0 si wp = 0)
    if (!wp) return 0;
    std::string s = wide(wp);
    u32 p = heap_alloc((u32)s.size() + 1);
    gstrcpy(p, s);
    return p;
}
void widen_into(u32 dst, const std::string &s, u32 cap_chars) {
    u32 n = std::min<u32>((u32)s.size(), cap_chars ? cap_chars - 1 : 0);
    for (u32 i = 0; i < n; i++) wr16(dst + 2 * i, (u8)s[i]);
    if (cap_chars) wr16(dst + 2 * n, 0);
}

// llama a la versión A de una función con los argumentos de cadena (índices) ya convertidos a ANSI
void call_a(void (*a)(void), std::initializer_list<u32> string_args) {
    std::vector<u32> tmp;
    for (u32 i : string_args) {
        u32 p = narrow_copy(arg(i));
        tmp.push_back(p);
        if (p) wr32(esp + 4 + 4 * i, p);
    }
    a();
    for (u32 p : tmp) if (p) heap_free(p);
}


}  // namespace

// ---------------------------------------------------------------- ficheros con nombres Unicode

STUB(ext_kernel32_dll_CreateFileW) { call_a(ext_kernel32_dll_CreateFileA, {0}); }
STUB(ext_kernel32_dll_DeleteFileW) { call_a(ext_kernel32_dll_DeleteFileA, {0}); }
STUB(ext_kernel32_dll_CreateDirectoryW) { call_a(ext_kernel32_dll_CreateDirectoryA, {0}); }
STUB(ext_kernel32_dll_RemoveDirectoryW) { call_a(ext_kernel32_dll_RemoveDirectoryA, {0}); }
STUB(ext_kernel32_dll_MoveFileW) { call_a(ext_kernel32_dll_MoveFileA, {0, 1}); }
STUB(ext_kernel32_dll_GetFileAttributesW) { call_a(ext_kernel32_dll_GetFileAttributesA, {0}); }
STUB(ext_kernel32_dll_SetFileAttributesW) { call_a(ext_kernel32_dll_SetFileAttributesA, {0}); }
STUB(ext_kernel32_dll_SetCurrentDirectoryW) { call_a(ext_kernel32_dll_SetCurrentDirectoryA, {0}); }
STUB(ext_kernel32_dll_SetEnvironmentVariableW) { call_a(ext_kernel32_dll_SetEnvironmentVariableA, {0, 1}); }
STUB(ext_kernel32_dll_GetCurrentDirectoryW) {   // (cch, buf)
    u32 cap = arg(0), buf = arg(1);
    u32 tmp = heap_alloc(520);
    wr32(esp + 8, tmp); wr32(esp + 4, 260);
    ext_kernel32_dll_GetCurrentDirectoryA();
    u32 n = eax;
    std::string s = gstr(tmp);
    heap_free(tmp);
    if (cap < s.size() + 1) { eax = (u32)s.size() + 1; return; }
    widen_into(buf, s, cap);
    eax = n;
}
STUB(ext_kernel32_dll_GetModuleFileNameW) {   // (hmod, buf, cch)
    u32 buf = arg(1), cap = arg(2);
    u32 tmp = heap_alloc(520);
    wr32(esp + 8, tmp); wr32(esp + 12, 260);
    ext_kernel32_dll_GetModuleFileNameA();
    std::string s = gstr(tmp);
    heap_free(tmp);
    widen_into(buf, s, cap);
    eax = std::min<u32>((u32)s.size(), cap ? cap - 1 : 0);
}
STUB(ext_kernel32_dll_GetFullPathNameW) {   // (name, cch, buf, filepart)
    u32 cap = arg(1), buf = arg(2), fp = arg(3);
    u32 name = narrow_copy(arg(0));
    u32 tmp = heap_alloc(520);
    wr32(esp + 4, name); wr32(esp + 8, 260); wr32(esp + 12, tmp); wr32(esp + 16, 0);
    ext_kernel32_dll_GetFullPathNameA();
    std::string s = gstr(tmp);
    heap_free(tmp);
    heap_free(name);
    if (cap < s.size() + 1) { eax = (u32)s.size() + 1; return; }
    widen_into(buf, s, cap);
    if (fp) { size_t sl = s.find_last_of('\\'); wr32(fp, buf + 2 * (u32)(sl == std::string::npos ? 0 : sl + 1)); }
    eax = (u32)s.size();
}
// WIN32_FIND_DATAW: atributos, tiempos y tamaños igual; cFileName[260] y cAlternateFileName[14] en UTF-16
static void find_a_to_w(u32 a, u32 w) {
    std::memcpy(MEM + w, MEM + a, 44);
    widen_into(w + 44, gstr(a + 44, 260), 260);
    widen_into(w + 44 + 520, gstr(a + 304, 14), 14);
}
STUB(ext_kernel32_dll_FindFirstFileW) {   // (pattern, data)
    u32 data = arg(1);
    u32 pat = narrow_copy(arg(0)), tmp = heap_alloc(320, true);
    wr32(esp + 4, pat); wr32(esp + 8, tmp);
    ext_kernel32_dll_FindFirstFileA();
    if (eax != 0xFFFFFFFFu) find_a_to_w(tmp, data);
    heap_free(tmp);
    heap_free(pat);
}
STUB(ext_kernel32_dll_FindNextFileW) {   // (handle, data)
    u32 data = arg(1), tmp = heap_alloc(320, true);
    wr32(esp + 8, tmp);
    ext_kernel32_dll_FindNextFileA();
    if (eax) find_a_to_w(tmp, data);
    heap_free(tmp);
}
STUB(ext_kernel32_dll_GetLocaleInfoW) {   // (lcid, type, buf, cch)
    u32 buf = arg(2), cap = arg(3);
    u32 tmp = heap_alloc(256);
    wr32(esp + 12, tmp); wr32(esp + 16, 256);
    ext_kernel32_dll_GetLocaleInfoA();
    std::string s = gstr(tmp);
    heap_free(tmp);
    if (!cap) { eax = (u32)s.size() + 1; return; }
    widen_into(buf, s, cap);
    eax = std::min<u32>((u32)s.size() + 1, cap);
}
STUB(ext_kernel32_dll_GetStringTypeExW) {   // (lcid, type, src, n, out)
    u32 type = arg(1), src = arg(2), dst = arg(4);
    i32 n = (i32)arg(3);
    if (n < 0) { n = 0; while (rd16(src + 2 * (u32)n)) n++; n++; }
    for (i32 i = 0; i < n; i++) {
        u32 c = rd16(src + 2 * (u32)i);
        u32 r = 0;
        if (type == 1) {
            if (std::isupper((int)(c & 0x7F)) && c < 0x80) r |= 0x001 | 0x100;
            if (std::islower((int)(c & 0x7F)) && c < 0x80) r |= 0x002 | 0x100;
            if (c >= 0xC0 && c <= 0xDE && c != 0xD7) r |= 0x001 | 0x100;
            if (c >= 0xDF && c <= 0xFF && c != 0xF7) r |= 0x002 | 0x100;
            if (c >= '0' && c <= '9') r |= 0x004;
            if (c == ' ' || (c >= 9 && c <= 13)) r |= 0x008;
            if (c < 0x20) r |= 0x020;
            if (c > 0x20 && c < 0x7F && !std::isalnum((int)c)) r |= 0x010;
        }
        wr16(dst + 2 * (u32)i, r);
    }
    RET(1, 5);
}
STUB(ext_kernel32_dll_lstrcpyW) {
    u32 d = arg(0), s = arg(1), i = 0;
    for (;; i++) { u32 c = rd16(s + 2 * i); wr16(d + 2 * i, c); if (!c) break; }
    RET(d, 2);
}
STUB(ext_kernel32_dll_lstrcmpiW) {
    std::string a = wide(arg(0)), b = wide(arg(1));
    int r = strcasecmp(a.c_str(), b.c_str());
    RET(r < 0 ? (u32)-1 : r > 0, 2);
}
STUB(ext_kernel32_dll_IsBadStringPtrW) { RET(arg(0) == 0, 2); }
STUB(ext_kernel32_dll_FormatMessageW) {   // (flags, source, id, lang, buf, size, args)
    std::string s = "Error " + std::to_string(arg(2));
    u32 buf = arg(4);
    if (arg(0) & 0x100) { u32 p = heap_alloc(2 * ((u32)s.size() + 1)); widen_into(p, s, (u32)s.size() + 1); wr32(buf, p); }
    else widen_into(buf, s, arg(5));
    RET((u32)s.size(), 7);
}

// ---------------------------------------------------------------- ficheros de 16 bits (_lopen…) y OpenFile

STUB(ext_kernel32_dll__lopen) {   // (path, mode): OF_READ 0, OF_WRITE 1, OF_READWRITE 2
    u32 mode = arg(1) & 3;
    RET(call_stub(ext_kernel32_dll_CreateFileA, {arg(0), mode == 0 ? 0x80000000u : mode == 1 ? 0x40000000u : 0xC0000000u, 3, 0, 3, 0x80, 0}), 2);
}
STUB(ext_kernel32_dll__lread) {   // (h, buf, n) -> leídos o HFILE_ERROR
    u32 got = heap_alloc(4, true);
    u32 ok = call_stub(ext_kernel32_dll_ReadFile, {arg(0), arg(1), arg(2), got, 0});
    u32 r = ok ? rd32(got) : 0xFFFFFFFFu;
    heap_free(got);
    RET(r, 3);
}
STUB(ext_kernel32_dll__lwrite) {
    u32 put = heap_alloc(4, true);
    u32 ok = call_stub(ext_kernel32_dll_WriteFile, {arg(0), arg(1), arg(2), put, 0});
    u32 r = ok ? rd32(put) : 0xFFFFFFFFu;
    heap_free(put);
    RET(r, 3);
}
STUB(ext_kernel32_dll__llseek) { RET(call_stub(ext_kernel32_dll_SetFilePointer, {arg(0), arg(1), 0, arg(2)}), 3); }
STUB(ext_kernel32_dll__lclose) { call_stub(ext_kernel32_dll_CloseHandle, {arg(0)}); RET(0, 1); }
STUB(ext_kernel32_dll_OpenFile) {   // (name, OFSTRUCT*, style): OF_EXIST 0x4000, OF_CREATE 0x1000, OF_DELETE 0x200
    u32 name = arg(0), ofs = arg(1), style = arg(2);
    std::string n = gstr(name);
    if (ofs) { std::memset(MEM + ofs, 0, 136); wr8(ofs, 136); gstrcpy(ofs + 8, n, 128); }
    std::string hp = (style & 0x1200) ? host_path_w(n) : host_path(n);   // OF_DELETE / OF_CREATE
    if (style & 0x200) RET(::remove(hp.c_str()) == 0 ? 1 : 0xFFFFFFFFu, 3);
    if (style & 0x4000) { std::ifstream f(hp); RET(f ? 1 : 0xFFFFFFFFu, 3); }
    u32 mode = style & 3;
    RET(call_stub(ext_kernel32_dll_CreateFileA, {name, mode == 0 ? 0x80000000u : mode == 1 ? 0x40000000u : 0xC0000000u, 3, 0,
                                                 (style & 0x1000) ? 2u : 3u, 0x80, 0}), 3);
}

// ---------------------------------------------------------------- perfiles (.INI y WIN.INI)

namespace {
using Ini = std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>;
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
Ini read_ini_ordered(const std::string &host) {
    Ini ini;
    std::ifstream in(host);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') { ini.push_back({trim(line.substr(1, line.find(']') - 1)), {}}); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (ini.empty()) ini.push_back({"", {}});
        ini.back().second.push_back({trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
    }
    return ini;
}
void write_ini(const std::string &host, const Ini &ini) {
    std::ofstream out(host);
    for (auto &[sec, kv] : ini) {
        out << "[" << sec << "]\r\n";
        for (auto &[k, v] : kv) out << k << "=" << v << "\r\n";
        out << "\r\n";
    }
}
bool ini_write(const std::string &file, u32 sec_p, u32 key_p, u32 val_p) {
    std::string host = host_path_w(file);
    Ini ini = read_ini_ordered(host);
    std::string sec = gstr(sec_p);
    auto s = std::find_if(ini.begin(), ini.end(), [&](auto &x) { return !strcasecmp(x.first.c_str(), sec.c_str()); });
    if (!key_p) {                          // borrar la sección
        if (s != ini.end()) ini.erase(s);
    } else {
        if (s == ini.end()) { ini.push_back({sec, {}}); s = ini.end() - 1; }
        std::string key = gstr(key_p);
        auto k = std::find_if(s->second.begin(), s->second.end(), [&](auto &x) { return !strcasecmp(x.first.c_str(), key.c_str()); });
        if (!val_p) { if (k != s->second.end()) s->second.erase(k); }
        else if (k != s->second.end()) k->second = gstr(val_p);
        else s->second.push_back({key, gstr(val_p)});
    }
    write_ini(host, ini);
    return true;
}
std::string win_ini() { return "C:\\WINDOWS\\WIN.INI"; }
}  // namespace

STUB(ext_kernel32_dll_WritePrivateProfileStringA) { RET(ini_write(gstr(arg(3)), arg(0), arg(1), arg(2)) ? 1 : 0, 4); }
STUB(ext_kernel32_dll_WriteProfileStringA) { RET(ini_write(win_ini(), arg(0), arg(1), arg(2)) ? 1 : 0, 3); }
STUB(ext_kernel32_dll_GetProfileStringA) {   // (sec, key, def, buf, cch) = GetPrivateProfileString(…, WIN.INI)
    u32 f = heap_alloc(32);
    gstrcpy(f, win_ini());
    u32 r = call_stub(ext_kernel32_dll_GetPrivateProfileStringA, {arg(0), arg(1), arg(2), arg(3), arg(4), f});
    heap_free(f);
    RET(r, 5);
}

// ---------------------------------------------------------------- fechas y configuración regional

namespace {
void systemtime_tm(u32 p, std::tm &t) {
    if (p) {
        t = {};
        t.tm_year = (int)rd16(p) - 1900; t.tm_mon = (int)rd16(p + 2) - 1; t.tm_wday = (int)rd16(p + 4); t.tm_mday = (int)rd16(p + 6);
        t.tm_hour = (int)rd16(p + 8); t.tm_min = (int)rd16(p + 10); t.tm_sec = (int)rd16(p + 12);
    } else {
        std::time_t now = std::time(nullptr);
        localtime_r(&now, &t);
    }
}
u32 put_out(u32 buf, u32 cap, const std::string &s) {
    if (!cap) return (u32)s.size() + 1;
    gstrcpy(buf, s, cap);
    return std::min<u32>((u32)s.size() + 1, cap);
}
}  // namespace

STUB(ext_kernel32_dll_GetDateFormatA) {   // (lcid, flags, systemtime, format, buf, cch)
    std::tm t;
    systemtime_tm(arg(2), t);
    std::string fmt = arg(3) ? gstr(arg(3)) : ((arg(1) & 2) ? "dddd, d' de 'MMMM' de 'yyyy" : "dd/MM/yy");   // DATE_LONGDATE
    static const char *dias[] = {"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
    static const char *meses[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
    std::string out;
    for (size_t i = 0; i < fmt.size();) {
        char c = fmt[i];
        size_t n = 1;
        while (i + n < fmt.size() && fmt[i + n] == c) n++;
        char b[16];
        if (c == '\'') { size_t e = fmt.find('\'', i + 1); out += fmt.substr(i + 1, (e == std::string::npos ? fmt.size() : e) - i - 1); i = e == std::string::npos ? fmt.size() : e + 1; continue; }
        if (c == 'd') { if (n <= 2) { snprintf(b, sizeof b, n == 1 ? "%d" : "%02d", t.tm_mday); out += b; } else out += n == 3 ? std::string(dias[t.tm_wday]).substr(0, 3) : dias[t.tm_wday]; }
        else if (c == 'M') { if (n <= 2) { snprintf(b, sizeof b, n == 1 ? "%d" : "%02d", t.tm_mon + 1); out += b; } else out += n == 3 ? std::string(meses[t.tm_mon]).substr(0, 3) : meses[t.tm_mon]; }
        else if (c == 'y') { if (n <= 2) snprintf(b, sizeof b, "%02d", (t.tm_year + 1900) % 100); else snprintf(b, sizeof b, "%d", t.tm_year + 1900); out += b; }
        else out.append(n, c);
        i += n;
    }
    RET(put_out(arg(4), arg(5), out), 6);
}
STUB(ext_kernel32_dll_GetTimeFormatA) {   // (lcid, flags, systemtime, format, buf, cch)
    std::tm t;
    systemtime_tm(arg(2), t);
    char b[32];
    if (arg(1) & 2) snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min);   // TIME_NOSECONDS
    else snprintf(b, sizeof b, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    RET(put_out(arg(4), arg(5), b), 6);
}
STUB(ext_kernel32_dll_GetSystemDefaultLCID) { RET(0x0C0A, 0); }
STUB(ext_kernel32_dll_GetSystemDefaultLangID) { RET(0x0C0A, 0); }
STUB(ext_kernel32_dll_GetUserDefaultLangID) { RET(0x0C0A, 0); }

// ---------------------------------------------------------------- proceso, sincronización, memoria

STUB(ext_kernel32_dll_GetProcessHeap) { RET(0x00150000u, 0); }
STUB(ext_kernel32_dll_InterlockedExchange) { u32 p = arg(0), old = rd32(p); wr32(p, arg(1)); RET(old, 2); }
STUB(ext_kernel32_dll_VirtualQuery) {   // (addr, MEMORY_BASIC_INFORMATION*, len): todo es memoria confirmada de lectura y escritura
    u32 a = arg(0) & ~0xFFFu, p = arg(1);
    if (!p || arg(2) < 28) RET(0, 3);
    wr32(p, a); wr32(p + 4, a); wr32(p + 8, 0x04); wr32(p + 12, 0x10000); wr32(p + 16, 0x1000); wr32(p + 20, 0x04); wr32(p + 24, 0x20000);
    RET(28, 3);
}
STUB(ext_kernel32_dll_FlushInstructionCache) { RET(1, 3); }
STUB(ext_kernel32_dll_DisableThreadLibraryCalls) { RET(1, 1); }
STUB(ext_kernel32_dll_ResetEvent) { if (auto e = handles().get<KEvent>(arg(0))) e->signaled = false; RET(1, 1); }
STUB(ext_kernel32_dll_OpenMutexA) { set_last_error(2); RET(0, 3); }
STUB(ext_kernel32_dll_OpenFileMappingA) { set_last_error(2); RET(0, 3); }
STUB(ext_kernel32_dll_FreeResource) { RET(0, 1); }
STUB(ext_kernel32_dll_Beep) { RET(1, 2); }
STUB(ext_kernel32_dll_DebugBreak) { plog("DebugBreak (desde f_%x)", cur_fn); RET(0, 0); }
STUB(ext_kernel32_dll_FatalAppExitA) { plog("FatalAppExitA: %s", gstr(arg(1)).c_str()); process_exit(1); }
// no se lanzan otros procesos de Windows desde los programas (el lanzador es del host)
STUB(ext_kernel32_dll_CreateProcessA) { plog("CreateProcessA(\"%s\"): no soportado", gstr(arg(1) ? arg(1) : arg(0)).c_str()); set_last_error(2); RET(0, 10); }
STUB(ext_kernel32_dll_CreateProcessW) { plog("CreateProcessW: no soportado"); set_last_error(2); RET(0, 10); }
bool spawn_translated(const std::string &cmdline);   // loader.cpp
STUB(ext_kernel32_dll_WinExec) {
    std::string cmd = gstr(arg(0));
    if (spawn_translated(cmd)) RET(33, 2);   // > 31: éxito
    plog("WinExec(\"%s\"): no soportado", cmd.c_str());
    RET(2, 2);   // ERROR_FILE_NOT_FOUND
}
STUB(ext_kernel32_dll_GetExitCodeProcess) { if (arg(1)) wr32(arg(1), 0); RET(1, 2); }
STUB(ext_kernel32_dll_CreatePipe) { set_last_error(50); RET(0, 4); }
STUB(ext_kernel32_dll_PeekNamedPipe) { RET(0, 6); }
STUB(ext_kernel32_dll_LoadLibraryExA) { RET(call_stub(ext_kernel32_dll_LoadLibraryA, {arg(0)}), 3); }
// consola: no hay
STUB(ext_kernel32_dll_GetConsoleMode) { RET(0, 2); }
STUB(ext_kernel32_dll_SetConsoleMode) { RET(0, 2); }
STUB(ext_kernel32_dll_SetConsoleCtrlHandler) { RET(1, 2); }
STUB(ext_kernel32_dll_GetNumberOfConsoleInputEvents) { RET(0, 2); }
STUB(ext_kernel32_dll_PeekConsoleInputA) { RET(0, 4); }
STUB(ext_kernel32_dll_ReadConsoleInputA) { RET(0, 4); }
STUB(ext_kernel32_dll_ReadConsoleA) { RET(0, 5); }
STUB(ext_kernel32_dll_WriteConsoleA) { if (arg(3)) wr32(arg(3), arg(2)); RET(1, 5); }

// ---------------------------------------------------------------- USER32: rectángulos, caracteres, teclado

STUB(ext_user32_dll_UnionRect) {
    Rect a = rd_rect(arg(1)), b = rd_rect(arg(2));
    bool ea = a.l >= a.r || a.t >= a.b, eb = b.l >= b.r || b.t >= b.b;
    Rect r = ea ? b : eb ? a : Rect{std::min(a.l, b.l), std::min(a.t, b.t), std::max(a.r, b.r), std::max(a.b, b.b)};
    if (ea && eb) r = {};
    wr_rect(arg(0), r);
    RET(!(r.l >= r.r || r.t >= r.b), 3);
}
STUB(ext_user32_dll_SubtractRect) {   // solo si b tapa a por completo en un eje
    Rect a = rd_rect(arg(1)), b = rd_rect(arg(2)), r = a;
    if (b.t <= a.t && b.b >= a.b) { if (b.l <= a.l && b.r > a.l) r.l = std::min(b.r, a.r); else if (b.r >= a.r && b.l < a.r) r.r = std::max(b.l, a.l); }
    if (b.l <= a.l && b.r >= a.r) { if (b.t <= a.t && b.b > a.t) r.t = std::min(b.b, a.b); else if (b.b >= a.b && b.t < a.b) r.b = std::max(b.t, a.t); }
    if (r.l >= r.r || r.t >= r.b) r = {};
    wr_rect(arg(0), r);
    RET(!(r.l >= r.r || r.t >= r.b), 3);
}
STUB(ext_user32_dll_CharLowerBuffA) { for (u32 i = 0; i < arg(1); i++) { u32 c = rd8(arg(0) + i); if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) wr8(arg(0) + i, c + 32); } RET(arg(1), 2); }
STUB(ext_user32_dll_CharUpperBuffA) { for (u32 i = 0; i < arg(1); i++) { u32 c = rd8(arg(0) + i); if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) wr8(arg(0) + i, c - 32); } RET(arg(1), 2); }
STUB(ext_user32_dll_CharLowerBuffW) { for (u32 i = 0; i < arg(1); i++) { u32 c = rd16(arg(0) + 2 * i); if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) wr16(arg(0) + 2 * i, c + 32); } RET(arg(1), 2); }
STUB(ext_user32_dll_CharUpperBuffW) { for (u32 i = 0; i < arg(1); i++) { u32 c = rd16(arg(0) + 2 * i); if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) wr16(arg(0) + 2 * i, c - 32); } RET(arg(1), 2); }
STUB(ext_user32_dll_CharPrevA) { RET(arg(1) > arg(0) ? arg(1) - 1 : arg(0), 2); }
STUB(ext_user32_dll_IsCharAlphaA) { u32 c = arg(0) & 0xFF; RET(std::isalpha((int)c) || (c >= 0xC0 && c != 0xD7 && c != 0xF7) || c == 0xAA || c == 0xBA, 1); }
STUB(ext_user32_dll_GetKeyboardLayout) { RET(0x0C0A0C0Au, 1); }
STUB(ext_user32_dll_MapVirtualKeyA) {   // (code, type): 0 vk→scan, 1 scan→vk, 2 vk→char
    u32 c = arg(0), t = arg(1);
    if (t == 2) RET((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : c == 0x20 ? 0x20 : 0, 2);
    RET(0, 2);
}
STUB(ext_user32_dll_VkKeyScanA) {   // carácter → vk (byte bajo) y mayúsculas (byte alto)
    u32 c = arg(0) & 0xFF;
    if (c >= 'a' && c <= 'z') RET(c - 32, 1);
    if (c >= 'A' && c <= 'Z') RET(0x100 | c, 1);
    if (c >= '0' && c <= '9') RET(c, 1);
    if (c == ' ') RET(0x20, 1);
    RET(0xFFFF, 1);
}
STUB(ext_user32_dll_VkKeyScanW) { u32 c = arg(0) & 0xFFFF; RET(c >= 'a' && c <= 'z' ? c - 32 : c >= 'A' && c <= 'Z' ? 0x100 | c : c >= '0' && c <= '9' ? c : c == ' ' ? 0x20 : 0xFFFF, 1); }
STUB(ext_user32_dll_ToAscii) {   // (vk, scan, keystate[256], out WORD*, flags)
    u32 vk = arg(0), ks = arg(2), out = arg(3);
    bool shift = ks && (rd8(ks + 0x10) & 0x80);
    u32 c = 0;
    if (vk >= 'A' && vk <= 'Z') c = shift ? vk : vk + 32;
    else if (vk >= '0' && vk <= '9') c = vk;
    else if (vk == 0x20 || vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09) c = vk;
    if (!c) RET(0, 5);
    wr16(out, c);
    RET(1, 5);
}
STUB(ext_user32_dll_GetDoubleClickTime) { RET(500, 0); }
STUB(ext_user32_dll_SetMessageQueue) { RET(1, 1); }
STUB(ext_user32_dll_AttachThreadInput) { RET(1, 3); }
STUB(ext_user32_dll_WaitForInputIdle) { RET(0, 2); }
STUB(ext_user32_dll_SetWindowContextHelpId) { RET(1, 2); }

// DDE: no hay otros programas con los que hablar
STUB(ext_user32_dll_DdeInitializeA) { if (arg(0)) wr32(arg(0), 0x5100); RET(0, 4); }
STUB(ext_user32_dll_DdeUninitialize) { RET(1, 1); }
STUB(ext_user32_dll_DdeConnect) { RET(0, 4); }
STUB(ext_user32_dll_DdeDisconnect) { RET(1, 1); }
STUB(ext_user32_dll_DdeClientTransaction) { RET(0, 8); }
STUB(ext_user32_dll_DdeAbandonTransaction) { RET(1, 3); }
STUB(ext_user32_dll_DdeCreateStringHandleA) { RET(0x5200, 3); }
STUB(ext_user32_dll_DdeFreeStringHandle) { RET(1, 2); }
STUB(ext_user32_dll_DdeQueryStringA) { if (arg(2) && arg(3)) wr8(arg(2), 0); RET(0, 5); }
STUB(ext_user32_dll_DdeCmpStringHandles) { RET(arg(0) == arg(1) ? 0 : 1, 2); }
STUB(ext_user32_dll_DdeCreateDataHandle) { RET(0, 7); }
STUB(ext_user32_dll_DdeFreeDataHandle) { RET(1, 1); }
STUB(ext_user32_dll_DdeGetData) { RET(0, 4); }
STUB(ext_user32_dll_DdeGetLastError) { RET(0x4009, 1); }   // DMLERR_NO_CONV_ESTABLISHED
STUB(ext_user32_dll_DdeNameService) { RET(1, 4); }
STUB(ext_user32_dll_DdePostAdvise) { RET(1, 3); }
STUB(ext_user32_dll_DdeQueryConvInfo) { RET(0, 3); }
STUB(ext_user32_dll_DdeSetUserHandle) { RET(1, 3); }
STUB(ext_user32_dll_ReuseDDElParam) { RET(arg(0), 5); }
STUB(ext_user32_dll_UnpackDDElParam) { if (arg(2)) wr32(arg(2), arg(1) & 0xFFFF); if (arg(3)) wr32(arg(3), arg(1) >> 16); RET(1, 4); }

// ---------------------------------------------------------------- VERSION y SHELL32

// los módulos traducidos no llevan recurso de versión accesible: como un fichero sin VERSIONINFO
STUB(ext_version_dll_GetFileVersionInfoSizeA) { if (arg(1)) wr32(arg(1), 0); set_last_error(1813); RET(0, 2); }
STUB(ext_version_dll_GetFileVersionInfoA) { set_last_error(1813); RET(0, 4); }
STUB(ext_version_dll_VerQueryValueA) { if (arg(2)) wr32(arg(2), 0); if (arg(3)) wr32(arg(3), 0); RET(0, 4); }
STUB(ext_shell32_dll_ShellExecuteA) { plog("ShellExecuteA(\"%s\", \"%s\"): no soportado", gstr(arg(1)).c_str(), gstr(arg(2)).c_str()); RET(2, 6); }

// ---------------------------------------------------------------- mapas de bits e imágenes

// mapa de bits compatible con la pantalla a partir de un DIB empaquetado (BITMAPINFOHEADER + paleta + bits)
static u32 bitmap_from_packed_dib(u32 p) {
    u32 hdr = rd32(p), bpp = rd16(p + 14), comp = rd32(p + 16), used = rd32(p + 32);
    i32 w = (i32)rd32(p + 4), h = (i32)rd32(p + 8);
    if (hdr < 12 || hdr > 124 || w <= 0 || w > 8192 || h == 0 || h > 8192 || h < -8192) {
        plog("mapa de bits no válido en 0x%x (cabecera %u, %dx%d, %u bpp)", p, hdr, w, h, bpp);
        return 0;
    }
    u32 ncol = used ? used : (bpp <= 8 ? 1u << bpp : 0);
    if (comp == 3) ncol += 3;   // BI_BITFIELDS: tres máscaras
    u32 bits = p + hdr + 4 * ncol;
    u32 dc = gdi_window_dc(0, true);
    u32 hbm = call_stub(ext_gdi32_dll_CreateDIBitmap, {dc, p, 4 /*CBM_INIT*/, bits, p, 0 /*DIB_RGB_COLORS*/});
    gdi_release_dc(dc);
    return hbm;
}
static u32 load_bitmap_resource(u32 hinst, u32 name) {
    if (!hinst) return 0;   // mapas de bits OEM: no hay
    u32 res = call_stub(ext_kernel32_dll_FindResourceA, {hinst, name, 2 /*RT_BITMAP*/});
    u32 p = res ? call_stub(ext_kernel32_dll_LoadResource, {hinst, res}) : 0;
    return p ? bitmap_from_packed_dib(p) : 0;
}
static u32 load_bitmap_file(const std::string &winpath) {
    std::ifstream in(host_path(winpath), std::ios::binary);
    if (!in) return 0;
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 14 + 40 || data[0] != 'B' || data[1] != 'M') return 0;
    u32 p = heap_alloc((u32)data.size() - 14, false);
    for (size_t i = 14; i < data.size(); i++) wr8(p + (u32)(i - 14), (u8)data[i]);
    u32 hbm = bitmap_from_packed_dib(p);
    heap_free(p);
    return hbm;
}
STUB(ext_user32_dll_LoadBitmapA) { RET(load_bitmap_resource(arg(0), arg(1)), 2); }
STUB(ext_user32_dll_LoadImageA) {   // (hinst, nombre, tipo, cx, cy, fuLoad)
    u32 hinst = arg(0), name = arg(1), type = arg(2), flags = arg(5);
    bool file = flags & 0x10;   // LR_LOADFROMFILE
    u32 h = 0;
    switch (type) {
    case 0:   // IMAGE_BITMAP
        h = file ? load_bitmap_file(gstr(name)) : load_bitmap_resource(hinst, name);
        break;
    case 1:   // IMAGE_ICON
        h = call_stub(ext_user32_dll_LoadIconA, {hinst, name});
        break;
    case 2:   // IMAGE_CURSOR
        h = file ? call_stub(ext_user32_dll_LoadCursorFromFileA, {name}) : call_stub(ext_user32_dll_LoadCursorA, {hinst, name});
        break;
    }
    if (!h) plog("LoadImageA(tipo %u, %s) -> 0", type, name < 0x10000 ? ("#" + std::to_string(name)).c_str() : gstr(name).c_str());
    RET(h, 6);
}
