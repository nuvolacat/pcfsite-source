// PCF7 — KERNEL32: ficheros, directorios, unidades, ficheros mapeados y ficheros INI.
#include "handles.h"

u32 ticks_ms();

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>

#include <cstdlib>
static const bool g_trace_kfile = std::getenv("PCF7_TRACE_KFILE") != nullptr;   // traza de las APIs de ficheros
static const u64 EPOCH_DIFF_FT = 116444736000000000ull;   // 1601 → 1970 en unidades de 100 ns
#define TK(...) do { if (g_trace_kfile) plog(__VA_ARGS__); } while (0)

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

Handles &handles() { static Handles h; return h; }

KFile::~KFile() {
    if (fd >= 0) ::close(fd);
    if (escribe) web_abierto(path, -1);
}

static u32 win_error_from_errno() {
    switch (errno) {
        case ENOENT: return 2;     // ERROR_FILE_NOT_FOUND
        case ENOTDIR: return 3;    // ERROR_PATH_NOT_FOUND
        case EACCES: case EPERM: return 5;
        case EEXIST: return 80;    // ERROR_FILE_EXISTS
        default: return 31;
    }
}

static u32 attrs_of(const struct stat &st, const std::string &name) {
    u32 a = S_ISDIR(st.st_mode) ? 0x10 : 0x20;      // DIRECTORY / ARCHIVE
    if (!(st.st_mode & S_IWUSR)) a |= 0x01;          // READONLY
    if (!name.empty() && name[0] == '.' && name != "." && name != "..") a |= 0x02;
    return a;
}

// ---------------------------------------------------------------- abrir/leer/escribir

// Lee n bytes como ReadFile de Windows: solo se queda corto al final del fichero. read()/pread() de POSIX pueden
// devolver menos (el sistema de ficheros de la web entrega los datos por trozos). off < 0 = posición actual.
static ssize_t read_full(int fd, u8 *buf, u32 n, int64_t off = -1) {
    // PCF7_LECTURA_LENTA=cada:ms[:desde]: desde el ms `desde`, una de cada `cada` lecturas tarda `ms` (simula un
    // fichero que aún se descarga en la versión web)
    static const char *ll = std::getenv("PCF7_LECTURA_LENTA");
    if (ll) {
        static unsigned cada = 0, ms = 0, desde = 0, k = 0;
        if (!cada) std::sscanf(ll, "%u:%u:%u", &cada, &ms, &desde);
        if (cada && ticks_ms() >= desde && ++k % cada == 0) { usleep(ms * 1000); }
    }
    u32 hecho = 0;
    while (hecho < n) {
        ssize_t r = off < 0 ? ::read(fd, buf + hecho, n - hecho) : ::pread(fd, buf + hecho, n - hecho, off + hecho);
        if (r < 0) { if (errno == EINTR) continue; return hecho ? (ssize_t)hecho : -1; }
        if (r == 0) break;
        hecho += (u32)r;
    }
    return hecho;
}

