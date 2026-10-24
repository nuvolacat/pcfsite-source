// PCF7 — aplicar actualizaciones RTPatch (las 13 oficiales de PC Fútbol 2001) con patchw32.dll traducida
// (juegos/pcfpatch). Con PCF7_RTPATCH=<ruta Windows del .RTP>, en vez de arrancar el programa se llama al export
// RTPatchApply32 de la DLL con un callback propio (el mismo protocolo que atiende el diálogo PCFPATCH.exe de Dinamic:
// 2 aviso, 3/4 error, 5 progreso del fichero, 6 total de ficheros, 7 fichero actual, 8 fichero hecho), con el
// directorio actual en la instalación (C:\PCF2001). El resultado se registra; el proceso devuelve 0 si todo ha ido bien.
// Aquí van también las APIs que solo pide el parcheador (MoveFileEx, AreFileApisANSI, perfiles en W…).
#include "plat.h"

#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

extern "C" u32 gp_lookup(u32 namep, u32 modbase);
void set_last_error(u32 e);

namespace {

u32 g_resp = 0;   // respuesta a los avisos: una cadena vacía (= seguir)
int g_errores = 0, g_hechos = 0, g_total = 0;
std::string g_actual;

// CALLBACK (stdcall, 2 argumentos): devuelve un puntero (seguir) o 0 (parar)
void rtpatch_cb() {
    u32 id = arg(0), msg = arg(1);
    switch (id) {
    case 5: break;   // progreso dentro del fichero (0..0x8000)
    case 6: g_total = (int)rd32(msg); plog("rtpatch: %d ficheros que actualizar", g_total); break;
    case 7: g_actual = gstr(msg); plog("rtpatch: %s", g_actual.c_str()); break;
    case 8: g_hechos++; break;
    case 2: plog("rtpatch: aviso: %s", gstr(msg, 1024).c_str()); break;
    case 3: {   // mensaje de error; el diálogo original solo paraba en ept0015 y ept0036 (fichero que falta)
        std::string m = gstr(msg, 1024);
        plog("rtpatch: error: %s", m.c_str());
        g_errores++;
        if (m.find("ept0015") != std::string::npos || m.find("ept0036") != std::string::npos) RET(0, 2);
        break;
    }
    case 4: plog("rtpatch: error fatal: %s", gstr(msg, 1024).c_str()); g_errores++; RET(0, 2);
    default: plog("rtpatch: mensaje %u: %s", id, msg ? gstr(msg, 512).c_str() : ""); break;
    }
    RET(g_resp, 2);
}

std::string gwstr(u32 p) {   // UTF-16 del invitado → cadena de 8 bits (cp1252: lo que no cabe, '?')
    std::string s;
    for (u32 i = 0;; i += 2) { u32 c = rd16(p + i); if (!c) break; s.push_back(c < 256 ? (char)c : '?'); }
    return s;
}

const u64 EPOCH_DIFF = 116444736000000000ull;   // 1601 → 1970 en unidades de 100 ns

}  // namespace

int rtpatch_aplicar(const char *rtp) {
    u32 nombre = heap_alloc(32);
    gstrcpy(nombre, "RTPatchApply32@12");
    u32 fn = gp_lookup(nombre, 0);
    if (!fn) { plog("rtpatch: patchw32.dll no exporta RTPatchApply32"); return 2; }
    g_resp = heap_alloc(16, true);
    u32 cmd = heap_alloc(1024);
    gstrcpy(cmd, "\"" + std::string(rtp) + "\"", 1024);
    plog("rtpatch: aplicando %s en %s", rtp, g_cwd.c_str());
    u32 r = guest_call(fn, {cmd, host_fn(rtpatch_cb), 0});
    plog("rtpatch: RTPatchApply32 = %u; %d de %d ficheros, %d errores", r, g_hechos, g_total, g_errores);
    return (g_errores || r) ? 1 : 0;
}

// ---------------------------------------------------------------- APIs que solo pide el parcheador

