// lanzador — cabinets de InstallShield 6 (ver is6.h)
#include <algorithm>
#include "is6.h"

#include "inflar.h"
#include "md5.h"
#include "util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>

namespace {
uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32); }
const uint32_t FIRMA = 0x28635349;   // "ISc("
const uint16_t F_PARTIDO = 1, F_OFUSCADO = 2, F_COMPRIMIDO = 4, F_NO_VALIDO = 8;
const uint8_t ENLACE_PREV = 1;

bool lee_todo(int fd, uint64_t off, uint8_t *dst, size_t n) {
    size_t hecho = 0;
    while (hecho < n) {
        ssize_t r = ::pread(fd, dst + hecho, n - hecho, (off_t)(off + hecho));
        if (r <= 0) return false;
        hecho += (size_t)r;
    }
    return true;
}
}  // namespace

std::string Is6::cadena(uint32_t off) const {
    size_t p = (size_t)desc_ + off;
    std::string s;
    while (p < hdr_.size() && hdr_[p]) s.push_back((char)hdr_[p++]);
    return util::cp1252_a_utf8(s);
}

std::string Is6::ruta_volumen(unsigned n) const { return util::resolver(carpeta_, "data" + std::to_string(n) + ".cab"); }

bool Is6::abrir(const std::string &carpeta, std::string &error) {
    carpeta_ = carpeta;
    std::string hdr = util::resolver(carpeta, "data1.hdr");
    if (hdr.empty()) { error = "no hay DATA1.HDR en " + carpeta; return false; }
    std::string datos = util::leer(hdr);
    hdr_.assign(datos.begin(), datos.end());
    if (hdr_.size() < 20 || u32(hdr_.data()) != FIRMA) { error = "DATA1.HDR no es un cabinet de InstallShield"; return false; }
    uint32_t version = u32(hdr_.data() + 4);
    if (version >> 24 == 1) version_ = (int)((version >> 12) & 0xF);
    else if (version >> 24 == 2 || version >> 24 == 4) { version_ = (int)(version & 0xFFFF); if (version_) version_ /= 100; }
    desc_ = u32(hdr_.data() + 12);
    uint32_t desc_tam = u32(hdr_.data() + 16);
    if (!desc_tam || (size_t)desc_ + 0x30 + 0xe + 142 * 4 > hdr_.size()) { error = "DATA1.HDR sin descriptor del CAB"; return false; }
    if (version_ < 5 || version_ > 7) { error = "InstallShield versión " + std::to_string(version_) + ": solo se entienden la 5 y la 6"; return false; }
    const uint8_t *d = hdr_.data() + desc_;
    tabla_ = u32(d + 0xc);
    ndirs_ = u32(d + 0x1c);
    nfich_ = u32(d + 0x28);
    tabla2_ = u32(d + 0x2c);
    if ((size_t)desc_ + tabla_ + 4 * (ndirs_ + nfich_) > hdr_.size()) { error = "tabla de ficheros fuera del DATA1.HDR"; return false; }
    // la tabla: un offset por directorio y, en la versión 5, también uno por fichero (su descriptor)
    tabla_dirs_.resize(ndirs_ + (version_ == 5 ? nfich_ : 0));
    for (uint32_t i = 0; i < tabla_dirs_.size(); i++) tabla_dirs_[i] = u32(d + tabla_ + 4 * i);
    // grupos de ficheros: listas enlazadas desde los 71 huecos del descriptor
    const uint8_t *g = d + 0x30 + 0xe;
    for (int i = 0; i < 71; i++) {
        uint32_t sig = u32(g + 4 * i);
        int vueltas = 0;
        while (sig && vueltas++ < 1000) {
            if ((size_t)desc_ + sig + 12 > hdr_.size()) break;
            const uint8_t *nodo = d + sig;
            uint32_t dsc = u32(nodo + 4);
            sig = u32(nodo + 8);
            uint32_t salto = version_ == 5 ? 0x48 : 0x12;
            if (!dsc || (size_t)desc_ + dsc + 4 + salto + 8 > hdr_.size()) continue;
            Is6Grupo gr;
            gr.nombre = cadena(u32(d + dsc));
            gr.primero = u32(d + dsc + 4 + salto);
            gr.ultimo = u32(d + dsc + 4 + salto + 4);
            grupos_.push_back(gr);
        }
    }
    // los ficheros
    descriptores_.reserve(nfich_);
    ficheros_.reserve(nfich_);
    for (uint32_t i = 0; i < nfich_; i++) {
        Descriptor fd = descriptor(i);
        descriptores_.push_back(fd);
        Is6Fichero f;
        f.nombre = cadena(tabla_ + fd.nombre);   // los nombres de fichero van relativos a la tabla de ficheros
        if (fd.directorio < ndirs_) {
            size_t p = (size_t)desc_ + tabla_ + tabla_dirs_[fd.directorio];
            std::string s;
            while (p < hdr_.size() && hdr_[p]) s.push_back((char)hdr_[p++]);
            f.directorio = util::cp1252_a_utf8(s);
        }
        f.tam = fd.tam;
        f.valido = !(fd.flags & F_NO_VALIDO) && fd.datos != 0;
        ficheros_.push_back(f);
    }
    return true;
}

