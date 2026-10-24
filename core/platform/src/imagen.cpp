// PCF7 — la imagen inicial de la memoria SIN mem.bin: se reconstruye con los ficheros del propio juego.
//
// La app no lleva mem.bin (es el EXE y las DLL originales ya colocados en memoria). En su lugar, recomp/build.py
// deja en imagen.c una «receta» por módulo: qué fichero es (nombre, tamaño, SHA-256), qué trozos del fichero van a
// qué desplazamiento del módulo (lo mismo que hace pefile al mapear un PE: cabecera y secciones; en los LE del 4.0,
// las páginas del fichero) y, encima, las «diferencias»: los bytes de la imagen final que no son los del fichero
// (relocaciones, IAT resuelta, parches de juego.json). Al final se comprueba el SHA-256 de toda la imagen: si no
// coincide, el juego no arranca y se dice qué fichero no es el esperado.
//
// Los ficheros se buscan donde los dejó la instalación: C:\<carpeta del juego> (y sus subcarpetas), C:\WINDOWS\SYSTEM
// y el CD (D:\, D:\SYSTEM, D:\MMX); entre varios con el mismo nombre se queda el que tiene el SHA-256 esperado.
//
// Mods con EXE o DLL modificados (los mods de PC Fútbol cambian textos dentro del ejecutable, no código): si en una capa
// de mods (vfs.cpp) hay un fichero con el mismo nombre y tamaño pero otro contenido, sus diferencias en las secciones
// de datos del PE se aplican encima de la imagen ya reconstruida y verificada; las de código también, y surten efecto
// en los inmediatos «vivos» (lift.py lee de MEM los valores de mov/push/cmp…), no en el código en sí, que es el
// traducido. Un byte que ya cambió la receta (relocaciones, IAT) no se toca.
#include "plat.h"

#ifndef __EMSCRIPTEN__
#include "sha256.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <strings.h>
#include <vector>

extern "C" {
typedef struct { const char *fichero, *sha256; u32 tam, base, size; const u32 *copias; u32 ncopias; const unsigned char *dif; u32 ndif; } ImgModulo;
extern const ImgModulo IMAGEN_MODS[];
extern const unsigned IMAGEN_NMODS;
extern const u32 IMAGEN_FIN;
extern const char IMAGEN_SHA256[];
}

std::string g_imagen_error;   // por qué no se ha podido construir (para enseñárselo al usuario)
std::vector<std::string> g_imagen_ficheros;   // qué fichero se ha usado para cada módulo (registro)

namespace {

bool lee(const std::string &p, std::vector<unsigned char> &out, u32 tam_esperado) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || (u64)st.st_size != tam_esperado) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.resize(tam_esperado);
    in.read((char *)out.data(), tam_esperado);
    return (u32)in.gcount() == tam_esperado;
}

// busca `nombre` (sin distinguir mayúsculas) bajo `dir`, hasta `prof` niveles
void busca(const std::string &dir, const char *nombre, int prof, std::vector<std::string> &out) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (::stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (prof > 0) sub.push_back(p); }
        else if (strcasecmp(e->d_name, nombre) == 0) out.push_back(p);
    }
    closedir(d);
    for (auto &s : sub) busca(s, nombre, prof - 1, out);
}

