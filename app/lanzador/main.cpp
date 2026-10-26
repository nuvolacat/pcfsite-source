// Source – PCFSite — el lanzador de la app de escritorio (macOS; el mismo código vale para Windows y Linux).
//
// La app no lleva ningún fichero de los juegos. Al abrirla, pide la imagen ISO del CD (o la carpeta con su contenido,
// o la carpeta del juego ya instalado), reconoce qué versión es por las firmas de versiones.json (solo las que están
// traducidas), reproduce la instalación del instalador original en la carpeta de datos del usuario y arranca el juego:
// el ejecutable traducido (<id>, junto a este) con PCF7_WORK apuntando a esa carpeta. Los juegos reconstruyen en
// memoria sus EXE y DLL a partir de esos mismos ficheros (platform/src/imagen.cpp).
//
// Interfaz (diseño «Industry», ver ui.h): la BIBLIOTECA (rejilla de tarjetas, una por juego preparado, y la de
// «Añadir juego») y, encima, diálogos: FICHA de un juego (de dónde salió, jugar, quitar), ANADIR (elegir ISO o carpeta),
// VERSION (si un CD vale para varias), FUENTES (qué discos faltan), INSTALANDO, CONFIG (carpeta de instalación) y ACERCA.
#include "instalar.h"
#include "is6.h"
#include "mscab.h"
#include "ui.h"
#include "util.h"
#include "versiones.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>


