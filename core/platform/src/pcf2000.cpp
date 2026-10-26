// PCF2000 — APIs del sistema que PC Fútbol 2000 importa y la plataforma no tenía (ver juegos/pcf2000).
// Fichero aparte para no tocar el núcleo: OLEAUT32 (BSTR/VARIANT del panel de ayuda), kernel32 menores y
// WINMM (mmio de escritura y temporizadores multimedia de d2s2).
#include "handles.h"

#include <unistd.h>

// ---------------------------------------------------------------- OLEAUT32

// BSTR: 4 bytes de longitud (en bytes) y después la cadena UTF-16 con su cero final; el puntero apunta a la cadena.
static u32 bstr_alloc(u32 src, u32 nchars) {
    u32 b = heap_alloc(4 + 2 * nchars + 2, true);
    wr32(b, 2 * nchars);
    for (u32 i = 0; i < nchars; i++) wr16(b + 4 + 2 * i, src ? rd16(src + 2 * i) : 0);
    return b + 4;
}
static u32 wlen(u32 p) { u32 n = 0; if (p) while (rd16(p + 2 * n)) n++; return n; }
STUB(ext_oleaut32_dll_SysAllocString) { u32 p = arg(0); RET(p ? bstr_alloc(p, wlen(p)) : 0, 1); }
STUB(ext_oleaut32_dll_SysAllocStringLen) { RET(bstr_alloc(arg(0), arg(1)), 2); }
STUB(ext_oleaut32_dll_SysAllocStringByteLen) {   // (char*, bytes): BSTR de bytes
    u32 src = arg(0), n = arg(1);
    u32 b = heap_alloc(4 + n + 2, true);
    wr32(b, n);
    if (src) std::memcpy(MEM + b + 4, MEM + src, n);
    RET(b + 4, 2);
}
STUB(ext_oleaut32_dll_SysReAllocString) {   // (BSTR*, wchar*)
    u32 pb = arg(0), src = arg(1);
    if (rd32(pb)) heap_free(rd32(pb) - 4);
    wr32(pb, src ? bstr_alloc(src, wlen(src)) : 0);
    RET(1, 2);
}
STUB(ext_oleaut32_dll_SysReAllocStringLen) {
    u32 pb = arg(0), src = arg(1), n = arg(2);
    u32 nuevo = bstr_alloc(src, n);
    if (rd32(pb)) heap_free(rd32(pb) - 4);
    wr32(pb, nuevo);
    RET(1, 3);
}
STUB(ext_oleaut32_dll_SysFreeString) { if (arg(0)) heap_free(arg(0) - 4); RET(0, 1); }
STUB(ext_oleaut32_dll_SysStringLen) { RET(arg(0) ? rd32(arg(0) - 4) / 2 : 0, 1); }
STUB(ext_oleaut32_dll_SysStringByteLen) { RET(arg(0) ? rd32(arg(0) - 4) : 0, 1); }
// VARIANT (16 B): vt (u16), reservados, valor en +8
STUB(ext_oleaut32_dll_VariantInit) { std::memset(MEM + arg(0), 0, 16); RET(0, 1); }
STUB(ext_oleaut32_dll_VariantClear) {
    u32 v = arg(0);
    if (rd16(v) == 8 && rd32(v + 8)) heap_free(rd32(v + 8) - 4);   // VT_BSTR
    std::memset(MEM + v, 0, 16);
    RET(0, 1);
}
STUB(ext_oleaut32_dll_VariantCopy) {   // (dst, src)
    u32 d = arg(0), s = arg(1);
    if (rd16(d) == 8 && rd32(d + 8)) heap_free(rd32(d + 8) - 4);
    std::memcpy(MEM + d, MEM + s, 16);
    if (rd16(s) == 8 && rd32(s + 8)) wr32(d + 8, bstr_alloc(rd32(s + 8), rd32(rd32(s + 8) - 4) / 2));
    RET(0, 2);
}
STUB(ext_oleaut32_dll_VariantChangeType) {   // (dst, src, flags, vt): solo cuando ya es del tipo pedido
    u32 d = arg(0), s = arg(1), vt = arg(3) & 0xFFFF;
    if (rd16(s) == vt) { if (d != s) std::memcpy(MEM + d, MEM + s, 16); RET(0, 4); }
    plog_once("vct" + std::to_string(vt), "VariantChangeType: de %u a %u sin implementar", rd16(s), vt);
    RET(0x80020005u, 4);   // DISP_E_TYPEMISMATCH
}