STUB(ext_kernel32_dll_CreateFileA) {
    std::string w = gstr(arg(0));
    TK("CreateFileA(%s, acceso=%x, disp=%u)", w.c_str(), arg(1), arg(4));
    u32 access = arg(1), disp = arg(4);
    bool writes = (access & 0x40000000u) || disp == 1 || disp == 2 || disp == 4 || disp == 5;
    std::string p = writes ? host_path_w(w) : host_path(w);
    int flags = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
        case 1: flags |= O_CREAT | O_EXCL; break;   // CREATE_NEW
        case 2: flags |= O_CREAT | O_TRUNC; break;  // CREATE_ALWAYS
        case 3: break;                              // OPEN_EXISTING
        case 4: flags |= O_CREAT; break;            // OPEN_ALWAYS
        case 5: flags |= O_TRUNC; break;            // TRUNCATE_EXISTING
    }
    flags |= O_BINARY;
    int fd = ::open(p.c_str(), flags, 0644);
    if (fd < 0 && errno == ENOENT && (flags & O_CREAT)) {
        // carpetas que creaba el instalador original (save\manager\022-0…): se crean al escribir
        for (size_t k = p.find('/', 1); k != std::string::npos; k = p.find('/', k + 1)) ::mkdir(p.substr(0, k).c_str(), 0755);
        fd = ::open(p.c_str(), flags, 0644);
    }
    if (fd < 0 && (access & 0x40000000u) && errno == EACCES) fd = ::open(p.c_str(), O_RDONLY | O_BINARY);   // CD de solo lectura
    if (fd < 0) {
        set_last_error(win_error_from_errno());
        plog("CreateFileA(\"%s\") -> no existe (%s)", w.c_str(), p.c_str());
        RET(0xFFFFFFFFu, 7);
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { ::close(fd); set_last_error(5); RET(0xFFFFFFFFu, 7); }
    auto f = std::make_shared<KFile>();
    f->fd = fd; f->path = p;
    if (writes) { f->escribe = true; web_abierto(p, +1); }
    set_last_error(disp == 2 || disp == 4 ? 0 : 0);
    RET(handles().add(f), 7);
}

