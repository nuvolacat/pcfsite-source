// lanzador — la instalación (ver instalar.h)
//
// Los instaladores de Dinamic (4.0 a 7 Plus) son guiones de texto en la raíz del CD (INST_1xx.NFO; en el 7, .NFC:
// u16 longitud + bytes XOR 0xA5 por línea) con tres órdenes: F origen [> destino] copia un fichero, X origen [> destino]
// copia una carpeta mezclándola, D carpeta la crea. Un origen .CPR es un contenedor (entradas ZIP locales con deflate,
// sin directorio central): con F se extrae en la raíz de la instalación y con X en la carpeta de su nombre. El 4.0
// copia la carpeta FILES del CD entera. 2000 y 2001 (InstallShield) aún no se reproducen: se copia la carpeta instalada.
#include <strings.h>
#include "instalar.h"

#include "is6.h"
#include "mscab.h"
#include "zip.h"

#include "json.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#include <cstdio>
#include <memory>
#include <cstdlib>
#include <algorithm>
#include <cstring>

bool Tarea::completa() const {
    if (!v) return false;
    for (const std::string &rol : roles())
        if (!fuentes.count(rol)) return false;
    return true;
}

namespace {

uint32_t crc32(const unsigned char *p, size_t n) {
    static uint32_t tabla[256];
    static bool hecha = false;
    if (!hecha) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tabla[i] = c;
        }
        hecha = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tabla[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct Ejecutor {
    const Tarea &t;
    util::Progreso &pr;
    bool contar;          // primera pasada: solo sumar bytes
    std::string registro;
    std::string game;     // destino/game/<CARPETA>
    bool con_instalada = false;   // se parte de la carpeta instalada del usuario

    Ejecutor(const Tarea &t_, util::Progreso &pr_, bool c) : t(t_), pr(pr_), contar(c) { game = t.destino + "/game/" + t.v->carpeta; }

    void log(const std::string &s) { if (!contar) registro += s + "\n"; }
    bool fallo(const std::string &e) { pr.falla(e); log("ERROR: " + e); return false; }
    bool cancelado() { return pr.cancelar; }

    bool copia_fichero(const std::string &src, const std::string &dst) {
        if (contar) { pr.total += util::tam(src); return true; }
        pr.pon(util::nombre_base(dst));
        // el mismo nombre con otras mayúsculas (el CD es ISO 9660): se sustituye
        std::string dir = util::carpeta_de(dst);
        util::mkdirs(dir);
        for (const std::string &n : util::listar(dir))
            if (n != util::nombre_base(dst) && strcasecmp(n.c_str(), util::nombre_base(dst).c_str()) == 0) util::borrar_arbol(dir + "/" + n);
        if (!util::copiar_fichero(src, dst, &pr)) return cancelado() ? false : fallo("no se puede copiar " + src);
        return true;
    }
    bool copia_arbol(const std::string &src, const std::string &dst) {
        if (contar) { pr.total += util::tam_arbol(src); return true; }
        pr.pon(util::nombre_base(src) + "/");
        if (!util::copiar_arbol(src, dst, &pr)) return cancelado() ? false : fallo("no se puede copiar la carpeta " + src);
        return true;
    }

    // un contenedor .CPR del CD → carpeta destino
    bool extrae_cpr(const std::string &cpr, const std::string &dst) {
        std::string datos = util::leer(cpr);
        if (datos.size() < 30) return fallo("contenedor vacío: " + cpr);
        const unsigned char *d = (const unsigned char *)datos.data();
        size_t off = 0, n = 0;
        auto u16 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8; };
        auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
        while (off + 30 <= datos.size() && memcmp(d + off, "PK\x03\x04", 4) == 0) {
            uint32_t flags = u16(off + 6), metodo = u16(off + 8), crc = u32(off + 14), csize = u32(off + 18), usize = u32(off + 22);
            uint32_t nlen = u16(off + 26), xlen = u16(off + 28);
            std::string nombre = datos.substr(off + 30, nlen);
            for (auto &c : nombre) if (c == '\\') c = '/';
            size_t inicio = off + 30 + nlen + xlen;
            if (flags & 8) return fallo(cpr + ": entrada con descriptor de datos (no soportado)");
            if (inicio + csize > datos.size()) return fallo(cpr + ": contenedor truncado");
            off = inicio + csize;
            n++;
            if (nombre.empty() || nombre.back() == '/') { if (!contar) util::mkdirs(dst + "/" + nombre); continue; }
            if (contar) { pr.total += usize; continue; }
            if (cancelado()) return false;
            pr.pon(util::nombre_base(nombre));
            std::string salida;
            if (metodo == 8) {
                int outlen = 0;
                char *out = stbi_zlib_decode_malloc_guesssize_headerflag((const char *)d + inicio, (int)csize, (int)usize, &outlen, 0);
                if (!out) return fallo(cpr + ": " + nombre + ": no se puede descomprimir");
                salida.assign(out, (size_t)outlen);
                free(out);
            } else if (metodo == 0) {
                salida = datos.substr(inicio, csize);
            } else return fallo(cpr + ": " + nombre + ": método " + std::to_string(metodo) + " no soportado");
            if (salida.size() != usize || crc32((const unsigned char *)salida.data(), salida.size()) != crc)
                return fallo(cpr + ": " + nombre + ": tamaño o CRC incorrecto");
            if (!util::escribir_sustituyendo(dst + "/" + nombre, salida.data(), salida.size(), &pr)) return fallo("no se puede escribir " + dst + "/" + nombre);
        }
        log("  " + util::nombre_base(cpr) + ": " + std::to_string(n) + " entradas → " + dst.substr(t.destino.size() + 1));
        return true;
    }

    static std::vector<std::string> lee_guion(const std::string &path) {
        std::string d = util::leer(path);
        std::vector<std::string> lineas;
        if (util::termina_en(path, ".nfc")) {
            size_t o = 0;
            while (o + 2 <= d.size()) {
                size_t n = (unsigned char)d[o] | (unsigned char)d[o + 1] << 8;
                o += 2;
                std::string l = d.substr(o, n);
                for (auto &c : l) c = (char)((unsigned char)c ^ 0xA5);
                lineas.push_back(l);
                o += n;
            }
        } else {
            size_t i = 0;
            while (i <= d.size()) {
                size_t j = d.find('\n', i);
                if (j == std::string::npos) j = d.size();
                std::string l = d.substr(i, j - i);
                if (!l.empty() && l.back() == '\r') l.pop_back();
                lineas.push_back(l);
                i = j + 1;
            }
        }
        return lineas;
    }

    // un guion del instalador (.nfo / .NFC) desde el CD `cd`
    bool guion(const std::string &cd, const std::string &nombre) {
        std::string path = util::resolver(cd, nombre);
        if (path.empty()) return fallo("falta el guion " + nombre + " en " + cd);
        log("guion " + nombre);
        int faltan = 0;
        for (std::string l : lee_guion(path)) {
            while (!l.empty() && (l.back() == ' ' || l.back() == '\t')) l.pop_back();
            size_t a = l.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            l = l.substr(a);
            if (l[0] == '*') continue;
            char orden = (char)toupper((unsigned char)l[0]);
            std::string resto = l.substr(1);
            size_t b = resto.find_first_not_of(" \t");
            resto = b == std::string::npos ? "" : resto.substr(b);
            if (orden == 'D') {
                std::string dir = resto;
                for (auto &c : dir) if (c == '\\') c = '/';
                if (!contar) util::mkdirs(game + "/" + dir);
                continue;
            }
            if (orden != 'F' && orden != 'X') { log("  orden desconocida: " + l); continue; }
            std::string src = resto, dst;
            size_t g = resto.find('>');
            if (g != std::string::npos) {
                src = resto.substr(0, g);
                dst = resto.substr(g + 1);
                while (!src.empty() && src.back() == ' ') src.pop_back();
                size_t c = dst.find_first_not_of(" \t");
                dst = c == std::string::npos ? "" : dst.substr(c);
            }
            if (dst.empty()) dst = src;
            for (auto &c : dst) if (c == '\\') c = '/';
            std::string real = util::resolver(cd, src);
            if (real.empty()) { log("  no está en el CD: " + src); faltan++; continue; }
            if (util::termina_en(src, ".cpr")) {
                std::string carpeta = game;
                if (orden == 'X') {
                    std::string stem = util::nombre_base(dst);
                    size_t p = stem.rfind('.');
                    if (p != std::string::npos) stem.resize(p);
                    carpeta = game + "/" + util::carpeta_de(dst) + (util::carpeta_de(dst).empty() ? "" : "/") + stem;
                }
                if (!extrae_cpr(real, carpeta)) return false;
                continue;
            }
            bool ok = util::es_dir(real) ? copia_arbol(real, game + "/" + dst) : copia_fichero(real, game + "/" + dst);
            if (!ok) return false;
        }
        if (faltan) log("  " + std::to_string(faltan) + " entradas que no están en el CD");
        return true;
    }

    std::unique_ptr<Is6> is6_;
    std::string is6_cd_;
    // todos los ficheros de un grupo del cabinet de InstallShield 6 que hay en `cd` (DATA1.HDR, DATAn.CAB), a `destino`
    // respetando sus subcarpetas
    bool grupo_is6(const std::string &cd, const std::string &grupo, const std::string &destino) {
        std::string error;
        if (!is6_ || is6_cd_ != cd) {
            is6_ = std::make_unique<Is6>();
            if (!is6_->abrir(cd, error)) return fallo(error);
            is6_cd_ = cd;
        }
        const Is6Grupo *g = is6_->grupo(grupo);
        if (!g) return fallo("el instalador del CD no tiene el grupo «" + grupo + "»");
        log("InstallShield: grupo «" + grupo + "» → " + destino);
        int n = 0;
        for (uint32_t i = g->primero; i <= g->ultimo && i < is6_->ficheros().size(); i++) {
            const Is6Fichero &f = is6_->ficheros()[i];
            if (!f.valido) continue;
            if (contar) { pr.total += f.tam; continue; }
            if (cancelado()) return false;
            std::string dir = f.directorio;
            for (auto &ch : dir) if (ch == '\\') ch = '/';
            std::string dst = destino + (dir.empty() ? "" : "/" + dir) + "/" + f.nombre;
            pr.pon(f.nombre);
            if (!is6_->extraer(i, dst, error, &pr)) return fallo(error);
            n++;
        }
        if (!contar) log("  " + std::to_string(n) + " ficheros");
        return true;
    }

    // las actualizaciones oficiales: de cada paquete (EXE) se saca el .RTP (y el parcheador del primero) a
    // C:\WINDOWS\PARCHES, y el programa `pcfpatch` (patchw32.dll traducida) lo aplica sobre la instalación
    bool parches() {
        const Parches &P = t.v->parches;
        auto it = t.fuentes.find("parches");
        if (it == t.fuentes.end()) return fallo("falta " + P.nombre);
        const std::string &src = it->second;
        std::string pd = t.destino + "/drive_c/WINDOWS/PARCHES";
        if (contar) { for (const Parche &pa : P.ficheros) pr.total += pa.tam * 4; return true; }   // (estimación: lo que tarda en aplicarse)
        log("== actualizaciones: " + src);
        std::vector<std::string> orden = util::orden_programa("pcfpatch");
        std::string exe = orden[0];
        if (!util::existe(exe)) return fallo("falta el parcheador " + exe);
#ifdef _WIN32
        if (!util::existe(util::dir_ejecutables() + "/programas/pcfpatch.dll")) return fallo("falta el parcheador programas/pcfpatch.dll");
#endif
        util::mkdirs(pd);
        bool parcheador = false;
        int n = 0;
        for (const Parche &pa : P.ficheros) {
            if (cancelado()) return false;
            n++;
            std::string paquete = localizar_parche(src, pa);
            if (paquete.empty()) return fallo("falta " + pa.nombre + " en " + src);
            pr.pon("actualización " + std::to_string(n) + " de " + std::to_string(P.ficheros.size()) + ": " + pa.nombre);
            std::vector<MscabEntrada> entradas;
            std::string error;
            if (!mscab_extraer(paquete, entradas, error)) return fallo(pa.nombre + ": " + error);
            bool rtp = false;
            std::map<std::string, const MscabEntrada *> por_nombre;
            for (const MscabEntrada &e : entradas) por_nombre[util::minusculas(e.nombre)] = &e;
            for (const MscabEntrada &e : entradas) {
                std::string n = util::minusculas(e.nombre);
                bool es_rtp = n == util::minusculas(pa.rtp);
                bool es_parcheador = (n == "pcfpatch.exe" || n == "patchw32.dll") && !parcheador;
                if (!es_rtp && !es_parcheador) continue;
                if (!util::escribir(pd + "/" + e.nombre, std::string(e.datos.begin(), e.datos.end()))) return fallo("no se puede escribir en " + pd);
                if (es_rtp) rtp = true;
            }
            if (por_nombre.count("pcfpatch.exe") && por_nombre.count("patchw32.dll")) parcheador = true;
            if (!rtp) return fallo(pa.nombre + " no contiene " + pa.rtp);
            if (!parcheador) return fallo(pa.nombre + ": el paquete no trae el parcheador (PCFPATCH.exe y patchw32.dll)");
            // aplicar
            setenv("PCF7_WORK", t.destino.c_str(), 1);
            setenv("PCF7_RTPATCH", ("C:\\WINDOWS\\PARCHES\\" + pa.rtp).c_str(), 1);
            setenv("PCF7_HEADLESS", "1", 1);
            setenv("PCF7_NOLAUNCHER", "1", 1);
            std::string salida;
            int rc = util::ejecutar(orden, salida);
            unsetenv("PCF7_RTPATCH"); unsetenv("PCF7_HEADLESS"); unsetenv("PCF7_NOLAUNCHER");
            std::string registro_p = util::leer(t.destino + "/pcfpatch.log");
            std::string resumen;
            for (size_t i = 0, j; i < registro_p.size(); i = j + 1) {
                j = registro_p.find('\n', i); if (j == std::string::npos) j = registro_p.size();
                std::string l = registro_p.substr(i, j - i);
                if (l.rfind("rtpatch: RTPatchApply32", 0) == 0 || l.rfind("rtpatch: error", 0) == 0) resumen += "  " + l + "\n";
            }
            log("  " + pa.nombre + " → " + pa.rtp + (rc == 0 ? " aplicada" : " FALLIDA (" + std::to_string(rc) + ")") + "\n" + resumen);
            if (rc != 0) return fallo("la actualización " + pa.nombre + " no se ha podido aplicar (detalles en instalacion.log)");
            for (const auto &cp : pa.copias) {   // lo que el paquete deja aparte (Utils.dll con MMX)
                auto e = por_nombre.find(util::minusculas(cp.first));
                if (e == por_nombre.end()) return fallo(pa.nombre + " no contiene " + cp.first);
                if (!util::escribir_sustituyendo(game + "/" + cp.second, (const char *)e->second->datos.data(), e->second->datos.size(), &pr)) return fallo("no se puede escribir " + cp.second);
                log("  " + cp.first + " → " + cp.second);
            }
            pr.hecho += pa.tam * 4;
        }
        for (const auto &w : P.escribe) { util::escribir(game + "/" + w.first, w.second); log("  " + w.first + " = " + w.second); }
        util::borrar_arbol(pd);
        util::borrar_arbol(game + "/BACKUP");
        return true;
    }

    bool disco(const Disco &d) {
        auto it = t.fuentes.find(d.rol);
        if (it == t.fuentes.end()) return fallo("falta " + d.nombre);
        const std::string &cd = it->second;
        log("== " + d.nombre + ": " + cd);
        // con la carpeta instalada no se repite la instalación desde el disco: solo las DLL de sistema y lo que se lee como D:
        for (const auto &op : con_instalada ? std::vector<std::vector<std::string>>() : d.instalar) {
            if (cancelado()) return false;
            if (op.empty()) continue;
            if (op[0] == "nfo" || op[0] == "nfc") { if (!guion(cd, op[1])) return false; }
            else if (op[0] == "arbol") {
                std::string src = util::resolver(cd, op[1]);
                if (src.empty()) return fallo("falta la carpeta " + op[1] + " en el CD");
                if (!copia_arbol(src, game + (op.size() > 2 && !op[2].empty() ? "/" + op[2] : ""))) return false;
            } else if (op[0] == "copia") {
                std::string src = util::resolver(cd, op[1]);
                if (src.empty()) return fallo("falta " + op[1] + " en el CD");
                if (!copia_fichero(src, game + "/" + op[2])) return false;
            } else if (op[0] == "carpeta") {   // una carpeta vacía que el instalador original creaba (partidas, notas…)
                util::mkdirs(game + "/" + op[1]);
            } else if (op[0] == "is6") {   // un grupo del cabinet de InstallShield 6 del CD, a la instalación
                if (!grupo_is6(cd, op[1], game + (op.size() > 2 && !op[2].empty() ? "/" + op[2] : ""))) return false;
            } else log("  operación desconocida: " + op[0]);
        }
        // las DLL de sistema que el instalador dejaba en WINDOWS\SYSTEM
        for (const auto &s : d.sistema) {
            if (s.first.rfind("is6:", 0) == 0) { if (!grupo_is6(cd, s.first.substr(4), t.destino + "/drive_c/WINDOWS/SYSTEM")) return false; continue; }
            std::string src = util::resolver(cd, s.first);
            if (src.empty()) { log("  no está en el CD: " + s.first); continue; }
            if (!copia_fichero(src, t.destino + "/drive_c/WINDOWS/SYSTEM/" + s.second)) return false;
        }
        // lo que el juego lee del CD (D:): nada si se va a usar la ISO montada directamente o si el disco solo es para instalar
        if (d.solo_instalar) return true;
        if (t.cd_desde_iso()) { log("  el CD se usará desde la ISO " + t.isos.at(d.rol)); return true; }
        std::string iso = t.destino + "/iso";
        if (d.copiar_todo) { if (!copia_arbol(cd, iso)) return false; }
        else for (const std::string &e : d.copiar) {
            std::string src = util::resolver(cd, e);
            if (src.empty()) { log("  no está en el CD: " + e); continue; }
            bool ok = util::es_dir(src) ? copia_arbol(src, iso + "/" + util::nombre_base(src)) : copia_fichero(src, iso + "/" + util::nombre_base(src));
            if (!ok) return false;
        }
        return true;
    }

    bool todo() {
        std::vector<std::string> roles = t.roles();
        con_instalada = std::find(roles.begin(), roles.end(), "instalada") != roles.end();
        if (con_instalada) {
            auto it = t.fuentes.find("instalada");
            if (it == t.fuentes.end()) return fallo("falta la carpeta del juego instalado");
            log("== instalación: " + it->second);
            if (!copia_arbol(it->second, game)) return false;
        }
        for (const Disco &d : t.v->discos) {
            if (con_instalada && d.solo_instalar) continue;   // la carpeta instalada ya trae lo que se instalaba desde él
            if (!disco(d)) return false;
        }
        if (!t.v->parches.ficheros.empty() && !parches()) return false;
        if (!t.v->extra.empty()) {
            std::string ex = util::dir_recursos() + "/extra/" + t.v->extra;
#ifdef PCF_APP_SRC
            if (!util::es_dir(ex)) ex = std::string(PCF_APP_SRC) + "/../core/juegos/" + t.v->extra + "/datos_extra";   // desarrollo: el repo
#endif
            if (util::es_dir(ex)) { log("== extra: " + ex); if (!copia_arbol(ex, game)) return false; }
            else log("no hay carpeta extra " + ex);
            for (const auto &c : t.v->extra_copias) {
                std::string src = util::resolver(game, c.first);
                if (src.empty()) { log("  no está " + c.first); continue; }
                if (!copia_fichero(src, game + "/" + c.second)) return false;
            }
        }
        if (!contar) { util::mkdirs(t.destino + "/iso"); util::mkdirs(t.destino + "/drive_c/WINDOWS/SYSTEM"); }
        return true;
    }
};

// busca un fichero por nombre (sin mayúsculas) bajo dir, hasta `prof` niveles, con el tamaño y SHA-256 dados
bool busca(const std::string &dir, const Binario &b, int prof) {
    std::vector<std::string> sub;
    for (const std::string &n : util::listar(dir)) {
        std::string p = dir + "/" + n;
        if (util::es_dir(p)) { if (prof > 0) sub.push_back(p); continue; }
        if (strcasecmp(n.c_str(), b.nombre.c_str()) == 0 && util::tam(p) == b.tam && util::sha256_fichero(p) == b.sha256) return true;
    }
    for (const std::string &s : sub) if (busca(s, b, prof - 1)) return true;
    return false;
}

}  // namespace

