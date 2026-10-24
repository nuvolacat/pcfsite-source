// lanzador — utilidades (ver util.h)
#include "util.h"

#include "json.h"

#include "../../core/platform/src/sha256.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <shlobj.h>
#else
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <strings.h>

#ifndef _WIN32
extern char **environ;
#endif

namespace util {

bool existe(const std::string &p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
bool es_dir(const std::string &p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
uint64_t tam(const std::string &p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0; }

bool mkdirs(const std::string &p) {
    if (p.empty()) return false;
    if (es_dir(p)) return true;
    std::string padre = carpeta_de(p);
    if (!padre.empty() && padre != p && !mkdirs(padre)) return false;
    return ::mkdir(p.c_str(), 0755) == 0 || es_dir(p);
}

bool borrar_arbol(const std::string &p) {
    struct stat st;
    if (::lstat(p.c_str(), &st) != 0) return true;
    if (S_ISDIR(st.st_mode)) {
        for (const std::string &n : listar(p)) borrar_arbol(p + "/" + n);
        return ::rmdir(p.c_str()) == 0;
    }
    return ::unlink(p.c_str()) == 0;
}

std::vector<std::string> listar(const std::string &dir) {
    std::vector<std::string> out;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            out.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

std::string nombre_base(const std::string &p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}
std::string carpeta_de(const std::string &p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? "" : i == 0 ? "/" : p.substr(0, i);
}
std::string normaliza(std::string p) {
    for (auto &c : p) if (c == '\\') c = '/';
    while (p.size() > 1 && p.back() == '/' && !(p.size() == 3 && p[1] == ':')) p.pop_back();
    return p;
}
std::string nativa(std::string p) {
#ifdef _WIN32
    for (auto &c : p) if (c == '/') c = '\\';
#endif
    return p;
}

std::string resolver(const std::string &base, const std::string &rel) {
    std::string cur = base;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = rel.find_first_of("/\\", i);
        if (j == std::string::npos) j = rel.size();
        std::string c = rel.substr(i, j - i);
        i = j + 1;
        if (c.empty() || c == ".") continue;
        std::string hallado;
        for (const std::string &n : listar(cur))
            if (strcasecmp(n.c_str(), c.c_str()) == 0) { hallado = n; break; }
        if (hallado.empty()) return "";
        cur += "/" + hallado;
    }
    return cur;
}

std::string leer(const std::string &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool escribir(const std::string &p, const std::string &datos) {
    mkdirs(carpeta_de(p));
    std::string tmp = p + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(datos.data(), (std::streamsize)datos.size());
        if (!out) return false;
    }
    return pcf_rename(tmp.c_str(), p.c_str()) == 0;
}

std::string sha256_fichero(const std::string &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    Sha256 s;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        if (in.gcount() > 0) s.update(buf.data(), (size_t)in.gcount());
    }
    return s.hex();
}

uint64_t tam_arbol(const std::string &dir) {
    uint64_t t = 0;
    for (const std::string &n : listar(dir)) {
        std::string p = dir + "/" + n;
        t += es_dir(p) ? tam_arbol(p) : tam(p);
    }
    return t;
}

std::string cp1252_a_utf8(const std::string &s) {
    static const unsigned short alta[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                                            0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
    std::string o;
    for (unsigned char c : s) {
        unsigned cp = c < 0x80 ? c : c < 0xA0 ? alta[c - 0x80] : c;
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    return o;
}

std::string minusculas(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
std::string mayusculas(std::string s) { for (auto &c : s) c = (char)toupper((unsigned char)c); return s; }
bool termina_en(const std::string &s, const std::string &suf) {
    return s.size() >= suf.size() && strcasecmp(s.c_str() + s.size() - suf.size(), suf.c_str()) == 0;
}

std::string humano(uint64_t b) {
    char buf[64];
    if (b >= 1000000000ull) snprintf(buf, sizeof buf, "%.1f GB", b / 1e9);
    else if (b >= 1000000ull) snprintf(buf, sizeof buf, "%.0f MB", b / 1e6);
    else snprintf(buf, sizeof buf, "%.0f KB", b / 1e3);
    std::string s = buf;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string fecha_hoy() {
    char buf[32];
    time_t t = time(nullptr);
    strftime(buf, sizeof buf, "%Y-%m-%d", localtime(&t));
    return buf;
}

// una carpeta con el mismo nombre y otras mayúsculas: su ruta real (para mezclar dentro, no sustituirla)
static std::string carpeta_homonima(const std::string &dst) {
    std::string dir = carpeta_de(dst), n = nombre_base(dst);
    for (const std::string &e : listar(dir))
        if (strcasecmp(e.c_str(), n.c_str()) == 0 && es_dir(dir + "/" + e)) return dir + "/" + e;
    return dst;
}

std::string ruta_mezclada(const std::string &dst) {
    std::string dir = carpeta_de(dst);
    std::vector<std::string> partes;
    while (!dir.empty() && !es_dir(dir)) { partes.insert(partes.begin(), nombre_base(dir)); dir = carpeta_de(dir); }
    for (const std::string &p : partes) { dir = carpeta_homonima(dir + "/" + p); mkdirs(dir); }
    return partes.empty() ? dst : dir + "/" + nombre_base(dst);
}

bool copiar_fichero(const std::string &src, const std::string &dst_pedido, Progreso *pr) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::string dst = ruta_mezclada(dst_pedido);
    mkdirs(carpeta_de(dst));
    std::string tmp = dst + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    std::vector<char> buf(1 << 20);
    while (in) {
        if (pr && pr->cancelar) { out.close(); ::unlink(tmp.c_str()); return false; }
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = in.gcount();
        if (n > 0) { out.write(buf.data(), n); if (pr) pr->hecho += (uint64_t)n; }
    }
    out.close();
    if (!out) { ::unlink(tmp.c_str()); return false; }
    ::unlink(dst.c_str());
    return pcf_rename(tmp.c_str(), dst.c_str()) == 0;
}

static void quita_homonimo(const std::string &dst) {
    std::string dir = carpeta_de(dst), n = nombre_base(dst);
    for (const std::string &e : listar(dir))
        if (e != n && strcasecmp(e.c_str(), n.c_str()) == 0) borrar_arbol(dir + "/" + e);
}

bool copiar_arbol(const std::string &src, const std::string &dst_pedido, Progreso *pr) {
    std::string dst = carpeta_homonima(dst_pedido);
    mkdirs(dst);
    for (const std::string &n : listar(src)) {
        if (pr && pr->cancelar) return false;
        std::string s = src + "/" + n, d = dst + "/" + n;
        if (es_dir(s)) { if (!copiar_arbol(s, d, pr)) return false; }
        else { quita_homonimo(d); if (!copiar_fichero(s, d, pr)) return false; }
    }
    return true;
}

bool escribir_sustituyendo(const std::string &dst_pedido, const char *datos, size_t n, Progreso *pr) {
    // las carpetas del camino se mezclan con las que haya con otras mayúsculas (SIMULDAT/texturas y simuldat/TEXTURAS)
    std::string dst = ruta_mezclada(dst_pedido);
    mkdirs(carpeta_de(dst));
    quita_homonimo(dst);
    if (!escribir(dst, std::string(datos, n))) return false;
    if (pr) pr->hecho += n;
    return true;
}

#ifdef _WIN32
// la línea de órdenes de CreateProcess: cada argumento entre comillas si hace falta (reglas del CRT de Microsoft)
static std::string linea_ordenes(const std::vector<std::string> &argv) {
    std::string s;
    for (const std::string &a : argv) {
        if (!s.empty()) s += ' ';
        if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) { s += a; continue; }
        s += '"';
        size_t barras = 0;
        for (char ch : a) {
            if (ch == '\\') { barras++; continue; }
            if (ch == '"') { s.append(2 * barras + 1, '\\'); s += '"'; barras = 0; continue; }
            s.append(barras, '\\'); barras = 0;
            s += ch;
        }
        s.append(2 * barras, '\\');
        s += '"';
    }
    return s;
}

int ejecutar(const std::vector<std::string> &argv, std::string &salida) {
    salida.clear();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = linea_ordenes(argv);
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    char tmp[4096];
    DWORD n = 0;
    while (ReadFile(rd, tmp, sizeof tmp, &n, nullptr) && n > 0) salida.append(tmp, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)rc;
}

bool lanzar(const std::string &exe, const std::vector<std::string> &args, Proceso &p) {
    std::vector<std::string> argv{exe};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string cmd = linea_ordenes(argv);
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    p.h = (intptr_t)pi.hProcess;
    p.pid = (int)pi.dwProcessId;
    return true;
}

bool esperar(Proceso &p, bool bloquear, int &codigo) {
    if (!p.activo()) return true;
    if (WaitForSingleObject((HANDLE)p.h, bloquear ? INFINITE : 0) != WAIT_OBJECT_0) return false;
    DWORD rc = (DWORD)-1;
    GetExitCodeProcess((HANDLE)p.h, &rc);
    CloseHandle((HANDLE)p.h);
    p = Proceso{};
    codigo = rc >= 0xC0000000u ? -1 : (int)rc;   // un código de excepción (0xC0000005…) es una muerte violenta
    return true;
}

std::string exe_propio() {
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return n > 0 && n < sizeof buf ? normaliza(buf) : "";
}
std::vector<std::string> orden_programa(const std::string &nombre) { return {exe_propio(), "--programa", nombre}; }
#else
int ejecutar(const std::vector<std::string> &argv, std::string &salida) {
    salida.clear();
    int tubo[2];
    if (pipe(tubo) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, tubo[1], 1);
    posix_spawn_file_actions_adddup2(&fa, tubo[1], 2);
    posix_spawn_file_actions_addclose(&fa, tubo[0]);
    std::vector<char *> av;
    for (const std::string &a : argv) av.push_back((char *)a.c_str());
    av.push_back(nullptr);
    pid_t pid;
    int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(tubo[1]);
    if (rc != 0) { close(tubo[0]); return -1; }
    char buf[4096];
    ssize_t n;
    while ((n = read(tubo[0], buf, sizeof buf)) > 0) salida.append(buf, (size_t)n);
    close(tubo[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

bool lanzar(const std::string &exe, const std::vector<std::string> &args, Proceso &p) {
    std::vector<char *> av{(char *)exe.c_str()};
    for (const std::string &a : args) av.push_back((char *)a.c_str());
    av.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, av.data(), environ) != 0) return false;
    p.pid = (int)pid;
    return true;
}

bool esperar(Proceso &p, bool bloquear, int &codigo) {
    if (!p.activo()) return true;
    int st = 0;
    pid_t r = waitpid((pid_t)p.pid, &st, bloquear ? 0 : WNOHANG);
    if (r == 0) return false;
    p = Proceso{};
    codigo = r > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return true;
}

std::string exe_propio() { return dir_ejecutables() + "/lanzador"; }
std::vector<std::string> orden_programa(const std::string &nombre) { return {dir_ejecutables() + "/" + nombre}; }
#endif

std::string dir_ejecutables() {
    static std::string d;
    if (!d.empty()) return d;
    if (const char *v = getenv("PCF_EJECUTABLES"); v && *v) return d = v;   // desarrollo: los juegos en otra carpeta
#ifdef __APPLE__
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0) { char r[PATH_MAX]; d = carpeta_de(realpath(buf, r) ? r : buf); }
#elif defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; d = carpeta_de(buf); }
#elif defined(_WIN32)
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    if (n > 0 && n < sizeof buf) d = carpeta_de(normaliza(buf));
#endif
    if (d.empty()) d = normaliza(SDL_GetBasePath() ? SDL_GetBasePath() : ".");
    return d;
}

std::string dir_recursos() {
    static std::string d;
    if (d.empty()) d = normaliza(SDL_GetBasePath() ? SDL_GetBasePath() : "./");
    return d;
}

std::string dir_inicio() {
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
#else
    const char *h = getenv("HOME");
#endif
    return h && *h ? normaliza(h) : ".";
}

std::string dir_base() {
    static std::string d;
    if (d.empty()) {
        if (const char *v = getenv("PCF_DATOS"); v && *v) d = v;
        else {
#ifdef __APPLE__
            d = dir_inicio() + "/Library/Application Support/PCFSite - Source";
            std::string antes = dir_inicio() + "/Library/Application Support/PC Fútbol";   // nombre de las primeras pruebas
#elif defined(_WIN32)
            const char *x = getenv("LOCALAPPDATA");
            d = (x && *x ? normaliza(x) : dir_inicio() + "/AppData/Local") + "/PCFSite - Source";
            std::string antes = d;
#else
            const char *x = getenv("XDG_DATA_HOME");
            d = (x && *x ? std::string(x) : dir_inicio() + "/.local/share") + "/pcfsite-source";
            std::string antes = (x && *x ? std::string(x) : dir_inicio() + "/.local/share") + "/pcfutbol";
#endif
            if (!existe(d) && es_dir(antes)) ::rename(antes.c_str(), d.c_str());
        }
        mkdirs(d);
    }
    return d;
}

namespace {
Json ajustes() {
    Json j;
    std::string p = dir_base() + "/ajustes.json";
    if (!existe(p) || !Json::parsea(leer(p), j)) j = Json::objeto_vacio();
    return j;
}
}  // namespace

std::string dir_documentos() {
#ifdef _WIN32
    PWSTR w = nullptr;
    std::string d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &w)) && w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) { d.resize((size_t)n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, d.data(), n, nullptr, nullptr); }
        CoTaskMemFree(w);
    }
    if (d.empty()) d = dir_inicio() + "/Documents";
    return normaliza(d);
#elif defined(__APPLE__)
    return dir_inicio() + "/Documents";   // macOS lo enseña como «Documentos»
#else
    static std::string d;   // Linux: la carpeta de documentos del usuario según XDG (xdg-user-dir), si no ~/Documentos o ~/Documents
    if (d.empty()) {
        std::string s;
        if (ejecutar({"xdg-user-dir", "DOCUMENTS"}, s) == 0) { size_t a = s.find_first_not_of(" \r\n\t"), b = s.find_last_not_of(" \r\n\t"); if (a != std::string::npos && s[a] == '/') d = s.substr(a, b - a + 1); }
        if (d.empty() || d == dir_inicio()) d = es_dir(dir_inicio() + "/Documentos") ? dir_inicio() + "/Documentos" : dir_inicio() + "/Documents";
    }
    return d;
#endif
}