// ---------------------------------------------------------------- KERNEL32

STUB(ext_kernel32_dll_GetDriveTypeW) {
    u32 p = arg(0);
    char d = p ? (char)std::toupper((int)(rd16(p) & 0xFF)) : 'C';
    RET(d == 'C' ? 3u : d == 'D' ? 5u : 1u, 1);    // FIXED / CDROM / NO_ROOT_DIR
}
STUB(ext_kernel32_dll_GlobalFindAtomA) { RET(0, 1); }

// ---------------------------------------------------------------- WINMM: mmio de escritura
// HMMIO = handle de un KFile (misc.cpp). MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags.

STUB(ext_winmm_dll_mmioGetInfo) {   // (h, MMIOINFO*, flags): solo lo que se mira: lDiskOffset
    auto f = handles().get<KFile>(arg(0));
    u32 info = arg(1);
    if (!f) RET(256 + 5, 3);   // MMIOERR_CANNOTREAD
    std::memset(MEM + info, 0, 72);
    wr32(info + 48, (u32)::lseek(f->fd, 0, SEEK_CUR));   // lDiskOffset
    RET(0, 3);
}
STUB(ext_winmm_dll_mmioWrite) {
    auto f = handles().get<KFile>(arg(0));
    if (!f) RET(0xFFFFFFFFu, 3);
    ssize_t r = ::write(f->fd, MEM + arg(1), arg(2));
    RET(r < 0 ? 0xFFFFFFFFu : (u32)r, 3);
}
STUB(ext_winmm_dll_mmioCreateChunk) {   // (h, MMCKINFO*, flags): escribe la cabecera del trozo (RIFF/LIST con tipo)
    auto f = handles().get<KFile>(arg(0));
    u32 ck = arg(1), flags = arg(2);
    if (!f) RET(256 + 6, 3);   // MMIOERR_CANNOTWRITE
    u32 id = rd32(ck), size = rd32(ck + 4);
    bool riff = flags & 0x20, list = flags & 0x40;   // MMIO_CREATERIFF / MMIO_CREATELIST
    if (riff) id = 0x46464952u;   // 'RIFF'
    if (list) id = 0x5453494Cu;   // 'LIST'
    u8 h[12];
    std::memcpy(h, &id, 4); std::memcpy(h + 4, &size, 4);
    u32 n = 8;
    if (riff || list) { u32 t = rd32(ck + 8); std::memcpy(h + 8, &t, 4); n = 12; }
    off_t pos = ::lseek(f->fd, 0, SEEK_CUR);
    if (::write(f->fd, h, n) != (ssize_t)n) RET(256 + 6, 3);
    wr32(ck, id); wr32(ck + 12, (u32)pos + 8); wr32(ck + 16, 1);   // dwDataOffset, MMIO_DIRTY
    RET(0, 3);
}

// ---------------------------------------------------------------- WINMM: temporizadores multimedia
// timeSetEvent(delay, resolution, callback, user, flags): d2s2 los usa para su mezclador. Aún sin hilo de reloj:
// se avisa y se devuelve 0 (fallo), como en una máquina sin temporizadores libres.

STUB(ext_winmm_dll_timeSetEvent) {
    plog_once("timeSetEvent", "timeSetEvent(%u ms, cb 0x%x, flags 0x%x): sin implementar", arg(0), arg(2), arg(4));
    RET(0, 5);
}
STUB(ext_winmm_dll_timeKillEvent) { RET(0, 1); }

