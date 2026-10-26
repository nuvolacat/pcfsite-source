// PCF7 — estado compartido entre el hilo del juego y el hilo del host (SDL): la pantalla emulada y la
// cola de entrada.
//
// La pantalla es la de un PC de 1998: un framebuffer EN LA MEMORIA DEL JUEGO (MEM[fb]) con la
// profundidad del modo de vídeo (8 bits con paleta, 16 bits 565 o 32 bits) que comparten GDI y la
// superficie primaria de DirectDraw, como en el hardware real. El host lo convierte a RGB al presentar;
// en 8 bits la paleta del sistema se aplica en ese momento (un cambio de paleta cambia la imagen sin
// redibujar).
#pragma once
#include "rt.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

struct Screen {
    int w = 640, h = 480, bpp = 8, pitch = 640;
    u32 fb = 0;                // dirección en MEM del framebuffer visible
    u32 pal[256] = {};         // 0x00RRGGBB (solo 8 bits)
    u32 frame = 0;             // se incrementa en cada cambio (el host vuelve a presentar)
};

enum class InKind { MouseMove, MouseDown, MouseUp, Wheel, KeyDown, KeyUp, Char, Quit };
struct InEvent {
    InKind kind;
    int x = 0, y = 0;          // ratón: coordenadas de pantalla emulada
    int button = 0;            // 0 izq, 1 der, 2 central
    u32 vk = 0, scan = 0;      // teclado
    bool ext = false, repeat = false;
    int delta = 0;             // rueda
};

// Vídeo (DirectShow): una imagen RGB que el host pinta encima de la pantalla emulada en `dst`, como
// hacía la superposición de vídeo de la tarjeta gráfica (vale igual en 8 que en 16 bits)
struct VideoOverlay {
    bool on = false;
    int dx = 0, dy = 0, dw = 0, dh = 0;   // rectángulo de destino en la pantalla emulada
    int w = 0, h = 0;                     // tamaño del fotograma
    std::vector<u32> rgb;                 // 0x00RRGGBB
};

// cursor de texto (caret): rectángulo invertido en coordenadas de pantalla
struct CaretOverlay { bool on = false; int x = 0, y = 0, w = 1, h = 16; };

struct Display {
    std::mutex mx;
    Screen screen;
    VideoOverlay video;
    CaretOverlay caret;
    std::condition_variable input_cv;
    std::deque<InEvent> input;
    bool keys[256] = {};       // estado de teclas (GetAsyncKeyState)
    int mouse_x = 0, mouse_y = 0;
    u32 cursor = 0;            // HCURSOR actual (lo pinta el host)
    bool cursor_visible = true;

    void push(const InEvent &e) {
        { std::lock_guard<std::mutex> lk(mx); input.push_back(e); }
        input_cv.notify_all();
    }
};

Display &display();

// Cambia el modo de vídeo: reserva un framebuffer nuevo en MEM (con mx tomado o no, ver gdi32.cpp)
void set_display_mode(int w, int h, int bpp);
// Framebuffer visible (Flip de DirectDraw): con mx tomado
void set_visible_fb(u32 fb);

// Paleta: índice del color más cercano en la paleta del sistema (con caché por color)
u8 nearest_index(u32 rgb);
void palette_changed();
// color 0x00RRGGBB → valor de píxel en la profundidad de la pantalla
u32 rgb_to_pixel(u32 rgb);
