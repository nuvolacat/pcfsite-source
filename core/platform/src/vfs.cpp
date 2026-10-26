// PCF7 — rutas: de Windows (las del juego) al sistema de ficheros del host.
//
//   C:\FUTBOL7\...  → instalación del juego   (PCF7_GAME, por defecto ~/pcf7-work/game/FUTBOL7)
//   D:\...          → CD                       (PCF7_CD,   por defecto ~/pcf7-work/iso)
//   C:\...          → disco C aislado          (PCF7_DRIVE_C, por defecto ~/pcf7-work/drive_c)
//
// Mods (la app): la carpeta del juego puede ser varias capas, de más a menos prioridad: la de escritura
// (PCF7_ESCRITURA, las partidas de ese perfil), los mods (PCF7_CAPAS, carpetas separadas por '\n', cada una con la
// estructura de la carpeta del juego) y la instalación. Se lee de la primera capa donde esté el fichero; se escribe
// siempre en la de escritura (copiando antes allí el fichero de la capa de abajo si el juego lo abre para modificar); al
// listar una carpeta se mezclan las capas. Sin PCF7_ESCRITURA ni PCF7_CAPAS, la instalación es la única capa (como antes).
//
// Windows no distingue mayúsculas; el host sí (o no, según el volumen): cada componente se busca sin
// distinguir mayúsculas y, si no existe, se conserva tal cual (para poder crear ficheros nuevos).
#include "plat.h"

#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <vector>

std::string g_cwd = std::string("C:\\") + JUEGO_CARPETA;

static std::string env_or(const char *var, const std::string &def) {
    const char *v = std::getenv(var);
    return v && *v ? v : def;
}

static std::string home() { return env_or("HOME", env_or("USERPROFILE", ".")); }
std::string juego_trabajo() { return env_or("PCF7_WORK", home() + "/" + JUEGO_TRABAJO); }   // la app: PCF7_WORK

#ifdef __EMSCRIPTEN__
// En el navegador los datos vienen del servidor (WasmFS, de solo lectura, ver web.cpp) bajo /pcf7/ro, y lo que el
// juego escribe va a /pcf7/rw con la misma forma: se lee de rw si el fichero está allí y si no del original.
static const std::string RO = "/pcf7/ro", RW = "/pcf7/rw";
static const std::string &game_root() { static std::string s = RO + "/game/" + JUEGO_CARPETA; return s; }
static const std::string &cd_root() { static std::string s = RO + "/cd"; return s; }
static const std::string &c_root() { static std::string s = RO + "/c"; return s; }
#else
static const std::string &game_root() { static std::string s = env_or("PCF7_GAME", juego_trabajo() + "/game/" + JUEGO_CARPETA); return s; }
static const std::string &cd_root() { static std::string s = env_or("PCF7_CD", juego_trabajo() + "/iso"); return s; }
static const std::string &c_root() { static std::string s = env_or("PCF7_DRIVE_C", juego_trabajo() + "/drive_c"); return s; }
#endif

static bool ieq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

// Normaliza a absoluta "X:\a\b" (resuelve relativas, '.', '..', '/', separadores repetidos)
static std::string win_abs(std::string p) {
    for (auto &c : p) if (c == '/') c = '\\';
    if (p.size() >= 2 && p[1] == ':') {
        // "C:x" es relativa a la carpeta actual de esa unidad (PCF6 graba en "C:TACTICS\MANAGER.000"); de las
        // otras unidades solo se conoce la raíz
        if (p.size() == 2 || p[2] != '\\') {
            bool misma = std::toupper((unsigned char)p[0]) == std::toupper((unsigned char)g_cwd[0]);
            p = (misma ? g_cwd : p.substr(0, 2)) + "\\" + p.substr(2);
        }
    } else if (!p.empty() && p[0] == '\\') {
        p = g_cwd.substr(0, 2) + p;
    } else {
        p = g_cwd + "\\" + p;
    }
    std::vector<std::string> parts;
    std::string drive = std::string(1, (char)std::toupper((unsigned char)p[0])) + ":";
    size_t i = 3;
    while (i <= p.size()) {
        size_t j = p.find('\\', i);
        if (j == std::string::npos) j = p.size();
        std::string c = p.substr(i, j - i);
        if (c == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!c.empty() && c != ".") parts.push_back(c);
        i = j + 1;
    }
    std::string r = drive;
    for (auto &c : parts) r += "\\" + c;
    if (parts.empty()) r += "\\";
    return r;
}

