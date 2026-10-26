// lanzador — cabinets de Microsoft (ver mscab.h). Formato: CFHEADER («MSCF», tamaño, offset de los CFFILE, nº de
// carpetas y ficheros, flags), CFFOLDER (offset del primer CFDATA, nº de bloques, compresión), CFFILE (tamaño, offset
// dentro de la carpeta descomprimida, carpeta, fecha, atributos, nombre) y los CFDATA (suma, bytes comprimidos y
// descomprimidos, datos). MSZIP: cada bloque es «CK» + deflate crudo y la ventana se conserva entre bloques.
#include "mscab.h"

#include "inflar.h"
#include "util.h"

#include <cstring>

namespace {
uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// ¿empieza en `o` una cabecera MSCF creíble?
bool cabecera_valida(const std::vector<uint8_t> &d, size_t o) {
    if (o + 36 > d.size() || std::memcmp(d.data() + o, "MSCF", 4) != 0) return false;
    uint32_t tam = u32(d.data() + o + 8), off_ficheros = u32(d.data() + o + 16);
    uint16_t ncarp = u16(d.data() + o + 26), nfich = u16(d.data() + o + 28);
    return u32(d.data() + o + 4) == 0 && u32(d.data() + o + 12) == 0 && tam >= 36 && tam <= d.size() - o && off_ficheros < tam &&
           d[o + 24] == 3 && d[o + 25] == 1 && ncarp >= 1 && ncarp < 1000 && nfich >= 1 && nfich < 10000;
}
}  // namespace

bool mscab_extraer(const std::string &fichero, std::vector<MscabEntrada> &salida, std::string &error) {
    std::string s = util::leer(fichero);
    std::vector<uint8_t> d(s.begin(), s.end());
    if (d.empty()) { error = "no se puede leer " + fichero; return false; }
    size_t o = std::string::npos;
    for (size_t i = 0; i + 4 <= d.size(); i++)
        if (d[i] == 'M' && cabecera_valida(d, i)) { o = i; break; }
    if (o == std::string::npos) { error = util::nombre_base(fichero) + " no lleva ningún CAB de Microsoft"; return false; }
    const uint8_t *h = d.data() + o;
    uint32_t tam = u32(h + 8), off_ficheros = u32(h + 16);
    uint16_t ncarp = u16(h + 26), nfich = u16(h + 28), flags = u16(h + 30);
    size_t p = 36;
    uint16_t res_cab = 0; uint8_t res_carp = 0, res_dat = 0;
    if (flags & 4) { res_cab = u16(h + 36); res_carp = h[38]; res_dat = h[39]; p = 40 + res_cab; }
    if (flags & 1) { while (p < tam && h[p]) p++; p++; while (p < tam && h[p]) p++; p++; }   // cabinet anterior
    if (flags & 2) { while (p < tam && h[p]) p++; p++; while (p < tam && h[p]) p++; p++; }   // siguiente
    struct Carpeta { uint32_t datos; uint16_t bloques, tipo; std::vector<uint8_t> contenido; bool hecha = false; };
    std::vector<Carpeta> carpetas;
    for (uint16_t i = 0; i < ncarp; i++) {
        if (p + 8 > tam) { error = "CAB truncado (carpetas)"; return false; }
        carpetas.push_back({u32(h + p), u16(h + p + 4), u16(h + p + 6)});
        p += 8 + res_carp;
    }
    // los ficheros
    struct Fich { std::string nombre; uint32_t tam, off; uint16_t carpeta; };
    std::vector<Fich> ficheros;
    p = off_ficheros;
    for (uint16_t i = 0; i < nfich; i++) {
        if (p + 16 > tam) { error = "CAB truncado (ficheros)"; return false; }
        Fich f{"", u32(h + p), u32(h + p + 4), u16(h + p + 8)};
        uint16_t atrib = u16(h + p + 14);
        p += 16;
        std::string n;
        while (p < tam && h[p]) n.push_back((char)h[p++]);
        p++;
        for (auto &ch : n) if (ch == '\\') ch = '/';
        while (!n.empty() && n[0] == '/') n.erase(0, 1);   // los paquetes PackageForTheWeb guardan «\\nombre»
        f.nombre = (atrib & 0x80) ? n : util::cp1252_a_utf8(n);
        if (f.carpeta >= ncarp) f.carpeta = 0;   // (continuaciones entre cabinets: no se dan aquí)
        ficheros.push_back(f);
    }
    // cada carpeta se descomprime entera (los bloques MSZIP comparten la ventana)
    auto descomprime = [&](Carpeta &c) -> bool {
        if (c.hecha) return true;
        size_t q = c.datos;
        Inflador inf;
        uint16_t tipo = c.tipo & 0xF;
        for (uint16_t b = 0; b < c.bloques; b++) {
            if (q + 8 > tam) { error = "CAB truncado (datos)"; return false; }
            uint16_t ncomp = u16(h + q + 4), ndesc = u16(h + q + 6);
            q += 8 + res_dat;
            if (q + ncomp > tam) { error = "CAB truncado (bloque)"; return false; }
            if (tipo == 0) c.contenido.insert(c.contenido.end(), h + q, h + q + ncomp);
            else if (tipo == 1) {
                if (ncomp < 2 || h[q] != 'C' || h[q + 1] != 'K') { error = "bloque MSZIP sin firma CK"; return false; }
                size_t antes = c.contenido.size();
                if (!inf.inflar(h + q + 2, ncomp - 2, c.contenido, false, error)) return false;
                if (c.contenido.size() - antes != ndesc) { error = "bloque MSZIP de tamaño inesperado"; return false; }
            } else { error = "compresión " + std::to_string(tipo) + " no soportada (solo MSZIP)"; return false; }
            q += ncomp;
        }
        c.hecha = true;
        return true;
    };
    for (const Fich &f : ficheros) {
        Carpeta &c = carpetas[f.carpeta];
        if (!descomprime(c)) return false;
        if ((uint64_t)f.off + f.tam > c.contenido.size()) { error = f.nombre + ": fuera de la carpeta descomprimida"; return false; }
        MscabEntrada e;
        e.nombre = f.nombre; e.tam = f.tam;
        e.datos.assign(c.contenido.begin() + f.off, c.contenido.begin() + f.off + f.tam);
        salida.push_back(std::move(e));
    }
    return true;
}

bool mscab_extraer_en(const std::string &fichero, const std::string &destino, std::vector<std::string> &nombres, std::string &error) {
    std::vector<MscabEntrada> entradas;
    if (!mscab_extraer(fichero, entradas, error)) return false;
    util::mkdirs(destino);
    for (const MscabEntrada &e : entradas) {
        std::string ruta = destino + "/" + util::nombre_base(e.nombre);
        if (!util::escribir(ruta, std::string(e.datos.begin(), e.datos.end()))) { error = "no se puede escribir " + ruta; return false; }
        nombres.push_back(util::nombre_base(e.nombre));
    }
    return true;
}
