// PCF7 — DirectInput 5 (DINPUT.DLL): lo usa PC Fútbol 7 Plus para el teclado en el partido.
//
// IDirectInputA crea dispositivos (teclado y ratón del sistema; no hay mandos): el teclado da su estado inmediato
// (GetDeviceState: 256 bytes indexados por código DIK) y, si el juego pone búfer (DIPROP_BUFFERSIZE), los cambios con
// GetDeviceData. Los códigos DIK son los de rastreo del teclado (| 0x80 para las teclas extendidas), que ya trae cada
// pulsación del host (main.cpp → dinput_key). El ratón devuelve movimiento 0 y los botones sin pulsar.
#include "plat.h"
#include "com.h"

#include <cstring>
#include <deque>
#include <mutex>

namespace {

constexpr u32 DI_OK = 0, DIERR_DEVICENOTREG = 0x80040154u, E_NOTIMPL = 0x80004001u;


const ComGuid GUID_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const ComGuid GUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

std::mutex g_mx;
bool g_dik[256];                  // estado del teclado por código DIK
struct Ev { u8 dik; bool down; u32 t, seq; };
std::deque<Ev> g_cola;            // cambios para GetDeviceData (si algún teclado tiene búfer)
u32 g_seq = 0, g_buf = 0;         // tamaño de búfer pedido (0 = sin búfer)
u32 g_vt_di = 0, g_vt_dev = 0, g_teclado = 0, g_raton = 0;

}  // namespace
u32 ticks_ms();   // kernel32.cpp
namespace {
u32 ticks() { return ticks_ms(); }

// ---- IDirectInputDevice(2)A
void d_QueryInterface() { u32 o = arg(0); com_addref(o); wr32(arg(2), o); RET(DI_OK, 3); }   // Device/Device2: el mismo objeto
void d_AddRef() { RET(com_addref(arg(0)), 1); }
void d_Release() { RET(com_release(arg(0)), 1); }
void d_GetCapabilities() {   // DIDEVCAPS: dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs…
    u32 c = arg(1), sz = rd32(c);
    std::memset(MEM + c + 4, 0, sz > 4 ? sz - 4 : 0);
    bool kb = arg(0) == g_teclado;
    wr32(c + 4, 5);                                  // DIDC_ATTACHED | DIDC_POLLEDDEVICE
    wr32(c + 8, kb ? 0x13 : 0x12);                   // DIDEVTYPE_KEYBOARD / MOUSE
    wr32(c + 12, kb ? 0 : 2); wr32(c + 16, kb ? 101 : 3);
    RET(DI_OK, 2);
}
void d_EnumObjects() { RET(DI_OK, 4); }
void d_GetProperty() { RET(E_NOTIMPL, 3); }
void d_SetProperty() {   // (rguidProp, DIPROPHEADER*): DIPROP_BUFFERSIZE = (REFGUID)1 → DIPROPDWORD.dwData
    if (arg(1) == 1 && arg(0) == g_teclado) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_buf = rd32(arg(2) + 16);
    }
    RET(DI_OK, 3);
}
void d_Ok1() { RET(DI_OK, 1); }
void d_Ok2() { RET(DI_OK, 2); }
void d_Ok3() { RET(DI_OK, 3); }
void d_GetDeviceState() {   // (cbData, lpvData)
    u32 n = arg(1), p = arg(2);
    std::memset(MEM + p, 0, n);
    if (arg(0) == g_teclado) {
        std::lock_guard<std::mutex> lk(g_mx);
        for (u32 i = 0; i < 256 && i < n; i++) if (g_dik[i]) wr8(p + i, 0x80);
    }
    RET(DI_OK, 3);
}
void d_GetDeviceData() {   // (cbObjectData, rgdod, pdwInOut, dwFlags): DIDEVICEOBJECTDATA {ofs, data, timestamp, sequence}
    u32 tam = arg(1), dod = arg(2), inout = arg(3), flags = arg(4);
    u32 max = rd32(inout), n = 0;
    if (arg(0) == g_teclado) {
        std::lock_guard<std::mutex> lk(g_mx);
        bool peek = flags & 1;   // DIGDD_PEEK
        size_t i = 0;
        while (n < max && i < g_cola.size()) {
            const Ev &e = g_cola[i];
            if (dod) {
                u32 p = dod + n * tam;
                std::memset(MEM + p, 0, tam);
                wr32(p, e.dik); wr32(p + 4, e.down ? 0x80 : 0); wr32(p + 8, e.t); wr32(p + 12, e.seq);
            }
            n++;
            i++;
        }
        if (!peek) g_cola.erase(g_cola.begin(), g_cola.begin() + (long)i);
    }
    wr32(inout, n);
    RET(DI_OK, 5);
}
void d_SetDataFormat() { RET(DI_OK, 2); }
void d_GetObjectInfo() { RET(E_NOTIMPL, 4); }
void d_GetDeviceInfo() { RET(E_NOTIMPL, 2); }
void d_Unsup2() { RET(E_NOTIMPL, 2); }
void d_Unsup3() { RET(E_NOTIMPL, 3); }
void d_Unsup4() { RET(E_NOTIMPL, 4); }
void d_Unsup5() { RET(E_NOTIMPL, 5); }

