// PCF2001 — APIs del sistema que PC Fútbol 2001 importa y la plataforma no tenía (ver juegos/pcf2001).
// Fichero aparte para no tocar el núcleo: diálogos de MFC (user32), WinInet y Winsock (lo online: el juego
// recibe «sin red», como un Windows 98 sin conexión), y dos menores de kernel32/ole32.
#include "ctlutil.h"   // WM_SETTEXT, WM_GETTEXT
#include "handles.h"
#include "win.h"

#include <cstdlib>
#include <random>
#include <string>

// ---------------------------------------------------------------- KERNEL32 / OLE32

STUB(ext_kernel32_dll_GetThreadPriority) { RET(0, 1); }   // THREAD_PRIORITY_NORMAL

STUB(ext_ole32_dll_CoCreateGuid) {   // (GUID*): uno al azar (versión 4)
    static std::mt19937 rng(20011204);
    u32 g = arg(0);
    for (u32 i = 0; i < 16; i += 4) wr32(g + i, rng());
    wr8(g + 7, (rd8(g + 7) & 0x0F) | 0x40);
    wr8(g + 8, (rd8(g + 8) & 0x3F) | 0x80);
    RET(0, 1);   // S_OK
}

// ---------------------------------------------------------------- USER32: diálogos (los usa MFC)

static constexpr u32 BM_GETCHECK = 0xF0, BM_SETCHECK = 0xF1;

STUB(ext_user32_dll_SendDlgItemMessageA) {   // (hDlg, id, msg, wParam, lParam)
    u32 h = wm_dlg_item(arg(0), arg(1));
    RET(h ? wm_send(h, arg(2), arg(3), arg(4)) : 0, 5);
}
STUB(ext_user32_dll_SetDlgItemTextA) {   // (hDlg, id, texto)
    u32 h = wm_dlg_item(arg(0), arg(1));
    RET(h ? (wm_send(h, ctl::WM_SETTEXT, 0, arg(2)) ? 1 : 0) : 0, 3);
}
STUB(ext_user32_dll_GetDlgItemTextA) {   // (hDlg, id, buf, n)
    u32 h = wm_dlg_item(arg(0), arg(1)), b = arg(2), n = arg(3);
    if (!h || !b || !n) { if (b && n) wr8(b, 0); RET(0, 4); }
    RET(wm_send(h, ctl::WM_GETTEXT, n, b), 4);
}
STUB(ext_user32_dll_SetDlgItemInt) {   // (hDlg, id, valor, con signo)
    u32 h = wm_dlg_item(arg(0), arg(1));
    if (!h) RET(0, 4);
    std::string s = arg(3) ? std::to_string((i32)arg(2)) : std::to_string(arg(2));
    u32 p = heap_alloc((u32)s.size() + 1, true);
    gstrcpy(p, s);
    u32 r = wm_send(h, ctl::WM_SETTEXT, 0, p) ? 1 : 0;
    heap_free(p);
    RET(r, 4);
}
STUB(ext_user32_dll_GetDlgItemInt) {   // (hDlg, id, BOOL *traducido, con signo)
    u32 h = wm_dlg_item(arg(0), arg(1)), ok = arg(2);
    std::string s = h ? wm_text(h) : "";
    char *fin = nullptr;
    long v = std::strtol(s.c_str(), &fin, 10);
    bool bien = h && fin && fin != s.c_str();
    if (ok) wr32(ok, bien ? 1 : 0);
    RET(bien ? (u32)v : 0, 4);
}
STUB(ext_user32_dll_CheckDlgButton) {   // (hDlg, id, estado)
    u32 h = wm_dlg_item(arg(0), arg(1));
    if (h) wm_send(h, BM_SETCHECK, arg(2), 0);
    RET(h ? 1 : 0, 3);
}
STUB(ext_user32_dll_CheckRadioButton) {   // (hDlg, primero, último, marcado)
    u32 d = arg(0), a = arg(1), b = arg(2), m = arg(3);
    for (u32 id = a; id <= b; id++)
        if (u32 h = wm_dlg_item(d, id)) wm_send(h, BM_SETCHECK, id == m ? 1 : 0, 0);
    RET(1, 4);
}
STUB(ext_user32_dll_IsDlgButtonChecked) {   // (hDlg, id)
    u32 h = wm_dlg_item(arg(0), arg(1));
    RET(h ? wm_send(h, BM_GETCHECK, 0, 0) : 0, 2);
}
// Siguiente control del grupo / de la tabulación: sin recorrido real, el mismo control
STUB(ext_user32_dll_GetNextDlgGroupItem) { RET(arg(1), 3); }
STUB(ext_user32_dll_GetNextDlgTabItem) { RET(arg(1), 3); }
// MapDialogRect: las unidades de diálogo con la fuente del sistema (8x16 → x·4/4, y·8/8): tal cual
STUB(ext_user32_dll_MapDialogRect) { RET(1, 2); }
STUB(ext_user32_dll_EndDialog) {   // (hDlg, resultado): los diálogos modales de MFC no se crean aquí
    plog_once("EndDialog", "EndDialog(%#x, %u): sin diálogo modal", arg(0), arg(1));
    RET(1, 2);
}
STUB(ext_user32_dll_CreateDialogIndirectParamA) {   // (hInst, plantilla, padre, proc, param): no hay plantillas de recursos
    plog_once("CreateDialogIndirect", "CreateDialogIndirectParamA: sin implementar");
    RET(0, 5);
}
STUB(ext_user32_dll_DefMDIChildProcA) { RET(0, 4); }
STUB(ext_user32_dll_GetQueueStatus) { RET(0, 1); }
STUB(ext_user32_dll_WindowFromDC) { RET(0, 1); }