// ---------------------------------------------------------------- OLE32 por GetProcAddress
// La MFC42 del CD de PCF2000 carga ole32.dll con LoadLibrary y pide sus funciones con GetProcAddress (carga diferida de
// OLE al entrar en Euromanager: CHtmlView del panel de ayuda). Los stubs existen como importaciones estáticas; aquí se
// registran además como exportaciones dinámicas para que GetProcAddress las encuentre.
STUB(ext_ole32_dll_BindMoniker);
STUB(ext_ole32_dll_CLSIDFromProgID);
STUB(ext_ole32_dll_CLSIDFromString);
STUB(ext_ole32_dll_CoCreateInstance);
STUB(ext_ole32_dll_CoDisconnectObject);
STUB(ext_ole32_dll_CoFreeUnusedLibraries);
STUB(ext_ole32_dll_CoGetClassObject);
STUB(ext_ole32_dll_CoGetMalloc);
STUB(ext_ole32_dll_CoInitialize);
STUB(ext_ole32_dll_CoInitializeEx);
STUB(ext_ole32_dll_CoIsOle1Class);
STUB(ext_ole32_dll_CoLockObjectExternal);
STUB(ext_ole32_dll_CoMarshalHresult);
STUB(ext_ole32_dll_CoMarshalInterface);
STUB(ext_ole32_dll_CoRegisterClassObject);
STUB(ext_ole32_dll_CoRegisterMessageFilter);
STUB(ext_ole32_dll_CoRevokeClassObject);
STUB(ext_ole32_dll_CoSetState);
STUB(ext_ole32_dll_CoTaskMemAlloc);
STUB(ext_ole32_dll_CoTaskMemFree);
STUB(ext_ole32_dll_CoTaskMemRealloc);
STUB(ext_ole32_dll_CoUninitialize);
STUB(ext_ole32_dll_CoUnmarshalHresult);
STUB(ext_ole32_dll_CoUnmarshalInterface);
STUB(ext_ole32_dll_CreateBindCtx);
STUB(ext_ole32_dll_CreateDataAdviseHolder);
STUB(ext_ole32_dll_CreateErrorInfo);
STUB(ext_ole32_dll_CreateILockBytesOnHGlobal);
STUB(ext_ole32_dll_CreateItemMoniker);
STUB(ext_ole32_dll_CreateOleAdviseHolder);
STUB(ext_ole32_dll_CreateStreamOnHGlobal);
STUB(ext_ole32_dll_DoDragDrop);
STUB(ext_ole32_dll_GetClassFile);
STUB(ext_ole32_dll_GetErrorInfo);
STUB(ext_ole32_dll_GetHGlobalFromStream);
STUB(ext_ole32_dll_GetRunningObjectTable);
STUB(ext_ole32_dll_IIDFromString);
STUB(ext_ole32_dll_IsAccelerator);
STUB(ext_ole32_dll_MkParseDisplayName);
STUB(ext_ole32_dll_OleConvertIStorageToOLESTREAM);
STUB(ext_ole32_dll_OleConvertOLESTREAMToIStorage);
STUB(ext_ole32_dll_OleCreateFromData);
STUB(ext_ole32_dll_OleCreateFromFile);
STUB(ext_ole32_dll_OleCreateLink);
STUB(ext_ole32_dll_OleCreateLinkFromData);
STUB(ext_ole32_dll_OleCreateLinkToFile);
STUB(ext_ole32_dll_OleCreateMenuDescriptor);
STUB(ext_ole32_dll_OleDestroyMenuDescriptor);
STUB(ext_ole32_dll_OleDoAutoConvert);
STUB(ext_ole32_dll_OleDuplicateData);
STUB(ext_ole32_dll_OleFlushClipboard);
STUB(ext_ole32_dll_OleGetAutoConvert);
STUB(ext_ole32_dll_OleGetClipboard);
STUB(ext_ole32_dll_OleGetIconOfClass);
STUB(ext_ole32_dll_OleInitialize);
STUB(ext_ole32_dll_OleIsCurrentClipboard);
STUB(ext_ole32_dll_OleIsRunning);
STUB(ext_ole32_dll_OleLoad);
STUB(ext_ole32_dll_OleLoadFromStream);
STUB(ext_ole32_dll_OleLockRunning);
STUB(ext_ole32_dll_OleQueryCreateFromData);
STUB(ext_ole32_dll_OleQueryLinkFromData);
STUB(ext_ole32_dll_OleRegGetUserType);
STUB(ext_ole32_dll_OleRun);
STUB(ext_ole32_dll_OleSave);
STUB(ext_ole32_dll_OleSaveToStream);
STUB(ext_ole32_dll_OleSetClipboard);
STUB(ext_ole32_dll_OleSetMenuDescriptor);
STUB(ext_ole32_dll_OleTranslateAccelerator);
STUB(ext_ole32_dll_OleUninitialize);
STUB(ext_ole32_dll_ProgIDFromCLSID);
STUB(ext_ole32_dll_ReadClassStg);
STUB(ext_ole32_dll_ReadClassStm);
STUB(ext_ole32_dll_RegisterDragDrop);
STUB(ext_ole32_dll_ReleaseStgMedium);
STUB(ext_ole32_dll_RevokeDragDrop);
STUB(ext_ole32_dll_SetErrorInfo);
STUB(ext_ole32_dll_StgCreateDocfile);
STUB(ext_ole32_dll_StgCreateDocfileOnILockBytes);
STUB(ext_ole32_dll_StgIsStorageILockBytes);
STUB(ext_ole32_dll_StgOpenStorage);
STUB(ext_ole32_dll_StgOpenStorageOnILockBytes);
STUB(ext_ole32_dll_StringFromCLSID);
STUB(ext_ole32_dll_StringFromGUID2);
STUB(ext_ole32_dll_WriteClassStg);
STUB(ext_ole32_dll_WriteClassStm);
STUB(ext_ole32_dll_WriteFmtUserTypeStg);