Is6::Descriptor Is6::descriptor(uint32_t i) const {
    Descriptor fd;
    if (version_ == 5) {   // descriptor de 0x3a bytes, apuntado desde la tabla; los datos van en el primer volumen que los tenga
        size_t p = (size_t)desc_ + tabla_ + (ndirs_ + i < tabla_dirs_.size() ? tabla_dirs_[ndirs_ + i] : 0);
        if (ndirs_ + i >= tabla_dirs_.size() || p + 0x3a > hdr_.size()) { fd.flags = F_NO_VALIDO; return fd; }
        const uint8_t *q = hdr_.data() + p;
        fd.nombre = u32(q); fd.directorio = (uint16_t)u32(q + 4); fd.flags = u16(q + 8);
        fd.tam = u32(q + 10); fd.comprimido = u32(q + 14); fd.datos = u32(q + 0x26);
        std::memcpy(fd.md5, q + 0x2a, 16);
        fd.volumen = 0;
        return fd;
    }
    size_t p = (size_t)desc_ + tabla_ + tabla2_ + (size_t)i * 0x57;
    if (p + 0x57 > hdr_.size()) { fd.flags = F_NO_VALIDO; return fd; }
    const uint8_t *q = hdr_.data() + p;
    fd.flags = u16(q); fd.tam = u64(q + 2); fd.comprimido = u64(q + 10); fd.datos = u64(q + 18);
    std::memcpy(fd.md5, q + 26, 16);
    fd.nombre = u32(q + 0x3a); fd.directorio = u16(q + 0x3e);
    fd.enlace_prev = u32(q + 0x4c); fd.enlace_sig = u32(q + 0x50); fd.enlace = q[0x54]; fd.volumen = u16(q + 0x55);
    return fd;
}

const Is6Grupo *Is6::grupo(const std::string &nombre) const {
    for (auto &g : grupos_) if (g.nombre == nombre) return &g;
    return nullptr;
}

bool Is6::lee_volumen(unsigned n, Volumen &v, int &fd) const {
    std::string ruta = ruta_volumen(n);
    if (ruta.empty()) return false;
    fd = ::open(ruta.c_str(), O_RDONLY | O_BINARY);
    if (fd < 0) return false;
    uint8_t cab[20 + 64];
    if (!lee_todo(fd, 0, cab, sizeof cab) || u32(cab) != FIRMA) { ::close(fd); fd = -1; return false; }
    const uint8_t *p = cab + 20;
    if (version_ == 5) {   // versión 5: diez campos de 32 bits
        v.datos = u32(p); v.primero = u32(p + 8); v.ultimo = u32(p + 12);
        v.primero_off = u32(p + 16); v.primero_tam = u32(p + 20); v.primero_comp = u32(p + 24);
        v.ultimo_off = u32(p + 28); v.ultimo_tam = u32(p + 32); v.ultimo_comp = u32(p + 36);
        return true;
    }
    v.datos = u32(p) | ((uint64_t)u32(p + 4) << 32);
    v.primero = u32(p + 8); v.ultimo = u32(p + 12);
    v.primero_off = u32(p + 16) | ((uint64_t)u32(p + 20) << 32);
    v.primero_tam = u32(p + 24) | ((uint64_t)u32(p + 28) << 32);
    v.primero_comp = u32(p + 32) | ((uint64_t)u32(p + 36) << 32);
    v.ultimo_off = u32(p + 40) | ((uint64_t)u32(p + 44) << 32);
    v.ultimo_tam = u32(p + 48) | ((uint64_t)u32(p + 52) << 32);
    v.ultimo_comp = u32(p + 56) | ((uint64_t)u32(p + 60) << 32);
    return true;
}

