// lanzador — interfaz mínima sobre SDL3 con el diseño «Industry» de Source PCFSite: fondo claro, un acento azul
// acero, Barlow Condensed para títulos y Barlow para el texto, y los objetos (tarjetas, diálogos, botón principal)
// como dibujos de plano: esquinas rectas, borde fino y marcas «+» en las esquinas.
// Modo inmediato: cada fotograma se vuelve a dibujar todo y los botones devuelven si se han pulsado.
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace ui {

// colores del sistema (styles.css)
const SDL_Color FONDO{0xf2, 0xf2, 0xf3, 255}, SUPERFICIE{0xe9, 0xe9, 0xea, 255}, TEXTO{0x1d, 0x1f, 0x20, 255},
    ACENTO{0x59, 0x80, 0xa6, 255}, ACENTO_100{0xee, 0xf6, 0xff, 255}, ACENTO_600{0x59, 0x7e, 0xa3, 255}, ACENTO_700{0x41, 0x61, 0x80, 255},
    ACENTO_800{0x2c, 0x45, 0x5d, 255}, ACENTO_900{0x1d, 0x2d, 0x3d, 255}, NEUTRO_600{0x7a, 0x7a, 0x7d, 255}, NEUTRO_700{0x5d, 0x5d, 0x60, 255},
    NEUTRO_900{0x2b, 0x2b, 0x2d, 255}, ALERTA{0x9b, 0x3b, 0x3b, 255};   // (ALERTA: «ERROR» lo define windows.h)
inline SDL_Color alfa(SDL_Color c, int a) { c.a = (Uint8)a; return c; }
const SDL_Color DIVISOR = alfa(TEXTO, 41);   // texto al 16 %

enum Estilo {
    CUERPO,          // Barlow 14
    CUERPO_13,       // Barlow 13
    MEDIO,           // Barlow Medium 14
    PEQUENO,         // Barlow 11
    ETIQUETA,        // Barlow 12
    TITULO,          // Barlow Condensed SemiBold 26 (h2)
    DIALOGO,         // Barlow Condensed SemiBold 20
    TARJETA,         // Barlow Condensed SemiBold 17
    PORTADA,         // Barlow Condensed SemiBold 44
    PORTADA_SUB,     // Barlow Condensed SemiBold 18
    BOTON,           // Barlow Condensed SemiBold 14
    BARRA,           // Barlow Condensed SemiBold 14 (mayúsculas, con espaciado)
    ANADIR,          // Barlow Condensed SemiBold 15
    N_ESTILOS
};

bool init(SDL_Renderer *ren, float escala, const std::string &dir_fuentes);
void fotograma(float raton_x, float raton_y, bool pulsado, bool clic);   // estado del ratón de este fotograma
bool sobre(const SDL_FRect &r);       // ¿el ratón está encima? (respeta el recorte)
bool pulsado_en(const SDL_FRect &r);  // ¿se ha hecho clic encima en este fotograma?

void texto(float x, float y, const std::string &s, Estilo e = CUERPO, SDL_Color c = TEXTO, float espaciado = 0);
float ancho(const std::string &s, Estilo e = CUERPO, float espaciado = 0);
float alto(Estilo e = CUERPO);
// párrafo con ajuste de línea; devuelve la altura ocupada (con pintar=false solo mide)
float parrafo(float x, float y, float w, const std::string &s, Estilo e = CUERPO, SDL_Color c = TEXTO, bool pintar = true);
std::string recorta(const std::string &s, float w, Estilo e = CUERPO);   // «…/7.0/PCF7.iso» para que quepa en w
std::string mayusculas(const std::string &s);   // UTF-8 (latino)

void rect(const SDL_FRect &r, SDL_Color c);                 // relleno (con alfa)
void borde(const SDL_FRect &r, SDL_Color c = DIVISOR);      // línea fina
void borde_discontinuo(const SDL_FRect &r, SDL_Color c);
void linea(float x1, float y1, float x2, float y2, SDL_Color c);
void marcas(const SDL_FRect &r);                            // las «+» de las esquinas
void plano(const SDL_FRect &r);                             // borde + marcas (una tarjeta, un diálogo)
void sombra(const SDL_FRect &r);                            // sombra suave debajo de un diálogo

enum TipoBoton { PRIMARIO, SECUNDARIO, FANTASMA };
enum Icono { SIN_ICONO, ENGRANAJE, MAS, INFO, CARPETA, REJILLA, LISTA, VISTO };
bool boton(SDL_FRect r, const std::string &etiqueta, TipoBoton tipo = SECUNDARIO, bool activo = true, Icono icono = SIN_ICONO);
float ancho_boton(const std::string &etiqueta, Icono icono = SIN_ICONO);   // el ancho natural para esa etiqueta
void icono(Icono i, float x, float y, float tam, SDL_Color c);
void barra(const SDL_FRect &r, float fraccion);
// un control segmentado de iconos (.seg): devuelve la opción pulsada o -1; `elegida` es la activa
int segmentos(float x, float y, const std::vector<Icono> &opciones, int elegida);
// una casilla de verificación con su etiqueta (alto 24); true cuando se pulsa (cambia el valor)
bool casilla(float x, float y, float w, const std::string &etiqueta, bool &valor, bool activo = true);
void campo(const SDL_FRect &r, const std::string &valor, Estilo e = CUERPO_13);   // un .input (solo lectura)

SDL_Texture *cargar_png(const std::string &path, int &w, int &h);
void imagen(SDL_Texture *t, const SDL_FRect &r);
void imagen_cubrir(SDL_Texture *t, int tw, int th, const SDL_FRect &r);   // recortada para llenar r sin deformarse
void etiqueta(float x, float y, const std::string &s);   // un .tag sobre una imagen

// solo se pinta (y se pulsa) dentro de r hasta el siguiente fin_recorte()
void recorte(const SDL_FRect &r);
void fin_recorte();

}  // namespace ui