namespace {

const char *NOMBRE_APP = "Source – PCFSite";
const char *WEB = "www.pcfsite.com";
const char *AUTOR = "@jordirubo";
const char *AUTOR_URL = "https://x.com/jordirubo";
#ifndef PCF_VERSION
#define PCF_VERSION "desarrollo"
#endif
int ANCHO = 1000, ALTO = 780;   // el tamaño de la ventana (cabe la biblioteca entera: 9 juegos y «Añadir» en dos filas de cinco); redimensionable
const int ANCHO_MIN = 760, ALTO_MIN = 560;
const float MARGEN = 28;

enum Pantalla { BIBLIOTECA, FICHA, ANADIR, VERSION, FUENTES, INSTALANDO, CONFIG, ACERCA };

struct App {
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture *logo = nullptr;
    int logo_w = 0, logo_h = 0;
    std::string recursos;   // fonts/, assets/ (Resources de la app, o el repo en desarrollo)
    struct Portada { SDL_Texture *tex = nullptr; int w = 0, h = 0; };
    std::map<std::string, Portada> portadas;   // carátulas ya cargadas, por fichero
    Versiones versiones;
    std::vector<Instalado> instalados;
    Pantalla pantalla = BIBLIOTECA;
    std::string aviso, aviso_error;   // mensaje de la biblioteca o del diálogo actual
    // lo que se está añadiendo
    Tarea tarea;
    std::vector<Coincidencia> candidatas;   // para elegir versión
    std::string candidata_iso;              // la ISO de la que salieron
    std::string pendiente_rol;              // el rol para el que se abrió el diálogo ("" = el que salga)
    // diálogo de ficheros (la respuesta llega en otro hilo)
    std::mutex mx;
    bool hay_respuesta = false;
    std::string respuesta;
    bool dialogo_abierto = false;
    bool dialogo_config = false;            // la respuesta es para la carpeta de instalación
    bool dialogo_mod = false;               // la respuesta es un mod que importar (zip o carpeta) para el juego de la ficha
    // montajes de ISO (hdiutil)
    std::vector<std::pair<std::string, std::string>> montajes;   // (iso, punto de montaje) montados por esta ventana
    std::vector<std::pair<std::string, std::string>> ajenos;     // ya estaban montados (no se desmontan)
    // instalación
    util::Progreso progreso;
    std::thread hilo;
    // ficha
    std::string ficha_id;
    bool quitar_confirmar = false;
    // configuración
    std::string config_carpeta;
    float scroll = 0;   // de la rejilla o la lista
    util::Proceso jugando;   // el juego en marcha (proceso hijo); la ventana se oculta mientras tanto
    std::string jugando_carpeta;   // su carpeta (en Windows, para desmontar su CD al terminar)
    Uint64 reclamar_foco = 0;   // cuándo volver a pedir el primer plano (tras mostrar la ventana)
    bool terminal = false;   // modo terminal: el juego sustituye al lanzador
    bool lista = false;   // vista de lista en vez de rejilla (ajustes.json «vista»)
};
App A;

void dialogo_cb(void *, const char *const *lista, int) {
    std::lock_guard<std::mutex> lk(A.mx);
    A.hay_respuesta = true;
    A.respuesta = lista && lista[0] ? util::normaliza(lista[0]) : "";
    A.dialogo_abierto = false;
}

void abrir_dialogo(bool carpeta, const std::string &rol, bool para_config = false, bool para_mod = false) {
    if (A.dialogo_abierto) return;
    A.dialogo_abierto = true;
    A.dialogo_config = para_config;
    A.dialogo_mod = para_mod;
    A.pendiente_rol = rol;
    static const SDL_DialogFileFilter filtros[] = {{"Imágenes de CD (ISO, BIN, IMG, CDR)", "iso;bin;img;cdr;cue"}, {"Todos los ficheros", "*"}};
    static const SDL_DialogFileFilter filtros_mod[] = {{"Mods (zip, rar, 7z)", "zip;rar;7z"}, {"Todos los ficheros", "*"}};
    if (carpeta) SDL_ShowOpenFolderDialog(dialogo_cb, nullptr, A.win, nullptr, false);
    else if (para_mod) SDL_ShowOpenFileDialog(dialogo_cb, nullptr, A.win, filtros_mod, 2, nullptr, false);
    else SDL_ShowOpenFileDialog(dialogo_cb, nullptr, A.win, filtros, 2, nullptr, false);
}

#ifdef _WIN32
// Windows: las ISO se montan con Mount-DiskImage (PowerShell), que les asigna una letra de unidad. No admite ficheros
// en carpetas de red: una ISO que esté en una se copia antes a <base>/isos (una vez).
int powershell(const std::string &orden, std::string &salida) {
    return util::ejecutar({"powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", orden}, salida);
}
std::string ps_cadena(const std::string &s) {
    std::string o = "'";
    for (char ch : s) { if (ch == '\'') o += '\''; o += ch; }
    return o + "'";
}
bool ruta_remota(const std::string &p) {
    if (p.rfind("//", 0) == 0 || p.rfind("\\\\", 0) == 0) return true;
    if (p.size() >= 2 && p[1] == ':') { std::string raiz = p.substr(0, 2) + "\\"; return GetDriveTypeA(raiz.c_str()) == DRIVE_REMOTE; }
    return false;
}
// la ISO que se monta de verdad: la misma, o su copia local si está en red
std::string iso_local(const std::string &iso, bool copiar) {
    if (!ruta_remota(iso)) return iso;
    std::string dst = util::dir_base() + "/isos/" + util::nombre_base(iso);
    if (util::existe(dst) && util::tam(dst) == util::tam(iso)) return dst;
    if (!copiar) return "";
    return util::copiar_fichero(iso, dst) ? dst : "";
}
// monta una ISO; devuelve la unidad («E:») o ""
std::string montar_iso(const std::string &iso, std::string = "") {
    for (auto &m : A.montajes) if (m.first == iso) return m.second;
    std::string local = iso_local(iso, true);
    if (local.empty()) { SDL_Log("no se puede copiar %s a la carpeta de la app", iso.c_str()); return ""; }
    std::string orden = "$p = " + ps_cadena(util::nativa(local)) + "; $d = Get-DiskImage -ImagePath $p -ErrorAction Stop; "
        "if (-not $d.Attached) { $d = Mount-DiskImage -ImagePath $p -PassThru -ErrorAction Stop }; $v = $null; "
        "for ($i = 0; $i -lt 40; $i++) { $v = $d | Get-Volume -ErrorAction SilentlyContinue; if ($v -and $v.DriveLetter) { break }; "
        "Start-Sleep -Milliseconds 250; $d = Get-DiskImage -ImagePath $p }; "
        "if ($v -and $v.DriveLetter) { Write-Output ([string]$v.DriveLetter) } else { exit 3 }";
    std::string salida;
    int rc = powershell(orden, salida);
    size_t a = salida.find_first_not_of(" \r\n\t"), b = salida.find_last_not_of(" \r\n\t");
    std::string letra = a == std::string::npos ? "" : salida.substr(a, b - a + 1);
    if (rc != 0 || letra.size() != 1) { SDL_Log("Mount-DiskImage %s: %d\n%s", local.c_str(), rc, salida.c_str()); return ""; }
    std::string mp = letra + ":";
    A.montajes.emplace_back(iso, mp);
    return mp;
}
void desmontar_iso(const std::string &iso) {
    std::string local = iso_local(iso, false);
    if (local.empty()) return;
    std::string salida;
    powershell("Dismount-DiskImage -ImagePath " + ps_cadena(util::nativa(local)) + " -ErrorAction SilentlyContinue | Out-Null", salida);
}
// la ISO del CD de ese juego, si quedó montada de una partida (<carpeta>/cd-montado.txt), se desmonta
void desmontar_cd_de(const std::string &carpeta) {
    std::string f = carpeta + "/cd-montado.txt";
    std::string iso = util::leer(f);
    if (iso.empty()) return;
    desmontar_iso(iso);
    ::unlink(f.c_str());
}
void desmontar_todo() {
    for (auto &m : A.montajes) desmontar_iso(m.first);
    A.montajes.clear();
}
#elif defined(__APPLE__)
// ¿hay algo montado en esa carpeta? (otro dispositivo que su carpeta madre)
bool esta_montado(const std::string &mp) {
    struct stat a, b;
    return ::stat(mp.c_str(), &a) == 0 && ::stat(util::carpeta_de(mp).c_str(), &b) == 0 && a.st_dev != b.st_dev;
}

// monta una ISO (hdiutil) en `mp` (si no se dice, en <base>/montajes/n); devuelve la carpeta o ""
std::string montar_iso(const std::string &iso, std::string mp = "") {
    for (auto &m : A.montajes) if (m.first == iso) return m.second;
    for (auto &m : A.ajenos) if (m.first == iso) return m.second;
    if (mp.empty()) mp = util::dir_base() + "/montajes/" + std::to_string(A.montajes.size() + 1);
    if (esta_montado(mp)) {   // de una vez anterior (al jugar no se desmonta): se reutiliza
        A.montajes.emplace_back(iso, mp);
        return mp;
    }
    {   // ¿ya está montada en otro sitio (otra ventana, una partida en marcha)? hdiutil no deja montarla dos veces
        std::string info;
        if (util::ejecutar({"hdiutil", "info"}, info) == 0) {
            bool esta = false;
            size_t i = 0;
            while (i < info.size()) {
                size_t j = info.find('\n', i);
                if (j == std::string::npos) j = info.size();
                std::string l = info.substr(i, j - i);
                i = j + 1;
                if (l.rfind("image-path", 0) == 0) { size_t c = l.find(':'); std::string v = c == std::string::npos ? "" : l.substr(c + 1); size_t b = v.find_first_not_of(" \t"); esta = b != std::string::npos && v.substr(b) == iso; continue; }
                if (l.rfind("=====", 0) == 0) esta = false;
                if (esta && l.rfind("/dev/", 0) == 0) {
                    size_t t = l.find_last_of('\t');
                    if (t != std::string::npos && t + 1 < l.size() && l[t + 1] == '/') { A.ajenos.emplace_back(iso, l.substr(t + 1)); return l.substr(t + 1); }
                }
            }
        }
    }
    util::mkdirs(mp);
    std::string salida;
    int rc = util::ejecutar({"hdiutil", "attach", "-nobrowse", "-readonly", "-noverify", "-noautoopen", "-mountpoint", mp, iso}, salida);
    if (rc != 0) {
        SDL_Log("hdiutil attach %s: %d\n%s", iso.c_str(), rc, salida.c_str());
        return "";
    }
    A.montajes.emplace_back(iso, mp);
    return mp;
}

// si la ISO del CD de ese juego sigue montada de una partida anterior (<carpeta>/cd), se desmonta
void desmontar_cd_de(const std::string &carpeta) {
    std::string mp = carpeta + "/cd";
    if (!esta_montado(mp)) return;
    std::string salida;
    if (util::ejecutar({"hdiutil", "detach", mp}, salida) != 0) util::ejecutar({"hdiutil", "detach", "-force", mp}, salida);
}

void desmontar_todo() {
    for (auto &m : A.montajes) {
        std::string salida;
        if (util::ejecutar({"hdiutil", "detach", m.second}, salida) != 0) util::ejecutar({"hdiutil", "detach", "-force", m.second}, salida);
        rmdir(m.second.c_str());
    }
    A.montajes.clear();
}
#else
// Linux: montar una ISO sin privilegios. Primero fuseiso (FUSE, en la carpeta que se pida, como hdiutil); si no está,
// udisks2 (udisksctl loop-setup + mount: el punto de montaje lo elige él, /run/media/<usuario>/<etiqueta>). Lo que se
// montó queda apuntado en <carpeta>/cd-montado.txt («fuse <carpeta>» o «loop /dev/loopN») para desmontarlo al cerrar el
// juego o al quitarlo. Sin ninguno de los dos, se avisa de qué instalar.
bool esta_montado(const std::string &mp) {
    struct stat a, b;
    return ::stat(mp.c_str(), &a) == 0 && ::stat(util::carpeta_de(mp).c_str(), &b) == 0 && a.st_dev != b.st_dev;
}
static std::string linea_tras(const std::string &salida, const std::string &marca) {   // lo que sigue a `marca` hasta el fin de línea (sin el punto final)
    size_t i = salida.find(marca);
    if (i == std::string::npos) return "";
    i += marca.size();
    size_t j = salida.find_first_of("\r\n", i);
    std::string v = salida.substr(i, j == std::string::npos ? std::string::npos : j - i);
    while (!v.empty() && (v.back() == '.' || v.back() == ' ')) v.pop_back();
    return v;
}
static bool hay_programa(const std::string &nombre) { std::string s; return util::ejecutar({"sh", "-c", "command -v " + nombre}, s) == 0; }
static std::string montaje_de_loop(const std::string &dev) {
    std::string s;
    if (util::ejecutar({"findmnt", "-rn", "-o", "TARGET", "-S", dev}, s) != 0) return "";
    size_t a = s.find_first_not_of(" \r\n\t"), b = s.find_last_not_of(" \r\n\t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::map<std::string, std::string> apuntes;   // iso → fichero de apunte de su montaje (<carpeta>/cd-montado.txt, <base>/montajes/n-montado.txt)
static void desmontar_apunte(const std::string &apunte);
std::string montar_iso(const std::string &iso, std::string mp = "") {
    for (auto &m : A.montajes) if (m.first == iso) return m.second;
    if (mp.empty())   // una carpeta libre: fuseiso sigue montado aunque la app se cierre (issue #1: una ISO de otra vez
                      // en montajes/1 hacía que la elegida no se reconociera)
        for (int n = 1; mp.empty() || esta_montado(mp); n++) mp = util::dir_base() + "/montajes/" + std::to_string(n);
    std::string apunte = mp + "-montado.txt";
    apuntes[iso] = apunte;
    if (esta_montado(mp)) {   // fuseiso de una vez anterior: vale si es esta misma ISO (la segunda línea del apunte)
        std::string a = util::leer(apunte);
        size_t nl = a.find('\n');
        std::string antes = nl == std::string::npos ? "" : a.substr(nl + 1);
        while (!antes.empty() && (antes.back() == '\n' || antes.back() == '\r')) antes.pop_back();
        if (antes == iso) { A.montajes.emplace_back(iso, mp); return mp; }
        desmontar_apunte(apunte);
        if (esta_montado(mp)) {   // sin apunte (o uno viejo): la carpeta es nuestra, se desmonta igualmente
            std::string salida;
            if (util::ejecutar({"fusermount", "-u", mp}, salida) != 0) util::ejecutar({"fusermount", "-uz", mp}, salida);
        }
        if (esta_montado(mp)) { SDL_Log("%s sigue montado con otra ISO", mp.c_str()); return ""; }
    }
    std::string salida;
    if (hay_programa("fuseiso")) {
        util::mkdirs(mp);
        if (util::ejecutar({"fuseiso", iso, mp}, salida) == 0 && esta_montado(mp)) {
            util::escribir(apunte, "fuse " + mp + "\n" + iso + "\n");
            A.montajes.emplace_back(iso, mp);
            return mp;
        }
        SDL_Log("fuseiso %s: %s", iso.c_str(), salida.c_str());
    }
    if (hay_programa("udisksctl")) {
        int rc = util::ejecutar({"udisksctl", "loop-setup", "-r", "-f", iso}, salida);
        std::string dev = linea_tras(salida, " as ");   // «Mapped file X as /dev/loop3.»
        if (rc != 0 || dev.rfind("/dev/", 0) != 0) { SDL_Log("udisksctl loop-setup %s: %d\n%s", iso.c_str(), rc, salida.c_str()); return ""; }
        std::string punto;
        for (int intento = 0; intento < 20 && punto.empty(); intento++) {   // el escritorio puede montarlo él solo al aparecer el dispositivo
            if (util::ejecutar({"udisksctl", "mount", "-b", dev}, salida) == 0) punto = linea_tras(salida, " at ");   // «Mounted /dev/loop3 at /run/media/u/PCF7.»
            if (punto.empty()) punto = montaje_de_loop(dev);
            if (punto.empty()) SDL_Delay(250);
        }
        if (punto.empty()) { SDL_Log("udisksctl mount %s: %s", dev.c_str(), salida.c_str()); util::ejecutar({"udisksctl", "loop-delete", "-b", dev}, salida); return ""; }
        util::escribir(apunte, "loop " + dev + "\n" + iso + "\n");
        A.montajes.emplace_back(iso, punto);
        return punto;
    }
    SDL_Log("no hay forma de montar %s: instala fuseiso o udisks2", iso.c_str());
    return "";
}
static void desmontar_apunte(const std::string &apunte) {
    std::string a = util::leer(apunte), salida;
    if (a.empty()) return;
    a = a.substr(0, a.find('\n'));   // (la segunda línea es la ISO)
    while (!a.empty() && (a.back() == '\n' || a.back() == '\r')) a.pop_back();
    if (a.rfind("fuse ", 0) == 0) { std::string mp = a.substr(5); if (esta_montado(mp)) { if (util::ejecutar({"fusermount", "-u", mp}, salida) != 0) util::ejecutar({"fusermount", "-uz", mp}, salida); } }
    else if (a.rfind("loop ", 0) == 0) { std::string dev = a.substr(5); util::ejecutar({"udisksctl", "unmount", "-b", dev}, salida); util::ejecutar({"udisksctl", "loop-delete", "-b", dev}, salida); }
    ::unlink(apunte.c_str());
}
// lo que quedó montado del CD de ese juego (<carpeta>/cd-montado.txt) se desmonta
void desmontar_cd_de(const std::string &carpeta) { desmontar_apunte(carpeta + "/cd-montado.txt"); }
void desmontar_todo() {
    for (auto &m : A.montajes) {
        auto it = apuntes.find(m.first);
        if (it != apuntes.end()) { desmontar_apunte(it->second); apuntes.erase(it); }
        rmdir(m.second.c_str());   // (si era una carpeta nuestra y ya está vacía)
    }
    A.montajes.clear();
}
#endif

void app_en_dock(bool si);   // (más abajo)
void recargar_instalados() { A.instalados = instalados(util::carpetas_datos()); }

const Instalado *instalado(const std::string &id) { for (auto &i : A.instalados) if (i.id == id) return &i; return nullptr; }
Instalado *instalado_mut(const std::string &id) { for (auto &i : A.instalados) if (i.id == id) return &i; return nullptr; }

// importa un mod (zip o carpeta) al juego de la ficha
void importar_mod_a_ficha(const std::string &ruta) {
    Instalado *i = instalado_mut(A.ficha_id);
    if (!i) return;
    std::string nombre, error;
    A.aviso.clear(); A.aviso_error.clear();
    if (!importar_mod(*i, ruta, nombre, error)) { A.aviso_error = "No se ha podido importar el mod: " + error; return; }
    i->mods.push_back({nombre, true});
    guardar_mods(*i);
    A.aviso = "Mod «" + nombre + "» importado y activado." + (error.empty() ? "" : " " + error + ".");
}

void nueva_tarea() { A.tarea = Tarea(); A.candidatas.clear(); A.aviso.clear(); A.aviso_error.clear(); }

void cerrar_dialogo() { nueva_tarea(); A.pantalla = BIBLIOTECA; A.quitar_confirmar = false; }

std::string lista_versiones() {
    std::string s;
    for (const Version &v : A.versiones.lista) s += (s.empty() ? "" : ", ") + v.nombre;
    return s;
}

// una fuente elegida por el usuario (ISO o carpeta): qué es y a qué versión/rol va
void procesar_fuente(const std::string &ruta) {
    A.aviso.clear(); A.aviso_error.clear();
    std::string dir = ruta;
    if (!util::es_dir(ruta)) {
        dir = montar_iso(ruta);
        if (dir.empty()) { A.aviso_error = "No se puede montar la imagen " + util::nombre_base(ruta) + ". ¿Es una ISO de CD?"; return; }
    }
    std::vector<Coincidencia> c = A.versiones.identificar(dir);
    if (c.empty()) {
        A.aviso_error = "No se reconoce " + util::nombre_base(ruta) + ". Solo valen las versiones ya traducidas (" + lista_versiones() +
                        "), en su CD original o en su carpeta de instalación.";
        return;
    }
    bool es_iso = dir != ruta;
    if (A.tarea.v) {   // ya hay una versión elegida: tiene que ser un disco suyo
        for (const Coincidencia &x : c)
            if (x.v == A.tarea.v && (A.pendiente_rol.empty() || x.rol == A.pendiente_rol)) {
                A.tarea.fuentes[x.rol] = x.carpeta;
                if (es_iso) A.tarea.isos[x.rol] = ruta; else A.tarea.isos.erase(x.rol);
                A.aviso = "Reconocido: " + A.tarea.v->nombre_rol(x.rol) + ".";
                A.pantalla = FUENTES;
                return;
            }
        A.aviso_error = util::nombre_base(ruta) + " es " + c[0].v->nombre_rol(c[0].rol) + " de " + c[0].v->nombre + ", no lo que falta para " + A.tarea.v->nombre + ".";
        A.pantalla = FUENTES;
        return;
    }
    // versiones distintas que acepta esta fuente (un CD de PC Fútbol 7 vale para el 7 y para el 7 Plus)
    std::vector<Coincidencia> distintas;
    for (const Coincidencia &x : c) {
        bool ya = false;
        for (const Coincidencia &y : distintas) if (y.v == x.v) ya = true;
        if (!ya) distintas.push_back(x);
    }
    if (distintas.size() > 1) { A.candidatas = c; A.candidata_iso = es_iso ? ruta : ""; A.pantalla = VERSION; return; }
    A.tarea.v = c[0].v;
    A.tarea.destino = util::dir_datos() + "/" + c[0].v->id;
    for (const Coincidencia &x : c) if (x.v == A.tarea.v) { A.tarea.fuentes[x.rol] = x.carpeta; if (es_iso) A.tarea.isos[x.rol] = ruta; }
    A.aviso = "Reconocido: " + A.tarea.v->nombre_rol(c[0].rol) + ".";
    A.pantalla = FUENTES;
}

void elegir_version(const Version *v) {
    A.tarea.v = v;
    A.tarea.destino = util::dir_datos() + "/" + v->id;
    for (const Coincidencia &x : A.candidatas) if (x.v == v) { A.tarea.fuentes[x.rol] = x.carpeta; if (!A.candidata_iso.empty()) A.tarea.isos[x.rol] = A.candidata_iso; }
    A.candidatas.clear();
    A.pantalla = FUENTES;
}

void empezar_instalacion() {
    A.progreso.reinicia();
    A.pantalla = INSTALANDO;
    if (const Instalado *i = instalado(A.tarea.v->id)) { desmontar_cd_de(i->carpeta); if (i->carpeta != A.tarea.destino) util::borrar_arbol(i->carpeta); }
    desmontar_cd_de(A.tarea.destino);
    Tarea t = A.tarea;
    A.hilo = std::thread([t] { instalar(t, A.progreso); });
}

bool preparar_cd(const Instalado &i) {
    if (i.iso.empty()) return true;
    if (!util::existe(i.iso)) {
        A.aviso_error = "No se encuentra la ISO del CD: " + i.iso + ". Si la has movido, vuelve a añadir el juego.";
        return false;
    }
    std::string mp = montar_iso(i.iso, i.carpeta + "/cd");
    if (mp.empty()) { A.aviso_error = "No se puede montar la ISO " + i.iso; return false; }
#ifdef _WIN32
    util::escribir(i.carpeta + "/cd-montado.txt", i.iso);   // para desmontarla al terminar
#endif
    setenv("PCF7_CD", mp.c_str(), 1);
    return true;
}

// arranca el juego: en la app, como proceso hijo (la ventana se oculta y vuelve a la biblioteca cuando el juego se
// cierra); desde la terminal, sustituyendo al lanzador
void jugar(const Instalado &i) {
    if (!preparar_cd(i)) return;
    // la ISO del CD se queda montada mientras el juego corre (y hasta que se expulse o se reinicie)
    A.montajes.clear();
    std::vector<std::string> orden = util::orden_programa(i.id);
    std::string exe = orden[0];
    std::vector<std::string> args(orden.begin() + 1, orden.end());
    setenv("PCF7_WORK", i.carpeta.c_str(), 1);
    setenv("PCF7_APP", "1", 1);
    // mods: sus carpetas por encima de la instalación, y las partidas en el perfil de esa combinación
    std::string perfil = perfil_de(i), capas;
    for (const std::string &m : mods_activos(i)) capas += (capas.empty() ? "" : "\n") + m;
    if (!perfil.empty()) { util::mkdirs(perfil); setenv("PCF7_CAPAS", capas.c_str(), 1); setenv("PCF7_ESCRITURA", perfil.c_str(), 1); }
    else { unsetenv("PCF7_CAPAS"); unsetenv("PCF7_ESCRITURA"); }
    if (A.terminal) {
        SDL_Quit();
#ifdef _WIN32
        util::Proceso p;
        int codigo = 1;
        if (!util::lanzar(exe, args, p)) { fprintf(stderr, "no se puede ejecutar %s\n", exe.c_str()); _exit(1); }
        util::esperar(p, true, codigo);
        desmontar_cd_de(i.carpeta);
        _exit(codigo < 0 ? 1 : codigo);
#else
        char *av[] = {(char *)exe.c_str(), nullptr};
        execv(exe.c_str(), av);
        fprintf(stderr, "no se puede ejecutar %s\n", exe.c_str());
        _exit(1);
#endif
    }
    if (!util::lanzar(exe, args, A.jugando)) { A.aviso_error = "No se puede ejecutar " + exe; return; }
    A.jugando_carpeta = i.carpeta;
    nueva_tarea();
    A.pantalla = BIBLIOTECA;
    SDL_HideWindow(A.win);
    app_en_dock(false);
}

// macOS: la app en el Dock, o no. Mientras corre el juego (otro proceso, dentro del mismo paquete y con el mismo icono)
// la app se retira del Dock para que no haya dos iconos; al terminar vuelve.
void app_en_dock(bool si) {
#ifdef __APPLE__
    id nsapp = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    ((void (*)(id, SEL, long))objc_msgSend)(nsapp, sel_registerName("setActivationPolicy:"), si ? 0 : 1);   // Regular : Accessory
#else
    (void)si;
#endif
}

// ¿ha terminado el juego? entonces la ventana vuelve
void vigilar_juego() {
    if (!A.jugando.activo()) return;
    int codigo = 0;
    if (!util::esperar(A.jugando, false, codigo)) { SDL_Delay(100); return; }
#if defined(_WIN32) || defined(__linux__)
    desmontar_cd_de(A.jugando_carpeta);   // en Windows y Linux la ISO no se deja montada
#endif
    app_en_dock(true);
    recargar_instalados();   // las partidas guardadas cambian el tamaño
    A.aviso.clear(); A.aviso_error.clear();
    if (codigo > 0) A.aviso_error = "El juego ha terminado con un error (código " + std::to_string(codigo) + ").";
    else if (codigo < 0) A.aviso_error = "El juego se ha cerrado de forma inesperada.";
    SDL_SetWindowPosition(A.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(A.win);
    SDL_RaiseWindow(A.win);
    A.reclamar_foco = SDL_GetTicks() + 300;   // y otra vez enseguida: justo al volver a mostrarse no siempre se activa
}

void quitar(const Instalado &i) {
    desmontar_cd_de(i.carpeta);
    util::borrar_arbol(i.carpeta);
    recargar_instalados();
}

std::string con_mayuscula(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
    return s;
}

// ---------------------------------------------------------------- piezas de interfaz
// el avisos de la pantalla (error o información); devuelve la altura que ocupa
float avisos(float x, float y, float w) {
    if (!A.aviso_error.empty()) return ui::parrafo(x, y, w, A.aviso_error, ui::CUERPO_13, ui::ALERTA) + 8;
    if (!A.aviso.empty()) return ui::parrafo(x, y, w, A.aviso, ui::CUERPO_13, ui::ACENTO_700) + 8;
    return 0;
}

// un diálogo del diseño: caja de plano sobre un fondo atenuado, con barra de título; devuelve la caja del cuerpo
SDL_FRect dialogo(const std::string &titulo, float w, float alto_cuerpo) {
    ui::rect({0, 0, (float)ANCHO, (float)ALTO}, ui::alfa(ui::NEUTRO_900, 110));
    float h = 40 + alto_cuerpo;
    SDL_FRect r{((float)ANCHO - w) / 2, std::max(24.f, ((float)ALTO - h) / 2), w, h};
    ui::sombra(r);
    ui::rect(r, ui::FONDO);
    ui::plano(r);
    std::string t = ui::mayusculas(titulo);
    ui::texto(r.x + (r.w - ui::ancho(t, ui::BARRA, 0.9f)) / 2, r.y + (40 - ui::alto(ui::BARRA)) / 2 + 1, t, ui::BARRA, ui::TEXTO, 0.9f);
    ui::linea(r.x, r.y + 40, r.x + r.w, r.y + 40, ui::DIVISOR);
    return {r.x + MARGEN, r.y + 40 + MARGEN, r.w - 2 * MARGEN, alto_cuerpo - 2 * MARGEN};
}

// una fila de botones alineados a la derecha (de derecha a izquierda); devuelve el índice pulsado o -1
int fila_botones(float x_der, float y, const std::vector<std::pair<std::string, ui::TipoBoton>> &botones, bool activo_primero = true) {
    int pulsado = -1;
    float x = x_der;
    for (int k = (int)botones.size() - 1; k >= 0; k--) {
        float w = ui::ancho_boton(botones[k].first);
        x -= w;
        bool activo = k == 0 ? activo_primero : true;
        if (ui::boton({x, y, w, 36}, botones[k].first, botones[k].second, activo)) pulsado = k;
        x -= 8;
    }
    return pulsado;
}

// ---------------------------------------------------------------- la biblioteca
// la carátula de una versión (assets/portadas), cargada la primera vez que se pide
const App::Portada *portada_de(const Version *v) {
    if (!v || v->portada.empty()) return nullptr;
    auto it = A.portadas.find(v->portada);
    if (it == A.portadas.end()) {
        App::Portada p;
        p.tex = ui::cargar_png(A.recursos + "/assets/portadas/" + v->portada, p.w, p.h);
        it = A.portadas.emplace(v->portada, p).first;
    }
    return it->second.tex ? &it->second : nullptr;
}

// la vista de lista: una tabla con carátula pequeña, juego, año, tamaño y origen; debajo, «Añadir juego»
void lista_juegos(float arriba) {
    const float PAD_ARRIBA = 16, FILA = 56, CAB = 28, ANADIR_H = 44;
    float w = (float)ANCHO - 2 * MARGEN, x = MARGEN;
    float c_tam = 90, c_anio = 60, c_origen = std::max(220.f, w * 0.42f), c_juego = w - c_anio - c_tam - c_origen;
    float total = PAD_ARRIBA + CAB + A.instalados.size() * FILA + 16 + ANADIR_H + 32;
    float visible = (float)ALTO - arriba;
    float maximo = std::max(0.f, total - visible);
    A.scroll = std::min(std::max(A.scroll, 0.f), maximo);
    std::string abrir_ficha;
    bool anadir = false;
    ui::recorte({0, arriba, (float)ANCHO, visible});
    float y = arriba + PAD_ARRIBA - A.scroll;
    // cabecera de la tabla
    SDL_Color cab = ui::alfa(ui::TEXTO, 150);
    ui::texto(x + 12, y + 7, "JUEGO", ui::PEQUENO, cab, 0.9f);
    ui::texto(x + c_juego, y + 7, "AÑO", ui::PEQUENO, cab, 0.9f);
    ui::texto(x + c_juego + c_anio, y + 7, "TAMAÑO", ui::PEQUENO, cab, 0.9f);
    ui::texto(x + c_juego + c_anio + c_tam, y + 7, "ORIGEN", ui::PEQUENO, cab, 0.9f);
    y += CAB;
    ui::linea(x, y, x + w, y, ui::DIVISOR);
    for (const Instalado &i : A.instalados) {
        const Version *v = A.versiones.por_id(i.id);
        SDL_FRect f{x, y, w, FILA};
        if (y + FILA >= arriba && y <= (float)ALTO) {
            if (ui::sobre(f)) ui::rect(f, ui::alfa(ui::ACENTO, 15));
            SDL_FRect p{x + 12, y + 7, 34, FILA - 14};
            if (const App::Portada *po = portada_de(v)) ui::imagen_cubrir(po->tex, po->w, po->h, p);
            else { ui::rect(p, ui::ACENTO_900); }
            ui::borde(p);
            std::string nombre = v ? v->nombre : i.nombre;
            ui::texto(x + 58, y + 9, ui::recorta(nombre, c_juego - 70, ui::TARJETA), ui::TARJETA);
            ui::texto(x + 58, y + 9 + ui::alto(ui::TARJETA), i.faltan.empty() ? "preparado el " + i.fecha : "faltan ficheros", ui::PEQUENO, i.faltan.empty() ? ui::NEUTRO_600 : ui::ALERTA);
            ui::texto(x + c_juego, y + 19, v && v->anio ? std::to_string(v->anio) : "", ui::CUERPO_13, ui::NEUTRO_700);
            ui::texto(x + c_juego + c_anio, y + 19, util::humano(i.tam), ui::CUERPO_13, ui::NEUTRO_700);
            std::string origen;
            for (const Origen &o : i.origenes) origen += (origen.empty() ? "" : "  ·  ") + o.ruta;
            ui::texto(x + c_juego + c_anio + c_tam, y + 19, ui::recorta(origen, c_origen - 12, ui::CUERPO_13), ui::CUERPO_13, ui::NEUTRO_700);
            if (ui::pulsado_en(f)) abrir_ficha = i.id;
        }
        y += FILA;
        ui::linea(x, y, x + w, y, ui::alfa(ui::TEXTO, 20));
    }
    y += 16;
    SDL_FRect a{x, y, w, ANADIR_H};
    if (ui::sobre(a)) ui::rect(a, ui::alfa(ui::ACENTO, 20));
    ui::borde_discontinuo(a, ui::DIVISOR);
    ui::marcas(a);
    std::string e = "AÑADIR JUEGO";
    float we = ui::ancho(e, ui::ANADIR, 0.6f) + 28;
    ui::icono(ui::MAS, x + (w - we) / 2, y + (ANADIR_H - 20) / 2, 20, ui::ACENTO_700);
    ui::texto(x + (w - we) / 2 + 28, y + (ANADIR_H - ui::alto(ui::ANADIR)) / 2 + 1, e, ui::ANADIR, ui::ACENTO_700, 0.6f);
    if (ui::pulsado_en(a)) anadir = true;
    ui::fin_recorte();
    if (maximo > 0) {
        float h = visible * visible / total, sy = arriba + (visible - h) * (A.scroll / maximo);
        ui::rect({(float)ANCHO - 8, arriba, 4, visible}, ui::alfa(ui::TEXTO, 10));
        ui::rect({(float)ANCHO - 8, sy, 4, h}, ui::alfa(ui::TEXTO, 60));
    }
    if (anadir) { nueva_tarea(); A.pantalla = ANADIR; }
    if (!abrir_ficha.empty()) { A.ficha_id = abrir_ficha; A.quitar_confirmar = false; A.aviso.clear(); A.aviso_error.clear(); A.pantalla = FICHA; }
}

void pantalla_biblioteca() {
    // cabecera: logo a la izquierda, acciones a la derecha
    float y = 20;
    if (A.logo) {
        float h = 34, w = h * (float)A.logo_w / (float)A.logo_h;
        ui::imagen(A.logo, {MARGEN, y, w, h});
    } else ui::texto(MARGEN, y + 4, "source.pcfsite.com", ui::TITULO, ui::TEXTO);
    float wc = ui::ancho_boton("Configuración", ui::ENGRANAJE), wa = ui::ancho_boton("Acerca de");
    if (ui::boton({(float)ANCHO - MARGEN - wc, y, wc, 34}, "Configuración", ui::SECUNDARIO, true, ui::ENGRANAJE)) {
        A.config_carpeta = util::dir_datos(); A.aviso.clear(); A.aviso_error.clear(); A.pantalla = CONFIG;
    }
    if (ui::boton({(float)ANCHO - MARGEN - wc - 8 - wa, y, wa, 34}, "Acerca de", ui::FANTASMA)) A.pantalla = ACERCA;
    y += 34 + 6;
    std::string cuenta = A.instalados.empty() ? "Ninguno todavía" : std::to_string(A.instalados.size()) + (A.instalados.size() == 1 ? " juego" : " juegos");
    ui::texto(MARGEN, y, "Juegos instalados", ui::TITULO);
    ui::texto(MARGEN + ui::ancho("Juegos instalados", ui::TITULO) + 10, y + 11, cuenta, ui::ETIQUETA, ui::NEUTRO_600);
    int seg = ui::segmentos((float)ANCHO - MARGEN - 72, y + 2, {ui::REJILLA, ui::LISTA}, A.lista ? 1 : 0);
    if (seg >= 0 && (seg == 1) != A.lista) { A.lista = seg == 1; A.scroll = 0; util::pon_ajuste("vista", A.lista ? "lista" : "rejilla"); }
    y += ui::alto(ui::TITULO) + 6;
    float ya = A.pantalla == BIBLIOTECA ? avisos(MARGEN, y, ANCHO - 2 * MARGEN) : 0;   // (los diálogos enseñan los suyos)
    float arriba = y + ya;
    if (A.lista) { lista_juegos(arriba); return; }

    // la rejilla: tarjetas 4:5 + texto (tantas columnas como quepan de unos 170 px), y al final la de «Añadir juego»
    const float GAP_X = 24, GAP_Y = 28, PAD_ARRIBA = 24, PAD_ABAJO = 32;
    const int COLS = std::max(2, std::min(6, (int)(((float)ANCHO - 2 * MARGEN + GAP_X) / (160 + GAP_X))));
    float wt = ((float)ANCHO - 2 * MARGEN - GAP_X * (COLS - 1)) / COLS;
    float h_portada = wt * 5 / 4, h_texto = 10 + ui::alto(ui::TARJETA) + 2 + ui::alto(ui::PEQUENO) + 12, ht = h_portada + h_texto;
    int n = (int)A.instalados.size() + 1, filas = (n + COLS - 1) / COLS;
    float total = PAD_ARRIBA + filas * ht + (filas - 1) * GAP_Y + PAD_ABAJO;
    float visible = (float)ALTO - arriba;
    float maximo = std::max(0.f, total - visible);
    A.scroll = std::min(std::max(A.scroll, 0.f), maximo);

    std::string abrir_ficha;
    bool anadir = false;
    ui::recorte({0, arriba, (float)ANCHO, visible});
    for (int k = 0; k < n; k++) {
        float x = MARGEN + (k % COLS) * (wt + GAP_X), yy = arriba + PAD_ARRIBA + (k / COLS) * (ht + GAP_Y) - A.scroll;
        SDL_FRect t{x, yy, wt, ht};
        if (yy + ht < arriba || yy > (float)ALTO) continue;
        bool enc = ui::sobre(t);
        if (k == (int)A.instalados.size()) {   // añadir juego
            if (enc) ui::rect(t, ui::alfa(ui::ACENTO, 20));
            ui::borde_discontinuo(t, ui::DIVISOR);
            ui::marcas(t);
            float cx = x + wt / 2, cy = yy + ht / 2;
            ui::icono(ui::MAS, cx - 20, cy - 36, 40, ui::ACENTO_700);
            std::string e = "AÑADIR JUEGO";
            ui::texto(cx - ui::ancho(e, ui::ANADIR, 0.6f) / 2, cy + 12, e, ui::ANADIR, ui::ACENTO_700, 0.6f);
            if (ui::pulsado_en(t)) anadir = true;
            continue;
        }
        const Instalado &i = A.instalados[k];
        const Version *v = A.versiones.por_id(i.id);
        if (enc) ui::rect(t, ui::alfa(ui::ACENTO, 15));
        // portada: la carátula (pcfsite.com/historia) con la edición encima; si no hay, campo azul acero con «PCF» y la versión
        SDL_FRect p{x, yy, wt, h_portada};
        if (const App::Portada *po = portada_de(v)) {
            ui::imagen_cubrir(po->tex, po->w, po->h, p);
            if (enc) ui::rect(p, ui::alfa(ui::ACENTO, 30));
            if (!v->etiqueta.empty()) ui::etiqueta(x + 10, yy + h_portada - 32, v->etiqueta);
        } else {
            ui::rect(p, ui::ACENTO_900);
            std::string corto = v ? v->corto : "";
            float hp = ui::alto(ui::PORTADA) + (corto.empty() ? 0 : ui::alto(ui::PORTADA_SUB));
            float py = yy + (h_portada - hp) / 2;
            ui::texto(x + (wt - ui::ancho("PCF", ui::PORTADA, -0.9f)) / 2, py, "PCF", ui::PORTADA, ui::FONDO, -0.9f);
            if (!corto.empty()) ui::texto(x + (wt - ui::ancho(corto, ui::PORTADA_SUB, 1.5f)) / 2, py + ui::alto(ui::PORTADA), corto, ui::PORTADA_SUB, ui::alfa(ui::FONDO, 200), 1.5f);
        }
        ui::linea(x, yy + h_portada, x + wt, yy + h_portada, ui::DIVISOR);
        ui::plano(t);
        // texto
        std::string nombre = v ? v->nombre : i.nombre;
        ui::texto(x + 12, yy + h_portada + 10, ui::recorta(nombre, wt - 24, ui::TARJETA), ui::TARJETA);
        std::string meta = (v && v->anio ? std::to_string(v->anio) + " · " : "") + util::humano(i.tam);
        if (!i.faltan.empty()) meta += " · faltan ficheros";
        ui::texto(x + 12, yy + h_portada + 10 + ui::alto(ui::TARJETA) + 2, meta, ui::PEQUENO, i.faltan.empty() ? ui::NEUTRO_600 : ui::ALERTA);
        if (ui::pulsado_en(t)) abrir_ficha = i.id;
    }
    ui::fin_recorte();
    if (maximo > 0) {   // la barra de desplazamiento
        float h = visible * visible / total, sy = arriba + (visible - h) * (A.scroll / maximo);
        ui::rect({(float)ANCHO - 8, arriba, 4, visible}, ui::alfa(ui::TEXTO, 10));
        ui::rect({(float)ANCHO - 8, sy, 4, h}, ui::alfa(ui::TEXTO, 60));
    }
    if (anadir) { nueva_tarea(); A.pantalla = ANADIR; }
    if (!abrir_ficha.empty()) { A.ficha_id = abrir_ficha; A.quitar_confirmar = false; A.aviso.clear(); A.aviso_error.clear(); A.pantalla = FICHA; }
}

// ---------------------------------------------------------------- diálogos
void pantalla_ficha() {
    const Instalado *i = instalado(A.ficha_id);
    if (!i) { A.pantalla = BIBLIOTECA; return; }
    const Version *v = A.versiones.por_id(i->id);
    const float W = 600, w = W - 2 * MARGEN - (v && !v->portada.empty() ? 128 : 0);
    // medir
    std::string detalle = v ? v->detalle : "";
    float h = 0;
    if (!detalle.empty()) h += ui::parrafo(0, 0, w, detalle, ui::CUERPO_13, ui::NEUTRO_700, false) + 10;
    h += ui::alto(ui::ETIQUETA) + ui::alto(ui::CUERPO_13) + 10;   // preparado el
    for (const Origen &o : i->origenes) h += ui::alto(ui::ETIQUETA) + ui::parrafo(0, 0, w, o.ruta, ui::CUERPO_13, ui::TEXTO, false) + 10;
    h += ui::alto(ui::ETIQUETA) + ui::parrafo(0, 0, w, i->carpeta, ui::CUERPO_13, ui::TEXTO, false) + 10;
    std::string perfil = perfil_de(*i);
    h += ui::alto(ui::ETIQUETA) + 4 + (float)i->mods.size() * 26 + (perfil.empty() ? 0 : ui::alto(ui::PEQUENO) + 4) + 36 + 12;   // mods
    if (!i->faltan.empty()) h += ui::parrafo(0, 0, w, "Faltan ficheros que el juego necesita y no vienen en el CD: x", ui::CUERPO_13, ui::ALERTA, false) + 10;
    if (!A.aviso_error.empty()) h += ui::parrafo(0, 0, w, A.aviso_error, ui::CUERPO_13, ui::ALERTA, false) + 8;
    if (!A.aviso.empty()) h += ui::parrafo(0, 0, w, A.aviso, ui::CUERPO_13, ui::ACENTO_700, false) + 8;
    h += 14 + 36;
    const App::Portada *po = portada_de(v);
    const float WP = 110, HP = WP * 5 / 4;   // la carátula, a la izquierda
    float h_cuerpo = std::max(h, po ? HP + 14 + 36 : 0.f);
    SDL_FRect c = dialogo(v ? v->nombre : i->nombre, W, h_cuerpo + 2 * MARGEN);
    float y = c.y;
    if (po) {
        SDL_FRect r{c.x, y, WP, HP};
        ui::imagen_cubrir(po->tex, po->w, po->h, r);
        ui::plano(r);
        if (!v->etiqueta.empty()) ui::etiqueta(c.x + 8, y + HP - 30, v->etiqueta);
        c.x += WP + 18; c.w -= WP + 18;
    }
    const float w2 = c.w;
    auto campo = [&](const std::string &etiqueta, const std::string &valor) {
        ui::texto(c.x, y, etiqueta, ui::ETIQUETA, ui::alfa(ui::TEXTO, 180));
        y += ui::alto(ui::ETIQUETA);
        y += ui::parrafo(c.x, y, w2, valor, ui::CUERPO_13, ui::TEXTO) + 10;
    };
    if (!detalle.empty()) y += ui::parrafo(c.x, y, w2, con_mayuscula(detalle), ui::CUERPO_13, ui::NEUTRO_700) + 10;
    campo("Preparado el", i->fecha + "  ·  " + util::humano(i->tam));
    for (const Origen &o : i->origenes) campo(con_mayuscula(o.nombre) + (o.es_iso ? " (imagen ISO)" : " (carpeta)"), util::nativa(o.ruta));
    campo("Carpeta del juego (aquí van las partidas guardadas)", util::nativa(i->carpeta));
    {   // mods: casilla por mod (orden = prioridad), quitar, importar
        ui::texto(c.x, y, "Mods (ficheros del juego modificados; el primero manda)", ui::ETIQUETA, ui::alfa(ui::TEXTO, 180));
        y += ui::alto(ui::ETIQUETA) + 4;
        Instalado *im = instalado_mut(i->id);
        float wq = ui::ancho_boton("Quitar");
        std::string quitar;
        for (Mod &m : im->mods) {
            bool v = m.activo;
            if (ui::casilla(c.x, y, w2 - wq - 8, m.nombre, v)) { m.activo = v; guardar_mods(*im); A.aviso.clear(); A.aviso_error.clear(); }
            if (ui::boton({c.x + w2 - wq, y - 3, wq, 30}, "Quitar", ui::FANTASMA)) quitar = m.nombre;
            y += 26;
        }
        if (!quitar.empty()) { quitar_mod(*im, quitar); A.aviso = "Mod «" + quitar + "» quitado."; A.aviso_error.clear(); }
        if (!perfil.empty()) { ui::texto(c.x, y, "Con estos mods las partidas van aparte: " + util::nativa(perfil.substr(i->carpeta.size() + 1)), ui::PEQUENO, ui::NEUTRO_600); y += ui::alto(ui::PEQUENO) + 4; }
        float wz = ui::ancho_boton("Importar mod (zip, rar)…"), wc = ui::ancho_boton("Importar mod (carpeta)…");
        if (ui::boton({c.x, y, wz, 36}, "Importar mod (zip, rar)…", ui::SECUNDARIO, !A.dialogo_abierto)) abrir_dialogo(false, "", false, true);
        if (ui::boton({c.x + wz + 8, y, wc, 36}, "Importar mod (carpeta)…", ui::SECUNDARIO, !A.dialogo_abierto)) abrir_dialogo(true, "", false, true);
        y += 36 + 12;
    }
    if (!i->faltan.empty()) {
        std::string f = "Faltan ficheros que el juego necesita y no vienen en el CD: ";
        for (size_t k = 0; k < i->faltan.size(); k++) f += (k ? ", " : "") + i->faltan[k];
        y += ui::parrafo(c.x, y, w2, f + ". Cópialos (de la carpeta WINDOWS\\SYSTEM de una instalación) a " + util::nativa(i->carpeta + "/drive_c/WINDOWS/SYSTEM") + ".", ui::CUERPO_13, ui::ALERTA) + 10;
    }
    if (!A.aviso_error.empty()) y += ui::parrafo(c.x, y, w2, A.aviso_error, ui::CUERPO_13, ui::ALERTA) + 8;
    if (!A.aviso.empty()) y += ui::parrafo(c.x, y, w2, A.aviso, ui::CUERPO_13, ui::ACENTO_700) + 8;
    y = c.y + h_cuerpo - 36;
    if (A.quitar_confirmar) {
        ui::texto(c.x, y + 9, "¿Quitar el juego y sus partidas guardadas?", ui::MEDIO, ui::ALERTA);
        int b = fila_botones(c.x + w, y, {{"Sí, quitar", ui::PRIMARIO}, {"No", ui::SECUNDARIO}});
        if (b == 0) { Instalado copia = *i; quitar(copia); cerrar_dialogo(); return; }
        if (b == 1) A.quitar_confirmar = false;
    } else {
        float wq = ui::ancho_boton("Quitar");
        if (ui::boton({c.x, y, wq, 36}, "Quitar", ui::FANTASMA)) A.quitar_confirmar = true;
        int b = fila_botones(c.x + w, y, {{"Jugar", ui::PRIMARIO}, {"Cerrar", ui::SECUNDARIO}});
        if (b == 0) { Instalado copia = *i; jugar(copia); }
        if (b == 1) cerrar_dialogo();
    }
}

void pantalla_anadir() {
    const float W = 560, w = W - 2 * MARGEN;
    std::string t1 = "Elige la imagen ISO del CD original, la carpeta con el contenido del CD, o la carpeta del juego ya instalado.";
    std::string t2 = "Se aceptan: " + lista_versiones() + ". La app no incluye ningún fichero de los juegos: la instalación se reproduce "
                     "desde tu CD, tal como la hacía el instalador original, y los programas se reconstruyen en memoria a partir de tus "
                     "propios ficheros. La ISO se queda donde está: el juego la usa como su CD.";
    float h1 = ui::parrafo(0, 0, w, t1, ui::CUERPO, ui::TEXTO, false), h2 = ui::parrafo(0, 0, w, t2, ui::CUERPO_13, ui::NEUTRO_700, false);
    float ha = A.aviso_error.empty() ? 0 : ui::parrafo(0, 0, w, A.aviso_error, ui::CUERPO_13, ui::ALERTA, false) + 8;
    SDL_FRect c = dialogo("Añadir juego", W, h1 + 10 + h2 + 20 + ha + 36 + 2 * MARGEN);
    float y = c.y;
    y += ui::parrafo(c.x, y, w, t1, ui::CUERPO, ui::TEXTO) + 10;
    y += ui::parrafo(c.x, y, w, t2, ui::CUERPO_13, ui::NEUTRO_700) + 20;
    y += avisos(c.x, y, w);
    float wi = ui::ancho_boton("Elegir una ISO…"), wk = ui::ancho_boton("Elegir una carpeta…");
    if (ui::boton({c.x, y, wi, 36}, "Elegir una ISO…", ui::PRIMARIO, !A.dialogo_abierto)) abrir_dialogo(false, "");
    if (ui::boton({c.x + wi + 8, y, wk, 36}, "Elegir una carpeta…", ui::SECUNDARIO, !A.dialogo_abierto)) abrir_dialogo(true, "");
    if (fila_botones(c.x + w, y, {{"Cancelar", ui::SECUNDARIO}}) == 0) cerrar_dialogo();
}

void pantalla_version() {
    const float W = 560, w = W - 2 * MARGEN;
    std::vector<const Version *> vistas;
    for (const Coincidencia &x : A.candidatas) { bool ya = false; for (auto v : vistas) if (v == x.v) ya = true; if (!ya) vistas.push_back(x.v); }
    std::string t1 = "Este CD vale para más de una versión. Elige la que quieres preparar.";
    float h1 = ui::parrafo(0, 0, w, t1, ui::CUERPO, ui::TEXTO, false);
    SDL_FRect c = dialogo("¿Qué versión?", W, h1 + 16 + vistas.size() * 70 + 14 + 36 + 2 * MARGEN);
    float y = c.y;
    y += ui::parrafo(c.x, y, w, t1, ui::CUERPO, ui::TEXTO) + 16;
    for (const Version *v : vistas) {
        SDL_FRect t{c.x, y, w, 58};
        if (ui::sobre(t)) ui::rect(t, ui::alfa(ui::ACENTO, 15));
        ui::plano(t);
        ui::texto(c.x + 14, y + 9, v->nombre, ui::TARJETA);
        std::string det = v->detalle;
        for (const std::string &rol : v->roles_necesarios()) det += (det.empty() ? "Hace falta " : "  ·  hace falta ") + v->nombre_rol(rol);
        ui::texto(c.x + 14, y + 9 + ui::alto(ui::TARJETA), ui::recorta(det, w - 140, ui::PEQUENO), ui::PEQUENO, ui::NEUTRO_600);
        float we = ui::ancho_boton("Elegir");
        if (ui::boton({c.x + w - 14 - we, y + 11, we, 36}, "Elegir", ui::PRIMARIO)) { elegir_version(v); return; }
        y += 70;
    }
    y += 14;
    if (fila_botones(c.x + w, y, {{"Cancelar", ui::SECUNDARIO}}) == 0) cerrar_dialogo();
}

void pantalla_fuentes() {
    const Version &v = *A.tarea.v;
    const float W = 620, w = W - 2 * MARGEN;
    std::vector<std::string> roles = v.roles_necesarios();
    bool ya = instalado(v.id) != nullptr;
    std::string q;
    if (A.tarea.completa())
        q = A.tarea.cd_desde_iso()
            ? "El juego se prepara en " + util::nativa(A.tarea.destino) + " (la instalación, con sus partidas guardadas). "
              "El CD se usará directamente desde la ISO, sin copiarlo: déjala donde está."
            : "El juego se prepara en " + util::nativa(A.tarea.destino) + " (la instalación, con sus partidas guardadas) y ahí se copia también lo que el juego lee del CD.";
    std::string aviso_ya = "Este juego ya está preparado: hacerlo otra vez borra la carpeta anterior, con sus partidas guardadas.";
    float h = 0;
    if (!v.nota.empty()) h += ui::parrafo(0, 0, w, v.nota, ui::CUERPO_13, ui::ACENTO_700, false) + 12;
    h += roles.size() * 70;
    if (!q.empty()) h += ui::parrafo(0, 0, w, q, ui::CUERPO_13, ui::NEUTRO_700, false) + 10;
    if (ya) h += ui::parrafo(0, 0, w, aviso_ya, ui::CUERPO_13, ui::ALERTA, false) + 10;
    if (!A.aviso_error.empty()) h += ui::parrafo(0, 0, w, A.aviso_error, ui::CUERPO_13, ui::ALERTA, false) + 8;
    else if (!A.aviso.empty()) h += ui::parrafo(0, 0, w, A.aviso, ui::CUERPO_13, ui::ACENTO_700, false) + 8;
    h += 10 + 36;
    SDL_FRect c = dialogo("Preparar " + v.nombre, W, h + 2 * MARGEN);
    float y = c.y;
    if (!v.nota.empty()) y += ui::parrafo(c.x, y, w, v.nota, ui::CUERPO_13, ui::ACENTO_700) + 12;
    for (const std::string &rol : roles) {
        bool hay = A.tarea.fuentes.count(rol) > 0;
        SDL_FRect t{c.x, y, w, 58};
        ui::plano(t);
        std::string nr = v.nombre_rol(rol);
        if (nr.rfind("el ", 0) == 0 || nr.rfind("la ", 0) == 0) nr = nr.substr(3);
        if (hay) ui::icono(ui::VISTO, c.x + 14, y + 10, 18, ui::ACENTO_700);
        ui::texto(c.x + 14 + (hay ? 24 : 0), y + 9, con_mayuscula(nr), ui::TARJETA, hay ? ui::ACENTO_700 : ui::TEXTO);
        std::string sub = hay ? (A.tarea.isos.count(rol) ? A.tarea.isos.at(rol) : A.tarea.fuentes.at(rol))
                              : (rol == "instalada" ? "elige la carpeta" : "elige la ISO o la carpeta con su contenido");
        ui::texto(c.x + 14, y + 9 + ui::alto(ui::TARJETA), ui::recorta(sub, w - (hay ? 28 : 300), ui::PEQUENO), ui::PEQUENO, ui::NEUTRO_600);
        if (!hay) {
            float wi = ui::ancho_boton("Elegir ISO…"), wk = ui::ancho_boton("Elegir carpeta…");
            float x = c.x + w - 14 - wk;
            if (ui::boton({x, y + 11, wk, 36}, "Elegir carpeta…", ui::SECUNDARIO, !A.dialogo_abierto)) abrir_dialogo(true, rol);
            if (rol != "instalada" && ui::boton({x - 8 - wi, y + 11, wi, 36}, "Elegir ISO…", ui::SECUNDARIO, !A.dialogo_abierto)) abrir_dialogo(false, rol);
        }
        y += 70;
    }
    if (!q.empty()) y += ui::parrafo(c.x, y, w, q, ui::CUERPO_13, ui::NEUTRO_700) + 10;
    if (ya) y += ui::parrafo(c.x, y, w, aviso_ya, ui::CUERPO_13, ui::ALERTA) + 10;
    y += avisos(c.x, y, w);
    y += 10;
    int b = fila_botones(c.x + w, y, {{ya ? "Preparar de nuevo" : "Preparar", ui::PRIMARIO}, {"Cancelar", ui::SECUNDARIO}}, A.tarea.completa());
    if (b == 0) empezar_instalacion();
    if (b == 1) cerrar_dialogo();
}

void pantalla_instalando() {
    const Version &v = *A.tarea.v;
    const float W = 560, w = W - 2 * MARGEN;
    std::string t1 = "Preparando la instalación en " + A.tarea.destino + (A.tarea.cd_desde_iso() ? "." : ", y copiando lo que el juego lee del CD.");
    float h1 = ui::parrafo(0, 0, w, t1, ui::CUERPO_13, ui::NEUTRO_700, false);
    SDL_FRect c = dialogo("Preparando " + v.nombre, W, h1 + 16 + 22 + 10 + ui::alto(ui::CUERPO_13) + 20 + 36 + 2 * MARGEN);
    float y = c.y;
    y += ui::parrafo(c.x, y, w, t1, ui::CUERPO_13, ui::NEUTRO_700) + 16;
    uint64_t h = A.progreso.hecho, t = A.progreso.total;
    ui::barra({c.x, y, w, 22}, t ? (float)h / (float)t : 0);
    y += 32;
    ui::texto(c.x, y, ui::recorta(util::humano(h) + " de " + util::humano(t) + "  ·  " + A.progreso.lee(), w, ui::CUERPO_13), ui::CUERPO_13, ui::NEUTRO_700);
    y += ui::alto(ui::CUERPO_13) + 20;
    if (fila_botones(c.x + w, y, {{"Cancelar", ui::SECUNDARIO}}, !A.progreso.cancelar) == 0) A.progreso.cancelar = true;
    if (A.progreso.terminado) {
        if (A.hilo.joinable()) A.hilo.join();
        bool ok = A.progreso.ok;
        std::string err = A.progreso.fallo();
        bool cancelado = A.progreso.cancelar;
        recargar_instalados();
        std::string id = v.id;
        nueva_tarea();
        desmontar_todo();
        A.pantalla = BIBLIOTECA;
        if (ok) {
            A.aviso = "Listo. Ya puedes jugar.";
            if (const Instalado *i = instalado(id); i && !i->faltan.empty()) {
                A.aviso.clear();
                A.aviso_error = "Preparado, pero faltan ficheros que el juego necesita y no vienen en el CD: ";
                for (size_t k = 0; k < i->faltan.size(); k++) A.aviso_error += (k ? ", " : "") + i->faltan[k];
                A.aviso_error += ". Cópialos (de la carpeta WINDOWS\\SYSTEM de una instalación) a " + i->carpeta + "/drive_c/WINDOWS/SYSTEM.";
            }
        } else if (cancelado) A.aviso = "Cancelado.";
        else A.aviso_error = "Ha fallado: " + err;
    }
}

void pantalla_config() {
    const float W = 520, w = W - 2 * MARGEN;
    std::string nota = "Los juegos nuevos se prepararán aquí. Los ya preparados no se mueven.";
    float hn = ui::parrafo(0, 0, w, nota, ui::ETIQUETA, ui::NEUTRO_600, false);
    float ha = A.aviso_error.empty() ? 0 : ui::parrafo(0, 0, w, A.aviso_error, ui::CUERPO_13, ui::ALERTA, false) + 8;
    SDL_FRect c = dialogo("Configuración", W, ui::alto(ui::ETIQUETA) + 5 + 36 + 8 + hn + 24 + 1 + 14 + ui::alto(ui::ETIQUETA) + 24 + ha + 36 + 2 * MARGEN);
    float y = c.y;
    ui::texto(c.x, y, "Carpeta de instalación", ui::ETIQUETA, ui::alfa(ui::TEXTO, 180));
    y += ui::alto(ui::ETIQUETA) + 5;
    float we = ui::ancho_boton("Examinar…");
    ui::campo({c.x, y, w - we - 8, 36}, A.config_carpeta);
    if (ui::boton({c.x + w - we, y, we, 36}, "Examinar…", ui::SECUNDARIO, !A.dialogo_abierto)) abrir_dialogo(true, "", true);
    y += 36 + 8;
    y += ui::parrafo(c.x, y, w, nota, ui::ETIQUETA, ui::NEUTRO_600) + 24;
    ui::linea(c.x, y, c.x + w, y, ui::DIVISOR);
    y += 14;
    ui::texto(c.x, y, "Espacio libre en el disco", ui::ETIQUETA, ui::NEUTRO_700);
    std::string libre = util::humano(util::espacio_libre(util::es_dir(A.config_carpeta) ? A.config_carpeta : util::carpeta_de(A.config_carpeta)));
    ui::texto(c.x + w - ui::ancho(libre, ui::ETIQUETA), y, libre, ui::ETIQUETA, ui::NEUTRO_700);
    y += ui::alto(ui::ETIQUETA) + 24;
    y += avisos(c.x, y, w);
    int b = fila_botones(c.x + w, y, {{"Guardar", ui::PRIMARIO}, {"Cancelar", ui::SECUNDARIO}});
    if (b == 0) {
        if (util::cambiar_dir_datos(A.config_carpeta)) { recargar_instalados(); cerrar_dialogo(); }
        else A.aviso_error = "No se puede usar esa carpeta.";
    }
    if (b == 1) cerrar_dialogo();
}

void pantalla_acerca() {
    const float W = 560, w = W - 2 * MARGEN;
    std::string t1 = "Los PC Fútbol del 4.0 al 2001 traducidos desde su código original a C y compilados como programas nativos. "
                     "Sin emulador ni máquina virtual: cada juego corre tal cual, con sus partidas guardadas, sobre una capa que imita lo que les daba Windows 98.";
    std::string t2 = "Esta app no incluye ningún fichero de los juegos. Todo sale de tu CD original (la ISO o la carpeta) o de tu instalación: "
                     "con eso se prepara la carpeta del juego y se reconstruyen en memoria sus programas.";
    std::string t3 = "Acepta: " + lista_versiones() + ".";
    std::string t4 = "Software libre (GPL-3.0), hecho por aficionados. Sin afiliación con Dinamic Multimedia ni con los titulares de los derechos de los "
                     "juegos. PC Fútbol es una marca de sus propietarios.";
    std::string version = std::string("Versión ") + PCF_VERSION + "  ·  versiones de los juegos del " + A.versiones.generado;
    float h = ui::alto(ui::ETIQUETA) + 10;
    h += ui::parrafo(0, 0, w, t1, ui::CUERPO, ui::TEXTO, false) + 10;
    h += ui::parrafo(0, 0, w, t2, ui::CUERPO_13, ui::NEUTRO_700, false) + 10;
    h += ui::parrafo(0, 0, w, t3, ui::CUERPO_13, ui::NEUTRO_700, false) + 16;
    h += 36 + 10 + 36 + 16;   // comunidad, autor
    h += ui::parrafo(0, 0, w, t4, ui::PEQUENO, ui::NEUTRO_600, false) + 16 + 36;
    SDL_FRect c = dialogo("Acerca de", W, h + 2 * MARGEN);
    float y = c.y;
    ui::texto(c.x, y, version, ui::ETIQUETA, ui::NEUTRO_600);
    y += ui::alto(ui::ETIQUETA) + 10;
    y += ui::parrafo(c.x, y, w, t1, ui::CUERPO, ui::TEXTO) + 10;
    y += ui::parrafo(c.x, y, w, t2, ui::CUERPO_13, ui::NEUTRO_700) + 10;
    y += ui::parrafo(c.x, y, w, t3, ui::CUERPO_13, ui::NEUTRO_700) + 16;
    // comunidad y autor: una línea con el botón a la derecha
    auto enlace = [&](const std::string &texto, const std::string &etiqueta, const std::string &url) {
        ui::texto(c.x, y + 9, texto, ui::MEDIO, ui::TEXTO);
        float wb = ui::ancho_boton(etiqueta);
        if (ui::boton({c.x + w - wb, y, wb, 36}, etiqueta, ui::PRIMARIO)) SDL_OpenURL(url.c_str());
        y += 36 + 10;
    };
    enlace(std::string("Comunidad: ") + WEB, "Abrir www.pcfsite.com", std::string("https://") + WEB);
    enlace(std::string("Creada por ") + AUTOR, "Ver en X (Twitter)", AUTOR_URL);
    y += 6;
    y += ui::parrafo(c.x, y, w, t4, ui::PEQUENO, ui::NEUTRO_600) + 16;
    if (fila_botones(c.x + w, y, {{"Cerrar", ui::SECUNDARIO}}) == 0) cerrar_dialogo();
}

}  // namespace

// Sin ventana, desde la terminal (pruebas y automatización):
//   lanzador --listar                         las versiones aceptadas
//   lanzador --instalar FUENTE… [--version id]   reconoce e instala (FUENTE: ISO o carpeta; varias si hacen falta varios discos)
//   lanzador --jugar id                       arranca un juego preparado (con los mods que tenga activos)
//   lanzador --mod id <zip|carpeta>           importa un mod; --mods id [a,b…] deja activos exactamente esos
int modo_terminal(int argc, char **argv) {
    A.terminal = true;
    if (const char *f = getenv("PCF_SALIDA"); f && *f) { freopen(f, "w", stdout); freopen(f, "a", stderr); }   // pruebas sin consola
#ifdef _WIN32
    else if (AttachConsole(ATTACH_PARENT_PROCESS)) { freopen("CONOUT$", "w", stdout); freopen("CONOUT$", "w", stderr); }
#endif
    std::string error;
    std::string vpath = util::dir_recursos() + "/versiones.json";
    if (!util::existe(vpath)) vpath = std::string(PCF_APP_SRC) + "/versiones.json";
    if (!A.versiones.cargar(vpath, error)) { fprintf(stderr, "%s\n", error.c_str()); return 2; }
    std::string modo = argv[1];
    if (modo == "--listar") {
        for (const Version &v : A.versiones.lista) {
            printf("%-8s %s\n", v.id.c_str(), v.titulo().c_str());
            for (const std::string &rol : v.roles_necesarios()) printf("         necesita %s\n", v.nombre_rol(rol).c_str());
        }
        return 0;
    }
    if (modo == "--mod" && argc > 3) {   // --mod <id> <zip o carpeta>: importa un mod (y lo activa)
        for (Instalado &i : A.instalados = instalados(util::carpetas_datos()), A.instalados)
            if (i.id == argv[2]) {
                std::string nombre, error;
                if (!importar_mod(i, util::normaliza(argv[3]), nombre, error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
                i.mods.push_back({nombre, true});
                guardar_mods(i);
                printf("mod «%s» importado en %s%s%s\n", nombre.c_str(), dir_mods(i).c_str(), error.empty() ? "" : "; ", error.c_str());
                return 0;
            }
        fprintf(stderr, "%s no está instalado\n", argv[2]);
        return 1;
    }
    if (modo == "--mods" && argc > 2) {   // --mods <id> [nombre,nombre…]: activa exactamente esos (ninguno: todos desactivados)
        for (Instalado &i : A.instalados = instalados(util::carpetas_datos()), A.instalados)
            if (i.id == argv[2]) {
                std::string lista = argc > 3 ? argv[3] : "";
                for (Mod &m : i.mods) m.activo = ("," + lista + ",").find("," + m.nombre + ",") != std::string::npos;
                guardar_mods(i);
                for (const Mod &m : i.mods) printf("%s %s\n", m.activo ? "[x]" : "[ ]", m.nombre.c_str());
                return 0;
            }
        fprintf(stderr, "%s no está instalado\n", argv[2]);
        return 1;
    }
    if (modo == "--jugar" && argc > 2) {
        for (const Instalado &i : instalados(util::carpetas_datos()))
            if (i.id == argv[2]) { jugar(i); fprintf(stderr, "%s\n", A.aviso_error.c_str()); return 1; }
        fprintf(stderr, "%s no está instalado\n", argv[2]);
        return 1;
    }
    if (modo == "--is6" && argc > 4) {   // pruebas: lanzador --is6 <carpeta del CD> <grupo> <destino>
        Is6 is;
        if (!is.abrir(argv[2], error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
        for (const Is6Grupo &g : is.grupos()) printf("grupo %-32s %u..%u\n", g.nombre.c_str(), g.primero, g.ultimo);
        const Is6Grupo *g = is.grupo(argv[3]);
        if (!g) { fprintf(stderr, "no hay grupo %s\n", argv[3]); return 1; }
        int n = 0;
        for (uint32_t i = g->primero; i <= g->ultimo; i++) {
            const Is6Fichero &f = is.ficheros()[i];
            if (!f.valido) continue;
            std::string dir = f.directorio;
            for (auto &c : dir) if (c == '\\') c = '/';
            std::string dst = std::string(argv[4]) + (dir.empty() ? "" : "/" + dir) + "/" + f.nombre;
            if (!is.extraer(i, dst, error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
            n++;
        }
        printf("%d ficheros extraídos del grupo %s\n", n, argv[3]);
        return 0;
    }
    if (modo == "--cab" && argc > 3) {   // pruebas: lanzador --cab <exe o cab> <destino>
        std::vector<std::string> nombres;
        if (!mscab_extraer_en(argv[2], argv[3], nombres, error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
        for (auto &n : nombres) printf("%s\n", n.c_str());
        return 0;
    }
    if (modo != "--instalar") { fprintf(stderr, "uso: lanzador [--listar | --instalar FUENTE… [--version id] | --jugar id]\n"); return 2; }
    std::vector<std::string> fuentes;
    std::string quiere;
    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "--version" && i + 1 < argc) quiere = argv[++i];
        else fuentes.push_back(util::normaliza(argv[i]));
    }
    for (const std::string &f : fuentes) {
        std::string dir = util::es_dir(f) ? f : montar_iso(f);
        if (dir.empty()) { fprintf(stderr, "no se puede montar %s\n", f.c_str()); desmontar_todo(); return 1; }
        bool es_iso = dir != f;
        std::vector<Coincidencia> c = A.versiones.identificar(dir);
        if (c.empty()) { fprintf(stderr, "no se reconoce %s\n", f.c_str()); desmontar_todo(); return 1; }
        for (const Coincidencia &x : c) printf("%s: %s (%s)\n", f.c_str(), x.v->titulo().c_str(), x.v->nombre_rol(x.rol).c_str());
        if (!A.tarea.v) {
            const Coincidencia *e = nullptr;
            for (const Coincidencia &x : c) if (quiere.empty() || x.v->id == quiere) { e = &x; break; }
            if (!e) { fprintf(stderr, "%s no es de la versión %s\n", f.c_str(), quiere.c_str()); desmontar_todo(); return 1; }
            A.tarea.v = e->v;
            A.tarea.destino = util::dir_datos() + "/" + e->v->id;
        }
        for (const Coincidencia &x : c) if (x.v == A.tarea.v) { A.tarea.fuentes[x.rol] = x.carpeta; if (es_iso) A.tarea.isos[x.rol] = f; }
    }
    if (!A.tarea.completa()) {
        fprintf(stderr, "faltan fuentes para %s:\n", A.tarea.v->nombre.c_str());
        for (const std::string &rol : A.tarea.v->roles_necesarios()) if (!A.tarea.fuentes.count(rol)) fprintf(stderr, "  %s\n", A.tarea.v->nombre_rol(rol).c_str());
        desmontar_todo();
        return 1;
    }
    printf("preparando %s en %s\n", A.tarea.v->titulo().c_str(), A.tarea.destino.c_str());
    desmontar_cd_de(A.tarea.destino);
    A.progreso.reinicia();
    std::thread h([] { instalar(A.tarea, A.progreso); });
    uint64_t ultimo = 0;
    while (!A.progreso.terminado) {
        SDL_Delay(500);
        uint64_t hecho = A.progreso.hecho, total = A.progreso.total;
        if (hecho - ultimo > 50000000 || hecho == total) { printf("  %s de %s  %s\n", util::humano(hecho).c_str(), util::humano(total).c_str(), A.progreso.lee().c_str()); ultimo = hecho; }
    }
    h.join();
    desmontar_todo();
    if (!A.progreso.ok) { fprintf(stderr, "fallo: %s\n", A.progreso.fallo().c_str()); return 1; }
    printf("%s", util::leer(A.tarea.destino + "/instalacion.log").c_str());
    printf("instalado: %s\n", A.tarea.destino.c_str());
    return 0;
}

#ifdef _WIN32
// «--programa <nombre> [argumento]»: este mismo ejecutable hace de anfitrión de un programa traducido (programas/<nombre>.dll,
// que exporta pcf_main). Así el juego corre en un proceso aparte sin que haya más .exe que este.
int modo_programa(int argc, char **argv) {
    std::string dll = util::dir_ejecutables() + "/programas/" + argv[2] + ".dll";
    HMODULE h = LoadLibraryA(dll.c_str());
    if (!h) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NOMBRE_APP, ("No se puede cargar " + dll).c_str(), nullptr); return 1; }
    auto fn = (int (*)(int, char **))GetProcAddress(h, "pcf_main");
    if (!fn) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NOMBRE_APP, (dll + " no es un programa de la app").c_str(), nullptr); return 1; }
    return fn(argc - 2, argv + 2);   // argv[0] = el nombre del programa, argv[1] = su argumento, si lo hay
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc > 2 && std::string(argv[1]) == "--programa") return modo_programa(argc, argv);
#endif
    if (argc > 1) return modo_terminal(argc, argv);
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");   // Windows: el icono del .exe (win/lanzador.rc) en la ventana
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NOMBRE_APP, SDL_GetError(), nullptr); return 1; }
    {   // que quepa en la pantalla
        SDL_Rect util_;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &util_)) {
            ANCHO = std::max(ANCHO_MIN, std::min(ANCHO, util_.w - 40));
            ALTO = std::max(ALTO_MIN, std::min(ALTO, util_.h - 60));
        }
    }
    if (!SDL_CreateWindowAndRenderer(NOMBRE_APP, ANCHO, ALTO, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE, &A.win, &A.ren)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NOMBRE_APP, SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetWindowMinimumSize(A.win, ANCHO_MIN, ALTO_MIN);
    SDL_SetWindowPosition(A.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);   // siempre en el centro de la pantalla
    SDL_RaiseWindow(A.win);
    A.reclamar_foco = SDL_GetTicks() + 300;
    float escala = SDL_GetWindowPixelDensity(A.win);
    if (escala <= 0) escala = 1;
    SDL_SetRenderScale(A.ren, escala, escala);
    SDL_SetRenderVSync(A.ren, 1);
    A.recursos = util::dir_recursos();
    if (!util::existe(A.recursos + "/fonts/Barlow-Regular.ttf")) A.recursos = PCF_APP_SRC;   // desarrollo: el repo
    if (!ui::init(A.ren, escala, A.recursos + "/fonts")) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NOMBRE_APP, ("No se encuentran las fuentes en " + A.recursos + "/fonts").c_str(), A.win);
        return 1;
    }
    A.logo = ui::cargar_png(A.recursos + "/assets/logo.png", A.logo_w, A.logo_h);
    std::string error;
    std::string vpath = util::dir_recursos() + "/versiones.json";
    if (!util::existe(vpath)) vpath = std::string(PCF_APP_SRC) + "/versiones.json";   // desarrollo: el repo
    if (!A.versiones.cargar(vpath, error)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NOMBRE_APP, error.c_str(), A.win);
        return 1;
    }
    recargar_instalados();
    A.lista = util::ajuste("vista") == "lista";
    if (const char *pv = getenv("PCF_VISTA")) A.lista = std::string(pv) == "lista";   // pruebas
    if (const char *pp = getenv("PCF_PANTALLA")) {   // pruebas: abrir en un diálogo
        std::string p = pp;
        if (p == "acerca") A.pantalla = ACERCA;
        else if (p == "anadir") A.pantalla = ANADIR;
        else if (p == "config") { A.config_carpeta = util::dir_datos(); A.pantalla = CONFIG; }
        else if (p.rfind("ficha:", 0) == 0) { A.ficha_id = p.substr(6); A.pantalla = FICHA; }
        else if (p.rfind("fuentes:", 0) == 0) if (const Version *v = A.versiones.por_id(p.substr(8))) {   // con fuentes de mentira
            A.tarea.v = v; A.tarea.destino = util::dir_datos() + "/" + v->id;
            for (const std::string &rol : v->roles_necesarios()) { A.tarea.fuentes[rol] = "/tmp/" + rol; A.tarea.isos[rol] = "/Volumes/Dinamic/PC Fútbol/" + rol + ".iso"; }
            A.aviso = "Reconocido: " + v->nombre_rol(v->roles_necesarios().back()) + ".";
            A.pantalla = FUENTES;
        }
    }
    if (const char *pj = getenv("PCF_JUGAR")) if (const Instalado *i = instalado(pj)) { Instalado copia = *i; jugar(copia); }   // pruebas

    bool pulsado = false;
    while (true) {
        bool clic = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                if (A.pantalla == INSTALANDO) { A.progreso.cancelar = true; if (A.hilo.joinable()) A.hilo.join(); }
                desmontar_todo();
                SDL_Quit();
                return 0;
            }
            if (ev.type == SDL_EVENT_MOUSE_WHEEL && A.pantalla == BIBLIOTECA) A.scroll -= ev.wheel.y * 36;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) pulsado = true;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) { pulsado = false; clic = true; }
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && A.pantalla != INSTALANDO && A.pantalla != BIBLIOTECA) cerrar_dialogo();
        }
        vigilar_juego();
        if (A.reclamar_foco && SDL_GetTicks() > A.reclamar_foco) { A.reclamar_foco = 0; SDL_RaiseWindow(A.win); }
        {   // respuesta de un diálogo de ficheros
            std::string r;
            bool hay = false, para_config = false, para_mod = false;
            {
                std::lock_guard<std::mutex> lk(A.mx);
                if (A.hay_respuesta) { hay = true; r = A.respuesta; A.hay_respuesta = false; para_config = A.dialogo_config; para_mod = A.dialogo_mod; }
            }
            if (hay && !r.empty()) { if (para_config) A.config_carpeta = r; else if (para_mod) importar_mod_a_ficha(r); else procesar_fuente(r); }
        }
        SDL_GetWindowSize(A.win, &ANCHO, &ALTO);
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        ui::fotograma(mx, my, pulsado, clic);
        ui::rect({0, 0, (float)ANCHO, (float)ALTO}, ui::FONDO);
        // la biblioteca siempre está debajo; los diálogos, encima (y sin ratón para lo de debajo)
        if (A.pantalla != BIBLIOTECA) ui::fotograma(-1, -1, false, false);
        pantalla_biblioteca();
        ui::fotograma(mx, my, pulsado, clic);
        switch (A.pantalla) {
        case BIBLIOTECA: break;
        case FICHA: pantalla_ficha(); break;
        case ANADIR: pantalla_anadir(); break;
        case VERSION: pantalla_version(); break;
        case FUENTES: pantalla_fuentes(); break;
        case INSTALANDO: pantalla_instalando(); break;
        case CONFIG: pantalla_config(); break;
        case ACERCA: pantalla_acerca(); break;
        }
        if (const char *cap = getenv("PCF_CAPTURA")) {   // pruebas: guardar la pantalla y salir (antes de presentar: después el búfer ya no vale)
            static int n = 0;
            if (!A.jugando.activo() && ++n == 3) {   // (con un juego en marcha se espera a que termine)
                if (SDL_Surface *s = SDL_RenderReadPixels(A.ren, nullptr)) { SDL_SaveBMP(s, cap); SDL_DestroySurface(s); }
                SDL_Quit();
                return 0;
            }
        }
        SDL_RenderPresent(A.ren);
    }
}
