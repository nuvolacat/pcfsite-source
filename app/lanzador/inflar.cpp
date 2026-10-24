// lanzador — deflate (ver inflar.h). Decodificación de Huffman al estilo de puff.c (Mark Adler): tablas canónicas
// (cuenta por longitud + símbolos ordenados) y lectura bit a bit; lento para lo que es, pero sobra para unos MB.
#include "inflar.h"

#include <cstring>

namespace {

struct Bits {
    const uint8_t *p; size_t n, i = 0;
    uint32_t acum = 0; int cuantos = 0;
    bool fin = false;
    Bits(const uint8_t *p_, size_t n_) : p(p_), n(n_) {}
    int bits(int k) {   // k ≤ 24
        while (cuantos < k) {
            if (i >= n) { fin = true; return -1; }
            acum |= (uint32_t)p[i++] << cuantos;
            cuantos += 8;
        }
        int v = (int)(acum & ((1u << k) - 1));
        acum >>= k; cuantos -= k;
        return v;
    }
    void alinea() { acum = 0; cuantos = 0; }
};

struct Huffman {
    short cuenta[16];     // códigos de cada longitud
    short simbolo[288];   // símbolos ordenados por código
    // construye la tabla a partir de las longitudes; false si el código está mal formado (y no es incompleto permitido)
    bool construye(const short *long_, int n) {
        for (int i = 0; i < 16; i++) cuenta[i] = 0;
        for (int i = 0; i < n; i++) cuenta[long_[i]]++;
        if (cuenta[0] == n) return true;   // sin códigos
        int sobran = 1;
        for (int i = 1; i < 16; i++) { sobran <<= 1; sobran -= cuenta[i]; if (sobran < 0) return false; }
        short desp[16]; desp[1] = 0;
        for (int i = 1; i < 15; i++) desp[i + 1] = (short)(desp[i] + cuenta[i]);
        for (int i = 0; i < n; i++) if (long_[i]) simbolo[desp[long_[i]]++] = (short)i;
        return true;
    }
    int decodifica(Bits &b) const {
        int codigo = 0, primero = 0, indice = 0;
        for (int l = 1; l < 16; l++) {
            int bit = b.bits(1);
            if (bit < 0) return -1;
            codigo |= bit;
            int c = cuenta[l];
            if (codigo - c < primero) return simbolo[indice + (codigo - primero)];
            indice += c; primero += c; primero <<= 1; codigo <<= 1;
        }
        return -2;
    }
};

const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short LEXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short DEXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

}  // namespace

bool Inflador::inflar(const uint8_t *in, size_t n, std::vector<uint8_t> &out, bool zlib, std::string &error) {
    Bits b(in, n);
    if (zlib) {
        if (n < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0) { error = "cabecera zlib no válida"; return false; }
        b.i = 2;
    }
    // la salida de esta llamada se escribe directamente en `hist` (con la historia anterior delante) y al final se copia
    size_t inicio = hist.size();
    auto pon = [&](uint8_t c) { hist.push_back(c); };
    bool ultimo = false;
    while (!ultimo) {
        int bf = b.bits(1), tipo = b.bits(2);
        if (bf < 0 || tipo < 0) { error = "flujo deflate truncado"; return false; }
        ultimo = bf == 1;
        if (tipo == 0) {   // sin comprimir
            b.alinea();
            if (b.i + 4 > n) { error = "bloque almacenado truncado"; return false; }
            unsigned len = in[b.i] | (in[b.i + 1] << 8), nlen = in[b.i + 2] | (in[b.i + 3] << 8);
            b.i += 4;
            if ((len ^ 0xFFFF) != nlen) { error = "longitud del bloque almacenado incoherente"; return false; }
            if (b.i + len > n) { error = "bloque almacenado truncado"; return false; }
            for (unsigned k = 0; k < len; k++) pon(in[b.i + k]);
            b.i += len;
            continue;
        }
        if (tipo == 3) { error = "tipo de bloque 3"; return false; }
        Huffman hl, hd;
        if (tipo == 1) {   // códigos fijos
            short l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            hl.construye(l, 288);
            short d[30]; for (i = 0; i < 30; i++) d[i] = 5;
            hd.construye(d, 30);
        } else {   // dinámicos
            int nlen = b.bits(5) + 257, ndist = b.bits(5) + 1, ncode = b.bits(4) + 4;
            if (b.fin || nlen > 286 || ndist > 30) { error = "cabecera de bloque dinámico mal formada"; return false; }
            static const short orden[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            short lengths[320] = {0};
            for (int i = 0; i < ncode; i++) lengths[orden[i]] = (short)b.bits(3);
            Huffman hc;
            if (b.fin || !hc.construye(lengths, 19)) { error = "código de longitudes mal formado"; return false; }
            int i = 0;
            while (i < nlen + ndist) {
                int s = hc.decodifica(b);
                if (s < 0) { error = "código de longitudes mal formado"; return false; }
                if (s < 16) lengths[i++] = (short)s;
                else {
                    int l = 0, rep;
                    if (s == 16) { if (i == 0) { error = "repetición sin longitud previa"; return false; } l = lengths[i - 1]; rep = 3 + b.bits(2); }
                    else if (s == 17) rep = 3 + b.bits(3);
                    else rep = 11 + b.bits(7);
                    if (i + rep > nlen + ndist) { error = "demasiadas longitudes"; return false; }
                    while (rep--) lengths[i++] = (short)l;
                }
            }
            if (lengths[256] == 0) { error = "sin código de fin de bloque"; return false; }
            if (!hl.construye(lengths, nlen) || !hd.construye(lengths + nlen, ndist)) { error = "códigos de bloque dinámico mal formados"; return false; }
        }
        for (;;) {   // los símbolos del bloque
            int s = hl.decodifica(b);
            if (s < 0) { error = "símbolo no válido"; return false; }
            if (s < 256) { pon((uint8_t)s); continue; }
            if (s == 256) break;
            s -= 257;
            if (s >= 29) { error = "longitud no válida"; return false; }
            int len = LBASE[s] + b.bits(LEXTRA[s]);
            int ds = hd.decodifica(b);
            if (ds < 0 || ds >= 30) { error = "distancia no válida"; return false; }
            size_t dist = (size_t)DBASE[ds] + (size_t)b.bits(DEXTRA[ds]);
            if (b.fin) { error = "flujo deflate truncado"; return false; }
            if (dist > hist.size()) { error = "distancia fuera de la ventana"; return false; }
            for (int k = 0; k < len; k++) pon(hist[hist.size() - dist]);
        }
    }
    out.insert(out.end(), hist.begin() + (ptrdiff_t)inicio, hist.end());
    if (hist.size() > 32768) hist.erase(hist.begin(), hist.end() - 32768);
    return true;
}
