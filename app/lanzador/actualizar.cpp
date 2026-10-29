// lanzador — actualizaciones (ver actualizar.h). La consulta y la bajada van con curl, que traen macOS, Windows 10 (1803
// o posterior) y casi todos los Linux: así no hace falta una biblioteca de TLS. PCF_ACT_API cambia la dirección de la
// consulta (pruebas: un file:// con una release de mentira).
#include "actualizar.h"
#include "json.h"
#ifdef _WIN32
#include "zip.h"
#endif

#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace actualizar {

namespace {
const char *API = "https://api.github.com/repos/nuvolacat/pcfsite-source/releases/latest";
#ifdef __APPLE__
const char *EQUIPO_APPLE = "V9Q7Y5J9R9";            // Team ID del Developer ID con que se firman las de macOS
#endif
#ifdef _WIN32
const char *TITULAR = "CN=Jordi Ruiz Bover";        // sujeto del certificado con que se firman las de Windows
#endif

// el paquete de esta plataforma, por el final de su nombre ("" en Linux: no se publica)
const char *sufijo() {
#if defined(__APPLE__) && defined(__aarch64__)
    return "-macos-apple-silicon.dmg";
#elif defined(__APPLE__)
    return "-macos-intel.dmg";
#elif defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
    return "-windows-arm64.zip";
#elif defined(_WIN32)
    return "-windows-x64.zip";
#else
    return "";
#endif
}

std::string curl() {
#ifdef _WIN32
    char w[MAX_PATH];
    UINT n = GetSystemDirectoryA(w, sizeof w);   // el curl.exe de Windows (System32), no uno cualquiera del PATH
    return n ? std::string(w) + "\\curl.exe" : "curl.exe";
#else
    return "curl";
#endif
}

void falla(Estado &e, const std::string &m) {
    { std::lock_guard<std::mutex> lk(e.mx); e.error = m; }
    e.fase = FALLO;
}

std::string dir_trabajo() { return util::dir_base() + "/actualizacion"; }

std::string recorta(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

#ifdef _WIN32
std::string ps_cadena(const std::string &s) {   // 'texto' con las comillas simples dobladas
    std::string o = "'";
    for (char ch : s) { if (ch == '\'') o += '\''; o += ch; }
    return o + "'";
}
int powershell(const std::string &orden, std::string &salida) {
    return util::ejecutar({"powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", orden}, salida);
}
// la carpeta de la app: la del ejecutable
std::string app_actual() { return util::carpeta_de(util::exe_propio()); }
#else
std::string sh_cadena(const std::string &s) {   // 'texto' para sh
    std::string o = "'";
    for (char ch : s) { if (ch == '\'') o += "'\\''"; else o += ch; }
    return o + "'";
}
#ifdef __APPLE__
// el .app: Contents/MacOS/lanzador → tres carpetas más arriba ("" si no se está dentro de uno: desarrollo)
std::string app_actual() {
    std::string a = util::carpeta_de(util::carpeta_de(util::dir_ejecutables()));
    return util::termina_en(a, ".app") ? a : "";
}
#endif
#endif
}  // namespace

uint64_t Estado::bajado() {
    std::string p;
    { std::lock_guard<std::mutex> lk(mx); p = parcial; }
    return p.empty() || !util::existe(p) ? 0 : util::tam(p);
}

bool mas_nueva(const std::string &a, const std::string &b) {
    auto partes = [](const std::string &s) {
        std::vector<long> v;
        long n = -1;
        for (char ch : s) {
            if (ch >= '0' && ch <= '9') n = (n < 0 ? 0 : n * 10) + (ch - '0');
            else if (n >= 0) { v.push_back(n); n = -1; }
        }
        if (n >= 0) v.push_back(n);
        return v;
    };
    std::vector<long> x = partes(a), y = partes(b);
    if (x.empty() || y.empty()) return false;   // («desarrollo»: no se compara)
    return y > x;
}

void buscar(Estado &e, const std::string &actual) {
    e.fase = BUSCANDO;
    const char *api = getenv("PCF_ACT_API");
    std::string salida;
    int rc = util::ejecutar({curl(), "-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json", "-H", "User-Agent: PCFSite-Source",
                             api && *api ? api : API}, salida);
    Json j;
    if (rc != 0 || !Json::parsea(salida, j) || j["tag_name"].str().empty()) {
        falla(e, rc == -1 ? "no se encuentra curl" : "no se ha podido consultar GitHub (¿sin conexión?)");
        return;
    }
    std::string v = j["tag_name"].str();
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
    {
        std::lock_guard<std::mutex> lk(e.mx);
        e.version = v;
        e.pagina = j["html_url"].str();
        e.url.clear(); e.sha256.clear(); e.nombre.clear();
        std::string suf = sufijo();
        for (size_t k = 0; !suf.empty() && k < j["assets"].size(); k++) {
            const Json &a = j["assets"][k];
            if (!util::termina_en(a["name"].str(), suf)) continue;
            e.nombre = a["name"].str();
            e.url = a["browser_download_url"].str();
            std::string d = a["digest"].str();   // «sha256:…»
            if (d.rfind("sha256:", 0) == 0) e.sha256 = d.substr(7);
            e.total = (uint64_t)a["size"].num();
        }
    }
    e.fase = mas_nueva(actual, v) ? HAY : AL_DIA;
}

void preparar(Estado &e) {
    std::string url, nombre, sha;
    { std::lock_guard<std::mutex> lk(e.mx); url = e.url; nombre = e.nombre; sha = e.sha256; e.error.clear(); }
    if (url.empty()) { falla(e, "no hay paquete para este sistema"); return; }
    std::string d = dir_trabajo();
    util::borrar_arbol(d);
    util::mkdirs(d);
    std::string f = d + "/" + nombre;
    { std::lock_guard<std::mutex> lk(e.mx); e.parcial = f + ".parte"; }
    e.fase = BAJANDO;
    std::string salida;
    if (util::ejecutar({curl(), "-fsSL", "--retry", "3", "-H", "User-Agent: PCFSite-Source", "-o", f + ".parte", url}, salida) != 0 ||
        std::rename((f + ".parte").c_str(), f.c_str()) != 0) {
        falla(e, "no se ha podido bajar " + nombre);
        return;
    }
    e.fase = COMPROBANDO;
    if (sha.empty() || util::minusculas(util::sha256_fichero(f)) != util::minusculas(sha)) { falla(e, "el paquete bajado no es el que publica GitHub (SHA-256 distinto)"); return; }
#ifdef __APPLE__
    if (app_actual().empty()) { falla(e, "esta copia no es una app (.app): actualízala a mano"); return; }
    std::string mp = d + "/montaje";
    util::mkdirs(mp);
    if (util::ejecutar({"hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mp, f}, salida) != 0) { falla(e, "no se puede abrir el .dmg"); return; }
    std::string app;
    for (const std::string &n : util::listar(mp)) if (util::termina_en(n, ".app")) app = mp + "/" + n;
    std::string firma, gatekeeper;
    bool ok = !app.empty() && util::ejecutar({"codesign", "--verify", "--deep", "--strict", app}, salida) == 0 &&
              util::ejecutar({"codesign", "-dv", app}, firma) == 0 && firma.find(std::string("TeamIdentifier=") + EQUIPO_APPLE) != std::string::npos &&
              util::ejecutar({"spctl", "--assess", "--type", "execute", app}, gatekeeper) == 0;   // (notarizada)
    std::string nueva = d + "/" + util::nombre_base(app);
    if (ok) ok = util::ejecutar({"ditto", app, nueva}, salida) == 0;
    util::ejecutar({"hdiutil", "detach", "-force", mp}, salida);
    ::unlink(f.c_str());
    if (!ok) { falla(e, "la app bajada no lleva la firma de Jordi Ruiz Bover notarizada por Apple: no se instala"); return; }
    { std::lock_guard<std::mutex> lk(e.mx); e.preparada = nueva; }
#elif defined(_WIN32)
    std::string error;
    if (!zip_extraer(f, d + "/nueva", error)) { falla(e, "no se puede descomprimir el paquete: " + error); return; }
    ::unlink(f.c_str());
    std::string nueva;
    for (const std::string &n : util::listar(d + "/nueva")) if (util::existe(d + "/nueva/" + n + "/PCFSite - Source.exe")) nueva = d + "/nueva/" + n;
    if (nueva.empty()) { falla(e, "el paquete no trae PCFSite - Source.exe"); return; }
    std::string exe = util::nativa(nueva + "/PCFSite - Source.exe");
    powershell("$s = Get-AuthenticodeSignature -LiteralPath " + ps_cadena(exe) + "; \"$($s.Status)|$($s.SignerCertificate.Subject)\"", salida);
    salida = recorta(salida);
    if (salida.rfind("Valid|", 0) != 0 || salida.find(TITULAR) == std::string::npos) { falla(e, "el ejecutable bajado no lleva la firma de Jordi Ruiz Bover: no se instala"); return; }
    { std::lock_guard<std::mutex> lk(e.mx); e.preparada = nueva; }
#else
    falla(e, "en Linux la app se actualiza compilándola desde el código");
    return;
#endif
    e.fase = LISTA;
}

bool aplicar(Estado &e) {
    std::string nueva;
    { std::lock_guard<std::mutex> lk(e.mx); nueva = e.preparada; }
    if (e.fase != LISTA || nueva.empty()) return false;
    std::string d = dir_trabajo();
#ifdef __APPLE__
    std::string vieja = app_actual();
    if (vieja.empty() || access(util::carpeta_de(vieja).c_str(), W_OK) != 0) { falla(e, "no se puede escribir en " + util::carpeta_de(vieja) + ": actualízala a mano"); return false; }
    // cuando este proceso termine: la vieja aparte, la nueva en su sitio (si algo falla, vuelve la vieja) y se abre
    std::string s = "#!/bin/sh\n"
                    "while kill -0 " + std::to_string(getpid()) + " 2>/dev/null; do sleep 0.3; done\n"
                    "V=" + sh_cadena(vieja) + "; N=" + sh_cadena(nueva) + "\n"
                    "rm -rf \"$V.vieja\"\n"
                    "if mv \"$V\" \"$V.vieja\"; then\n"
                    "  if mv \"$N\" \"$V\"; then rm -rf \"$V.vieja\"; else mv \"$V.vieja\" \"$V\"; fi\n"
                    "fi\n"
                    "open \"$V\"\n";
    std::string guion = d + "/aplicar.sh";
    if (!util::escribir(guion, s)) { falla(e, "no se puede escribir " + guion); return false; }
    util::Proceso p;
    if (!util::lanzar("/bin/sh", {guion}, p)) { falla(e, "no se puede lanzar el ayudante"); return false; }
    return true;
#elif defined(_WIN32)
    std::string vieja = app_actual();
    std::string prueba = vieja + "/.escribible";
    if (!util::escribir(prueba, "")) { falla(e, "no se puede escribir en " + util::nativa(vieja) + " (¿Archivos de programa?): actualízala a mano"); return false; }
    ::unlink(prueba.c_str());
    // cuando este proceso termine: los ficheros nuevos encima de los viejos (robocopy reintenta si alguno sigue
    // abierto) y se abre la app; los ajustes, juegos y partidas no están aquí
    std::string s = "Start-Transcript -Path " + ps_cadena(util::nativa(d + "/aplicar.log")) + " | Out-Null\r\n"
                    "Wait-Process -Id " + std::to_string(GetCurrentProcessId()) + " -Timeout 120 -ErrorAction SilentlyContinue\r\n"
                    "Start-Sleep -Milliseconds 500\r\n"
                    "robocopy " + ps_cadena(util::nativa(nueva)) + " " + ps_cadena(util::nativa(vieja)) + " /E /R:10 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null\r\n"
                    "Start-Process -FilePath " + ps_cadena(util::nativa(vieja + "/PCFSite - Source.exe")) + "\r\n"
                    "Remove-Item -Recurse -Force -LiteralPath " + ps_cadena(util::nativa(d + "/nueva")) + " -ErrorAction SilentlyContinue\r\n"
                    "Stop-Transcript | Out-Null\r\n";
    std::string guion = d + "/aplicar.ps1";
    if (!util::escribir(guion, "\xEF\xBB\xBF" + s)) { falla(e, "no se puede escribir " + guion); return false; }   // (con BOM: rutas con acentos)
    char w[MAX_PATH];
    UINT n = GetSystemDirectoryA(w, sizeof w);
    std::string ps = std::string(n ? w : "C:\\Windows\\System32") + "\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::string cmd = "\"" + ps + "\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + util::nativa(guion) + "\"";
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // sin ventana y fuera del grupo de procesos (job) de este, si lo hay: si no, al cerrarse la app se lo llevaría por delante
    if (!CreateProcessA(ps.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi) &&
        !CreateProcessA(ps.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { falla(e, "no se puede lanzar el ayudante"); return false; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
#else
    falla(e, "en Linux la app se actualiza compilándola desde el código");
    return false;
#endif
}

}  // namespace actualizar
