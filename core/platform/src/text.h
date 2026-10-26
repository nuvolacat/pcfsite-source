// PCF7 — texto de GDI (text.cpp): fuentes de Windows dibujadas con fuentes TrueType libres.
#pragma once
#include "plat.h"

#include <functional>
#include <string>
#include <vector>

struct FontSpec {
    i32 height = -11;          // lfHeight
    i32 weight = 400;
    bool italic = false, underline = false, strikeout = false;
    u8 pitch_family = 0;
    std::string face = "MS Sans Serif";
};
struct TextMetrics {
    int height = 13, ascent = 10, descent = 3, internal_leading = 0, external_leading = 0, ave_width = 5, max_width = 10,
        weight = 400;
    u8 pitch_family = 0x26;
};

FontSpec font_from_logfont(const u8 *logfont);   // LOGFONTA (60 bytes)
FontSpec stock_font(int index);                  // GetStockObject(SYSTEM_FONT…)
TextMetrics text_metrics(const FontSpec &f);
int char_advance(const FontSpec &f, u8 c);
int text_width(const FontSpec &f, const char *s, size_t n);
// dibuja una línea (sin fondo): llama a plot(x, y) por cada píxel de tinta; y_top = arriba de la celda
void draw_text_run(const FontSpec &f, const char *s, size_t n, int x, int y_top, const std::vector<int> *dx,
                   const std::function<void(int, int)> &plot);
