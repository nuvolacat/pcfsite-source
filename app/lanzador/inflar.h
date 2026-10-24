// lanzador — descompresor deflate (RFC 1951) propio, con la ventana de 32 KB persistente entre llamadas: los bloques
// MSZIP de un CAB de Microsoft son flujos deflate independientes que comparten la historia; los trozos de un CAB de
// InstallShield son flujos sueltos. Acepta también la cabecera zlib (RFC 1950).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Inflador {
public:
    // descomprime los `n` bytes de `in` y añade el resultado a `out`; false (con `error`) si el flujo está mal
    bool inflar(const uint8_t *in, size_t n, std::vector<uint8_t> &out, bool zlib, std::string &error);
    void reinicia() { hist.clear(); }

private:
    std::vector<uint8_t> hist;   // lo último producido (hasta 32 KB): la ventana de las referencias hacia atrás
};
