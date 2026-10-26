// lanzador — JSON mínimo (leer versiones.json e instalado.json; escribir instalado.json).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json {
    enum Tipo { NULO, BOOL, NUMERO, CADENA, LISTA, OBJETO } tipo = NULO;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> lista;
    std::vector<std::pair<std::string, Json>> objeto;   // en orden

    static Json nulo() { return Json(); }
    static Json de(bool v) { Json j; j.tipo = BOOL; j.b = v; return j; }
    static Json de(double v) { Json j; j.tipo = NUMERO; j.n = v; return j; }
    static Json de(int v) { return de((double)v); }
    static Json de(const std::string &v) { Json j; j.tipo = CADENA; j.s = v; return j; }
    static Json de(const char *v) { return de(std::string(v)); }
    static Json lista_vacia() { Json j; j.tipo = LISTA; return j; }
    static Json objeto_vacio() { Json j; j.tipo = OBJETO; return j; }

    bool es_nulo() const { return tipo == NULO; }
    bool tiene(const std::string &k) const { return tipo == OBJETO && busca(k) != nullptr; }
    const Json &operator[](const std::string &k) const { static const Json vacio; const Json *j = busca(k); return j ? *j : vacio; }
    const Json &operator[](size_t i) const { static const Json vacio; return tipo == LISTA && i < lista.size() ? lista[i] : vacio; }
    size_t size() const { return tipo == LISTA ? lista.size() : tipo == OBJETO ? objeto.size() : 0; }
    const std::string &str() const { return s; }
    std::string str(const std::string &def) const { return tipo == CADENA ? s : def; }
    double num() const { return n; }
    bool bul(bool def = false) const { return tipo == BOOL ? b : def; }
    Json &pon(const std::string &k, const Json &v);   // objeto[k] = v
    void anade(const Json &v) { tipo = LISTA; lista.push_back(v); }

    static bool parsea(const std::string &texto, Json &out, std::string *error = nullptr);
    std::string serializa(int sangria = 0) const;

private:
    const Json *busca(const std::string &k) const { for (auto &kv : objeto) if (kv.first == k) return &kv.second; return nullptr; }
};
