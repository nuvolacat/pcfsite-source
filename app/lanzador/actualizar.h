// lanzador — actualizaciones: la última release de github.com/nuvolacat/pcfsite-source; si es más nueva, se baja el
// paquete de esta plataforma, se comprueba (SHA-256 de GitHub y la firma: Developer ID notarizado en macOS, Authenticode
// en Windows, las dos de Jordi Ruiz Bover) y se sustituye la app, que vuelve a abrirse sola. Los juegos, las partidas y
// los mods no se tocan (están en Documentos). En Linux no hay paquete: solo se avisa.
#pragma once
#include "util.h"

#include <atomic>
#include <mutex>
#include <string>

namespace actualizar {

enum Fase { NADA, BUSCANDO, AL_DIA, HAY, BAJANDO, COMPROBANDO, LISTA, FALLO };

struct Estado {
    std::atomic<int> fase{NADA};
    std::atomic<uint64_t> total{0};   // lo que mide el paquete
    std::mutex mx;
    std::string version, pagina;       // la versión nueva («1.26.1030.1200») y la página de la release
    std::string url, sha256, nombre;   // el paquete de esta plataforma ("" si no hay: Linux)
    std::string parcial;               // el fichero que se está bajando (para el progreso)
    std::string preparada;             // la app nueva, lista para sustituir a esta
    std::string error;
    std::string lee_error() { std::lock_guard<std::mutex> lk(mx); return error; }
    uint64_t bajado();                 // lo que lleva bajado
};

// ¿b es más nueva que a? (1.AA.MMDD.HHMM, por números)
bool mas_nueva(const std::string &a, const std::string &b);
// mira la última release (bloquea: llamarla en un hilo); fase AL_DIA, HAY o FALLO
void buscar(Estado &e, const std::string &actual);
// baja el paquete y deja la app nueva preparada y comprobada (bloquea); fase LISTA o FALLO
void preparar(Estado &e);
// lanza el ayudante que, cuando este proceso termine, pone la app nueva en el sitio de esta y la abre; true = salir ya
bool aplicar(Estado &e);

}  // namespace actualizar
