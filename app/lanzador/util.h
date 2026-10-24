// lanzador — utilidades: ficheros y carpetas (sin distinguir mayúsculas, como el juego), procesos, rutas de la app.
#pragma once
#include "../../core/platform/src/compat.h"   // lo que le falta a Windows de POSIX

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace util {

bool existe(const std::string &p);
bool es_dir(const std::string &p);
uint64_t tam(const std::string &p);
bool mkdirs(const std::string &p);
bool borrar_arbol(const std::string &p);   // rm -rf
std::vector<std::string> listar(const std::string &dir);   // nombres (sin . ni ..), ordenados
std::string nombre_base(const std::string &p);   // (con / o \\ como separador)
std::string carpeta_de(const std::string &p);
std::string normaliza(std::string p);   // separadores a «/» (las rutas que vienen de Windows: diálogos, argumentos)
std::string nativa(std::string p);      // para enseñarla: en Windows con «\\»
std::string resolver(const std::string &base, const std::string &rel);   // ruta real sin distinguir mayúsculas ("" si no existe)
std::string leer(const std::string &p);
bool escribir(const std::string &p, const std::string &datos);
std::string sha256_fichero(const std::string &p);
uint64_t tam_arbol(const std::string &dir);
std::string cp1252_a_utf8(const std::string &s);
std::string minusculas(std::string s);
std::string mayusculas(std::string s);
bool termina_en(const std::string &s, const std::string &suf);   // sin distinguir mayúsculas
std::string humano(uint64_t bytes);   // "1,2 GB"
std::string fecha_hoy();

// progreso de una tarea larga (la instalación), compartido entre el hilo que trabaja y la interfaz
struct Progreso {
    std::atomic<uint64_t> hecho{0}, total{0};
    std::atomic<bool> cancelar{false}, terminado{false}, ok{false};
    std::mutex mx;
    std::string actual, error;
    void reinicia() { hecho = 0; total = 0; cancelar = false; terminado = false; ok = false; std::lock_guard<std::mutex> lk(mx); actual.clear(); error.clear(); }
    void pon(const std::string &a) { std::lock_guard<std::mutex> lk(mx); actual = a; }
    std::string lee() { std::lock_guard<std::mutex> lk(mx); return actual; }
    void falla(const std::string &e) { std::lock_guard<std::mutex> lk(mx); if (error.empty()) error = e; }
    std::string fallo() { std::lock_guard<std::mutex> lk(mx); return error; }
};

// copia un fichero (sumando a progreso.hecho); false si falla o se cancela
bool copiar_fichero(const std::string &src, const std::string &dst, Progreso *pr = nullptr);
// copia una carpeta entera sobre otra (mezclando); un fichero con el mismo nombre y otras mayúsculas se sustituye
bool copiar_arbol(const std::string &src, const std::string &dst, Progreso *pr = nullptr);
// escribe un fichero en dst quitando antes el que tenga el mismo nombre con otras mayúsculas
bool escribir_sustituyendo(const std::string &dst, const char *datos, size_t n, Progreso *pr = nullptr);

// ejecuta un programa (sin shell, sin ventana de consola) y recoge su salida; devuelve el código de salida (-1 si no se pudo lanzar)
int ejecutar(const std::vector<std::string> &argv, std::string &salida);
// un proceso hijo que sigue corriendo (el juego): lanzar y preguntar si ha terminado
struct Proceso { intptr_t h = 0; int pid = 0; bool activo() const { return pid != 0; } };
bool lanzar(const std::string &exe, const std::vector<std::string> &args, Proceso &p);
// ¿ha terminado? (bloquear: espera); devuelve el código de salida, -1 si murió por una señal
bool esperar(Proceso &p, bool bloquear, int &codigo);
// cómo arrancar un programa traducido (el juego <id>, <id>-<programa>, pcfpatch): el ejecutable y sus argumentos. En
// macOS es el ejecutable hermano del lanzador; en Windows, este mismo ejecutable con «--programa <nombre>» (que carga
// programas/<nombre>.dll)
std::vector<std::string> orden_programa(const std::string &nombre);
std::string exe_propio();   // la ruta de este ejecutable

// rutas de la app
std::string dir_ejecutables();   // donde están el lanzador y los juegos (Contents/MacOS)
std::string dir_recursos();      // fuentes, versiones.json, extra/ (Contents/Resources)
std::string dir_base();          // ~/Library/Application Support/PCFSite - Source: ajustes.json, montajes/ (y los juegos, si no se cambia)
std::string dir_documentos();    // la carpeta Documentos del usuario (~/Documents; en Windows, la de Documentos)
std::string dir_datos_defecto(); // Documentos/PCFSite - Source
std::string dir_datos();         // donde se preparan los juegos nuevos: la carpeta de ajustes.json, si no dir_datos_defecto()
std::vector<std::string> carpetas_datos();   // todas las carpetas donde puede haber juegos: la actual, la base y las anteriores
bool cambiar_dir_datos(const std::string &nueva);   // guarda la carpeta en ajustes.json (la anterior pasa a «anteriores»)
uint64_t espacio_libre(const std::string &p);
std::string ajuste(const std::string &clave);                        // un valor de ajustes.json ("" si no está)
bool pon_ajuste(const std::string &clave, const std::string &valor);
std::string dir_inicio();        // $HOME

}  // namespace util