void instalar(const Tarea &t, util::Progreso &pr) {
    pr.hecho = 0; pr.total = 0; pr.ok = false;
    std::string registro;
    {
        Ejecutor cuenta(t, pr, true);
        cuenta.todo();
        pr.total = pr.total + 1;
    }
    util::borrar_arbol(t.destino);
    util::mkdirs(t.destino);
    Ejecutor e(t, pr, false);
    bool ok = e.todo();
    registro = e.registro;
    if (ok) {
        // ¿están los binarios que la plataforma reconstruye? (en la instalación, en WINDOWS\SYSTEM o en el CD)
        pr.pon("comprobando los binarios");
        Json faltan = Json::lista_vacia();
        for (const Binario &b : t.v->binarios) {
            bool hay = busca(t.destino + "/game", b, 4) || busca(t.destino + "/drive_c", b, 3) || busca(t.destino + "/iso", b, 2);
            if (!hay && t.cd_desde_iso()) hay = busca(t.fuentes.at(t.v->discos[0].rol), b, 2);
            registro += std::string(hay ? "ok     " : "FALTA  ") + b.nombre + "\n";
            if (!hay) faltan.anade(Json::de(b.nombre));
        }
        Json j = Json::objeto_vacio();
        j.pon("id", Json::de(t.v->id)).pon("nombre", Json::de(t.v->titulo())).pon("carpeta", Json::de(t.v->carpeta)).pon("fecha", Json::de(util::fecha_hoy()));
        Json f = Json::objeto_vacio();
        for (auto &kv : t.fuentes) f.pon(kv.first, Json::de(kv.second));
        Json origenes = Json::lista_vacia();   // lo que eligió el usuario: la ISO si la hubo, si no la carpeta
        for (const std::string &rol : t.roles()) {
            if (!t.fuentes.count(rol)) continue;
            bool es_iso = t.isos.count(rol) > 0;
            Json o = Json::objeto_vacio();
            std::string nombre = t.v->nombre_rol(rol);
            if (nombre.rfind("el ", 0) == 0) nombre = nombre.substr(3); else if (nombre.rfind("la ", 0) == 0) nombre = nombre.substr(3);
            o.pon("rol", Json::de(rol)).pon("nombre", Json::de(nombre)).pon("ruta", Json::de(es_iso ? t.isos.at(rol) : t.fuentes.at(rol))).pon("iso", Json::de(es_iso));
            origenes.anade(o);
        }
        j.pon("fuentes", f).pon("origenes", origenes).pon("faltan", faltan);
        if (t.cd_desde_iso()) j.pon("iso", Json::de(t.isos.at(t.v->discos[0].rol)));
        util::escribir(t.destino + "/instalado.json", j.serializa() + "\n");
    } else if (pr.cancelar) {
        registro += "cancelada\n";
        util::borrar_arbol(t.destino);
    } else {
        registro += "fallida: " + pr.fallo() + "\n";
    }
    util::mkdirs(t.destino);
    util::escribir(t.destino + "/instalacion.log", registro);
    if (pr.cancelar) util::borrar_arbol(t.destino);
    pr.ok = ok;
    pr.terminado = true;
}

