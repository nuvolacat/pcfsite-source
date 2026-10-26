// PCF7 — arranque en el navegador (Emscripten). Solo se compila para la web.
//
// Los datos del juego (instalación, CD, disco C, mem.bin, fuentes) no se descargan de una vez: se monta en
// /pcf7/ro un sistema de ficheros de WasmFS que los pide al servidor por trozos (peticiones Range) a medida que el
// juego los lee. El árbol se crea a partir de datos/indice.txt (lo genera tools/web_datos.py): una línea por
// entrada, «d ruta» o «f ruta<TAB>fecha». Lo que el juego escribe va a /pcf7/rw (memoria), ver vfs.cpp.
#ifdef __EMSCRIPTEN__
#include "plat.h"

#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr uint32_t CHUNK = 512 * 1024;   // tamaño de cada petición al servidor

const std::string RW = "/pcf7/rw/";

void mkdirs(const std::string &p) {
    for (size_t k = p.find('/', 1); k != std::string::npos; k = p.find('/', k + 1)) ::mkdir(p.substr(0, k).c_str(), 0755);
}

// ---- partidas: lo que cambia en /pcf7/rw se manda a la página (web/inicio.js), que lo guarda en el navegador
// (IndexedDB) y en la cuenta. Se manda cuando nadie lo tiene abierto para escribir y lleva un rato sin cambiar.
std::mutex g_mx;
std::map<std::string, double> g_sucios;   // ruta del host → cuándo cambió (ms)
std::map<std::string, int> g_abiertos;    // ruta del host → handles escribiendo

// Las partidas guardadas (Module.pcf7Restaurar, las prepara inicio.js antes de arrancar) a /pcf7/rw.
void restaura() {
    // inicio.js las lee del navegador y de la cuenta mientras se carga el programa; se espera a que acabe
    for (int i = 0; i < 3000 && !MAIN_THREAD_EM_ASM_INT({ return Module.pcf7PartidasListas ? 1 : 0; }); i++) ::usleep(10000);
    int n = MAIN_THREAD_EM_ASM_INT({ return (Module.pcf7Restaurar || []).length; });
    unsigned bien = 0;
    for (int i = 0; i < n; i++) {
        char *ruta = (char *)MAIN_THREAD_EM_ASM_PTR({ return stringToNewUTF8(Module.pcf7Restaurar[$0].ruta); }, i);
        std::string rel = ruta;
        std::free(ruta);
        if (rel.empty() || rel.find("..") != std::string::npos) continue;
        int len = MAIN_THREAD_EM_ASM_INT({ return Module.pcf7Restaurar[$0].datos.length; }, i);
        std::vector<char> buf(len > 0 ? len : 1);
        MAIN_THREAD_EM_ASM({ HEAPU8.set(Module.pcf7Restaurar[$0].datos, $1); }, i, buf.data());
        std::string p = RW + rel;
        mkdirs(p);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(buf.data(), len);
        if (out) bien++;
    }
    MAIN_THREAD_EM_ASM({ Module.pcf7Restaurar = null; });
    if (n) plog("web: %u de %d ficheros de partidas restaurados", bien, n);
}

bool touch(const std::string &p) {
    int fd = ::open(p.c_str(), O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

}  // namespace

EM_JS_DEPS(pcf7_partidas, "$stringToNewUTF8,$UTF8ToString");

void web_sucio(const std::string &hostpath) {
    if (hostpath.compare(0, RW.size(), RW) != 0) return;
    std::lock_guard<std::mutex> lk(g_mx);
    g_sucios[hostpath] = emscripten_get_now();
}

void web_abierto(const std::string &hostpath, int d) {
    if (hostpath.compare(0, RW.size(), RW) != 0) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if ((g_abiertos[hostpath] += d) <= 0) g_abiertos.erase(hostpath);
    g_sucios[hostpath] = emscripten_get_now();
}

// Hilo principal (cada fotograma): manda a la página lo que ya se puede guardar.
void web_persistir(bool todo) {
    static double ultimo = 0;
    double ahora = emscripten_get_now();
    if (!todo && ahora - ultimo < 250) return;
    ultimo = ahora;
    std::vector<std::string> listos;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto it = g_sucios.begin(); it != g_sucios.end();) {
            if (todo || (ahora - it->second >= 400 && !g_abiertos.count(it->first))) { listos.push_back(it->first); it = g_sucios.erase(it); }
            else ++it;
        }
    }
    for (const std::string &p : listos) {
        std::string rel = p.substr(RW.size());
        if (rel.compare(0, 15, "c/WINDOWS/TEMP/") == 0) continue;   // temporales de Windows: no son partidas
        struct stat st;
        if (::stat(p.c_str(), &st) != 0) {   // borrado (o renombrado): fuera también de lo guardado
            EM_ASM({ Module.pcf7Borrar && Module.pcf7Borrar(UTF8ToString($0)); }, rel.c_str());
            continue;
        }
        if (S_ISDIR(st.st_mode)) continue;
        std::ifstream in(p, std::ios::binary);
        std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.eof() && in.fail()) continue;
        EM_ASM({ Module.pcf7Guardar && Module.pcf7Guardar(UTF8ToString($0), HEAPU8.slice($1, $1 + $2)); },
               rel.c_str(), buf.data(), (int)buf.size());
    }
}

// Se llama desde el hilo del juego antes de cargar nada (el backend de fetch no puede crearse en el hilo principal
// del navegador). base = URL de los datos, relativa a la página.
bool web_mount(const char *base) {
    backend_t fb = wasmfs_create_fetch_backend(base, CHUNK);
    ::mkdir("/pcf7", 0755);
    if (wasmfs_create_directory("/pcf7/ro", 0755, fb) != 0) { plog("web: no se puede montar /pcf7/ro"); return false; }
    ::mkdir("/pcf7/rw", 0755);
    restaura();

    touch("/pcf7/ro/indice.txt");
    std::ifstream in("/pcf7/ro/indice.txt");
    if (!in) { plog("web: no se puede leer %s/indice.txt", base); return false; }
    std::string line;
    unsigned nd = 0, nf = 0, mal = 0;
    while (std::getline(in, line)) {
        if (line.size() < 3 || line[1] != ' ') continue;
        std::string p = "/pcf7/ro/" + line.substr(2);
        if (line[0] == 'd') { if (::mkdir(p.c_str(), 0755) == 0) nd++; else mal++; }
        else if (line[0] == 'f') {   // «f ruta<TAB>fecha»: la fecha real del fichero (el juego la mira)
            size_t tab = p.find('\t');
            long fecha = tab == std::string::npos ? 0 : std::atol(p.c_str() + tab + 1);
            if (tab != std::string::npos) p.resize(tab);
            if (!touch(p)) { mal++; continue; }
            nf++;
            if (fecha) { struct timespec ts[2] = {{fecha, 0}, {fecha, 0}}; ::utimensat(AT_FDCWD, p.c_str(), ts, 0); }
        }
    }
    plog("web: datos en %s (%u carpetas, %u ficheros%s)", base, nd, nf, mal ? ", con errores" : "");

    // el registro de Windows de partida (lo que dejaba el instalador) se copia a la capa escribible
    std::ifstream r("/pcf7/ro/registro.txt", std::ios::binary);
    struct stat st;
    if (r && ::stat("/pcf7/rw/registro.txt", &st) != 0) {   // si no venía en las partidas
        std::ofstream w("/pcf7/rw/registro.txt", std::ios::binary | std::ios::trunc);
        w << r.rdbuf();
    }
    return true;
}
#endif
