// lanzador — JSON mínimo (ver json.h)
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

Json &Json::pon(const std::string &k, const Json &v) {
    tipo = OBJETO;
    for (auto &kv : objeto) if (kv.first == k) { kv.second = v; return *this; }
    objeto.emplace_back(k, v);
    return *this;
}

namespace {

struct P {
    const std::string &t;
    size_t i = 0;
    std::string err;
    explicit P(const std::string &s) : t(s) {}
    void blancos() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++; }
    bool falla(const char *m) { if (err.empty()) err = std::string(m) + " en el byte " + std::to_string(i); return false; }
    static void utf8(std::string &o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool cadena(std::string &o) {
        if (i >= t.size() || t[i] != '"') return falla("se esperaba una cadena");
        i++;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { o += c; continue; }
            if (i >= t.size()) return falla("escape incompleto");
            char e = t[i++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                if (i + 4 > t.size()) return falla("\\u incompleto");
                unsigned cp = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    unsigned lo = (unsigned)strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
                utf8(o, cp);
                break;
            }
            default: return falla("escape desconocido");
            }
        }
        if (i >= t.size()) return falla("cadena sin cerrar");
        i++;
        return true;
    }
    bool valor(Json &j) {
        blancos();
        if (i >= t.size()) return falla("fin inesperado");
        char c = t[i];
        if (c == '{') {
            i++;
            j.tipo = Json::OBJETO;
            blancos();
            if (i < t.size() && t[i] == '}') { i++; return true; }
            while (true) {
                blancos();
                std::string k;
                if (!cadena(k)) return false;
                blancos();
                if (i >= t.size() || t[i] != ':') return falla("se esperaba ':'");
                i++;
                Json v;
                if (!valor(v)) return false;
                j.objeto.emplace_back(k, std::move(v));
                blancos();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; return true; }
                return falla("se esperaba ',' o '}'");
            }
        }
        if (c == '[') {
            i++;
            j.tipo = Json::LISTA;
            blancos();
            if (i < t.size() && t[i] == ']') { i++; return true; }
            while (true) {
                Json v;
                if (!valor(v)) return false;
                j.lista.push_back(std::move(v));
                blancos();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return true; }
                return falla("se esperaba ',' o ']'");
            }
        }
        if (c == '"') { j.tipo = Json::CADENA; return cadena(j.s); }
        if (!t.compare(i, 4, "true")) { i += 4; j = Json::de(true); return true; }
        if (!t.compare(i, 5, "false")) { i += 5; j = Json::de(false); return true; }
        if (!t.compare(i, 4, "null")) { i += 4; j = Json::nulo(); return true; }
        char *fin = nullptr;
        double d = strtod(t.c_str() + i, &fin);
        if (fin == t.c_str() + i) return falla("valor desconocido");
        i = (size_t)(fin - t.c_str());
        j = Json::de(d);
        return true;
    }
};

void escapa(std::string &o, const std::string &s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    o += '"';
}

}  // namespace

bool Json::parsea(const std::string &texto, Json &out, std::string *error) {
    P p(texto);
    bool ok = p.valor(out);
    if (ok) { p.blancos(); if (p.i != texto.size()) ok = p.falla("sobra texto"); }
    if (!ok && error) *error = p.err;
    return ok;
}

std::string Json::serializa(int sangria) const {
    std::string o;
    std::string pad(sangria, ' ');
    switch (tipo) {
    case NULO: return "null";
    case BOOL: return b ? "true" : "false";
    case NUMERO: {
        char buf[64];
        if (std::floor(n) == n && std::fabs(n) < 1e15) snprintf(buf, sizeof buf, "%.0f", n);
        else snprintf(buf, sizeof buf, "%.17g", n);
        return buf;
    }
    case CADENA: escapa(o, s); return o;
    case LISTA:
        o += "[";
        for (size_t i = 0; i < lista.size(); i++) o += (i ? ", " : "") + lista[i].serializa(sangria);
        return o + "]";
    case OBJETO:
        o += "{\n";
        for (size_t i = 0; i < objeto.size(); i++) {
            o += pad + " ";
            escapa(o, objeto[i].first);
            o += ": " + objeto[i].second.serializa(sangria + 1) + (i + 1 < objeto.size() ? ",\n" : "\n");
        }
        return o + pad + "}";
    }
    return o;
}