namespace { void volcar_vistas_de(const std::string &path); }
STUB(ext_kernel32_dll_ReadFile) {
    TK("ReadFile(h=%x, n=%u)", arg(0), arg(2));
    u32 buf = arg(1), n = arg(2), pread_ = arg(3);
    auto f = handles().get<KFile>(arg(0));
    if (!f) { set_last_error(6); RET(0, 5); }
    volcar_vistas_de(f->path);
    if ((u64)buf + n > MEMSZ) n = MEMSZ - buf;
    ssize_t r = read_full(f->fd, MEM + buf, n);
    if (pread_) wr32(pread_, r > 0 ? (u32)r : 0);
    RET(r >= 0, 5);
}
STUB(ext_kernel32_dll_WriteFile) {
    TK("WriteFile(h=%x, n=%u)", arg(0), arg(2));
    u32 buf = arg(1), n = arg(2), pw = arg(3);
    u32 h = arg(0);
    if (h == 0x14 || h == 0x18) {   // stdout/stderr
        fwrite(MEM + buf, 1, n, stderr);
        if (pw) wr32(pw, n);
        RET(1, 5);
    }
    auto f = handles().get<KFile>(h);
    if (!f) { set_last_error(6); RET(0, 5); }
    ssize_t r = ::write(f->fd, MEM + buf, n);
    if (pw) wr32(pw, r > 0 ? (u32)r : 0);
    RET(r >= 0, 5);
}
STUB(ext_kernel32_dll_SetFilePointer) {
    TK("SetFilePointer(h=%x, %d, modo %u)", arg(0), (int)arg(1), arg(3));
    auto f = handles().get<KFile>(arg(0));
    if (!f) { set_last_error(6); RET(0xFFFFFFFFu, 4); }
    i64 off = (i32)arg(1);
    if (arg(2)) off = (i64)(((u64)rd32(arg(2)) << 32) | (u32)arg(1));
    int whence = arg(3) == 0 ? SEEK_SET : arg(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = ::lseek(f->fd, off, whence);
    if (r < 0) { set_last_error(131); RET(0xFFFFFFFFu, 4); }
    if (arg(2)) wr32(arg(2), (u32)((u64)r >> 32));
    RET((u32)r, 4);
}
STUB(ext_kernel32_dll_GetFileSize) {
    TK("GetFileSize(h=%x)", arg(0));
    auto f = handles().get<KFile>(arg(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0xFFFFFFFFu, 2);
    if (arg(1)) wr32(arg(1), (u32)((u64)st.st_size >> 32));
    RET((u32)st.st_size, 2);
}
STUB(ext_kernel32_dll_GetFileType) {
    u32 h = arg(0);
    RET(h >= 0x10 && h <= 0x18 ? 2u : handles().get<KFile>(h) ? 1u : 0u, 1);   // CHAR / DISK / UNKNOWN
}
STUB(ext_kernel32_dll_FlushFileBuffers) { RET(1, 1); }
STUB(ext_kernel32_dll_SetEndOfFile) {
    TK("SetEndOfFile(h=%x)", arg(0));
    auto f = handles().get<KFile>(arg(0));
    if (!f) RET(0, 1);
    off_t pos = ::lseek(f->fd, 0, SEEK_CUR);
    RET(::ftruncate(f->fd, pos) == 0, 1);
}
STUB(ext_kernel32_dll_LockFile) { RET(1, 5); }
STUB(ext_kernel32_dll_UnlockFile) { RET(1, 5); }

static void put_filetimes(u32 c, u32 a, u32 w, const struct stat &st) {
    u64 ft = unix_to_filetime(st.st_mtime);
    if (c) wr64(c, ft);
    if (a) wr64(a, ft);
    if (w) wr64(w, ft);
}
STUB(ext_kernel32_dll_GetFileTime) {
    TK("GetFileTime(h=%x)", arg(0));
    auto f = handles().get<KFile>(arg(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0, 4);
    put_filetimes(arg(1), arg(2), arg(3), st);
    RET(1, 4);
}
STUB(ext_kernel32_dll_SetFileTime) {   // (h, creación*, acceso*, escritura*): la fecha de modificación, de verdad (el
    // parcheador RTPatch pone la del registro del parche y luego la comprueba al verificar el fichero)
    auto f = handles().get<KFile>(arg(0));
    if (!f) { set_last_error(6); RET(0, 4); }
    u32 pw = arg(3) ? arg(3) : arg(2);
    if (!pw) RET(1, 4);
    u64 ft = rd64(pw);
    if (ft < EPOCH_DIFF_FT) RET(1, 4);
    u64 u = ft - EPOCH_DIFF_FT;
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)(u / 10000000ull); ts[0].tv_nsec = (long)((u % 10000000ull) * 100);
    ts[1] = ts[0];
    if (::futimens(f->fd, ts) != 0) { set_last_error(5); RET(0, 4); }
    web_sucio(f->path);
    RET(1, 4);
}
STUB(ext_kernel32_dll_GetFileInformationByHandle) {
    auto f = handles().get<KFile>(arg(0));
    struct stat st;
    u32 p = arg(1);
    if (!f || fstat(f->fd, &st)) RET(0, 2);
    std::memset(MEM + p, 0, 52);
    wr32(p, attrs_of(st, ""));
    put_filetimes(p + 4, p + 12, p + 20, st);
    wr32(p + 28, 0x1234); wr32(p + 32, (u32)((u64)st.st_size >> 32)); wr32(p + 36, (u32)st.st_size);
    wr32(p + 40, 1); wr32(p + 44, (u32)(st.st_ino >> 32)); wr32(p + 48, (u32)st.st_ino);
    RET(1, 2);
}

// ---------------------------------------------------------------- atributos, directorios

STUB(ext_kernel32_dll_GetFileAttributesA) {
    std::string p = host_path(gstr(arg(0)));
    struct stat st;
    if (::stat(p.c_str(), &st)) { TK("GetFileAttributesA(%s) -> no existe (%s)", gstr(arg(0)).c_str(), p.c_str()); set_last_error(2); RET(0xFFFFFFFFu, 1); }
    TK("GetFileAttributesA(%s) -> %x (%s)", gstr(arg(0)).c_str(), attrs_of(st, ""), p.c_str());
    RET(attrs_of(st, ""), 1);
}
STUB(ext_kernel32_dll_SetFileAttributesA) { RET(1, 2); }
STUB(ext_kernel32_dll_DeleteFileA) {
    TK("DeleteFileA(%s)", gstr(arg(0)).c_str());
    std::string p = host_path_w(gstr(arg(0)));
    if (std::getenv("PCF7_NO_DELETE")) { plog("DeleteFileA(\"%s\"): conservado (PCF7_NO_DELETE)", gstr(arg(0)).c_str()); RET(1, 1); }   // diagnóstico
    if (::unlink(p.c_str())) { set_last_error(win_error_from_errno()); RET(0, 1); }
    RET(1, 1);
}
STUB(ext_kernel32_dll_CopyFileA) {
    std::string a = host_path(gstr(arg(0))), b = host_path_w(gstr(arg(1)));
    if (arg(2) && ::access(b.c_str(), F_OK) == 0) { set_last_error(80); RET(0, 3); }
    std::ifstream in(a, std::ios::binary);
    if (!in) { set_last_error(2); RET(0, 3); }
    std::ofstream out(b, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
    RET(out.good(), 3);
}
STUB(ext_kernel32_dll_MoveFileA) {
    TK("MoveFileA(%s -> %s)", gstr(arg(0)).c_str(), gstr(arg(1)).c_str());
    std::string a = host_path_w(gstr(arg(0))), b = host_path_w(gstr(arg(1)));
    RET(pcf_rename(a.c_str(), b.c_str()) == 0, 2);
}
STUB(ext_kernel32_dll_CreateDirectoryA) {
    std::string p = host_path_w(gstr(arg(0)));
    if (::mkdir(p.c_str(), 0755)) { set_last_error(errno == EEXIST ? 183 : win_error_from_errno()); RET(0, 2); }
    RET(1, 2);
}
STUB(ext_kernel32_dll_RemoveDirectoryA) { RET(::rmdir(host_path_w(gstr(arg(0))).c_str()) == 0, 1); }

STUB(ext_kernel32_dll_GetCurrentDirectoryA) {
    u32 cap = arg(0), buf = arg(1);
    if (cap < g_cwd.size() + 1) RET(g_cwd.size() + 1, 2);
    RET(gstrcpy(buf, g_cwd, cap), 2);
}
STUB(ext_kernel32_dll_SetCurrentDirectoryA) {
    std::string w = gstr(arg(0));
    std::string p = host_path(w);
    struct stat st;
    if (::stat(p.c_str(), &st) || !S_ISDIR(st.st_mode)) { set_last_error(3); RET(0, 1); }
    // normaliza: absoluta, sin barra final
    if (w.size() >= 2 && w[1] == ':') g_cwd = w;
    else if (!w.empty() && w[0] == '\\') g_cwd = g_cwd.substr(0, 2) + w;
    else g_cwd = g_cwd + "\\" + w;
    while (g_cwd.size() > 3 && (g_cwd.back() == '\\' || g_cwd.back() == '/')) g_cwd.pop_back();
    RET(1, 1);
}
STUB(ext_kernel32_dll_GetFullPathNameA) {
    std::string w = gstr(arg(0));
    u32 cap = arg(1), buf = arg(2), pfile = arg(3);
    std::string full;
    if (w.size() >= 2 && w[1] == ':') full = w;
    else if (!w.empty() && (w[0] == '\\' || w[0] == '/')) full = g_cwd.substr(0, 2) + w;
    else full = g_cwd + (g_cwd.back() == '\\' ? "" : "\\") + w;
    for (auto &c : full) if (c == '/') c = '\\';
    if (cap < full.size() + 1) RET(full.size() + 1, 4);
    gstrcpy(buf, full, cap);
    if (pfile) {
        size_t s = full.find_last_of('\\');
        wr32(pfile, buf + (u32)(s == std::string::npos ? 0 : s + 1));
    }
    RET(full.size(), 4);
}
STUB(ext_kernel32_dll_GetShortPathNameA) { u32 n = gstrcpy(arg(1), gstr(arg(0)), arg(2)); RET(n, 3); }
STUB(ext_kernel32_dll_GetWindowsDirectoryA) { RET(gstrcpy(arg(0), "C:\\WINDOWS", arg(1)), 2); }
STUB(ext_kernel32_dll_GetSystemDirectoryA) { RET(gstrcpy(arg(0), "C:\\WINDOWS\\SYSTEM", arg(1)), 2); }
STUB(ext_kernel32_dll_GetTempPathA) { RET(gstrcpy(arg(1), "C:\\WINDOWS\\TEMP\\", arg(0)), 2); }
STUB(ext_kernel32_dll_GetTempFileNameA) {
    static u32 n = 1;
    std::string s = gstr(arg(0)) + "\\" + gstr(arg(1)).substr(0, 3) + std::to_string(n++) + ".TMP";
    gstrcpy(arg(3), s);
    RET(n, 4);
}

// ---------------------------------------------------------------- búsquedas (FindFirstFile)

static bool wild_match(const char *pat, const char *s) {
    // comodines de Windows, sin distinguir mayúsculas; "*.*" casa con todo
    if (!std::strcmp(pat, "*.*")) return true;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            for (; *s; s++) if (wild_match(pat, s)) return true;
            return !std::strcmp(pat, ".*") || !std::strcmp(pat, ".");
        }
        if (!*s) return !std::strcmp(pat, ".*") || !std::strcmp(pat, ".");
        if (*pat != '?' && std::tolower((unsigned char)*pat) != std::tolower((unsigned char)*s)) return false;
        pat++; s++;
    }
    return !*s;
}

static void put_find_data(u32 p, const KFindEntry &e) {
    std::memset(MEM + p, 0, 320);
    wr32(p, e.attrs);
    u64 ft = unix_to_filetime(e.mtime);
    wr64(p + 4, ft); wr64(p + 12, ft); wr64(p + 20, ft);
    wr32(p + 28, (u32)(e.size >> 32)); wr32(p + 32, (u32)e.size);
    gstrcpy(p + 44, e.name, 260);
    gstrcpy(p + 304, e.name.size() <= 12 ? e.name : e.name.substr(0, 12), 14);
}

STUB(ext_kernel32_dll_FindFirstFileA) {
    TK("FindFirstFileA(%s)", gstr(arg(0)).c_str());
    std::string w = gstr(arg(0));
    for (auto &c : w) if (c == '/') c = '\\';
    size_t s = w.find_last_of('\\');
    std::string dir = s == std::string::npos ? "." : (s == 2 && w[1] == ':' ? w.substr(0, 3) : w.substr(0, s));
    std::string pat = s == std::string::npos ? w : w.substr(s + 1);
    auto f = std::make_shared<KFind>();
    std::set<std::string> seen;   // en el navegador la carpeta son dos (escribible y original): manda la primera
    for (const std::string &hd : host_dirs(dir)) {
        DIR *d = opendir(hd.c_str());
        if (!d) continue;
        while (dirent *e = readdir(d)) {
            if (!wild_match(pat.c_str(), e->d_name)) continue;
            std::string up = e->d_name;
            for (auto &c : up) c = (char)std::toupper((unsigned char)c);
            if (!seen.insert(up).second) continue;
            struct stat st;
            if (::stat((hd + "/" + e->d_name).c_str(), &st)) continue;
            f->entries.push_back({e->d_name, attrs_of(st, e->d_name), (u64)st.st_size, (i64)st.st_mtime});
        }
        closedir(d);
    }
    if (f->entries.empty()) { set_last_error(2); RET(0xFFFFFFFFu, 2); }
    // orden alfabético sin mayúsculas, como en NTFS: así no depende del sistema de ficheros del host (APFS, WasmFS…)
    std::sort(f->entries.begin(), f->entries.end(), [](const auto &a, const auto &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    put_find_data(arg(1), f->entries[0]);
    f->next = 1;
    RET(handles().add(f), 2);
}
STUB(ext_kernel32_dll_FindNextFileA) {
    auto f = handles().get<KFind>(arg(0));
    if (!f || f->next >= f->entries.size()) { set_last_error(18); RET(0, 2); }   // ERROR_NO_MORE_FILES
    put_find_data(arg(1), f->entries[f->next++]);
    RET(1, 2);
}
STUB(ext_kernel32_dll_FindClose) { handles().close(arg(0)); RET(1, 1); }

// ---------------------------------------------------------------- unidades: C: (disco), D: (CD-ROM)

STUB(ext_kernel32_dll_GetLogicalDrives) { RET(0x0C, 0); }   // C, D
STUB(ext_kernel32_dll_GetLogicalDriveStringsA) {
    u32 cap = arg(0), buf = arg(1);
    static const char s[] = "C:\\\0D:\\\0";
    if (cap < sizeof s) RET(sizeof s, 2);
    for (u32 i = 0; i < sizeof s; i++) wr8(buf + i, (u8)s[i]);
    RET(sizeof s - 1, 2);
}
STUB(ext_kernel32_dll_GetDriveTypeA) {
    std::string r = gstr(arg(0));
    char d = r.empty() ? g_cwd[0] : (char)std::toupper((unsigned char)r[0]);
    RET(d == 'C' ? 3u : d == 'D' ? 5u : 1u, 1);    // FIXED / CDROM / NO_ROOT_DIR
}
STUB(ext_kernel32_dll_GetVolumeInformationA) {
    std::string r = gstr(arg(0));
    char d = r.empty() ? g_cwd[0] : (char)std::toupper((unsigned char)r[0]);
    if (arg(1)) gstrcpy(arg(1), d == 'D' ? "PCF7" : "DISCO_C", arg(2));
    if (arg(3)) wr32(arg(3), d == 'D' ? 0x19981228u : 0x12345678u);
    if (arg(4)) wr32(arg(4), 255);
    if (arg(5)) wr32(arg(5), 0);
    if (arg(6)) gstrcpy(arg(6), d == 'D' ? "CDFS" : "FAT32", arg(7));
    RET(1, 8);
}
STUB(ext_kernel32_dll_GetDiskFreeSpaceA) {
    if (arg(1)) wr32(arg(1), 8);         // sectores por clúster
    if (arg(2)) wr32(arg(2), 512);       // bytes por sector
    if (arg(3)) wr32(arg(3), 0x7FFF0);   // clústeres libres (~2 GB, el máximo que entendía Win9x)
    if (arg(4)) wr32(arg(4), 0xFFFF0);
    RET(1, 5);
}
STUB(ext_kernel32_dll_SearchPathA) { RET(0, 6); }

// ---------------------------------------------------------------- ficheros mapeados
// La vista se copia en una zona propia de MEM (mm::MAP_LO…): el juego lee los datos por puntero. Las vistas de
// escritura (FILE_MAP_WRITE sobre un mapeo PAGE_READWRITE: el parcheador RTPatch escribe así el fichero nuevo) se
// vuelcan al fichero al desmapear; CreateFileMapping con un tamaño mayor que el fichero lo extiende, como en Windows.

namespace {
u32 g_maptop = mm::MAP_LO;
struct Vista { u32 n = 0, off = 0, raw = 0, refs = 1; bool escribe = false; std::shared_ptr<KMapping> m; };   // raw ≠ 0: bloque del montículo
std::map<u32, Vista> g_vistas;   // dirección → vista
bool hay_vistas_zona() { for (auto &v : g_vistas) if (!v.second.raw) return true; return false; }

void volcar_vista(u32 p, const Vista &v) {
    if (!v.escribe || !v.m || !v.m->file) return;
    u32 n = v.n;
    u32 hecho = 0;
    while (hecho < n) {
        ssize_t w = ::pwrite(v.m->file->fd, MEM + p + hecho, n - hecho, (off_t)v.off + hecho);
        if (w <= 0) { plog("UnmapViewOfFile: no se puede escribir %s", v.m->file->path.c_str()); break; }
        hecho += (u32)w;
    }
    web_sucio(v.m->file->path);
}
// coherencia con las vistas de escritura abiertas: antes de leer el fichero por otra vía (ReadFile, otra vista), lo
// escrito en ellas tiene que estar en el fichero (en Windows comparten la memoria)
void volcar_vistas_de(const std::string &path) {
    for (auto &kv : g_vistas) if (kv.second.escribe && kv.second.m && kv.second.m->file && kv.second.m->file->path == path) volcar_vista(kv.first, kv.second);
}
}  // namespace

STUB(ext_kernel32_dll_CreateFileMappingA) {
    TK("CreateFileMappingA(h=%x, prot=%x, tam=%u)", arg(0), arg(2), arg(4));
    auto f = handles().get<KFile>(arg(0));
    if (!f) { set_last_error(6); RET(0, 6); }
    struct stat st;
    fstat(f->fd, &st);
    auto m = std::make_shared<KMapping>();
    m->file = f;
    m->escribe = (arg(2) & 0x0C) != 0;   // PAGE_READWRITE (4) / PAGE_WRITECOPY (8)
    m->size = arg(4) ? arg(4) : (u32)st.st_size;
    if (m->escribe && arg(4) && (u64)arg(4) > (u64)st.st_size) {
        if (::ftruncate(f->fd, (off_t)arg(4)) != 0) { set_last_error(112); RET(0, 6); }   // ERROR_DISK_FULL
        f->escribe = true;
    }
    RET(handles().add(m), 6);
}
STUB(ext_kernel32_dll_MapViewOfFile) {
    TK("MapViewOfFile(m=%x, acceso=%x, off=%u, n=%u)", arg(0), arg(1), arg(3), arg(4));
    auto m = handles().get<KMapping>(arg(0));
    if (!m) { set_last_error(6); RET(0, 5); }
    u32 acceso = arg(1), off = arg(3), n = arg(4);
    if (off > m->size) { set_last_error(87); RET(0, 5); }
    if (!n || off + n > m->size) n = m->size - off;
    Vista v;
    v.n = n; v.off = off; v.m = m;
    v.escribe = m->escribe && (acceso & 0x2) != 0;   // FILE_MAP_WRITE (FILE_MAP_ALL_ACCESS la incluye)
    // Una vista de solo lectura del mismo trozo del mismo fichero que otra abierta comparte su memoria (el contenido es
    // el mismo y nadie la puede escribir). El 6.0 Edición de Oro mapea SFX\COMPCF6.PKF (100 MB) otra vez al empezar el
    // partido sin soltar la primera: en Windows sobra espacio; aquí no cabían las dos y el juego pasaba a leer el
    // fichero a trozos de 1-4 bytes (decenas de miles de lecturas durante el partido: tirones).
    if (!v.escribe && m->file)
        for (auto &kv : g_vistas)
            if (!kv.second.escribe && kv.second.off == off && kv.second.n == n && kv.second.m && kv.second.m->file &&
                kv.second.m->file->path == m->file->path) { kv.second.refs++; RET(kv.first, 5); }
    u32 p = (g_maptop + 0xFFFFu) & ~0xFFFFu;
    if ((u64)p + n > mm::MAP_HI) {
        // sin sitio: se reutiliza la zona si ya no hay vistas (el juego mapea y desmapea por partes)
        if (!hay_vistas_zona()) p = mm::MAP_LO;
        if ((u64)p + n > mm::MAP_HI) {
            // zona llena (PCF7 Plus tiene muchas vistas abiertas a la vez): la vista sale del montículo
            u32 raw = heap_alloc(n + 0x10000, false);
            if (!raw) { plog("MapViewOfFile: sin espacio para %u B", n); set_last_error(8); RET(0, 5); }
            u32 q = (raw + 0xFFFFu) & ~0xFFFFu;
            volcar_vistas_de(m->file->path);
            ssize_t rr = read_full(m->file->fd, MEM + q, n, (int64_t)off);
            u32 r = rr > 0 ? (u32)rr : 0;
            if (r < n) std::memset(MEM + q + r, 0, n - r);
            v.raw = raw;
            g_vistas[q] = v;
            RET(q, 5);
        }
    }
    volcar_vistas_de(m->file->path);
    ssize_t rr = read_full(m->file->fd, MEM + p, n, (int64_t)off);
    u32 r = rr > 0 ? (u32)rr : 0;
    if (r < n) std::memset(MEM + p + r, 0, n - r);
    g_vistas[p] = v;
    g_maptop = p + n;
    RET(p, 5);
}
STUB(ext_kernel32_dll_FlushViewOfFile) {   // (dirección, bytes): se vuelca la vista entera
    for (auto &kv : g_vistas)
        if (arg(0) >= kv.first && arg(0) < kv.first + kv.second.n) { volcar_vista(kv.first, kv.second); RET(1, 2); }
    RET(0, 2);
}
STUB(ext_kernel32_dll_UnmapViewOfFile) {
    TK("UnmapViewOfFile(%x)", arg(0));
    auto it = g_vistas.find(arg(0));
    if (it == g_vistas.end()) RET(0, 1);
    if (--it->second.refs) RET(1, 1);   // vista compartida: sigue para las demás
    volcar_vista(it->first, it->second);
    if (it->second.raw) heap_free(it->second.raw);
    g_vistas.erase(it);
    if (!hay_vistas_zona()) g_maptop = mm::MAP_LO;
    RET(1, 1);
}

// ---------------------------------------------------------------- ficheros INI

namespace {
using Ini = std::map<std::string, std::map<std::string, std::string>>;

std::string lc(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Ini read_ini(const std::string &winpath) {
    std::string w = winpath;
    if (w.find('\\') == std::string::npos && w.find(':') == std::string::npos) w = "C:\\WINDOWS\\" + w;
    Ini ini;
    std::ifstream in(host_path(w));
    std::string line, sec;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') { sec = lc(trim(line.substr(1, line.find(']') - 1))); continue; }
        size_t eq = line.find('=');
        if (eq != std::string::npos) ini[sec][lc(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }
    return ini;
}
}  // namespace

STUB(ext_kernel32_dll_GetPrivateProfileStringA) {
    std::string sec = gstr(arg(0)), key = gstr(arg(1)), def = gstr(arg(2)), file = gstr(arg(5));
    u32 buf = arg(3), cap = arg(4);
    Ini ini = read_ini(file);
    std::string v = def;
    if (!arg(0) || !arg(1)) {
        plog_once("inienum", "GetPrivateProfileStringA: enumeración no soportada");
        if (cap >= 2) { wr8(buf, 0); wr8(buf + 1, 0); }
        RET(0, 6);
    }
    auto s = ini.find(lc(sec));
    if (s != ini.end()) { auto k = s->second.find(lc(key)); if (k != s->second.end()) v = k->second; }
    RET(gstrcpy(buf, v, cap), 6);
}
STUB(ext_kernel32_dll_GetPrivateProfileIntA) {
    std::string sec = gstr(arg(0)), key = gstr(arg(1)), file = gstr(arg(3));
    Ini ini = read_ini(file);
    auto s = ini.find(lc(sec));
    if (s != ini.end()) { auto k = s->second.find(lc(key)); if (k != s->second.end()) RET((u32)std::strtol(k->second.c_str(), nullptr, 10), 4); }
    RET(arg(2), 4);
}
STUB(ext_kernel32_dll_GetProfileIntA) { RET(arg(2), 3); }

