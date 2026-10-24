// PCF7 — objetos COM del host en la memoria del juego: [vtable][refcount][…]. Las vtables apuntan a
// funciones del host (host_fn) que leen sus argumentos de la pila emulada (arg(0) = this).
#pragma once
#include "plat.h"

#include <initializer_list>

inline u32 com_vtbl(std::initializer_list<void (*)(void)> fns) {
    u32 v = heap_alloc(4 * (u32)fns.size(), true);
    u32 i = 0;
    for (auto f : fns) wr32(v + 4 * i++, host_fn(f));
    return v;
}
inline u32 com_object(u32 vtbl, u32 extra = 0) {
    u32 o = heap_alloc(16 + extra, true);
    wr32(o, vtbl);
    wr32(o + 4, 1);
    return o;
}
inline u32 com_addref(u32 o) { u32 n = rd32(o + 4) + 1; wr32(o + 4, n); return n; }
inline u32 com_release(u32 o) { u32 n = rd32(o + 4); if (n) n--; wr32(o + 4, n); return n; }

struct ComGuid { u32 a; u16 b, c; u8 d[8]; };
inline bool com_guid_eq(u32 p, const ComGuid &g) {
    return rd32(p) == g.a && rd16(p + 4) == g.b && rd16(p + 6) == g.c && std::memcmp(MEM + p + 8, g.d, 8) == 0;
}
