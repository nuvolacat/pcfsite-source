// PCF7 — texto de GDI: fuentes de Windows (LOGFONT) dibujadas con las Liberation (Sans/Serif/Mono:
// mismas métricas que Arial, Times New Roman y Courier New) y stb_truetype, en monocromo como
// Windows 98 (sin suavizado). Las fuentes van en platform/fonts y se copian junto al ejecutable.
#include "text.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <fstream>
#include <map>
#include <strings.h>
#include <tuple>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"

namespace {

struct Face {
    std::vector<u8> ttf;
    stbtt_fontinfo info{};
    bool ok = false;
};

enum Family { SANS, SERIF, MONO };

const char *file_of(Family f, bool bold, bool italic) {
    switch (f) {
    case SERIF: return bold ? "LiberationSerif-Bold.ttf" : "LiberationSerif-Regular.ttf";
    case MONO: return "LiberationMono-Regular.ttf";
    default:
        return bold ? (italic ? "LiberationSans-BoldItalic.ttf" : "LiberationSans-Bold.ttf")
                    : (italic ? "LiberationSans-Italic.ttf" : "LiberationSans-Regular.ttf");
    }
}

Face *face(Family f, bool bold, bool italic) {
    static std::map<std::string, Face> cache;
    std::string name = file_of(f, bold, italic);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second.ok ? &it->second : nullptr;
    Face &fc = cache[name];
    std::string dir = SDL_GetBasePath() ? SDL_GetBasePath() : "./";
    for (std::string p : {dir + "fonts/" + name, dir + "../../platform/fonts/" + name, "/pcf7/ro/fonts/" + name}) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        fc.ttf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        fc.ok = stbtt_InitFont(&fc.info, fc.ttf.data(), stbtt_GetFontOffsetForIndex(fc.ttf.data(), 0)) != 0;
        break;
    }
    if (!fc.ok) plog_once("font" + name, "texto: no se encuentra la fuente %s (platform/fonts)", name.c_str());
    return fc.ok ? &fc : nullptr;
}

Family family_of(const std::string &face, u8 pitch_family) {
    const char *f = face.c_str();
    if (!strcasecmp(f, "Courier New") || !strcasecmp(f, "Courier") || !strcasecmp(f, "FixedSys") || !strcasecmp(f, "Terminal") ||
        !strcasecmp(f, "Lucida Console"))
        return MONO;
    if (!strcasecmp(f, "Times New Roman") || !strcasecmp(f, "MS Serif") || !strcasecmp(f, "Times") || !strcasecmp(f, "Roman") ||
        !strcasecmp(f, "Georgia"))
        return SERIF;
    if (face.empty()) {
        if ((pitch_family & 3) == 1) return MONO;            // FIXED_PITCH
        if ((pitch_family & 0xF0) == 0x10) return SERIF;     // FF_ROMAN
        if ((pitch_family & 0xF0) == 0x30) return MONO;      // FF_MODERN
    }
    return SANS;
}

// Windows-1252 → Unicode
u32 cp1252(u8 c) {
    static const u16 hi[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                               0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
    return c >= 0x80 && c < 0xA0 ? hi[c - 0x80] : c;
}

struct Glyph { int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0; std::vector<u8> bits; };

}  // namespace

FontSpec font_from_logfont(const u8 *lf) {
    FontSpec f;
    i32 h;
    std::memcpy(&h, lf, 4);
    i32 wgt;
    std::memcpy(&wgt, lf + 16, 4);
    f.height = h;
    f.weight = wgt;
    f.italic = lf[20] != 0;
    f.underline = lf[21] != 0;
    f.strikeout = lf[22] != 0;
    f.pitch_family = lf[27];
    char name[33] = {};
    std::memcpy(name, lf + 28, 32);
    f.face = name;
    return f;
}

FontSpec stock_font(int i) {
    FontSpec f;
    switch (i) {
    case 10: case 11: case 16: f.face = "Courier New"; f.height = -13; break;   // OEM/ANSI/SYSTEM_FIXED
    case 13: case 14: f.face = "MS Sans Serif"; f.height = 16; f.weight = 700; break;   // SYSTEM_FONT, DEVICE_DEFAULT
    default: f.face = "MS Sans Serif"; f.height = -11; break;   // ANSI_VAR_FONT, DEFAULT_GUI_FONT
    }
    return f;
}

