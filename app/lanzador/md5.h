// lanzador — MD5 (RFC 1321), solo para comprobar los ficheros de los CAB de InstallShield 6, que llevan su resumen.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

struct Md5 {
    uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu, d = 0x10325476u;
    uint64_t total = 0;
    uint8_t buf[64];
    size_t nbuf = 0;

    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void bloque(const uint8_t *p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16) { f = (B & C) | (~B & D); g = i; }
            else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { f = C ^ (B | ~D); g = (7 * i) % 16; }
            uint32_t t = D; D = C; C = B;
            B = B + rol(A + f + K[i] + m[g], R[i]);
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }
    void update(const void *datos, size_t n) {
        const uint8_t *p = (const uint8_t *)datos;
        total += n;
        while (n) {
            size_t k = 64 - nbuf < n ? 64 - nbuf : n;
            std::memcpy(buf + nbuf, p, k);
            nbuf += k; p += k; n -= k;
            if (nbuf == 64) { bloque(buf); nbuf = 0; }
        }
    }
    void final(uint8_t out[16]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t cero = 0;
        while (nbuf != 56) update(&cero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (8 * i));
        update(len, 8);
        uint32_t v[4] = {a, b, c, d};
        for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(v[i] >> (8 * k));
    }
};
