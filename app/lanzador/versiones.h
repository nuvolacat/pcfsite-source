// lanzador — las versiones soportadas (versiones.json, lo genera app/tools/versiones.py) y el reconocimiento de un CD
// o de una carpeta instalada del usuario por las firmas (nombre, tamaño y SHA-256 de unos ficheros).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Firma { std::string rel, sha256; uint64_t tam = 0; };

struct Disco {
    std::string rol;       // "cd", "cd2"
    std::string nombre;    // "CD de PC Fútbol 7"
    std::vector<Firma> firmas;
    std::vector<std::vector<std::string>> instalar;   // [["nfc","INST_100.NFC"], ["arbol","FILES",""], …]
    bool copiar_todo = true;
    std::vector<std::string> copiar;                 // si no es todo: entradas de la raíz que van a D:
    std::vector<std::pair<std::string, std::string>> sistema;   // (ruta en el CD, NOMBRE.DLL) → C:\WINDOWS\SYSTEM
};

struct Binario { std::string nombre, sha256; uint64_t tam = 0; bool solo_sistema = false; };

// las actualizaciones oficiales que se aplican tras instalar (PC Fútbol 2001): cada paquete (EXE) lleva un .RTP que
// aplica el parcheador traducido; algunos dejan además ficheros que se copian a la instalación
struct Parche { std::string nombre, sha256, rtp; uint64_t tam = 0; std::vector<std::pair<std::string, std::string>> copias; };
struct Parches {
    std::string nombre;                                             // "la carpeta ACTUALIZ con las 13 actualizaciones…"
    std::vector<Parche> ficheros;                                   // en el orden en que se aplican
    std::vector<std::pair<std::string, std::string>> escribe;       // ficheros que quedan al final (update.ini → "V3.6")
};

struct Version {
    std::string id, nombre, detalle, carpeta, nota, extra;
    std::string corto;   // para la portada de la tarjeta si no hay carátula: «7 PLUS», «5.0 ORO»
    std::string portada, etiqueta;   // carátula (assets/portadas/<portada>) y la edición que se escribe encima («Plus»)
    int anio = 0;
    std::vector<std::pair<std::string, std::string>> extra_copias;   // copias de ficheros extra con otro nombre
    std::vector<std::string> programas;
    bool dos = false, necesita_cd = true, solo_instalada = false;
    std::vector<Disco> discos;
    std::vector<Binario> binarios;
    std::vector<Firma> instalada;   // firmas de una carpeta C:\<carpeta> ya instalada
    Parches parches;                // si no está vacío, hace falta además la carpeta de las actualizaciones (rol "parches")

    std::string titulo() const { return nombre + (detalle.empty() ? "" : " · " + detalle); }
    const Disco *disco(const std::string &rol) const { for (auto &d : discos) if (d.rol == rol) return &d; return nullptr; }
    // qué fuentes hacen falta: los discos (por rol), si solo_instalada "instalada", y "parches" si hay actualizaciones
    std::vector<std::string> roles_necesarios() const;
    std::string nombre_rol(const std::string &rol) const;
};

struct Coincidencia { const Version *v; std::string rol; std::string carpeta; };   // carpeta: donde está de verdad

struct Versiones {
    std::vector<Version> lista;
    std::string generado;
    bool cargar(const std::string &path, std::string &error);
    // qué es la carpeta `dir` (un CD, una instalación); también mira un nivel de subcarpetas
    std::vector<Coincidencia> identificar(const std::string &dir) const;
    const Version *por_id(const std::string &id) const { for (auto &v : lista) if (v.id == id) return &v; return nullptr; }
};

bool firma_coincide(const std::string &dir, const Firma &f);
// la ruta de un paquete de actualización dentro de `dir`: por su nombre o, si no está, por tamaño y SHA-256 (los
// paquetes circulan renombrados, «01 - PCFACT01.exe»); "" si no está
std::string localizar_parche(const std::string &dir, const Parche &pa);