#ifndef __EMSCRIPTEN__
// las capas de la carpeta del juego (sin la instalación, que es game_root()): la de escritura y los mods
static std::string g_escritura;
static std::vector<std::string> g_mods;
static bool capas_init() {
    static bool hecho = false;
    if (hecho) return !g_mods.empty() || !g_escritura.empty();
    hecho = true;
    g_escritura = env_or("PCF7_ESCRITURA", "");
    std::string c = env_or("PCF7_CAPAS", "");
    for (size_t i = 0; i < c.size();) {
        size_t j = c.find('\n', i);
        if (j == std::string::npos) j = c.size();
        if (j > i) g_mods.push_back(c.substr(i, j - i));
        i = j + 1;
    }
    if (!g_escritura.empty() && g_escritura == game_root()) g_escritura.clear();
    if (!g_mods.empty() && g_escritura.empty()) g_escritura = game_root();   // mods sin perfil: se escribe en la instalación
    if (!g_mods.empty() || !g_escritura.empty()) {
        plog("capas de C:\\%s: escritura %s, %zu mods, instalación %s", JUEGO_CARPETA, g_escritura.empty() ? "(la instalación)" : g_escritura.c_str(), g_mods.size(), game_root().c_str());
        for (const std::string &m : g_mods) plog("  mod: %s", m.c_str());
    }
    return !g_mods.empty() || !g_escritura.empty();
}
// todas las capas, de más a menos prioridad (la primera es donde se escribe)
static std::vector<std::string> capas_juego() {
    std::vector<std::string> v;
    if (!g_escritura.empty()) v.push_back(g_escritura);
    for (const std::string &m : g_mods) if (m != g_escritura) v.push_back(m);
    if (game_root() != g_escritura) v.push_back(game_root());
    return v;
}
static void mkdirs_host(const std::string &p) {
    for (size_t k = p.find('/', 1); k != std::string::npos; k = p.find('/', k + 1)) ::mkdir(p.substr(0, k).c_str(), 0755);
}
#endif

// componentes desde `k` bajo `root`, sin distinguir mayúsculas
static std::string host_path_in(const std::string &root, const std::vector<std::string> &comps, size_t k) {
    std::string cur = root;
    for (; k < comps.size(); k++) {
        const std::string &c = comps[k];
        std::string found;
        if (DIR *d = opendir(cur.c_str())) {
            while (dirent *e = readdir(d)) {
                if (ieq(e->d_name, c)) { found = e->d_name; break; }
            }
            closedir(d);
        }
        cur += "/" + (found.empty() ? c : found);
    }
    return cur;
}

std::string host_path(const std::string &winpath) {
    std::string w = win_abs(winpath);
    std::vector<std::string> comps;
    for (size_t i = 3; i < w.size();) {
        size_t j = w.find('\\', i);
        if (j == std::string::npos) j = w.size();
        comps.push_back(w.substr(i, j - i));
        i = j + 1;
    }
    std::string root;
    size_t k = 0;
    if (w[0] == 'D') root = cd_root();
    else if (!comps.empty() && ieq(comps[0], JUEGO_CARPETA)) { root = game_root(); k = 1; }
    else root = c_root();

#ifdef __EMSCRIPTEN__
    struct stat st;
    std::string rw = host_path_in(RW + root.substr(RO.size()), comps, k);
    if (::stat(rw.c_str(), &st) == 0) return rw;
    std::string ro = host_path_in(root, comps, k);
    if (::stat(ro.c_str(), &st) == 0) return ro;
    return rw;   // no existe: si se crea, va a la capa escribible
#else
    std::string hp;
    if (root == game_root() && capas_init()) {   // con mods: la primera capa donde esté; si no está, la de escritura
        struct stat st;
        std::vector<std::string> capas = capas_juego();
        for (const std::string &capa : capas) {
            std::string p = host_path_in(capa, comps, k);
            if (::stat(p.c_str(), &st) == 0) { hp = p; break; }
        }
        if (hp.empty()) hp = host_path_in(capas[0], comps, k);
    } else hp = host_path_in(root, comps, k);
    // PCF7_TRACE_FILES: cada fichero que el juego busca, una vez (para elegir qué datos precargar en la web)
    static const bool traza = std::getenv("PCF7_TRACE_FILES") != nullptr;
    if (traza) plog_once("tf" + hp, "fichero: %s", hp.c_str());
    return hp;
#endif
}