// ---- IDirectInputA
void di_QueryInterface() { u32 o = arg(0); com_addref(o); wr32(arg(2), o); RET(DI_OK, 3); }
void di_CreateDevice() {   // (rguid, LPDIRECTINPUTDEVICE*, pUnkOuter)
    u32 g = arg(1), out = arg(2);
    u32 *slot = com_guid_eq(g, GUID_SysKeyboard) ? &g_teclado : com_guid_eq(g, GUID_SysMouse) ? &g_raton : nullptr;
    if (!slot) { wr32(out, 0); RET(DIERR_DEVICENOTREG, 4); }
    if (!*slot) *slot = com_object(g_vt_dev); else com_addref(*slot);
    wr32(out, *slot);
    plog("DirectInput: CreateDevice(%s) -> 0x%x", slot == &g_teclado ? "teclado" : "ratón", *slot);
    RET(DI_OK, 4);
}
void di_EnumDevices() { RET(DI_OK, 5); }   // sin mandos
void di_GetDeviceStatus() { RET(com_guid_eq(arg(1), GUID_SysKeyboard) || com_guid_eq(arg(1), GUID_SysMouse) ? DI_OK : 1u, 2); }

void init() {
    if (g_vt_di) return;
    g_vt_di = com_vtbl({di_QueryInterface, d_AddRef, d_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
                        d_Ok3, d_Ok3});
    // IDirectInputDevice2A: los 18 de IDirectInputDevice y CreateEffect… SendDeviceData
    g_vt_dev = com_vtbl({d_QueryInterface, d_AddRef, d_Release, d_GetCapabilities, d_EnumObjects, d_GetProperty,
                         d_SetProperty, d_Ok1, d_Ok1, d_GetDeviceState, d_GetDeviceData, d_SetDataFormat, d_Ok2, d_Ok3,
                         d_GetObjectInfo, d_GetDeviceInfo, d_Ok3, d_Ok3,
                         d_Unsup5, d_Unsup5, d_Unsup3, d_Unsup2, d_Unsup2, d_Unsup5, d_Unsup3, d_Ok1, d_Unsup5});
}

}  // namespace

void dinput_key(u32 dik, bool down) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_dik[dik & 0xFF] == down) return;   // repeticiones del sistema: DirectInput no las da
    g_dik[dik & 0xFF] = down;
    if (g_buf) {
        g_cola.push_back({(u8)dik, down, ticks(), ++g_seq});
        while (g_cola.size() > g_buf) g_cola.pop_front();
    }
}

// DirectInputCreateA(hinst, dwVersion, LPDIRECTINPUTA*, pUnkOuter)
STUB(ext_dinput_dll_DirectInputCreateA) {
    init();
    u32 o = com_object(g_vt_di);
    wr32(arg(2), o);
    plog("DirectInputCreateA(versión 0x%x) -> 0x%x", arg(1), o);
    RET(DI_OK, 4);
}