std::vector<Instalado> instalados(const std::string &dir_datos) {
    std::vector<Instalado> out;
    for (const std::string &n : util::listar(dir_datos)) {
        std::string p = dir_datos + "/" + n + "/instalado.json";
        Json j;
        if (!util::existe(p) || !Json::parsea(util::leer(p), j)) continue;
        Instalado i;
        i.id = j["id"].str(); i.nombre = j["nombre"].str(); i.fecha = j["fecha"].str(); i.carpeta = dir_datos + "/" + n; i.iso = j["iso"].str();
        for (size_t k = 0; k < j["faltan"].size(); k++) i.faltan.push_back(j["faltan"][k].str());
        for (size_t k = 0; k < j["origenes"].size(); k++) {
            const Json &o = j["origenes"][k];
            i.origenes.push_back({o["nombre"].str(), o["ruta"].str(), o["iso"].bul()});
        }
        if (i.origenes.empty() && !i.iso.empty()) i.origenes.push_back({"CD", i.iso, true});   // fichas de antes
        for (size_t k = 0; k < j["mods"].size(); k++) {
            const Json &m = j["mods"][k];
            if (!m["nombre"].str().empty() && util::es_dir(i.carpeta + "/mods/" + m["nombre"].str())) i.mods.push_back({m["nombre"].str(), m["activo"].bul()});
        }
        for (const std::string &m : util::listar(i.carpeta + "/mods")) {   // copiados a mano: aparecen, desactivados
            if (m[0] == '.' || !util::es_dir(i.carpeta + "/mods/" + m)) continue;
            bool ya = false;
            for (const Mod &x : i.mods) if (x.nombre == m) ya = true;
            if (!ya) i.mods.push_back({m, false});
        }
        i.tam = util::tam_arbol(i.carpeta + "/game") + util::tam_arbol(i.carpeta + "/iso") + util::tam_arbol(i.carpeta + "/drive_c");
        if (!i.id.empty()) out.push_back(i);
    }
    return out;
}