#ifdef __EMSCRIPTEN__
static void mkdirs(const std::string &p) {
    for (size_t k = p.find('/', 1); k != std::string::npos; k = p.find('/', k + 1)) ::mkdir(p.substr(0, k).c_str(), 0755);
}
#endif

// Ruta para escribir: en el navegador, el fichero original se copia antes a la capa escribible
std::string host_path_w(const std::string &winpath) {
    std::string p = host_path(winpath);
#ifdef __EMSCRIPTEN__
    if (p.compare(0, RO.size() + 1, RO + "/") == 0) {
        std::string rw = RW + p.substr(RO.size());
        mkdirs(rw);
        struct stat st;
        if (::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { ::mkdir(rw.c_str(), 0755); web_sucio(rw); return rw; }
        if (::stat(p.c_str(), &st) == 0) {
            std::ifstream in(p, std::ios::binary);
            std::ofstream out(rw, std::ios::binary | std::ios::trunc);
            out << in.rdbuf();
        }
        web_sucio(rw);
        return rw;
    }
    if (p.compare(0, RW.size() + 1, RW + "/") == 0) { mkdirs(p); web_sucio(p); }
#else
    if (capas_init() && !g_escritura.empty()) {
        // ¿de qué capa ha salido? si no es la de escritura, se copia allí (fichero) o se crea (carpeta) y se escribe ahí
        for (const std::string &capa : capas_juego()) {
            if (p.size() < capa.size() + 1 || p.compare(0, capa.size() + 1, capa + "/") != 0) continue;
            std::string dst = g_escritura + p.substr(capa.size());
            if (capa == g_escritura) { mkdirs_host(dst); return p; }
            mkdirs_host(dst);
            struct stat st;
            if (::stat(p.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) ::mkdir(dst.c_str(), 0755);
                else if (::stat(dst.c_str(), &st) != 0) {
                    std::ifstream in(p, std::ios::binary);
                    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
                    out << in.rdbuf();
                    plog("capas: %s copiado a la capa de escritura para modificarlo", p.c_str());
                }
            }
            return dst;
        }
    }
#endif
    return p;
}

// Carpetas del host que forman una carpeta del juego (en el navegador, la escribible y la original)
std::vector<std::string> host_dirs(const std::string &winpath) {
    std::string p = host_path(winpath);
#ifdef __EMSCRIPTEN__
    std::vector<std::string> v{p};
    if (p.compare(0, RW.size() + 1, RW + "/") == 0) {
        std::string ro = RO + p.substr(RW.size());
        struct stat st;
        if (::stat(ro.c_str(), &st) == 0) v.push_back(ro);
    }
    return v;
#else
    std::vector<std::string> v;
    if (capas_init()) {   // con mods: la carpeta en cada capa donde exista (manda la primera)
        std::string w = win_abs(winpath);
        if (w.size() > 3 && (w[0] == 'C' || w[0] == 'c')) {
            std::vector<std::string> comps;
            for (size_t i = 3; i < w.size();) { size_t j = w.find('\\', i); if (j == std::string::npos) j = w.size(); comps.push_back(w.substr(i, j - i)); i = j + 1; }
            if (!comps.empty() && ieq(comps[0], JUEGO_CARPETA)) {
                struct stat st;
                for (const std::string &capa : capas_juego()) {
                    std::string d = host_path_in(capa, comps, 1);
                    if (::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) v.push_back(d);
                }
                if (!v.empty()) return v;
            }
        }
    }
    return {p};
#endif
}