void register_dynamic_export(const char *dll, const char *name, void (*f)(void));   // kernel32.cpp
struct RegistrarOle32 {
    RegistrarOle32() {
        register_dynamic_export("ole32.dll", "BindMoniker", ext_ole32_dll_BindMoniker);
        register_dynamic_export("ole32.dll", "CLSIDFromProgID", ext_ole32_dll_CLSIDFromProgID);
        register_dynamic_export("ole32.dll", "CLSIDFromString", ext_ole32_dll_CLSIDFromString);
        register_dynamic_export("ole32.dll", "CoCreateInstance", ext_ole32_dll_CoCreateInstance);
        register_dynamic_export("ole32.dll", "CoDisconnectObject", ext_ole32_dll_CoDisconnectObject);
        register_dynamic_export("ole32.dll", "CoFreeUnusedLibraries", ext_ole32_dll_CoFreeUnusedLibraries);
        register_dynamic_export("ole32.dll", "CoGetClassObject", ext_ole32_dll_CoGetClassObject);
        register_dynamic_export("ole32.dll", "CoGetMalloc", ext_ole32_dll_CoGetMalloc);
        register_dynamic_export("ole32.dll", "CoInitialize", ext_ole32_dll_CoInitialize);
        register_dynamic_export("ole32.dll", "CoInitializeEx", ext_ole32_dll_CoInitializeEx);
        register_dynamic_export("ole32.dll", "CoIsOle1Class", ext_ole32_dll_CoIsOle1Class);
        register_dynamic_export("ole32.dll", "CoLockObjectExternal", ext_ole32_dll_CoLockObjectExternal);
        register_dynamic_export("ole32.dll", "CoMarshalHresult", ext_ole32_dll_CoMarshalHresult);
        register_dynamic_export("ole32.dll", "CoMarshalInterface", ext_ole32_dll_CoMarshalInterface);
        register_dynamic_export("ole32.dll", "CoRegisterClassObject", ext_ole32_dll_CoRegisterClassObject);
        register_dynamic_export("ole32.dll", "CoRegisterMessageFilter", ext_ole32_dll_CoRegisterMessageFilter);
        register_dynamic_export("ole32.dll", "CoRevokeClassObject", ext_ole32_dll_CoRevokeClassObject);
        register_dynamic_export("ole32.dll", "CoSetState", ext_ole32_dll_CoSetState);
        register_dynamic_export("ole32.dll", "CoTaskMemAlloc", ext_ole32_dll_CoTaskMemAlloc);
        register_dynamic_export("ole32.dll", "CoTaskMemFree", ext_ole32_dll_CoTaskMemFree);
        register_dynamic_export("ole32.dll", "CoTaskMemRealloc", ext_ole32_dll_CoTaskMemRealloc);
        register_dynamic_export("ole32.dll", "CoUninitialize", ext_ole32_dll_CoUninitialize);
        register_dynamic_export("ole32.dll", "CoUnmarshalHresult", ext_ole32_dll_CoUnmarshalHresult);
        register_dynamic_export("ole32.dll", "CoUnmarshalInterface", ext_ole32_dll_CoUnmarshalInterface);
        register_dynamic_export("ole32.dll", "CreateBindCtx", ext_ole32_dll_CreateBindCtx);
        register_dynamic_export("ole32.dll", "CreateDataAdviseHolder", ext_ole32_dll_CreateDataAdviseHolder);
        register_dynamic_export("ole32.dll", "CreateErrorInfo", ext_ole32_dll_CreateErrorInfo);
        register_dynamic_export("ole32.dll", "CreateILockBytesOnHGlobal", ext_ole32_dll_CreateILockBytesOnHGlobal);
        register_dynamic_export("ole32.dll", "CreateItemMoniker", ext_ole32_dll_CreateItemMoniker);
        register_dynamic_export("ole32.dll", "CreateOleAdviseHolder", ext_ole32_dll_CreateOleAdviseHolder);
        register_dynamic_export("ole32.dll", "CreateStreamOnHGlobal", ext_ole32_dll_CreateStreamOnHGlobal);
        register_dynamic_export("ole32.dll", "DoDragDrop", ext_ole32_dll_DoDragDrop);
        register_dynamic_export("ole32.dll", "GetClassFile", ext_ole32_dll_GetClassFile);
        register_dynamic_export("ole32.dll", "GetErrorInfo", ext_ole32_dll_GetErrorInfo);
        register_dynamic_export("ole32.dll", "GetHGlobalFromStream", ext_ole32_dll_GetHGlobalFromStream);
        register_dynamic_export("ole32.dll", "GetRunningObjectTable", ext_ole32_dll_GetRunningObjectTable);
        register_dynamic_export("ole32.dll", "IIDFromString", ext_ole32_dll_IIDFromString);
        register_dynamic_export("ole32.dll", "IsAccelerator", ext_ole32_dll_IsAccelerator);
        register_dynamic_export("ole32.dll", "MkParseDisplayName", ext_ole32_dll_MkParseDisplayName);
        register_dynamic_export("ole32.dll", "OleConvertIStorageToOLESTREAM", ext_ole32_dll_OleConvertIStorageToOLESTREAM);
        register_dynamic_export("ole32.dll", "OleConvertOLESTREAMToIStorage", ext_ole32_dll_OleConvertOLESTREAMToIStorage);
        register_dynamic_export("ole32.dll", "OleCreateFromData", ext_ole32_dll_OleCreateFromData);
        register_dynamic_export("ole32.dll", "OleCreateFromFile", ext_ole32_dll_OleCreateFromFile);
        register_dynamic_export("ole32.dll", "OleCreateLink", ext_ole32_dll_OleCreateLink);
        register_dynamic_export("ole32.dll", "OleCreateLinkFromData", ext_ole32_dll_OleCreateLinkFromData);
        register_dynamic_export("ole32.dll", "OleCreateLinkToFile", ext_ole32_dll_OleCreateLinkToFile);
        register_dynamic_export("ole32.dll", "OleCreateMenuDescriptor", ext_ole32_dll_OleCreateMenuDescriptor);
        register_dynamic_export("ole32.dll", "OleDestroyMenuDescriptor", ext_ole32_dll_OleDestroyMenuDescriptor);
        register_dynamic_export("ole32.dll", "OleDoAutoConvert", ext_ole32_dll_OleDoAutoConvert);
        register_dynamic_export("ole32.dll", "OleDuplicateData", ext_ole32_dll_OleDuplicateData);
        register_dynamic_export("ole32.dll", "OleFlushClipboard", ext_ole32_dll_OleFlushClipboard);
        register_dynamic_export("ole32.dll", "OleGetAutoConvert", ext_ole32_dll_OleGetAutoConvert);
        register_dynamic_export("ole32.dll", "OleGetClipboard", ext_ole32_dll_OleGetClipboard);
        register_dynamic_export("ole32.dll", "OleGetIconOfClass", ext_ole32_dll_OleGetIconOfClass);
        register_dynamic_export("ole32.dll", "OleInitialize", ext_ole32_dll_OleInitialize);
        register_dynamic_export("ole32.dll", "OleIsCurrentClipboard", ext_ole32_dll_OleIsCurrentClipboard);
        register_dynamic_export("ole32.dll", "OleIsRunning", ext_ole32_dll_OleIsRunning);
        register_dynamic_export("ole32.dll", "OleLoad", ext_ole32_dll_OleLoad);
        register_dynamic_export("ole32.dll", "OleLoadFromStream", ext_ole32_dll_OleLoadFromStream);
        register_dynamic_export("ole32.dll", "OleLockRunning", ext_ole32_dll_OleLockRunning);
        register_dynamic_export("ole32.dll", "OleQueryCreateFromData", ext_ole32_dll_OleQueryCreateFromData);
        register_dynamic_export("ole32.dll", "OleQueryLinkFromData", ext_ole32_dll_OleQueryLinkFromData);
        register_dynamic_export("ole32.dll", "OleRegGetUserType", ext_ole32_dll_OleRegGetUserType);
        register_dynamic_export("ole32.dll", "OleRun", ext_ole32_dll_OleRun);
        register_dynamic_export("ole32.dll", "OleSave", ext_ole32_dll_OleSave);
        register_dynamic_export("ole32.dll", "OleSaveToStream", ext_ole32_dll_OleSaveToStream);
        register_dynamic_export("ole32.dll", "OleSetClipboard", ext_ole32_dll_OleSetClipboard);
        register_dynamic_export("ole32.dll", "OleSetMenuDescriptor", ext_ole32_dll_OleSetMenuDescriptor);
        register_dynamic_export("ole32.dll", "OleTranslateAccelerator", ext_ole32_dll_OleTranslateAccelerator);
        register_dynamic_export("ole32.dll", "OleUninitialize", ext_ole32_dll_OleUninitialize);
        register_dynamic_export("ole32.dll", "ProgIDFromCLSID", ext_ole32_dll_ProgIDFromCLSID);
        register_dynamic_export("ole32.dll", "ReadClassStg", ext_ole32_dll_ReadClassStg);
        register_dynamic_export("ole32.dll", "ReadClassStm", ext_ole32_dll_ReadClassStm);
        register_dynamic_export("ole32.dll", "RegisterDragDrop", ext_ole32_dll_RegisterDragDrop);
        register_dynamic_export("ole32.dll", "ReleaseStgMedium", ext_ole32_dll_ReleaseStgMedium);
        register_dynamic_export("ole32.dll", "RevokeDragDrop", ext_ole32_dll_RevokeDragDrop);
        register_dynamic_export("ole32.dll", "SetErrorInfo", ext_ole32_dll_SetErrorInfo);
        register_dynamic_export("ole32.dll", "StgCreateDocfile", ext_ole32_dll_StgCreateDocfile);
        register_dynamic_export("ole32.dll", "StgCreateDocfileOnILockBytes", ext_ole32_dll_StgCreateDocfileOnILockBytes);
        register_dynamic_export("ole32.dll", "StgIsStorageILockBytes", ext_ole32_dll_StgIsStorageILockBytes);
        register_dynamic_export("ole32.dll", "StgOpenStorage", ext_ole32_dll_StgOpenStorage);
        register_dynamic_export("ole32.dll", "StgOpenStorageOnILockBytes", ext_ole32_dll_StgOpenStorageOnILockBytes);
        register_dynamic_export("ole32.dll", "StringFromCLSID", ext_ole32_dll_StringFromCLSID);
        register_dynamic_export("ole32.dll", "StringFromGUID2", ext_ole32_dll_StringFromGUID2);
        register_dynamic_export("ole32.dll", "WriteClassStg", ext_ole32_dll_WriteClassStg);
        register_dynamic_export("ole32.dll", "WriteClassStm", ext_ole32_dll_WriteClassStm);
        register_dynamic_export("ole32.dll", "WriteFmtUserTypeStg", ext_ole32_dll_WriteFmtUserTypeStg);
        // oleaut32: la MFC42 la pide por ORDINAL (2 SysAllocString, 3 SysReAllocString, 4 SysAllocStringLen,
        // 5 SysReAllocStringLen, 6 SysFreeString, 7 SysStringLen, 8 VariantInit, 9 VariantClear, 10 VariantCopy,
        // 12 VariantChangeType, 149 SysStringByteLen, 150 SysAllocStringByteLen)
        struct { const char *n, *ord; void (*f)(void); } ole[] = {
            {"SysAllocString", "#2", ext_oleaut32_dll_SysAllocString}, {"SysReAllocString", "#3", ext_oleaut32_dll_SysReAllocString},
            {"SysAllocStringLen", "#4", ext_oleaut32_dll_SysAllocStringLen}, {"SysReAllocStringLen", "#5", ext_oleaut32_dll_SysReAllocStringLen},
            {"SysFreeString", "#6", ext_oleaut32_dll_SysFreeString}, {"SysStringLen", "#7", ext_oleaut32_dll_SysStringLen},
            {"VariantInit", "#8", ext_oleaut32_dll_VariantInit}, {"VariantClear", "#9", ext_oleaut32_dll_VariantClear},
            {"VariantCopy", "#10", ext_oleaut32_dll_VariantCopy}, {"VariantChangeType", "#12", ext_oleaut32_dll_VariantChangeType},
            {"SysStringByteLen", "#149", ext_oleaut32_dll_SysStringByteLen}, {"SysAllocStringByteLen", "#150", ext_oleaut32_dll_SysAllocStringByteLen},
        };
        for (auto &e : ole) { register_dynamic_export("oleaut32.dll", e.n, e.f); register_dynamic_export("oleaut32.dll", e.ord, e.f); }
    }
} g_registrar_ole32;
