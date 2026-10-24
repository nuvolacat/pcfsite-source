// lanzador — la instalación: reproduce lo que hacía el instalador original a partir del CD (o copia la carpeta ya
// instalada), deja la parte del CD que el juego lee como D:, las DLL de sistema y comprueba que están los binarios.
//
// Carpeta de datos de un juego (PCF7_WORK para la plataforma): <datos>/<id>/
//   game/<CARPETA>/   la instalación (C:\<CARPETA>)
//   iso/              lo que el juego ve como D:
//   drive_c/          el resto de C: (WINDOWS/SYSTEM con las DLL compartidas)
//   instalado.json    qué versión es, cuándo y desde qué fuentes (ISO o carpeta) se preparó, qué binarios faltan
#pragma once
#include "util.h"
#include "versiones.h"

#include <map>
#include <string>

struct Tarea {
    const Version *v = nullptr;
    std::map<std::string, std::string> fuentes;   // rol ("cd", "cd2", "instalada") → carpeta (la ISO montada, o la carpeta)
    std::map<std::string, std::string> isos;      // rol → fichero ISO del que viene la carpeta (si lo es)
    std::string destino;                          // <datos>/<id>
    bool completa() const;
    // el CD se usa directamente desde la ISO (sin copiarlo) cuando hay un solo disco y es una ISO; con carpetas o dos
    // discos (el 7 Plus mezcla los dos como D:) se copia lo que el juego lee
    bool cd_desde_iso() const { return v && v->discos.size() == 1 && isos.count(v->discos[0].rol) > 0; }
};

// corre en un hilo aparte; al acabar pone progreso.terminado (y ok, o error)
void instalar(const Tarea &t, util::Progreso &pr);

// la ficha de un juego instalado (instalado.json)
struct Origen { std::string nombre, ruta; bool es_iso = false; };   // "CD de PC Fútbol 7", "/Volumes/…/PCF7.iso"
// un mod: una carpeta <carpeta del juego preparado>/mods/<nombre> con la misma estructura que la carpeta del juego
// (DBDAT/…, SFX/…); los activos se ponen por encima de la instalación al jugar (ver platform/src/vfs.cpp, PCF7_CAPAS)
struct Mod { std::string nombre; bool activo = false; };

struct Instalado {
    std::string id, nombre, fecha, carpeta;
    std::vector<Mod> mods;             // los importados, en orden de prioridad (el primero manda)
    std::string iso;                   // si el CD se usa directamente desde la ISO: el fichero (se monta al jugar)
    std::vector<Origen> origenes;      // de dónde salió cada parte (la ISO o la carpeta que eligió el usuario)
    std::vector<std::string> faltan;   // binarios que no se encontraron al instalar
    uint64_t tam = 0;                  // lo que ocupa la carpeta del juego
};
std::vector<Instalado> instalados(const std::string &dir_datos);
std::vector<Instalado> instalados(const std::vector<std::string> &carpetas);   // varias carpetas de datos (la primera manda si se repite un id)

// mods de un juego preparado
std::string dir_mods(const Instalado &i);                 // <carpeta>/mods
std::string dir_mod(const Instalado &i, const Mod &m);    // <carpeta>/mods/<nombre>
// importa una carpeta o un archivo (.zip con extractor propio; .rar/.7z con el tar del sistema) como mod (copia su contenido a mods/<nombre>, localizando dentro la carpeta que replica
// la del juego; de los EXE y DLL se aplican luego solo sus textos); deja el nombre en `nombre`
bool importar_mod(const Instalado &i, const std::string &ruta, std::string &nombre, std::string &error);
bool quitar_mod(Instalado &i, const std::string &nombre);
bool guardar_mods(const Instalado &i);                   // escribe la lista (y qué está activo) en instalado.json
// el perfil de partidas de los mods activos: "" sin mods (se juega en la instalación), si no <carpeta>/perfiles/<mods>
std::string perfil_de(const Instalado &i);
std::vector<std::string> mods_activos(const Instalado &i);   // sus carpetas, en orden de prioridad
