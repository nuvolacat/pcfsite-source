// PCF7 — superficies de DirectDraw compartidas con Direct3D (ddraw.cpp ↔ d3d.cpp).
#pragma once
#include "plat.h"

struct Surface {
    u32 obj = 0;
    int w = 0, h = 0, bpp = 8, pitch = 0;
    u32 mem = 0;                 // píxeles en MEM
    u32 caps = 0;
    u32 pf_flags = 0;            // DDPIXELFORMAT: flags y máscaras (16 bits: 565, 555, 1555 o 4444)
    u32 rmask = 0, gmask = 0, bmask = 0, amask = 0;
    bool primary = false;
    u32 back = 0;                // primaria: objeto del búfer trasero
    u32 zbuf = 0;                // z-buffer adjunto (AddAttachedSurface)
    u32 palette = 0;             // objeto IDirectDrawPalette
    u32 clipper = 0;
    u32 tex = 0;                 // interfaz IDirect3DTexture2 de esta superficie (d3d.cpp)
    bool has_ck = false;
    u32 ck_lo = 0, ck_hi = 0;    // clave de color de origen
};

Surface *dd_surf(u32 obj);
const u32 *dd_palette_rgb(u32 pal);     // 256 colores 0x00RRGGBB, o nullptr
const u32 *dd_surface_palette_rgb(Surface *s);   // la de la superficie o, si no tiene, la de la primaria
void dd_present_if_primary(u32 obj);
u32 dd_get_dd4();                       // IDirectDraw4 (con una referencia más)
u32 dd_surface_addref(u32 obj);
u32 dd_surface_release(u32 obj);        // a 0 libera la memoria de la superficie

// Direct3D (d3d.cpp)
u32 dd_create_d3d(u32 iid);                  // IDirect3D/IDirect3D3 pedido a IDirectDraw::QueryInterface
u32 d3d_surface_qi(u32 surface, u32 iid);    // IDirect3DTexture(2) pedido a una superficie, o 0
void d3d_surface_gone(u32 surface);          // la superficie se ha liberado