// el fichero de un módulo: el que tiene el nombre, el tamaño y el SHA-256 esperados
bool fichero_de(const ImgModulo &m, std::vector<unsigned char> &datos, std::string &ruta) {
    std::string carpeta = std::string("C:\\") + JUEGO_CARPETA + "\\";
    std::vector<std::string> cand;
    for (const std::string &w : {carpeta + m.fichero, carpeta + "Extension\\" + m.fichero, carpeta + "PROQUINI\\" + m.fichero,
                                 std::string("C:\\WINDOWS\\SYSTEM\\") + m.fichero, std::string("D:\\") + m.fichero,
                                 std::string("D:\\SYSTEM\\") + m.fichero, std::string("D:\\MMX\\") + m.fichero})
        cand.push_back(host_path(w));
    for (const std::string &d : host_dirs(std::string("C:\\") + JUEGO_CARPETA)) busca(d, m.fichero, 3, cand);   // (todas las capas: mods e instalación)
    busca(host_path("C:\\WINDOWS"), m.fichero, 2, cand);
    busca(host_path("D:\\"), m.fichero, 2, cand);
    bool tam_ok = false;
    for (const std::string &p : cand) {
        if (!lee(p, datos, m.tam)) continue;
        tam_ok = true;
        if (sha256_hex(datos.data(), datos.size()) == m.sha256) { ruta = p; return true; }
    }
    g_imagen_error = tam_ok ? std::string("El fichero ") + m.fichero + " no es el de la versión que soporta este programa (otra edición o modificado)."
                            : std::string("No se encuentra ") + m.fichero + " en la instalación ni en el CD.";
    return false;
}

}  // namespace

// las secciones de un PE: [desplazamiento en el fichero, fin) y si son de código
struct SeccionPE { u32 ini, fin; bool codigo; };
static std::vector<SeccionPE> secciones_pe(const std::vector<unsigned char> &f) {
    std::vector<SeccionPE> v;
    auto u16 = [&](size_t o) { return (u32)f[o] | (u32)f[o + 1] << 8; };
    auto u32_ = [&](size_t o) { return (u32)f[o] | (u32)f[o + 1] << 8 | (u32)f[o + 2] << 16 | (u32)f[o + 3] << 24; };
    if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return v;
    size_t pe = u32_(0x3C);
    if (pe + 24 > f.size() || f[pe] != 'P' || f[pe + 1] != 'E' || f[pe + 2] || f[pe + 3]) return v;
    u32 n = u16(pe + 6), opt = u16(pe + 20);
    size_t t = pe + 24 + opt;
    for (u32 i = 0; i < n && t + 40 <= f.size(); i++, t += 40) {
        u32 raw = u32_(t + 16), off = u32_(t + 20), car = u32_(t + 36);
        if (raw) v.push_back({off, off + raw, (car & 0x20000020u) != 0});   // IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE
    }
    return v;
}

// un fichero del mismo nombre y tamaño en las capas de mods (todas menos la instalación, la última), con otro contenido
static bool fichero_de_mod(const ImgModulo &m, const std::string &original, std::vector<unsigned char> &datos, std::string &ruta) {
    std::vector<std::string> capas = host_dirs(std::string("C:\\") + JUEGO_CARPETA);
    for (size_t i = 0; i + 1 < capas.size(); i++) {
        std::vector<std::string> cand;
        busca(capas[i], m.fichero, 3, cand);
        for (const std::string &p : cand) {
            if (p == original || !lee(p, datos, m.tam)) continue;
            if (sha256_hex(datos.data(), datos.size()) != m.sha256) { ruta = p; return true; }
        }
    }
    return false;
}

// aplica sobre el módulo ya reconstruido las diferencias de datos de un fichero modificado por un mod
static void aplicar_mod(const ImgModulo &m, u8 *mod, const std::vector<unsigned char> &orig, const std::vector<unsigned char> &mio, const std::string &ruta) {
    std::vector<SeccionPE> secs = secciones_pe(orig);
    if (secs.empty()) { plog("imagen: %s: el mod %s no es un PE: se ignora", m.fichero, ruta.c_str()); return; }
    u32 datos = 0, codigo = 0, tocados = 0;
    for (u32 k = 0; k < m.ncopias; k++) {
        u32 src = m.copias[3 * k], d = m.copias[3 * k + 1], n = m.copias[3 * k + 2];
        if (src >= orig.size() || d >= m.size) continue;
        n = std::min(n, (u32)(orig.size() - src));
        n = std::min(n, m.size - d);
        for (u32 i = 0; i < n; i++) {
            if (orig[src + i] == mio[src + i]) continue;
            bool en_codigo = false;
            for (const SeccionPE &s : secs) if (src + i >= s.ini && src + i < s.fin) { en_codigo = s.codigo; break; }
            if (mod[d + i] != orig[src + i]) { tocados++; continue; }   // la receta ya lo cambió (relocación, IAT)
            // en código solo surten efecto los inmediatos «vivos» (lift.py: mov/push/cmp…); el resto se aplica igual y
            // no cambia nada (el código traducido es el original)
            mod[d + i] = mio[src + i];
            if (en_codigo) codigo++; else datos++;
        }
    }
    plog("imagen: %s: mod %s: %u bytes de datos y %u de código (inmediatos) aplicados%s", m.fichero, ruta.c_str(), datos, codigo,
         tocados ? (", " + std::to_string(tocados) + " ya cambiados por la receta, no").c_str() : "");
}