bool Is6::extraer(uint32_t indice, const std::string &destino, std::string &error, util::Progreso *pr) {
    if (indice >= descriptores_.size()) { error = "índice de fichero fuera de la tabla"; return false; }
    Descriptor fd = descriptores_[indice];
    if (fd.enlace & ENLACE_PREV) return extraer(fd.enlace_prev, destino, error, pr);   // los datos son los de otro
    if ((fd.flags & F_NO_VALIDO) || !fd.datos) { error = ficheros_[indice].nombre + ": entrada no válida"; return false; }
    if (fd.flags & F_OFUSCADO) { error = ficheros_[indice].nombre + ": fichero ofuscado (no soportado)"; return false; }
    bool comprimido = fd.flags & F_COMPRIMIDO;

    // el lector por volúmenes: dónde empiezan los datos y cuántos quedan en el volumen actual
    unsigned vol = fd.volumen ? fd.volumen : 1;
    int vfd = -1;
    Volumen v;
    if (version_ == 5)   // el descriptor no dice el volumen: el primero cuyo rango de ficheros lo incluye
        for (unsigned n = 1; n < 100; n++) {
            int f5 = -1;
            if (!lee_volumen(n, v, f5)) break;
            ::close(f5);
            if (indice >= v.primero && indice <= v.ultimo) { vol = n; break; }
        }
    uint64_t pos = 0, quedan_vol = 0;
    auto abre = [&](unsigned n) -> bool {
        if (vfd >= 0) ::close(vfd);
        if (!lee_volumen(n, v, vfd)) { error = "no se puede leer DATA" + std::to_string(n) + ".CAB"; return false; }
        vol = n;
        uint64_t comp, tam;
        if (fd.flags & F_PARTIDO) {
            if (indice == v.ultimo && v.ultimo_off != 0x7FFFFFFFu) { pos = v.ultimo_off; tam = v.ultimo_tam; comp = v.ultimo_comp; }
            else if (indice == v.primero) { pos = v.primero_off; tam = v.primero_tam; comp = v.primero_comp; }
            else { error = ficheros_[indice].nombre + ": fichero partido sin datos en el volumen " + std::to_string(n); return false; }
        } else { pos = fd.datos; tam = fd.tam; comp = fd.comprimido; }
        quedan_vol = comprimido ? comp : tam;
        return true;
    };
    auto lee = [&](uint8_t *dst, size_t n) -> bool {
        while (n) {
            size_t k = (size_t)std::min<uint64_t>(n, quedan_vol);
            if (!k) { if (!abre(vol + 1)) return false; continue; }
            if (!lee_todo(vfd, pos, dst, k)) { error = ficheros_[indice].nombre + ": lectura fallida en DATA" + std::to_string(vol) + ".CAB"; return false; }
            pos += k; quedan_vol -= k; dst += k; n -= k;
        }
        return true;
    };
    if (!abre(vol)) return false;

    util::mkdirs(util::carpeta_de(destino));
    std::ofstream out(destino, std::ios::binary | std::ios::trunc);
    if (!out) { if (vfd >= 0) ::close(vfd); error = "no se puede escribir " + destino; return false; }
    Md5 md5;
    uint64_t escrito = 0, quedan = comprimido ? fd.comprimido : fd.tam;
    std::vector<uint8_t> in, salida;
    Inflador inf;
    bool ok = true;
    while (quedan > 0 && ok) {
        if (comprimido) {
            uint8_t lb[2];
            if (!(ok = lee(lb, 2))) break;
            size_t n = u16(lb);
            if (!n) { error = ficheros_[indice].nombre + ": trozo vacío"; ok = false; break; }
            in.resize(n);
            if (!(ok = lee(in.data(), n))) break;
            salida.clear();
            inf.reinicia();
            if (!inf.inflar(in.data(), n, salida, false, error)) { error = ficheros_[indice].nombre + ": " + error; ok = false; break; }
            quedan -= 2 + n;
        } else {
            size_t n = (size_t)std::min<uint64_t>(quedan, 1u << 20);
            salida.resize(n);
            if (!(ok = lee(salida.data(), n))) break;
            quedan -= n;
        }
        out.write((const char *)salida.data(), (std::streamsize)salida.size());
        md5.update(salida.data(), salida.size());
        escrito += salida.size();
        if (pr) { pr->hecho += salida.size(); if (pr->cancelar) { error = "cancelado"; ok = false; } }
    }
    if (vfd >= 0) ::close(vfd);
    out.close();
    if (!ok) return false;
    if (escrito != fd.tam) { error = ficheros_[indice].nombre + ": tamaño " + std::to_string(escrito) + " en vez de " + std::to_string(fd.tam); return false; }
    uint8_t r[16];
    md5.final(r);
    if (std::memcmp(r, fd.md5, 16) != 0) { error = ficheros_[indice].nombre + ": el MD5 no coincide (datos corruptos en el CD)"; return false; }
    return true;
}
