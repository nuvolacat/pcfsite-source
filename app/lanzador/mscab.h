// lanzador — extractor de cabinets de Microsoft (MSCF) con compresión MSZIP, también cuando el CAB va pegado al final
// de un ejecutable (los envoltorios «PackageForTheWeb» de las actualizaciones de PC Fútbol 2001).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace util { struct Progreso; }

struct MscabEntrada { std::string nombre; uint32_t tam = 0; std::vector<uint8_t> datos; };

// extrae todos los ficheros del CAB que hay en `fichero` (buscándolo si va dentro de un EXE); false con `error`
bool mscab_extraer(const std::string &fichero, std::vector<MscabEntrada> &salida, std::string &error);
// lo mismo, escribiendo cada fichero en la carpeta `destino`; devuelve los nombres
bool mscab_extraer_en(const std::string &fichero, const std::string &destino, std::vector<std::string> &nombres, std::string &error);