// Construye la imagen en `dst` (al menos IMAGEN_FIN bytes). false (y g_imagen_error) si falta algo o no coincide.
bool imagen_construir(u8 *dst) {
    g_imagen_error.clear();
    g_imagen_ficheros.clear();
    std::memset(dst, 0, IMAGEN_FIN);
    for (unsigned i = 0; i < IMAGEN_NMODS; i++) {
        const ImgModulo &m = IMAGEN_MODS[i];
        std::vector<unsigned char> datos;
        std::string ruta;
        if (!fichero_de(m, datos, ruta)) { plog("imagen: %s: %s", m.fichero, g_imagen_error.c_str()); return false; }
        g_imagen_ficheros.push_back(ruta);
        u8 *mod = dst + m.base;
        for (u32 k = 0; k < m.ncopias; k++) {
            u32 src = m.copias[3 * k], d = m.copias[3 * k + 1], n = m.copias[3 * k + 2];
            if (src >= datos.size() || d >= m.size) continue;
            n = std::min(n, (u32)(datos.size() - src));
            n = std::min(n, m.size - d);
            std::memcpy(mod + d, datos.data() + src, n);
        }
        for (u32 o = 0; o + 6 <= m.ndif;) {
            u32 a = (u32)m.dif[o] | (u32)m.dif[o + 1] << 8 | (u32)m.dif[o + 2] << 16 | (u32)m.dif[o + 3] << 24;
            u32 t = (u32)m.dif[o + 4] | (u32)m.dif[o + 5] << 8;
            o += 6;
            if (a + t <= m.size && o + t <= m.ndif) std::memcpy(mod + a, m.dif + o, t);
            o += t;
        }
        plog("imagen: %s <- %s", m.fichero, ruta.c_str());
    }
    std::string h = sha256_hex(dst, IMAGEN_FIN);
    if (h != IMAGEN_SHA256) {
        g_imagen_error = "La imagen de memoria reconstruida no coincide con la esperada (receta de imagen.c).";
        plog("imagen: SHA-256 %s, esperado %s", h.c_str(), IMAGEN_SHA256);
        return false;
    }
    plog("imagen: %u módulos, %u B, SHA-256 correcto", IMAGEN_NMODS, IMAGEN_FIN);
    // encima, los ficheros modificados por los mods (solo sus datos: textos)
    for (unsigned i = 0; i < IMAGEN_NMODS; i++) {
        const ImgModulo &m = IMAGEN_MODS[i];
        std::vector<unsigned char> orig, mio;
        std::string ruta;
        if (!fichero_de_mod(m, g_imagen_ficheros[i], mio, ruta)) continue;
        if (!lee(g_imagen_ficheros[i], orig, m.tam)) continue;
        aplicar_mod(m, dst + m.base, orig, mio, ruta);
    }
    return true;
}

u32 imagen_fin() { return IMAGEN_FIN; }

#else
#include <string>
#include <vector>
std::string g_imagen_error;
std::vector<std::string> g_imagen_ficheros;
bool imagen_construir(u8 *) { g_imagen_error = "sin receta en la web"; return false; }
u32 imagen_fin() { return 0; }
#endif
