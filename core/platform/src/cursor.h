// PCF7 — cursores cargados de fichero (los pinta el host).
#pragma once
#include "plat.h"

#include <vector>

struct CursorImage {
    int w = 0, h = 0, hot_x = 0, hot_y = 0;
    std::vector<u32> rgba;   // 0xAARRGGBB, de arriba abajo
};

u32 load_cursor_file(const std::string &winpath);
const CursorImage *cursor_image(u32 hcursor);