// ---------------------------------------------------------------- WININET: sin conexión

STUB(ext_wininet_dll_InternetOpenA) { plog_once("InternetOpenA", "WinInet: sin conexión"); RET(0, 5); }
STUB(ext_wininet_dll_InternetOpenUrlA) { RET(0, 6); }
STUB(ext_wininet_dll_InternetReadFile) { if (arg(3)) wr32(arg(3), 0); RET(0, 4); }
STUB(ext_wininet_dll_InternetQueryDataAvailable) { if (arg(1)) wr32(arg(1), 0); RET(0, 4); }
STUB(ext_wininet_dll_InternetCloseHandle) { RET(1, 1); }

// ---------------------------------------------------------------- WSOCK32: sin red
// WSAStartup falla (WSASYSNOTREADY) y todo lo demás devuelve error con WSANOTINITIALISED, como en un Windows 98
// sin Winsock arrancado: el juego deja lo online apagado.

static constexpr u32 SOCKET_ERROR = 0xFFFFFFFFu, INVALID_SOCKET = 0xFFFFFFFFu;
STUB(ext_wsock32_dll_WSAStartup) { plog_once("WSAStartup", "Winsock: sin red"); RET(10091, 2); }   // WSASYSNOTREADY
STUB(ext_wsock32_dll_WSACleanup) { RET(0, 0); }
STUB(ext_wsock32_dll_WSAGetLastError) { RET(10093, 0); }   // WSANOTINITIALISED
STUB(ext_wsock32_dll___WSAFDIsSet) { RET(0, 2); }
STUB(ext_wsock32_dll_socket) { RET(INVALID_SOCKET, 3); }
STUB(ext_wsock32_dll_closesocket) { RET(0, 1); }
STUB(ext_wsock32_dll_accept) { RET(INVALID_SOCKET, 3); }
STUB(ext_wsock32_dll_bind) { RET(SOCKET_ERROR, 3); }
STUB(ext_wsock32_dll_connect) { RET(SOCKET_ERROR, 3); }
STUB(ext_wsock32_dll_listen) { RET(SOCKET_ERROR, 2); }
STUB(ext_wsock32_dll_shutdown) { RET(SOCKET_ERROR, 2); }
STUB(ext_wsock32_dll_recv) { RET(SOCKET_ERROR, 4); }
STUB(ext_wsock32_dll_send) { RET(SOCKET_ERROR, 4); }
STUB(ext_wsock32_dll_select) { RET(SOCKET_ERROR, 5); }
STUB(ext_wsock32_dll_setsockopt) { RET(SOCKET_ERROR, 5); }
STUB(ext_wsock32_dll_ioctlsocket) { RET(SOCKET_ERROR, 3); }
STUB(ext_wsock32_dll_getsockname) { RET(SOCKET_ERROR, 3); }
STUB(ext_wsock32_dll_gethostname) { if (arg(0) && arg(1)) gstrcpy(arg(0), "pcf2001", arg(1)); RET(SOCKET_ERROR, 2); }
STUB(ext_wsock32_dll_gethostbyname) { RET(0, 1); }
STUB(ext_wsock32_dll_inet_addr) { RET(0xFFFFFFFFu, 1); }   // INADDR_NONE
STUB(ext_wsock32_dll_htons) { u32 v = arg(0) & 0xFFFF; RET(((v & 0xFF) << 8) | (v >> 8), 1); }
STUB(ext_wsock32_dll_ntohs) { u32 v = arg(0) & 0xFFFF; RET(((v & 0xFF) << 8) | (v >> 8), 1); }