std::vector<Instalado> instalados(const std::vector<std::string> &carpetas) {
    std::vector<Instalado> out;
    for (const std::string &c : carpetas)
        for (const Instalado &i : instalados(c)) {
            bool ya = false;
            for (const Instalado &o : out) if (o.id == i.id) ya = true;
            if (!ya) out.push_back(i);
        }
    return out;
}

// ---------------------------------------------------------------- mods

std::string dir_mods(const Instalado &i) { return i.carpeta + "/mods"; }
std::string dir_mod(const Instalado &i, const Mod &m) { return i.carpeta + "/mods/" + m.nombre; }

namespace {
bool es_binario(const std::string &n) { return util::termina_en(n, ".exe") || util::termina_en(n, ".dll") || util::termina_en(n, ".ex_") || util::termina_en(n, ".dl_"); }

// (los EXE y DLL cuentan aparte: se copian, pero solo valen sus textos)
// la carpeta de `dir` que replica la del juego: la propia, o la única subcarpeta (zips con una carpeta de más), o
// una que se llame como la carpeta del juego, o la que tenga dentro algo que exista en la instalación (DBDAT…)
std::string raiz_del_mod(const std::string &dir, const Instalado &i, const std::string &carpeta_juego) {
    std::vector<std::string> arriba;
    for (const std::string &n : util::listar(i.carpeta + "/game/" + carpeta_juego)) arriba.push_back(util::minusculas(n));
    std::string cur = dir;
    for (int prof = 0; prof < 4; prof++) {
        std::vector<std::string> entradas, subdirs;
        for (const std::string &n : util::listar(cur)) {
            if (n[0] == '.' || n == "__MACOSX") continue;
            entradas.push_back(n);
            if (util::es_dir(cur + "/" + n)) subdirs.push_back(n);
        }
        for (const std::string &n : entradas)
            if (std::find(arriba.begin(), arriba.end(), util::minusculas(n)) != arriba.end()) return cur;   // ya es la buena
        for (const std::string &n : subdirs)
            if (util::minusculas(n) == util::minusculas(carpeta_juego)) { cur += "/" + n; return cur; }
        if (entradas.size() == 1 && subdirs.size() == 1) { cur += "/" + subdirs[0]; continue; }
        return cur;
    }
    return cur;
}

// fallos: los que no se han podido copiar (y en primer_fallo, el primero)
int copia_mod(const std::string &src, const std::string &dst, int &binarios, int &fallos, std::string &primer_fallo) {
    int n = 0;
    util::mkdirs(dst);
    for (const std::string &e : util::listar(src)) {
        if (e[0] == '.' || e == "__MACOSX") continue;
        std::string s = src + "/" + e;
        if (util::es_dir(s)) n += copia_mod(s, dst + "/" + e, binarios, fallos, primer_fallo);
        else if (util::copiar_fichero(s, dst + "/" + e)) { n++; if (es_binario(e)) binarios++; }
        else if (fallos++ == 0) primer_fallo = dst + "/" + e;
    }
    return n;
}
// los EXE y DLL de un mod (nombre en minúsculas, tamaño), hasta 4 niveles de carpetas
void binarios_de(const std::string &dir, int nivel, std::vector<std::pair<std::string, uint64_t>> &v) {
    for (const std::string &e : util::listar(dir)) {
        if (e[0] == '.' || e == "__MACOSX") continue;
        std::string p = dir + "/" + e;
        if (util::es_dir(p)) { if (nivel < 4) binarios_de(p, nivel + 1, v); }
        else if (es_binario(util::minusculas(e))) v.push_back({util::minusculas(e), util::tam(p)});
    }
}
}  // namespace

