// lanzador — versiones soportadas y reconocimiento (ver versiones.h)
#include "versiones.h"

#include "json.h"
#include "util.h"

std::vector<std::string> Version::roles_necesarios(bool con_instalada) const {
    std::vector<std::string> r;
    bool instalada = solo_instalada || (con_instalada && admite_instalada());
    if (instalada) r.push_back("instalada");
    for (auto &d : discos) if (!(instalada && d.solo_instalar)) r.push_back(d.rol);
    if (!parches.ficheros.empty()) r.push_back("parches");
    return r;
}

std::string Version::nombre_rol(const std::string &rol) const {
    if (rol == "instalada") return "la carpeta del juego instalado (C:\\" + carpeta + ")";
    if (rol == "parches") return parches.nombre;
    const Disco *d = disco(rol);
    return d ? "el " + d->nombre : rol;
}

bool firma_coincide(const std::string &dir, const Firma &f) {
    std::string p = util::resolver(dir, f.rel);
    if (p.empty() || util::es_dir(p) || util::tam(p) != f.tam) return false;
    return util::sha256_fichero(p) == f.sha256;
}

static std::string parche_en(const std::string &dir, const Parche &pa) {
    if (firma_coincide(dir, {pa.nombre, pa.sha256, pa.tam})) return util::resolver(dir, pa.nombre);
    for (const std::string &n : util::listar(dir)) {
        if (n.empty() || n[0] == '.') continue;
        std::string p = dir + "/" + n;
        if (util::es_dir(p) || util::tam(p) != pa.tam) continue;
        if (util::sha256_fichero(p) == pa.sha256) return p;
    }
    return "";
}

// Los paquetes se buscan en la carpeta elegida y, si no están, en sus subcarpetas; y si la carpeta ya tiene alguno
// (es una carpeta de actualizaciones a medias), también en la de encima y sus subcarpetas. Así vale la recopilación
// del foro tal cual se descomprime: las 12 actualizaciones en una carpeta y el parche 3.6 (PCFPATCH.EXE) al lado.
std::string localizar_parche(const std::string &dir, const Parche &pa) {
    std::string p = parche_en(dir, pa);
    if (!p.empty()) return p;
    auto en_subcarpetas = [&](const std::string &d, const std::string &salvo) -> std::string {
        for (const std::string &n : util::listar(d)) {
            if (n.empty() || n[0] == '.' || d + "/" + n == salvo || !util::es_dir(d + "/" + n)) continue;
            std::string q = parche_en(d + "/" + n, pa);
            if (!q.empty()) return q;
        }
        return "";
    };
    p = en_subcarpetas(dir, "");
    if (!p.empty()) return p;
    size_t barra = dir.find_last_of("/\\");
    if (barra == std::string::npos || barra == 0) return "";
    std::string arriba = dir.substr(0, barra);
    bool tiene_alguno = false;   // algún PCF*.EXE: si no, no es una carpeta de actualizaciones
    for (const std::string &n : util::listar(dir)) {
        std::string m = util::minusculas(n);
        if (m.size() > 4 && m.substr(m.size() - 4) == ".exe" && m.find("pcf") != std::string::npos) { tiene_alguno = true; break; }
    }
    if (!tiene_alguno) return "";
    p = parche_en(arriba, pa);
    if (!p.empty()) return p;
    return en_subcarpetas(arriba, dir);
}

static std::vector<Firma> firmas_de(const Json &j) {
    std::vector<Firma> out;
    for (size_t i = 0; i < j.size(); i++) out.push_back({j[i][0].str(), j[i][1].str(), (uint64_t)j[i][2].num()});
    return out;
}

