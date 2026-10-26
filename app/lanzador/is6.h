// lanzador — lector de los cabinets de InstallShield 6 (DATA1.HDR + DATA1.CAB, DATA2.CAB…), el instalador de
// PC Fútbol 2000 y 2001. Reescrito a partir del formato que documenta unshield (David Eriksson, licencia MIT):
// un descriptor común, el descriptor del CAB con la tabla de ficheros (directorios y descriptores de 0x57 bytes),
// los grupos de ficheros (nombre, primer y último índice) y los datos en volúmenes, comprimidos a trozos (u16 de
// longitud + deflate crudo) y con el MD5 de cada fichero para comprobarlo.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace util { struct Progreso; }

struct Is6Fichero {
    std::string nombre;      // nombre del fichero
    std::string directorio;  // carpeta dentro del grupo ("", "Ayuda", "Simuldat\\Estadios")
    uint64_t tam = 0;        // tamaño descomprimido
    bool valido = true;      // false: entrada vacía o marcada como no válida
};
struct Is6Grupo { std::string nombre; uint32_t primero = 0, ultimo = 0; };

class Is6 {
public:
    bool abrir(const std::string &carpeta, std::string &error);   // la carpeta con DATA1.HDR y los DATAn.CAB (el CD)
    const std::vector<Is6Grupo> &grupos() const { return grupos_; }
    const std::vector<Is6Fichero> &ficheros() const { return ficheros_; }
    const Is6Grupo *grupo(const std::string &nombre) const;
    // extrae el fichero `indice` en la ruta `destino` (comprobando tamaño y MD5); pr: suma los bytes escritos
    bool extraer(uint32_t indice, const std::string &destino, std::string &error, util::Progreso *pr = nullptr);

private:
    struct Descriptor {
        uint16_t flags = 0; uint64_t tam = 0, comprimido = 0, datos = 0; uint8_t md5[16] = {};
        uint32_t nombre = 0; uint16_t directorio = 0; uint32_t enlace_prev = 0, enlace_sig = 0; uint8_t enlace = 0; uint16_t volumen = 0;
    };
    struct Volumen {   // la cabecera de un DATAn.CAB (versión 6: 64 bytes tras los 20 comunes)
        uint32_t primero = 0, ultimo = 0; uint64_t datos = 0, primero_off = 0, primero_tam = 0, primero_comp = 0, ultimo_off = 0, ultimo_tam = 0, ultimo_comp = 0;
    };
    std::string carpeta_;
    std::vector<uint8_t> hdr_;
    uint32_t desc_ = 0;                 // offset del descriptor del CAB dentro de hdr_
    uint32_t tabla_ = 0, tabla2_ = 0;   // file_table_offset, file_table_offset2
    uint32_t ndirs_ = 0, nfich_ = 0;
    std::vector<uint32_t> tabla_dirs_;
    std::vector<Is6Grupo> grupos_;
    std::vector<Is6Fichero> ficheros_;
    std::vector<Descriptor> descriptores_;
    int version_ = 0;

    std::string cadena(uint32_t off) const;
    Descriptor descriptor(uint32_t indice) const;
    std::string ruta_volumen(unsigned n) const;
    bool lee_volumen(unsigned n, Volumen &v, int &fd) const;
};