const Version *edicion_del_mod(const std::string &dir, const Version &v, const Versiones &vs, std::string &aviso) {
    aviso.clear();
    Json j;
    if (Json::parsea(util::leer(dir + "/pcfsite-mod.json"), j)) {
        const Version *o = vs.por_id(j["juego"].str());
        if (o) return o == &v || o->id == v.id ? nullptr : o;
    }
    std::vector<std::pair<std::string, uint64_t>> bins;
    binarios_de(dir, 0, bins);
    auto es_de = [](const Version &x, const std::pair<std::string, uint64_t> &b) {
        for (const Binario &y : x.binarios) if (util::minusculas(y.nombre) == b.first && y.tam == b.second) return true;
        return false;
    };
    std::map<const Version *, int> votos;   // ediciones a las que pertenecen los que no son de esta
    std::string sueltos;
    for (const auto &b : bins) {
        if (es_de(v, b)) continue;
        bool alguna = false;
        for (const Version &x : vs.lista) if (&x != &v && x.id != v.id && es_de(x, b)) { votos[&x]++; alguna = true; }
        if (alguna) continue;
        bool suyo = false;   // solo se avisa de los que esta edición reconstruye (un instalador o una herramienta del mod da igual)
        for (const Binario &y : v.binarios) if (util::minusculas(y.nombre) == b.first) suyo = true;
        if (suyo) sueltos += (sueltos.empty() ? "" : ", ") + b.first;
    }
    if (!sueltos.empty()) aviso = sueltos + " no " + (sueltos.find(',') == std::string::npos ? "es el" : "son los") + " de " + v.nombre +
                                  " ni de otra edición: no se " + (sueltos.find(',') == std::string::npos ? "aplicará" : "aplicarán");
    const Version *mejor = nullptr;
    for (const auto &x : votos) if (!mejor || x.second > votos[mejor]) mejor = x.first;
    return mejor;
}