std::string dir_datos_defecto() {
    if (const char *v = getenv("PCF_DATOS"); v && *v) return v;   // pruebas: todo en esa carpeta
    return dir_documentos() + "/PCFSite - Source";
}

std::string dir_datos() {
    std::string c = ajustes()["carpeta"].str();
    if (c.empty()) c = dir_datos_defecto();
    mkdirs(c);
    return c;
}

std::vector<std::string> carpetas_datos() {
    std::vector<std::string> out{dir_datos()};
    auto anade = [&](const std::string &c) { if (!c.empty()) for (auto &o : out) if (o == c) return; if (!c.empty()) out.push_back(c); };
    anade(dir_datos_defecto());
    anade(dir_base());
    Json j = ajustes();
    for (size_t k = 0; k < j["anteriores"].size(); k++) anade(j["anteriores"][k].str());
    return out;
}

bool cambiar_dir_datos(const std::string &nueva) {
    Json j = ajustes();
    std::string actual = dir_datos();
    if (!nueva.empty() && !mkdirs(nueva)) return false;
    Json ant = Json::lista_vacia();
    std::vector<std::string> vistas;
    auto anade = [&](const std::string &c) {
        if (c.empty() || c == nueva || c == dir_base() || c == dir_datos_defecto()) return;
        for (auto &v : vistas) if (v == c) return;
        vistas.push_back(c); ant.anade(Json::de(c));
    };
    anade(actual);
    for (size_t k = 0; k < j["anteriores"].size(); k++) anade(j["anteriores"][k].str());
    j.pon("carpeta", Json::de(nueva == dir_datos_defecto() ? "" : nueva)).pon("anteriores", ant);
    return escribir(dir_base() + "/ajustes.json", j.serializa() + "\n");
}

std::string ajuste(const std::string &clave) { return ajustes()[clave].str(); }

bool pon_ajuste(const std::string &clave, const std::string &valor) {
    Json j = ajustes();
    j.pon(clave, Json::de(valor));
    return escribir(dir_base() + "/ajustes.json", j.serializa() + "\n");
}

uint64_t espacio_libre(const std::string &p) {
#ifdef _WIN32
    ULARGE_INTEGER libre{};
    std::string q = p;
    while (!q.empty() && !es_dir(q)) q = carpeta_de(q);   // la carpeta puede no existir aún: su primera madre que exista
    if (q.empty() || !GetDiskFreeSpaceExA(q.c_str(), &libre, nullptr, nullptr)) return 0;
    return (uint64_t)libre.QuadPart;
#else
    struct statvfs s;
    if (::statvfs(p.c_str(), &s) != 0) return 0;
    return (uint64_t)s.f_bavail * (uint64_t)s.f_frsize;
#endif
}

}  // namespace util