STUB(ext_kernel32_dll_MoveFileExA) {   // (src, dst, flags): MOVEFILE_REPLACE_EXISTING=1; dst nulo = borrar
    std::string a = host_path_w(gstr(arg(0)));
    if (!arg(1)) { RET(::unlink(a.c_str()) == 0, 3); }
    std::string b = host_path_w(gstr(arg(1)));
    if (!(arg(2) & 1) && ::access(b.c_str(), F_OK) == 0) { set_last_error(183); RET(0, 3); }
    if (pcf_rename(a.c_str(), b.c_str()) != 0) { set_last_error(errno == ENOENT ? 2 : 5); RET(0, 3); }
    RET(1, 3);
}
STUB(ext_kernel32_dll_MoveFileExW) {
    std::string a = host_path_w(gwstr(arg(0)));
    if (!arg(1)) { RET(::unlink(a.c_str()) == 0, 3); }
    std::string b = host_path_w(gwstr(arg(1)));
    if (!(arg(2) & 1) && ::access(b.c_str(), F_OK) == 0) { set_last_error(183); RET(0, 3); }
    if (pcf_rename(a.c_str(), b.c_str()) != 0) { set_last_error(errno == ENOENT ? 2 : 5); RET(0, 3); }
    RET(1, 3);
}
STUB(ext_kernel32_dll_AreFileApisANSI) { RET(1, 0); }
STUB(ext_kernel32_dll_SetFileApisToANSI) { RET(0, 0); }
STUB(ext_kernel32_dll_SetFileApisToOEM) { RET(0, 0); }
STUB(ext_kernel32_dll_DosDateTimeToFileTime) {   // (fecha DOS, hora DOS, FILETIME*)
    u32 d = arg(0) & 0xFFFF, h = arg(1) & 0xFFFF;
    std::tm t{};
    t.tm_year = (int)((d >> 9) & 0x7F) + 80; t.tm_mon = (int)((d >> 5) & 0xF) - 1; t.tm_mday = (int)(d & 0x1F);
    t.tm_hour = (int)(h >> 11); t.tm_min = (int)((h >> 5) & 0x3F); t.tm_sec = (int)(h & 0x1F) * 2;
    std::time_t s = timegm(&t);
    wr64(arg(2), (u64)s * 10000000ull + EPOCH_DIFF);
    RET(1, 3);
}
STUB(ext_kernel32_dll_GetShortPathNameW) {   // (ruta, buf, cch): la misma ruta
    u32 n = 0;
    for (;; n++) { u32 c = rd16(arg(0) + 2 * n); if (!c) break; }
    if (arg(2) > n) { for (u32 i = 0; i <= n; i++) wr16(arg(1) + 2 * i, rd16(arg(0) + 2 * i)); RET(n, 3); }
    RET(n + 1, 3);
}
// perfiles .ini en Unicode: no hay (el parcheador los consulta para opciones que no usamos)
STUB(ext_kernel32_dll_GetPrivateProfileStringW) {   // (app, key, def, buf, cch, file)
    u32 def = arg(2), buf = arg(3), cch = arg(4), n = 0;
    if (def && cch) { for (; n + 1 < cch; n++) { u32 c = rd16(def + 2 * n); wr16(buf + 2 * n, c); if (!c) break; } wr16(buf + 2 * n, 0); }
    else if (buf && cch) wr16(buf, 0);
    RET(n, 6);
}
STUB(ext_kernel32_dll_GetProfileStringW) {   // (app, key, def, buf, cch)
    u32 def = arg(2), buf = arg(3), cch = arg(4), n = 0;
    if (def && cch) { for (; n + 1 < cch; n++) { u32 c = rd16(def + 2 * n); wr16(buf + 2 * n, c); if (!c) break; } wr16(buf + 2 * n, 0); }
    else if (buf && cch) wr16(buf, 0);
    RET(n, 5);
}
STUB(ext_kernel32_dll_WritePrivateProfileStringW) { RET(1, 4); }
STUB(ext_kernel32_dll_WriteProfileStringW) { RET(1, 3); }
STUB(ext_advapi32_dll_GetFileSecurityW) { set_last_error(120); RET(0, 5); }   // ERROR_CALL_NOT_IMPLEMENTED (Windows 98 tampoco lo tiene)
STUB(ext_advapi32_dll_SetFileSecurityW) { set_last_error(120); RET(0, 3); }