bool importar_mod(const Instalado &i, const Versiones &vs, const std::string &ruta, std::string &nombre, std::string &error) {
    Json j;
    std::string carpeta_juego;
    if (Json::parsea(util::leer(i.carpeta + "/instalado.json"), j)) carpeta_juego = j["carpeta"].str();
    if (carpeta_juego.empty()) { error = "no sé cuál es la carpeta del juego"; return false; }
    std::string base = util::nombre_base(ruta);
    for (const char *ext : {".zip", ".rar", ".7z"}) if (util::termina_en(base, ext)) base.resize(base.size() - std::strlen(ext));
    if (base.empty()) base = "mod";
    nombre = base;
    for (int k = 2; util::existe(dir_mods(i) + "/" + nombre); k++) nombre = base + " (" + std::to_string(k) + ")";
    std::string tmp = dir_mods(i) + "/.importando";
    util::borrar_arbol(tmp);
    std::string origen = ruta;
    if (!util::es_dir(ruta)) {
        if (util::termina_en(ruta, ".zip")) {
            if (!zip_extraer(ruta, tmp, error)) { util::borrar_arbol(tmp); return false; }
        } else {   // .rar, .7z…: el tar del sistema (libarchive: macOS, y Windows desde la versión 10 1803); en Linux el tar
                   // es GNU y no los abre: bsdtar (libarchive-tools), 7z o unrar, el que haya
            util::mkdirs(tmp);
            std::string salida;
            int rc = -1;
            std::vector<std::vector<std::string>> ordenes = {{"tar", "-xf", ruta, "-C", tmp}};
#ifdef _WIN32   // el de Windows (System32), no otro tar que haya en el PATH (Git, MSYS…), que no abre rar
            if (const char *sr = getenv("SystemRoot")) ordenes = {{std::string(sr) + "\\System32\\tar.exe", "-xf", ruta, "-C", tmp}};
#endif
#ifdef __linux__
            ordenes = {{"bsdtar", "-xf", ruta, "-C", tmp}, {"7z", "x", "-y", "-o" + tmp, ruta}, {"unrar", "x", "-y", ruta, tmp + "/"}, {"tar", "-xf", ruta, "-C", tmp}};
#endif
            for (const auto &o : ordenes) { salida.clear(); if ((rc = util::ejecutar(o, salida)) == 0) break; }
            if (rc != 0) {
                util::borrar_arbol(tmp);
                error = "no se puede descomprimir " + util::nombre_base(ruta) + (salida.empty() ? "" : ": " + salida.substr(0, 200));
#ifdef _WIN32   // el tar de Windows 10 anterior a 2020 no abre RAR5: descomprimido a mano, se importa como carpeta
                error += ". Prueba a descomprimirlo tú (con 7-Zip o WinRAR) e importa la carpeta con «Importar mod (carpeta)…»";
#endif
                return false;
            }
        }
        origen = tmp;
    }
    std::string raiz = raiz_del_mod(origen, i, carpeta_juego), dst = dir_mods(i) + "/" + nombre;
    int binarios = 0, fallos = 0;
    std::string primer_fallo;
    int n = copia_mod(raiz, dst, binarios, fallos, primer_fallo);
    if (fallos) {   // a medias no vale: el juego mezclaría ficheros del mod con los de fábrica
        util::borrar_arbol(tmp);
        util::borrar_arbol(dst);
        error = "no se han podido copiar " + std::to_string(fallos) + " de " + std::to_string(n + fallos) + " ficheros (el primero, " +
                util::nativa(primer_fallo) + ")";
#ifdef _WIN32
        error += ". Puede que Windows no deje escribir en esa carpeta: mira si tienes activado «Acceso controlado a carpetas» "
                 "(Seguridad de Windows) o si Documentos está en OneDrive; en Configuración puedes llevar los juegos a otra carpeta";
#endif
        return false;
    }
    // pcfsite-mod.json puede ir en la raíz del zip, por encima de la carpeta del juego: se guarda con el mod
    if (n > 0 && !util::existe(dst + "/pcfsite-mod.json") && util::existe(origen + "/pcfsite-mod.json"))
        util::copiar_fichero(origen + "/pcfsite-mod.json", dst + "/pcfsite-mod.json");
    util::borrar_arbol(tmp);
    if (n == 0) { util::borrar_arbol(dst); error = "no hay ningún fichero en " + util::nombre_base(ruta); return false; }
    std::string aviso;
    if (const Version *v = vs.por_id(i.id))
        if (const Version *otra = edicion_del_mod(dst, *v, vs, aviso)) {
            util::borrar_arbol(dst);
            error = "este mod es para " + otra->nombre + ": impórtalo en ese juego";
            return false;
        }
    // (los EXE y DLL del mod se copian: la plataforma aplica sobre la imagen reconstruida sus diferencias de datos y de
    // código; las de código solo cuentan si son inmediatos, que el traductor lee de la memoria: ver platform/src/imagen.cpp)
    if (binarios) error = "trae " + std::to_string(binarios) + " ejecutable(s): se aplican sus textos, equipos y valores, pero no los cambios en la lógica del juego";   // aviso, no fallo
    if (!aviso.empty()) error += (error.empty() ? "" : "; ") + aviso;
    return true;
}