namespace {

struct Metrics { float scale; int ascent, descent, line_gap, em_px; Face *fc; };

// escala en píxeles: lfHeight < 0 = altura del carácter (em), > 0 = altura de celda (ascent+descent)
Metrics metrics_of(const FontSpec &s) {
    Metrics m{};
    m.fc = face(family_of(s.face, s.pitch_family), s.weight >= 600, s.italic);
    if (!m.fc) { m.ascent = 10; m.descent = 3; m.em_px = 11; return m; }
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&m.fc->info, &asc, &desc, &gap);
    i32 h = s.height ? s.height : -11;
    if (h < 0) m.scale = stbtt_ScaleForMappingEmToPixels(&m.fc->info, (float)-h);
    else m.scale = stbtt_ScaleForPixelHeight(&m.fc->info, (float)h);
    m.ascent = (int)std::lround(asc * m.scale);
    m.descent = (int)std::lround(-desc * m.scale);
    m.line_gap = (int)std::lround(gap * m.scale);
    m.em_px = h < 0 ? -h : (int)std::lround((asc - desc) * m.scale);
    return m;
}

const Glyph &glyph(const FontSpec &s, const Metrics &m, u32 cp) {
    using Key = std::tuple<const Face *, float, u32>;
    static std::map<Key, Glyph> cache;
    Key k{m.fc, m.scale, cp};
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    Glyph &g = cache[k];
    if (!m.fc) { g.advance = (int)std::lround(m.em_px * 0.55); return g; }
    int gi = stbtt_FindGlyphIndex(&m.fc->info, (int)cp);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&m.fc->info, gi, &adv, &lsb);
    g.advance = (int)std::lround(adv * m.scale);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&m.fc->info, gi, m.scale, m.scale, &x0, &y0, &x1, &y1);
    g.w = x1 - x0; g.h = y1 - y0; g.xoff = x0; g.yoff = y0;
    if (g.w > 0 && g.h > 0) {
        std::vector<u8> cov((size_t)g.w * g.h);
        stbtt_MakeGlyphBitmap(&m.fc->info, cov.data(), g.w, g.h, g.w, m.scale, m.scale, gi);
        g.bits.resize(cov.size());
        for (size_t i = 0; i < cov.size(); i++) g.bits[i] = cov[i] >= 100;   // monocromo
    }
    (void)s;
    return g;
}

}  // namespace

TextMetrics text_metrics(const FontSpec &s) {
    Metrics m = metrics_of(s);
    TextMetrics t;
    t.ascent = m.ascent;
    t.descent = m.descent;
    t.height = m.ascent + m.descent;
    t.internal_leading = std::max(0, t.height - m.em_px);
    t.external_leading = m.line_gap;
    t.ave_width = glyph(s, m, 'x').advance;
    t.max_width = glyph(s, m, 'W').advance;
    t.weight = s.weight ? s.weight : 400;
    t.pitch_family = (u8)(family_of(s.face, s.pitch_family) == MONO ? 0x30 : family_of(s.face, s.pitch_family) == SERIF ? 0x16 : 0x26);
    return t;
}

int char_advance(const FontSpec &s, u8 c) {
    Metrics m = metrics_of(s);
    return glyph(s, m, cp1252(c)).advance + (s.weight >= 600 && !m.fc ? 1 : 0);
}

int text_width(const FontSpec &s, const char *str, size_t n) {
    Metrics m = metrics_of(s);
    int w = 0;
    for (size_t i = 0; i < n; i++) w += glyph(s, m, cp1252((u8)str[i])).advance;
    return w;
}

void draw_text_run(const FontSpec &s, const char *str, size_t n, int x, int y_top, const std::vector<int> *dx,
                   const std::function<void(int, int)> &plot) {
    Metrics m = metrics_of(s);
    int pen = x, base = y_top + m.ascent;
    for (size_t i = 0; i < n; i++) {
        const Glyph &g = glyph(s, m, cp1252((u8)str[i]));
        for (int yy = 0; yy < g.h; yy++)
            for (int xx = 0; xx < g.w; xx++)
                if (g.bits[(size_t)yy * g.w + xx]) plot(pen + g.xoff + xx, base + g.yoff + yy);
        int adv = dx && i < dx->size() ? (*dx)[i] : g.advance;
        pen += adv;
    }
    int width = pen - x;
    if (s.underline) for (int xx = 0; xx < width; xx++) plot(x + xx, base + 1);
    if (s.strikeout) for (int xx = 0; xx < width; xx++) plot(x + xx, base - m.ascent / 3);
}