bool Versiones::cargar(const std::string &path, std::string &error) {
    Json j;
    std::string texto = util::leer(path);
    if (texto.empty()) { error = "no se puede leer " + path; return false; }
    if (!Json::parsea(texto, j, &error)) { error = path + ": " + error; return false; }
    generado = j["generado"].str();
    lista.clear();
    const Json &vs = j["versiones"];
    for (size_t i = 0; i < vs.size(); i++) {
        const Json &x = vs[i];
        Version v;
        v.id = x["id"].str(); v.nombre = x["nombre"].str(); v.detalle = x["detalle"].str(); v.carpeta = x["carpeta"].str();
        v.corto = x["corto"].str(); v.anio = (int)x["anio"].num(); v.portada = x["portada"].str(); v.etiqueta = x["etiqueta"].str();
        v.nota = x["nota"].str(); v.extra = x["extra"].str();
        for (size_t k = 0; k < x["extra_copias"].size(); k++) v.extra_copias.emplace_back(x["extra_copias"][k][0].str(), x["extra_copias"][k][1].str());
        v.dos = x["dos"].bul(); v.necesita_cd = x["necesita_cd"].bul(true); v.solo_instalada = x["solo_instalada"].bul();
        for (size_t k = 0; k < x["programas"].size(); k++) v.programas.push_back(x["programas"][k].str());
        for (size_t k = 0; k < x["discos"].size(); k++) {
            const Json &d = x["discos"][k];
            Disco disco;
            disco.rol = d["rol"].str(); disco.nombre = d["nombre"].str();
            disco.firmas = firmas_de(d["firmas"]);
            for (size_t m = 0; m < d["instalar"].size(); m++) {
                std::vector<std::string> op;
                for (size_t q = 0; q < d["instalar"][m].size(); q++) op.push_back(d["instalar"][m][q].str());
                disco.instalar.push_back(op);
            }
            if (d["copiar"].tipo == Json::CADENA) disco.copiar_todo = true;
            else { disco.copiar_todo = false; for (size_t m = 0; m < d["copiar"].size(); m++) disco.copiar.push_back(d["copiar"][m].str()); }
            for (size_t m = 0; m < d["sistema"].size(); m++) disco.sistema.emplace_back(d["sistema"][m][0].str(), d["sistema"][m][1].str());
            disco.solo_instalar = d["solo_instalar"].bul();
            v.discos.push_back(disco);
        }
        for (size_t k = 0; k < x["binarios"].size(); k++) {
            const Json &b = x["binarios"][k];
            Binario bin;
            bin.nombre = b["nombre"].str(); bin.sha256 = b["sha256"].str(); bin.tam = (uint64_t)b["tam"].num();
            bin.solo_sistema = true;
            for (size_t m = 0; m < b["origen"].size(); m++) if (b["origen"][m][0].str() != "sistema") bin.solo_sistema = false;
            v.binarios.push_back(bin);
        }
        v.instalada = firmas_de(x["instalada"]["firmas"]);
        if (x.tiene("parches")) {
            const Json &pj = x["parches"];
            v.parches.nombre = pj["nombre"].str();
            for (size_t k = 0; k < pj["ficheros"].size(); k++) {
                const Json &f = pj["ficheros"][k];
                Parche pa;
                pa.nombre = f["nombre"].str(); pa.sha256 = f["sha256"].str(); pa.tam = (uint64_t)f["tam"].num(); pa.rtp = f["rtp"].str();
                for (size_t m = 0; m < f["copias"].size(); m++) pa.copias.emplace_back(f["copias"][m][0].str(), f["copias"][m][1].str());
                v.parches.ficheros.push_back(pa);
            }
            for (size_t k = 0; k < pj["escribe"].size(); k++) v.parches.escribe.emplace_back(pj["escribe"][k][0].str(), pj["escribe"][k][1].str());
        }
        lista.push_back(v);
    }
    if (lista.empty()) { error = path + ": sin versiones"; return false; }
    return true;
}

static void identifica_en(const Versiones &vs, const std::string &dir, std::vector<Coincidencia> &out) {
    for (const Version &v : vs.lista) {
        bool es_cd = false;
        for (const Disco &d : v.discos) {
            if (d.firmas.empty()) continue;
            bool ok = true;
            for (const Firma &f : d.firmas) if (!firma_coincide(dir, f)) { ok = false; break; }
            if (ok) { out.push_back({&v, d.rol, dir}); es_cd = true; }
        }
        // la carpeta de las actualizaciones: todos los paquetes, con su SHA-256
        if (!v.parches.ficheros.empty()) {
            bool ok = true;
            for (const Parche &pa : v.parches.ficheros) if (localizar_parche(dir, pa).empty()) { ok = false; break; }
            if (ok) { out.push_back({&v, "parches", dir}); continue; }
        }
        // la raíz de un CD de Dinamic lleva los mismos EXE que la instalación: si es un CD, no es la carpeta instalada
        if (!v.instalada.empty() && !es_cd) {
            bool ok = true;
            for (const Firma &f : v.instalada) if (!firma_coincide(dir, f)) { ok = false; break; }
            if (ok) out.push_back({&v, "instalada", dir});
        }
    }
}

std::vector<Coincidencia> Versiones::identificar(const std::string &dir) const {
    std::vector<Coincidencia> out;
    identifica_en(*this, dir, out);
    if (out.empty())   // quizá ha elegido la carpeta de encima (la que contiene FUTBOL7, o la copia del CD)
        for (const std::string &n : util::listar(dir))
            if (n[0] != '.' && util::es_dir(dir + "/" + n)) identifica_en(*this, dir + "/" + n, out);
    return out;
}