bool quitar_mod(Instalado &i, const std::string &nombre) {
    for (size_t k = 0; k < i.mods.size(); k++)
        if (i.mods[k].nombre == nombre) { i.mods.erase(i.mods.begin() + (long)k); break; }
    util::borrar_arbol(dir_mods(i) + "/" + nombre);
    return guardar_mods(i);
}

bool guardar_mods(const Instalado &i) {
    Json j;
    if (!Json::parsea(util::leer(i.carpeta + "/instalado.json"), j)) return false;
    Json lista = Json::lista_vacia();
    for (const Mod &m : i.mods) { Json x = Json::objeto_vacio(); x.pon("nombre", Json::de(m.nombre)).pon("activo", Json::de(m.activo)); lista.anade(x); }
    j.pon("mods", lista);
    return util::escribir(i.carpeta + "/instalado.json", j.serializa() + "\n");
}

std::vector<std::string> mods_activos(const Instalado &i) {
    std::vector<std::string> v;
    for (const Mod &m : i.mods) if (m.activo) v.push_back(dir_mod(i, m));
    return v;
}

std::string perfil_de(const Instalado &i) {
    std::string clave;
    for (const Mod &m : i.mods) if (m.activo) clave += (clave.empty() ? "" : " + ") + m.nombre;
    if (clave.empty()) return "";
    for (auto &ch : clave) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
    return i.carpeta + "/perfiles/" + clave;
}
