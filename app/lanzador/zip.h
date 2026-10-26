// lanzador — extractor de ficheros .zip (los mods suelen venir así): directorio central, entradas sin comprimir o con
// deflate (inflar.h), nombres en UTF-8 o CP437/CP1252. Sin zip64 ni cifrado.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ZipEntrada { std::string nombre; bool directorio = false; uint64_t tam = 0; };

// las entradas del zip; false con `error` si no es un zip que se entienda
bool zip_listar(const std::string &fichero, std::vector<ZipEntrada> &salida, std::string &error);
// extrae todo en `destino` (creando las carpetas); false con `error`
bool zip_extraer(const std::string &fichero, const std::string &destino, std::string &error);
