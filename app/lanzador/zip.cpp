// lanzador — extractor de .zip (ver zip.h). Formato: entradas locales (firma PK\3\4, cabecera de 30 bytes + nombre +
// extra + datos) y, al final, el directorio central (PK\1\2, 46 bytes + nombre + extra + comentario) con el
// desplazamiento de cada entrada local, y el registro final (PK\5\6, 22 bytes + comentario). Se lee el directorio
// central, que es el que manda.
#include "zip.h"

#include "inflar.h"
#include "util.h"

#include <cstring>
#include <fstream>

namespace {
uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

struct Entrada { std::string nombre; uint16_t metodo = 0, flags = 0; uint32_t comprimido = 0, tam = 0, offset = 0; };

bool utf8_valido(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) return false;
        for (int k = 1; k <= n; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

bool directorio_central(const std::vector<uint8_t> &d, std::vector<Entrada> &out, std::string &error) {
    if (d.size() < 22) { error = "no es un zip"; return false; }
    size_t fin = std::string::npos;
    for (size_t i = d.size() - 22; i + 1 > 0 && d.size() - i < 22 + 65536; i--)
        if (d[i] == 'P' && d[i + 1] == 'K' && d[i + 2] == 5 && d[i + 3] == 6) { fin = i; break; }
    if (fin == std::string::npos) { error = "no es un zip (falta el directorio central)"; return false; }
    uint16_t n = u16(d.data() + fin + 10);
    uint32_t off = u32(d.data() + fin + 16);
    if (off == 0xFFFFFFFFu) { error = "zip64 no soportado"; return false; }
    size_t p = off;
    for (uint16_t i = 0; i < n; i++) {
        if (p + 46 > d.size() || std::memcmp(d.data() + p, "PK\1\2", 4) != 0) { error = "directorio central corrupto"; return false; }
        Entrada e;
        e.flags = u16(d.data() + p + 8); e.metodo = u16(d.data() + p + 10);
        e.comprimido = u32(d.data() + p + 20); e.tam = u32(d.data() + p + 24);
        uint16_t ln = u16(d.data() + p + 28), lx = u16(d.data() + p + 30), lc = u16(d.data() + p + 32);
        e.offset = u32(d.data() + p + 42);
        if (p + 46 + ln > d.size()) { error = "directorio central corrupto"; return false; }
        std::string nombre((const char *)d.data() + p + 46, ln);
        for (auto &ch : nombre) if (ch == '\\') ch = '/';
        e.nombre = (e.flags & 0x800) || utf8_valido(nombre) ? nombre : util::cp1252_a_utf8(nombre);
        out.push_back(e);
        p += 46 + ln + lx + lc;
    }
    return true;
}

bool datos_de(const std::vector<uint8_t> &d, const Entrada &e, std::vector<uint8_t> &out, std::string &error) {
    size_t p = e.offset;
    if (p + 30 > d.size() || std::memcmp(d.data() + p, "PK\3\4", 4) != 0) { error = e.nombre + ": entrada local corrupta"; return false; }
    uint16_t ln = u16(d.data() + p + 26), lx = u16(d.data() + p + 28);
    size_t datos = p + 30 + ln + lx;
    if (datos + e.comprimido > d.size()) { error = e.nombre + ": datos fuera del fichero"; return false; }
    if (e.flags & 1) { error = e.nombre + ": zip cifrado"; return false; }
    if (e.metodo == 0) { out.assign(d.begin() + (long)datos, d.begin() + (long)(datos + e.comprimido)); return true; }
    if (e.metodo != 8) { error = e.nombre + ": compresión " + std::to_string(e.metodo) + " no soportada (solo deflate)"; return false; }
    out.clear();
    Inflador inf;
    if (!inf.inflar(d.data() + datos, e.comprimido, out, false, error)) { error = e.nombre + ": " + error; return false; }
    if (out.size() != e.tam) { error = e.nombre + ": tamaño " + std::to_string(out.size()) + " en vez de " + std::to_string(e.tam); return false; }
    return true;
}

// un nombre de entrada seguro: sin raíz ni «..»
bool nombre_seguro(const std::string &n) {
    if (n.empty() || n[0] == '/' || (n.size() > 1 && n[1] == ':')) return false;
    size_t i = 0;
    while (i <= n.size()) {
        size_t j = n.find('/', i);
        if (j == std::string::npos) j = n.size();
        if (n.substr(i, j - i) == "..") return false;
        i = j + 1;
    }
    return true;
}
}  // namespace

bool zip_listar(const std::string &fichero, std::vector<ZipEntrada> &salida, std::string &error) {
    std::string s = util::leer(fichero);
    std::vector<uint8_t> d(s.begin(), s.end());
    std::vector<Entrada> es;
    if (!directorio_central(d, es, error)) return false;
    for (const Entrada &e : es) salida.push_back({e.nombre, !e.nombre.empty() && e.nombre.back() == '/', e.tam});
    return true;
}

bool zip_extraer(const std::string &fichero, const std::string &destino, std::string &error) {
    std::string s = util::leer(fichero);
    if (s.empty()) { error = "no se puede leer " + fichero; return false; }
    std::vector<uint8_t> d(s.begin(), s.end());
    std::vector<Entrada> es;
    if (!directorio_central(d, es, error)) return false;
    util::mkdirs(destino);
    std::vector<uint8_t> datos;
    for (const Entrada &e : es) {
        if (!nombre_seguro(e.nombre)) { error = e.nombre + ": nombre no permitido"; return false; }
        std::string ruta = destino + "/" + e.nombre;
        if (e.nombre.back() == '/') { util::mkdirs(ruta); continue; }
        if (!datos_de(d, e, datos, error)) return false;
        util::mkdirs(util::carpeta_de(ruta));
        if (!util::escribir(ruta, std::string(datos.begin(), datos.end()))) { error = "no se puede escribir " + ruta; return false; }
    }
    return true;
}
