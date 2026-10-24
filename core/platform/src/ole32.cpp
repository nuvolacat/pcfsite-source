// PCF7 — OLE32: COM en proceso sobre el registro (registry.cpp) y los módulos traducidos.
//
// CoCreateInstance busca la clase en HKCR\CLSID\{…}\InprocServer32 (lo escribe el DllRegisterServer de
// cada OCX), carga ese módulo traducido con LoadLibrary y pide la fábrica a su DllGetClassObject, igual
// que Windows. Además: IMalloc, flujos sobre memoria (IStream), almacenamiento estructurado en memoria
// (IStorage/ILockBytes, lo usa la persistencia de los controles), avisos (IOleAdviseHolder,
// IDataAdviseHolder) y lo que un sistema sin portapapeles OLE ni monikers responde al resto.
#include "com.h"

#include <map>
#include <memory>
#include <vector>

u32 video_create(u32 clsid, u32 iid);   // video.cpp (DirectShow: CLSID_FilterGraph)
u32 webbrowser_class_object(u32 clsid, u32 iid);   // webbrowser.cpp (PCF2000: control del panel de ayuda)
u32 dmusic_create(u32 clsid, u32 iid);
#include "dshow.h"                                  // dshow_create_filter (PCF2001: filtros MPEG de la música)             // dmusic.cpp (PCF2000: música MIDI por DirectMusic)
u32 load_translated(const std::string &lower_name);
extern "C" u32 gp_lookup(u32 namep, u32 modbase);

namespace {

constexpr u32 S_OK = 0, S_FALSE = 1, E_NOTIMPL = 0x80004001u, E_NOINTERFACE = 0x80004002u, E_POINTER = 0x80004003u,
              E_FAIL = 0x80004005u, E_OUTOFMEMORY = 0x8007000Eu, E_INVALIDARG = 0x80070057u,
              REGDB_E_CLASSNOTREG = 0x80040154u, CO_E_CLASSSTRING = 0x800401F3u, REGDB_E_READREGDB = 0x80040150u,
              OLE_E_ADVISENOTSUPPORTED = 0x80040003u, MK_E_UNAVAILABLE = 0x800401E3u, CLIPBRD_E_CANT_OPEN = 0x800401D0u,
              STG_E_FILENOTFOUND = 0x80030002u, STG_E_INVALIDFUNCTION = 0x80030001u, STG_E_FILEALREADYEXISTS = 0x80030050u,
              DRAGDROP_S_CANCEL = 0x00040101u, OLE_S_USEREG = 0x00040000u;

const ComGuid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IClassFactory = {0x00000001, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IMalloc = {0x00000002, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IStream = {0x0000000C, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_ISequentialStream = {0x0C733A30, 0x2A1C, 0x11CE, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}};
const ComGuid IID_IStorage = {0x0000000B, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_ILockBytes = {0x0000000A, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IOleAdviseHolder = {0x00000111, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const ComGuid IID_IDataAdviseHolder = {0x00000110, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

// ---------------------------------------------------------------- GUID ↔ texto

std::string guid_text(u32 g) {
    char b[48];
    snprintf(b, sizeof b, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(g), rd16(g + 4), rd16(g + 6), MEM[g + 8],
             MEM[g + 9], MEM[g + 10], MEM[g + 11], MEM[g + 12], MEM[g + 13], MEM[g + 14], MEM[g + 15]);
    return b;
}
bool parse_guid(const std::string &s0, u8 out[16]) {
    std::string s;
    for (char c : s0) if (std::isxdigit((unsigned char)c)) s.push_back(c);
    if (s.size() != 32) return false;
    auto hx = [&](size_t i, size_t n) { return (u32)std::strtoul(s.substr(i, n).c_str(), nullptr, 16); };
    u32 a = hx(0, 8);
    u16 b = (u16)hx(8, 4), c = (u16)hx(12, 4);
    std::memcpy(out, &a, 4); std::memcpy(out + 4, &b, 2); std::memcpy(out + 6, &c, 2);
    for (int i = 0; i < 8; i++) out[8 + i] = (u8)hx(16 + 2 * (size_t)i, 2);
    return true;
}
std::string wstr(u32 p) {
    std::string s;
    for (u32 i = 0; p && i < 4096; i++) { u32 c = rd16(p + 2 * i); if (!c) break; s.push_back(c < 256 ? (char)c : '?'); }
    return s;
}
u32 wput(u32 dst, const std::string &s) { for (size_t i = 0; i <= s.size(); i++) wr16(dst + 2 * (u32)i, i < s.size() ? (u8)s[i] : 0); return (u32)s.size(); }
u32 co_wstring(const std::string &s) { u32 p = heap_alloc(2 * ((u32)s.size() + 1)); wput(p, s); return p; }

}  // namespace

// el registro expone esta consulta para OLE32 (registry.cpp)
std::string reg_query_default(u32 root, const std::string &path, bool *found);

// PCF7_TRACE_COM: cada método de los objetos COM del host, con sus primeros argumentos
static const bool g_trace_com = std::getenv("PCF7_TRACE_COM") != nullptr;
#define TRACE_COM() do { if (g_trace_com) plog("COM %s(%x, %x, %x, %x)", __func__, arg(0), arg(1), arg(2), arg(3)); } while (0)

namespace {

// ---------------------------------------------------------------- IMalloc

u32 g_malloc = 0;
void ma_QueryInterface() { TRACE_COM();
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_IMalloc)) { wr32(out, arg(0)); RET(S_OK, 3); }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void ma_AddRef() { TRACE_COM(); RET(2, 1); }
void ma_Release() { TRACE_COM(); RET(1, 1); }
void ma_Alloc() { TRACE_COM(); RET(heap_alloc(arg(1) ? arg(1) : 1), 2); }
void ma_Realloc() { TRACE_COM(); RET(arg(1) ? heap_realloc(arg(1), arg(2) ? arg(2) : 1) : heap_alloc(arg(2) ? arg(2) : 1), 3); }
void ma_Free() { TRACE_COM(); heap_free(arg(1)); RET(0, 2); }
void ma_GetSize() { TRACE_COM(); RET(arg(1) ? heap_size(arg(1)) : 0xFFFFFFFFu, 2); }
void ma_DidAlloc() { TRACE_COM(); RET(arg(1) && heap_size(arg(1)) ? 1 : 0, 2); }
void ma_HeapMinimize() { TRACE_COM(); RET(0, 1); }
u32 imalloc() {
    if (!g_malloc) g_malloc = com_object(com_vtbl({ma_QueryInterface, ma_AddRef, ma_Release, ma_Alloc, ma_Realloc, ma_Free,
                                                   ma_GetSize, ma_DidAlloc, ma_HeapMinimize}));
    return g_malloc;
}

// ---------------------------------------------------------------- IStream e ILockBytes sobre memoria

struct Blob { std::vector<u8> data; u32 hglobal = 0; bool delete_on_release = false; };
struct Stream { std::shared_ptr<Blob> blob; u64 pos = 0; u32 refs = 1; std::string name; };
std::map<u32, Stream> g_streams;
std::map<u32, std::shared_ptr<Blob>> g_lockbytes;
u32 g_vt_stream = 0, g_vt_lockbytes = 0;

// si el flujo se creó sobre un HGLOBAL, su contenido se refleja allí (GetHGlobalFromStream, VB lo lee)
void sync_hglobal(Blob &b) {
    if (!b.hglobal) return;
    if (heap_size(b.hglobal) < b.data.size()) b.hglobal = heap_realloc(b.hglobal, (u32)b.data.size());
    if (!b.data.empty()) std::memcpy(MEM + b.hglobal, b.data.data(), b.data.size());
}

u32 new_stream(std::shared_ptr<Blob> blob, const std::string &name = "") {
    u32 o = com_object(g_vt_stream);
    Stream s;
    s.blob = std::move(blob);
    s.name = name;
    g_streams[o] = s;
    return o;
}
Stream *stream(u32 o) { auto it = g_streams.find(o); return it == g_streams.end() ? nullptr : &it->second; }

void st_QueryInterface() { TRACE_COM();
    u32 iid = arg(1), out = arg(2);
    Stream *s = stream(arg(0));
    if (s && (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_IStream) || com_guid_eq(iid, IID_ISequentialStream))) {
        s->refs++;
        wr32(out, arg(0));
        RET(S_OK, 3);
    }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void st_AddRef() { TRACE_COM(); Stream *s = stream(arg(0)); RET(s ? ++s->refs : 1, 1); }
void st_Release() { TRACE_COM();
    Stream *s = stream(arg(0));
    if (!s) RET(0, 1);
    u32 n = --s->refs;
    if (!n) {
        if (s->blob->delete_on_release && s->blob.use_count() == 1 && s->blob->hglobal) heap_free(s->blob->hglobal);
        g_streams.erase(arg(0));
    }
    RET(n, 1);
}
void st_Read() { TRACE_COM();   // (pv, cb, pcbRead)
    Stream *s = stream(arg(0));
    if (!s) RET(E_POINTER, 4);
    u64 avail = s->blob->data.size() > s->pos ? s->blob->data.size() - s->pos : 0;
    u32 n = (u32)std::min<u64>(arg(2), avail);
    if (n) std::memcpy(MEM + arg(1), s->blob->data.data() + s->pos, n);
    s->pos += n;
    if (arg(3)) wr32(arg(3), n);
    RET(n < arg(2) ? S_FALSE : S_OK, 4);
}
void st_Write() { TRACE_COM();   // (pv, cb, pcbWritten)
    Stream *s = stream(arg(0));
    if (!s) RET(E_POINTER, 4);
    u32 n = arg(2);
    if (s->pos + n > s->blob->data.size()) s->blob->data.resize(s->pos + n);
    if (n) std::memcpy(s->blob->data.data() + s->pos, MEM + arg(1), n);
    s->pos += n;
    sync_hglobal(*s->blob);
    if (arg(3)) wr32(arg(3), n);
    RET(S_OK, 4);
}
void st_Seek() { TRACE_COM();   // (LARGE_INTEGER move [2 dwords], origin, ULARGE_INTEGER *newpos)
    Stream *s = stream(arg(0));
    if (!s) RET(E_POINTER, 5);
    i64 mv = (i64)((u64)arg(1) | (u64)arg(2) << 32);
    i64 base = arg(3) == 0 ? 0 : arg(3) == 1 ? (i64)s->pos : (i64)s->blob->data.size();
    i64 np = base + mv;
    if (np < 0) RET(STG_E_INVALIDFUNCTION, 5);
    s->pos = (u64)np;
    if (arg(4)) wr64(arg(4), s->pos);
    RET(S_OK, 5);
}
void st_SetSize() { TRACE_COM(); Stream *s = stream(arg(0)); if (s) { s->blob->data.resize((size_t)arg(1)); sync_hglobal(*s->blob); } RET(S_OK, 3); }
void st_CopyTo() { TRACE_COM();   // (IStream *dst, ULARGE cb [2], ULARGE *read, ULARGE *written)
    Stream *s = stream(arg(0)), *d = stream(arg(1));
    if (!s) RET(E_POINTER, 6);
    u64 want = (u64)arg(2) | (u64)arg(3) << 32;
    u64 avail = s->blob->data.size() > s->pos ? s->blob->data.size() - s->pos : 0;
    u64 n = std::min(want, avail);
    if (d) {
        if (d->pos + n > d->blob->data.size()) d->blob->data.resize(d->pos + n);
        std::memcpy(d->blob->data.data() + d->pos, s->blob->data.data() + s->pos, (size_t)n);
        d->pos += n;
        sync_hglobal(*d->blob);
    } else if (n) {   // destino que no es nuestro: por su Write
        u32 tmp = heap_alloc((u32)n);
        std::memcpy(MEM + tmp, s->blob->data.data() + s->pos, (size_t)n);
        guest_call(rd32(rd32(arg(1)) + 16), {arg(1), tmp, (u32)n, 0});
        heap_free(tmp);
    }
    s->pos += n;
    if (arg(4)) wr64(arg(4), n);
    if (arg(5)) wr64(arg(5), n);
    RET(S_OK, 6);
}
void st_Stat() { TRACE_COM();   // (STATSTG*, flag): pwcsName, type, cbSize, mtime, ctime, atime, mode, locks, clsid, state, reserved
    Stream *s = stream(arg(0));
    u32 p = arg(1);
    if (!s || !p) RET(E_POINTER, 3);
    std::memset(MEM + p, 0, 72);
    wr32(p, (arg(2) & 1) || s->name.empty() ? 0 : co_wstring(s->name));
    wr32(p + 4, 2);   // STGTY_STREAM
    wr64(p + 8, s->blob->data.size());
    RET(S_OK, 3);
}
void st_Clone() { TRACE_COM();
    Stream *s = stream(arg(0));
    if (!s) RET(E_POINTER, 2);
    u32 o = new_stream(s->blob, s->name);
    g_streams[o].pos = s->pos;
    wr32(arg(1), o);
    RET(S_OK, 2);
}
void m_Ok1() { RET(S_OK, 1); }
void m_Ok2() { RET(S_OK, 2); }
void m_Ok3() { RET(S_OK, 3); }
void m_Ok4() { RET(S_OK, 4); }
void m_Ok6() { RET(S_OK, 6); }

// ILockBytes: ReadAt(offset [2], pv, cb, pcbRead), WriteAt, Flush, SetSize, LockRegion, UnlockRegion, Stat
std::shared_ptr<Blob> lockbytes(u32 o) { auto it = g_lockbytes.find(o); return it == g_lockbytes.end() ? nullptr : it->second; }
void lb_QueryInterface() { TRACE_COM();
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_ILockBytes)) { com_addref(arg(0)); wr32(out, arg(0)); RET(S_OK, 3); }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void lb_AddRef() { TRACE_COM(); RET(com_addref(arg(0)), 1); }
void lb_Release() { TRACE_COM(); u32 n = com_release(arg(0)); if (!n) g_lockbytes.erase(arg(0)); RET(n, 1); }
void lb_ReadAt() { TRACE_COM();
    auto b = lockbytes(arg(0));
    if (!b) RET(E_POINTER, 6);
    u64 off = (u64)arg(1) | (u64)arg(2) << 32;
    u32 n = off < b->data.size() ? (u32)std::min<u64>(arg(4), b->data.size() - off) : 0;
    if (n) std::memcpy(MEM + arg(3), b->data.data() + off, n);
    if (arg(5)) wr32(arg(5), n);
    RET(S_OK, 6);
}
void lb_WriteAt() { TRACE_COM();
    auto b = lockbytes(arg(0));
    if (!b) RET(E_POINTER, 6);
    u64 off = (u64)arg(1) | (u64)arg(2) << 32;
    if (off + arg(4) > b->data.size()) b->data.resize(off + arg(4));
    if (arg(4)) std::memcpy(b->data.data() + off, MEM + arg(3), arg(4));
    sync_hglobal(*b);
    if (arg(5)) wr32(arg(5), arg(4));
    RET(S_OK, 6);
}
void lb_SetSize() { TRACE_COM(); auto b = lockbytes(arg(0)); if (b) { b->data.resize(arg(1)); sync_hglobal(*b); } RET(S_OK, 3); }
void lb_Stat() { TRACE_COM();
    auto b = lockbytes(arg(0));
    u32 p = arg(1);
    std::memset(MEM + p, 0, 72);
    wr32(p + 4, 3);   // STGTY_LOCKBYTES
    wr64(p + 8, b ? b->data.size() : 0);
    RET(S_OK, 3);
}

// ---------------------------------------------------------------- IStorage en memoria
//
// Almacenamiento estructurado propio (no el formato de fichero compuesto): un árbol de flujos y
// subalmacenamientos. Si va sobre un ILockBytes se serializa en él con un formato simple para que
// StgOpenStorageOnILockBytes lo pueda volver a abrir.

struct Storage {
    u8 clsid[16] = {};
    std::map<std::string, std::shared_ptr<Blob>> streams;
    std::map<std::string, std::shared_ptr<Storage>> stores;
};
struct StorageObj { std::shared_ptr<Storage> st; u32 lockbytes = 0; u32 refs = 1; };
std::map<u32, StorageObj> g_storages;
u32 g_vt_storage = 0;

void serialize(const Storage &s, std::vector<u8> &o) {
    auto put32 = [&](u32 v) { for (int i = 0; i < 4; i++) o.push_back((u8)(v >> (8 * i))); };
    auto puts = [&](const std::string &x) { put32((u32)x.size()); o.insert(o.end(), x.begin(), x.end()); };
    o.insert(o.end(), s.clsid, s.clsid + 16);
    put32((u32)s.streams.size());
    for (auto &[n, b] : s.streams) { puts(n); put32((u32)b->data.size()); o.insert(o.end(), b->data.begin(), b->data.end()); }
    put32((u32)s.stores.size());
    for (auto &[n, c] : s.stores) { puts(n); serialize(*c, o); }
}
bool deserialize(const std::vector<u8> &d, size_t &p, Storage &s) {
    auto get32 = [&](u32 &v) { if (p + 4 > d.size()) return false; v = d[p] | d[p + 1] << 8 | d[p + 2] << 16 | (u32)d[p + 3] << 24; p += 4; return true; };
    auto gets = [&](std::string &x) { u32 n; if (!get32(n) || p + n > d.size()) return false; x.assign((const char *)d.data() + p, n); p += n; return true; };
    if (p + 16 > d.size()) return false;
    std::memcpy(s.clsid, d.data() + p, 16);
    p += 16;
    u32 ns, nc;
    if (!get32(ns)) return false;
    for (u32 i = 0; i < ns; i++) {
        std::string n;
        u32 sz;
        if (!gets(n) || !get32(sz) || p + sz > d.size()) return false;
        auto b = std::make_shared<Blob>();
        b->data.assign(d.begin() + (long)p, d.begin() + (long)(p + sz));
        p += sz;
        s.streams[n] = b;
    }
    if (!get32(nc)) return false;
    for (u32 i = 0; i < nc; i++) {
        std::string n;
        if (!gets(n)) return false;
        auto c = std::make_shared<Storage>();
        if (!deserialize(d, p, *c)) return false;
        s.stores[n] = c;
    }
    return true;
}
constexpr char STG_MAGIC[] = "PCF7STG1";

u32 new_storage(std::shared_ptr<Storage> st, u32 lockbytes_obj = 0) {
    u32 o = com_object(g_vt_storage);
    g_storages[o] = {std::move(st), lockbytes_obj, 1};
    if (lockbytes_obj) com_addref(lockbytes_obj);
    return o;
}
StorageObj *storage(u32 o) { auto it = g_storages.find(o); return it == g_storages.end() ? nullptr : &it->second; }
void commit(StorageObj &s) {
    if (!s.lockbytes) return;
    auto b = lockbytes(s.lockbytes);
    if (!b) return;
    b->data.assign(STG_MAGIC, STG_MAGIC + 8);
    serialize(*s.st, b->data);
    sync_hglobal(*b);
}

void sg_QueryInterface() { TRACE_COM();
    u32 iid = arg(1), out = arg(2);
    StorageObj *s = storage(arg(0));
    if (s && (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_IStorage))) { s->refs++; wr32(out, arg(0)); RET(S_OK, 3); }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void sg_AddRef() { TRACE_COM(); StorageObj *s = storage(arg(0)); RET(s ? ++s->refs : 1, 1); }
void sg_Release() { TRACE_COM();
    StorageObj *s = storage(arg(0));
    if (!s) RET(0, 1);
    u32 n = --s->refs;
    if (!n) {
        commit(*s);
        if (s->lockbytes) com_release(s->lockbytes);
        g_storages.erase(arg(0));
    }
    RET(n, 1);
}
void sg_CreateStream() { TRACE_COM();   // (name, mode, r1, r2, IStream **)
    StorageObj *s = storage(arg(0));
    if (!s) RET(E_POINTER, 6);
    std::string n = wstr(arg(1));
    auto b = std::make_shared<Blob>();
    s->st->streams[n] = b;
    wr32(arg(5), new_stream(b, n));
    RET(S_OK, 6);
}
void sg_OpenStream() { TRACE_COM();   // (name, r1, mode, r2, IStream **)
    StorageObj *s = storage(arg(0));
    if (!s) RET(E_POINTER, 6);
    std::string n = wstr(arg(1));
    auto it = s->st->streams.find(n);
    if (it == s->st->streams.end()) { wr32(arg(5), 0); RET(STG_E_FILENOTFOUND, 6); }
    wr32(arg(5), new_stream(it->second, n));
    RET(S_OK, 6);
}
void sg_CreateStorage() { TRACE_COM();   // (name, mode, r1, r2, IStorage **)
    StorageObj *s = storage(arg(0));
    if (!s) RET(E_POINTER, 6);
    auto c = std::make_shared<Storage>();
    s->st->stores[wstr(arg(1))] = c;
    wr32(arg(5), new_storage(c));
    RET(S_OK, 6);
}
void sg_OpenStorage() { TRACE_COM();   // (name, priority, mode, exclude, reserved, IStorage **)
    StorageObj *s = storage(arg(0));
    if (!s) RET(E_POINTER, 7);
    auto it = s->st->stores.find(wstr(arg(1)));
    if (it == s->st->stores.end()) { wr32(arg(6), 0); RET(STG_E_FILENOTFOUND, 7); }
    wr32(arg(6), new_storage(it->second));
    RET(S_OK, 7);
}
void sg_CopyTo() { TRACE_COM();   // (ciidExclude, rgiidExclude, snbExclude, IStorage *dest)
    StorageObj *s = storage(arg(0)), *d = storage(arg(4));
    if (s && d) { *d->st = *s->st; commit(*d); }
    RET(S_OK, 5);
}
void sg_Commit() { TRACE_COM(); if (StorageObj *s = storage(arg(0))) commit(*s); RET(S_OK, 2); }
void sg_EnumElements() { TRACE_COM(); wr32(arg(4), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 5); }
void sg_DestroyElement() { TRACE_COM();
    StorageObj *s = storage(arg(0));
    if (s) { std::string n = wstr(arg(1)); s->st->streams.erase(n); s->st->stores.erase(n); }
    RET(S_OK, 2);
}
void sg_SetClass() { TRACE_COM(); if (StorageObj *s = storage(arg(0))) std::memcpy(s->st->clsid, MEM + arg(1), 16); RET(S_OK, 2); }
void sg_Stat() { TRACE_COM();
    StorageObj *s = storage(arg(0));
    u32 p = arg(1);
    std::memset(MEM + p, 0, 72);
    wr32(p + 4, 1);   // STGTY_STORAGE
    if (s) std::memcpy(MEM + p + 0x38 - 8, s->st->clsid, 16);
    RET(S_OK, 3);
}

// ---------------------------------------------------------------- avisos (IOleAdviseHolder, IDataAdviseHolder)

struct Advise { std::map<u32, u32> sinks; u32 next = 1; };
std::map<u32, Advise> g_advise;
void ah_QueryInterface() {
    u32 iid = arg(1), out = arg(2);
    if (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_IOleAdviseHolder) || com_guid_eq(iid, IID_IDataAdviseHolder)) {
        com_addref(arg(0));
        wr32(out, arg(0));
        RET(S_OK, 3);
    }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}
void ah_AddRef() { RET(com_addref(arg(0)), 1); }
void ah_Release() {
    u32 n = com_release(arg(0));
    if (!n) {
        for (auto &[c, sink] : g_advise[arg(0)].sinks) guest_call(rd32(rd32(sink) + 8), {sink});
        g_advise.erase(arg(0));
    }
    RET(n, 1);
}
// IOleAdviseHolder::Advise(IAdviseSink*, DWORD*)
void oah_Advise() { TRACE_COM();
    Advise &a = g_advise[arg(0)];
    u32 c = a.next++;
    a.sinks[c] = arg(1);
    guest_call(rd32(rd32(arg(1)) + 4), {arg(1)});   // AddRef
    wr32(arg(2), c);
    RET(S_OK, 3);
}
void ah_Unadvise(u32 nargs) {
    Advise &a = g_advise[arg(0)];
    auto it = a.sinks.find(arg(1));
    if (it == a.sinks.end()) { RET(0x80040001u, nargs); }   // OLE_E_NOCONNECTION
    u32 sink = it->second;
    a.sinks.erase(it);
    guest_call(rd32(rd32(sink) + 8), {sink});
    RET(S_OK, nargs);
}
void oah_Unadvise() { TRACE_COM(); ah_Unadvise(2); }
void oah_EnumAdvise() { TRACE_COM(); wr32(arg(1), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 2); }
// avisos de IAdviseSink: OnDataChange 3, OnViewChange 4, OnRename 5, OnSave 6, OnClose 7
void notify(u32 holder, u32 slot, std::initializer_list<u32> extra) {
    auto copy = g_advise[holder].sinks;
    for (auto &[c, sink] : copy) {
        std::vector<u32> args{sink};
        args.insert(args.end(), extra);
        u32 fn = rd32(rd32(sink) + 4 * slot);
        switch (args.size()) {
        case 1: guest_call(fn, {args[0]}); break;
        case 2: guest_call(fn, {args[0], args[1]}); break;
        default: guest_call(fn, {args[0], args[1], args[2]}); break;
        }
    }
}
void oah_SendOnRename() { TRACE_COM(); notify(arg(0), 5, {arg(1)}); RET(S_OK, 2); }
void oah_SendOnSave() { TRACE_COM(); notify(arg(0), 6, {}); RET(S_OK, 1); }
void oah_SendOnClose() { TRACE_COM(); notify(arg(0), 7, {}); RET(S_OK, 1); }
// IDataAdviseHolder::Advise(IDataObject*, FORMATETC*, advf, IAdviseSink*, DWORD*)
void dah_Advise() { TRACE_COM();
    Advise &a = g_advise[arg(0)];
    u32 c = a.next++;
    a.sinks[c] = arg(4);
    guest_call(rd32(rd32(arg(4)) + 4), {arg(4)});
    wr32(arg(5), c);
    RET(S_OK, 6);
}
void dah_Unadvise() { TRACE_COM(); ah_Unadvise(2); }
void dah_EnumAdvise() { TRACE_COM(); wr32(arg(1), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 2); }
void dah_SendOnDataChange() { TRACE_COM(); RET(S_OK, 4); }

u32 g_vt_oah = 0, g_vt_dah = 0;

void init() {
    if (g_vt_stream) return;
    g_vt_stream = com_vtbl({st_QueryInterface, st_AddRef, st_Release, st_Read, st_Write, st_Seek, st_SetSize, st_CopyTo, m_Ok2,
                            m_Ok1, m_Ok6, m_Ok6, st_Stat, st_Clone});
    g_vt_lockbytes = com_vtbl({lb_QueryInterface, lb_AddRef, lb_Release, lb_ReadAt, lb_WriteAt, m_Ok1, lb_SetSize, m_Ok6, m_Ok6, lb_Stat});
    g_vt_storage = com_vtbl({sg_QueryInterface, sg_AddRef, sg_Release, sg_CreateStream, sg_OpenStream, sg_CreateStorage,
                             sg_OpenStorage, sg_CopyTo, m_Ok6, sg_Commit, m_Ok1, sg_EnumElements, sg_DestroyElement, m_Ok3,
                             m_Ok4, sg_SetClass, m_Ok3, sg_Stat});
    g_vt_oah = com_vtbl({ah_QueryInterface, ah_AddRef, ah_Release, oah_Advise, oah_Unadvise, oah_EnumAdvise, oah_SendOnRename,
                         oah_SendOnSave, oah_SendOnClose});
    g_vt_dah = com_vtbl({ah_QueryInterface, ah_AddRef, ah_Release, dah_Advise, dah_Unadvise, dah_EnumAdvise, dah_SendOnDataChange});
}

// ---------------------------------------------------------------- clases COM

struct RegisteredClass { u32 factory; };
std::map<std::string, RegisteredClass> g_class_objects;   // CoRegisterClassObject
u32 g_cookie = 1;

// CoGetClassObject: fábrica de la clase (registrada en el proceso o en el registro)
u32 get_class_object(u32 clsid, u32 iid, u32 out) {
    if (u32 o = webbrowser_class_object(clsid, iid)) { wr32(out, o); return 0; }
    std::string g = guid_text(clsid);
    if (auto it = g_class_objects.find(g); it != g_class_objects.end()) {
        u32 f = it->second.factory;
        return guest_call(rd32(rd32(f)), {f, iid, out});   // QueryInterface
    }
    bool found = false;
    std::string server = reg_query_default(0x80000000u, "CLSID\\" + g + "\\InprocServer32", &found);
    if (!found || server.empty()) {
        plog("CoGetClassObject(%s): clase no registrada", g.c_str());
        return REGDB_E_CLASSNOTREG;
    }
    size_t sl = server.find_last_of("\\/");
    std::string base = sl == std::string::npos ? server : server.substr(sl + 1);
    for (auto &c : base) c = (char)std::tolower((unsigned char)c);
    u32 mod = load_translated(base);
    if (!mod) { plog("CoGetClassObject(%s): el servidor %s no está traducido", g.c_str(), server.c_str()); return REGDB_E_CLASSNOTREG; }
    u32 name = heap_alloc(32);
    gstrcpy(name, "DllGetClassObject");
    u32 fn = gp_lookup(name, mod);
    heap_free(name);
    if (!fn) return E_FAIL;
    u32 hr = guest_call(fn, {clsid, iid, out});
    plog("CoGetClassObject(%s): DllGetClassObject de %s -> 0x%x", g.c_str(), base.c_str(), hr);
    return hr;
}

}  // namespace

// ---------------------------------------------------------------- inicialización y memoria

STUB(ext_ole32_dll_CoInitialize) { RET(S_OK, 1); }
STUB(ext_ole32_dll_CoInitializeEx) { RET(S_OK, 2); }
STUB(ext_ole32_dll_CoUninitialize) { RET(0, 0); }
STUB(ext_ole32_dll_OleInitialize) { init(); RET(S_OK, 1); }
STUB(ext_ole32_dll_OleUninitialize) { RET(0, 0); }
STUB(ext_ole32_dll_CoGetMalloc) { wr32(arg(1), imalloc()); RET(S_OK, 2); }
STUB(ext_ole32_dll_CoTaskMemAlloc) { RET(heap_alloc(arg(0) ? arg(0) : 1), 1); }
STUB(ext_ole32_dll_CoTaskMemFree) { heap_free(arg(0)); RET(0, 1); }
STUB(ext_ole32_dll_CoTaskMemRealloc) { RET(arg(0) ? heap_realloc(arg(0), arg(1) ? arg(1) : 1) : heap_alloc(arg(1) ? arg(1) : 1), 2); }
STUB(ext_ole32_dll_CoFreeUnusedLibraries) { RET(0, 0); }
STUB(ext_ole32_dll_CoSetState) { RET(S_OK, 1); }
STUB(ext_ole32_dll_CoRegisterMessageFilter) { if (arg(1)) wr32(arg(1), 0); RET(S_OK, 2); }
STUB(ext_ole32_dll_CoLockObjectExternal) { RET(S_OK, 3); }
STUB(ext_ole32_dll_CoDisconnectObject) { RET(S_OK, 2); }
STUB(ext_ole32_dll_CoIsOle1Class) { RET(0, 1); }

// ---------------------------------------------------------------- GUID

STUB(ext_ole32_dll_StringFromGUID2) {   // (guid, lpsz, cch) -> caracteres con el nulo
    std::string s = guid_text(arg(0));
    if (arg(2) < s.size() + 1) RET(0, 3);
    wput(arg(1), s);
    RET((u32)s.size() + 1, 3);
}
STUB(ext_ole32_dll_StringFromCLSID) { wr32(arg(1), co_wstring(guid_text(arg(0)))); RET(S_OK, 2); }
STUB(ext_ole32_dll_CLSIDFromString) {
    u8 g[16];
    std::string s = wstr(arg(0));
    if (!parse_guid(s, g)) {
        bool found = false;   // también acepta un ProgID
        std::string c = reg_query_default(0x80000000u, s + "\\CLSID", &found);
        if (!found || !parse_guid(c, g)) { std::memset(MEM + arg(1), 0, 16); RET(CO_E_CLASSSTRING, 2); }
    }
    std::memcpy(MEM + arg(1), g, 16);
    RET(S_OK, 2);
}
STUB(ext_ole32_dll_IIDFromString) {
    u8 g[16];
    if (!parse_guid(wstr(arg(0)), g)) RET(E_INVALIDARG, 2);
    std::memcpy(MEM + arg(1), g, 16);
    RET(S_OK, 2);
}
STUB(ext_ole32_dll_CLSIDFromProgID) {
    bool found = false;
    std::string c = reg_query_default(0x80000000u, wstr(arg(0)) + "\\CLSID", &found);
    u8 g[16];
    if (!found || !parse_guid(c, g)) RET(CO_E_CLASSSTRING, 2);
    std::memcpy(MEM + arg(1), g, 16);
    RET(S_OK, 2);
}
STUB(ext_ole32_dll_ProgIDFromCLSID) {
    bool found = false;
    std::string p = reg_query_default(0x80000000u, "CLSID\\" + guid_text(arg(0)) + "\\ProgID", &found);
    if (!found) { wr32(arg(1), 0); RET(REGDB_E_CLASSNOTREG, 2); }
    wr32(arg(1), co_wstring(p));
    RET(S_OK, 2);
}

// ---------------------------------------------------------------- crear objetos

STUB(ext_ole32_dll_CoGetClassObject) {   // (clsid, ctx, serverinfo, iid, ppv)
    init();
    u32 out = arg(4);
    wr32(out, 0);
    RET(get_class_object(arg(0), arg(3), out), 5);
}
// IClassFactory::CreateInstance de la clase (S_OK y *out, o el error)
static u32 create_instance(u32 clsid, u32 outer, u32 iid, u32 out) {
    wr32(out, 0);
    u32 tmp = heap_alloc(16, true);
    u32 cf_iid = heap_alloc(16);
    std::memcpy(MEM + cf_iid, &IID_IClassFactory, 16);
    u32 hr = get_class_object(clsid, cf_iid, tmp);
    if (hr == S_OK) {
        u32 cf = rd32(tmp);
        hr = guest_call(rd32(rd32(cf) + 12), {cf, outer, iid, out});   // IClassFactory::CreateInstance
        guest_call(rd32(rd32(cf) + 8), {cf});                           // Release
        if (hr) plog("CoCreateInstance(%s): CreateInstance -> 0x%x", guid_text(clsid).c_str(), hr);
    }
    heap_free(tmp);
    heap_free(cf_iid);
    return hr;
}

STUB(ext_ole32_dll_CoCreateInstance) {   // (clsid, outer, ctx, iid, ppv)
    init();
    u32 clsid = arg(0), outer = arg(1), iid = arg(3), out = arg(4);
    if (u32 o = video_create(clsid, iid)) { plog("CoCreateInstance(FilterGraph) -> 0x%x", o); wr32(out, o); RET(S_OK, 5); }
    if (u32 o = dmusic_create(clsid, iid)) { wr32(out, o); RET(S_OK, 5); }
    if (u32 o = dshow_create_filter(clsid, iid)) { wr32(out, o); RET(S_OK, 5); }
    RET(create_instance(clsid, outer, iid, out), 5);
}
STUB(ext_ole32_dll_CoRegisterClassObject) {   // (clsid, IUnknown*, ctx, flags, cookie*)
    g_class_objects[guid_text(arg(0))] = {arg(1)};
    guest_call(rd32(rd32(arg(1)) + 4), {arg(1)});
    if (arg(4)) wr32(arg(4), g_cookie++);
    RET(S_OK, 5);
}
STUB(ext_ole32_dll_CoRevokeClassObject) { RET(S_OK, 1); }

// ---------------------------------------------------------------- flujos y almacenamiento

STUB(ext_ole32_dll_CreateStreamOnHGlobal) {   // (hglobal, fDeleteOnRelease, IStream **)
    init();
    auto b = std::make_shared<Blob>();
    if (arg(0)) {
        u32 n = heap_size(arg(0));
        b->data.assign(MEM + arg(0), MEM + arg(0) + n);
        b->hglobal = arg(0);
    } else {
        b->hglobal = heap_alloc(16, true);
    }
    b->delete_on_release = arg(1) != 0;
    wr32(arg(2), new_stream(b));
    RET(S_OK, 3);
}
STUB(ext_ole32_dll_GetHGlobalFromStream) {
    Stream *s = stream(arg(0));
    if (!s) RET(E_INVALIDARG, 2);
    if (!s->blob->hglobal) s->blob->hglobal = heap_alloc((u32)std::max<size_t>(16, s->blob->data.size()), true);
    sync_hglobal(*s->blob);
    wr32(arg(1), s->blob->hglobal);
    RET(S_OK, 2);
}
STUB(ext_ole32_dll_CreateILockBytesOnHGlobal) {   // (hglobal, fDeleteOnRelease, ILockBytes **)
    init();
    auto b = std::make_shared<Blob>();
    if (arg(0)) { b->data.assign(MEM + arg(0), MEM + arg(0) + heap_size(arg(0))); b->hglobal = arg(0); }
    b->delete_on_release = arg(1) != 0;
    u32 o = com_object(g_vt_lockbytes);
    g_lockbytes[o] = b;
    wr32(arg(2), o);
    RET(S_OK, 3);
}
STUB(ext_ole32_dll_StgCreateDocfileOnILockBytes) {   // (ILockBytes*, mode, reserved, IStorage **)
    init();
    u32 o = new_storage(std::make_shared<Storage>(), arg(0));
    commit(g_storages[o]);
    wr32(arg(3), o);
    RET(S_OK, 4);
}
STUB(ext_ole32_dll_StgOpenStorageOnILockBytes) {   // (ILockBytes*, priority, mode, snbExclude, reserved, IStorage **)
    init();
    auto b = lockbytes(arg(0));
    auto st = std::make_shared<Storage>();
    if (!b || b->data.size() < 8 || std::memcmp(b->data.data(), STG_MAGIC, 8)) { wr32(arg(5), 0); RET(0x800300FCu, 6); }   // STG_E_FILEALREADYEXISTS-ish: no es un almacenamiento
    size_t p = 8;
    deserialize(b->data, p, *st);
    wr32(arg(5), new_storage(st, arg(0)));
    RET(S_OK, 6);
}
STUB(ext_ole32_dll_StgIsStorageILockBytes) {
    auto b = lockbytes(arg(0));
    RET(b && b->data.size() >= 8 && !std::memcmp(b->data.data(), STG_MAGIC, 8) ? S_OK : S_FALSE, 1);
}
STUB(ext_ole32_dll_StgCreateDocfile) {   // (nombre, modo, reservado, IStorage **)
    // sin nombre = almacenamiento temporal (OLEAUT32 lo usa como almacén interno): en memoria
    if (!arg(0)) { init(); wr32(arg(3), new_storage(std::make_shared<Storage>())); RET(S_OK, 4); }
    if (arg(3)) wr32(arg(3), 0);
    RET(STG_E_INVALIDFUNCTION, 4);   // en disco: no se usa
}
STUB(ext_ole32_dll_StgOpenStorage) { if (arg(5)) wr32(arg(5), 0); RET(STG_E_FILENOTFOUND, 6); }
STUB(ext_ole32_dll_ReadClassStg) {
    StorageObj *s = storage(arg(0));
    if (s) std::memcpy(MEM + arg(1), s->st->clsid, 16); else std::memset(MEM + arg(1), 0, 16);
    RET(S_OK, 2);
}
STUB(ext_ole32_dll_WriteClassStg) { if (StorageObj *s = storage(arg(0))) std::memcpy(s->st->clsid, MEM + arg(1), 16); RET(S_OK, 2); }
STUB(ext_ole32_dll_ReadClassStm) {   // el CLSID son los 16 primeros bytes del flujo
    Stream *s = stream(arg(0));
    if (!s || s->pos + 16 > s->blob->data.size()) RET(0x80030026u, 2);   // STG_E_READFAULT
    std::memcpy(MEM + arg(1), s->blob->data.data() + s->pos, 16);
    s->pos += 16;
    RET(S_OK, 2);
}
STUB(ext_ole32_dll_WriteClassStm) {
    Stream *s = stream(arg(0));
    if (!s) RET(E_INVALIDARG, 2);
    if (s->pos + 16 > s->blob->data.size()) s->blob->data.resize(s->pos + 16);
    std::memcpy(s->blob->data.data() + s->pos, MEM + arg(1), 16);
    s->pos += 16;
    sync_hglobal(*s->blob);
    RET(S_OK, 2);
}
STUB(ext_ole32_dll_WriteFmtUserTypeStg) { RET(S_OK, 3); }
STUB(ext_ole32_dll_ReleaseStgMedium) {   // STGMEDIUM { tymed, union, pUnkForRelease }
    u32 m = arg(0);
    u32 tymed = rd32(m), h = rd32(m + 4), unk = rd32(m + 8);
    if (unk) guest_call(rd32(rd32(unk) + 8), {unk});
    else if (tymed == 1 && h) heap_free(h);                                   // TYMED_HGLOBAL
    else if ((tymed == 4 || tymed == 8) && h) guest_call(rd32(rd32(h) + 8), {h});   // TYMED_ISTREAM / ISTORAGE
    RET(0, 1);
}

// ---------------------------------------------------------------- OLE: contenedores

STUB(ext_ole32_dll_CreateOleAdviseHolder) { init(); wr32(arg(0), com_object(g_vt_oah)); RET(S_OK, 1); }
STUB(ext_ole32_dll_CreateDataAdviseHolder) { init(); wr32(arg(0), com_object(g_vt_dah)); RET(S_OK, 1); }
STUB(ext_ole32_dll_OleCreateMenuDescriptor) { RET(heap_alloc(16, true), 2); }
STUB(ext_ole32_dll_OleDestroyMenuDescriptor) { heap_free(arg(0)); RET(S_OK, 1); }
STUB(ext_ole32_dll_OleSetMenuDescriptor) { RET(S_OK, 5); }
STUB(ext_ole32_dll_OleTranslateAccelerator) { RET(S_FALSE, 3); }
STUB(ext_ole32_dll_IsAccelerator) { RET(0, 4); }
STUB(ext_ole32_dll_RegisterDragDrop) { RET(S_OK, 2); }
STUB(ext_ole32_dll_RevokeDragDrop) { RET(S_OK, 1); }
STUB(ext_ole32_dll_DoDragDrop) { if (arg(3)) wr32(arg(3), 0); RET(DRAGDROP_S_CANCEL, 4); }
STUB(ext_ole32_dll_OleIsRunning) { RET(1, 1); }
STUB(ext_ole32_dll_OleLockRunning) { RET(S_OK, 3); }
STUB(ext_ole32_dll_OleRun) { RET(S_OK, 1); }
STUB(ext_ole32_dll_OleRegGetUserType) {   // (clsid, form, LPOLESTR *)
    bool found = false;
    std::string n = reg_query_default(0x80000000u, "CLSID\\" + guid_text(arg(0)), &found);
    if (!found) { wr32(arg(2), 0); RET(REGDB_E_CLASSNOTREG, 3); }
    wr32(arg(2), co_wstring(n));
    RET(S_OK, 3);
}
STUB(ext_ole32_dll_OleGetIconOfClass) { RET(0, 3); }
STUB(ext_ole32_dll_OleGetAutoConvert) { RET(0x80040152u, 2); }   // REGDB_E_KEYMISSING
STUB(ext_ole32_dll_OleDoAutoConvert) { RET(S_OK, 2); }
STUB(ext_ole32_dll_OleDuplicateData) { RET(0, 3); }
// no hay portapapeles OLE ni monikers ni objetos enlazados/incrustados de otros programas
STUB(ext_ole32_dll_OleGetClipboard) { wr32(arg(0), 0); RET(CLIPBRD_E_CANT_OPEN, 1); }
STUB(ext_ole32_dll_OleSetClipboard) { RET(S_OK, 1); }
STUB(ext_ole32_dll_OleFlushClipboard) { RET(S_OK, 0); }
STUB(ext_ole32_dll_OleIsCurrentClipboard) { RET(S_FALSE, 1); }
STUB(ext_ole32_dll_OleQueryCreateFromData) { RET(S_FALSE, 1); }
STUB(ext_ole32_dll_OleQueryLinkFromData) { RET(S_FALSE, 1); }
STUB(ext_ole32_dll_OleCreateFromData) { wr32(arg(6), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 7); }
STUB(ext_ole32_dll_OleCreateFromFile) { wr32(arg(7), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 8); }
STUB(ext_ole32_dll_OleCreateLink) { wr32(arg(6), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 7); }
STUB(ext_ole32_dll_OleCreateLinkFromData) { wr32(arg(6), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 7); }
STUB(ext_ole32_dll_OleCreateLinkToFile) { wr32(arg(6), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 7); }
STUB(ext_ole32_dll_OleLoad) { wr32(arg(3), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 4); }
STUB(ext_ole32_dll_OleSave) { plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 3); }
// Objeto guardado en un flujo: su CLSID (16 bytes) y lo que escriba su IPersistStream::Save. El flujo
// puede ser del invitado: se usa por su vtable (Read = 3, Write = 4).
const ComGuid IID_IPersistStream = {0x00000109, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
STUB(ext_ole32_dll_OleLoadFromStream) {   // (IStream*, iid, void **)
    init();
    u32 stm = arg(0), iid = arg(1), out = arg(2);
    wr32(out, 0);
    u32 buf = heap_alloc(32, true);
    u32 hr = guest_call(rd32(rd32(stm) + 12), {stm, buf, 16, buf + 16});   // Read del CLSID
    if (hr == S_OK && rd32(buf + 16) == 16) hr = create_instance(buf, 0, iid, out);
    else hr = 0x80030026u;   // STG_E_READFAULT
    if (hr == S_OK) {
        u32 obj = rd32(out), ps_iid = heap_alloc(16), ps = heap_alloc(4, true);
        std::memcpy(MEM + ps_iid, &IID_IPersistStream, 16);
        hr = guest_call(rd32(rd32(obj)), {obj, ps_iid, ps});                // QueryInterface
        if (hr == S_OK) {
            u32 p = rd32(ps);
            hr = guest_call(rd32(rd32(p) + 20), {p, stm});                   // IPersistStream::Load
            guest_call(rd32(rd32(p) + 8), {p});
        }
        if (hr != S_OK) { guest_call(rd32(rd32(obj) + 8), {obj}); wr32(out, 0); }
        heap_free(ps_iid);
        heap_free(ps);
    }
    if (hr) plog("OleLoadFromStream(%s) -> 0x%x", guid_text(buf).c_str(), hr);
    heap_free(buf);
    RET(hr, 3);
}
STUB(ext_ole32_dll_OleSaveToStream) {   // (IPersistStream*, IStream*)
    u32 p = arg(0), stm = arg(1);
    if (!p) RET(E_POINTER, 2);
    u32 buf = heap_alloc(32, true);
    u32 hr = guest_call(rd32(rd32(p) + 12), {p, buf});                      // GetClassID
    if (hr == S_OK) hr = guest_call(rd32(rd32(stm) + 16), {stm, buf, 16, buf + 16});   // Write
    if (hr == S_OK) hr = guest_call(rd32(rd32(p) + 24), {p, stm, 1});       // Save(clearDirty)
    heap_free(buf);
    RET(hr, 2);
}
STUB(ext_ole32_dll_OleConvertIStorageToOLESTREAM) { plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 2); }
STUB(ext_ole32_dll_OleConvertOLESTREAMToIStorage) { plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 3); }
STUB(ext_ole32_dll_CreateBindCtx) { wr32(arg(1), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 2); }
STUB(ext_ole32_dll_CreateItemMoniker) { wr32(arg(2), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 3); }
STUB(ext_ole32_dll_MkParseDisplayName) { wr32(arg(3), 0); RET(MK_E_UNAVAILABLE, 4); }
STUB(ext_ole32_dll_BindMoniker) { wr32(arg(3), 0); RET(MK_E_UNAVAILABLE, 4); }
STUB(ext_ole32_dll_GetRunningObjectTable) { wr32(arg(1), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 2); }
STUB(ext_ole32_dll_GetClassFile) { RET(MK_E_UNAVAILABLE, 2); }
STUB(ext_ole32_dll_CoMarshalInterface) { plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 6); }
STUB(ext_ole32_dll_CoUnmarshalInterface) { wr32(arg(2), 0); plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 3); }
STUB(ext_ole32_dll_CoMarshalHresult) { plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 2); }
STUB(ext_ole32_dll_CoUnmarshalHresult) { plog_once(__func__, "%s: no implementado", __func__); RET(E_NOTIMPL, 2); }

// ---------------------------------------------------------------- IErrorInfo (SetErrorInfo, GetErrorInfo, CreateErrorInfo)
//
// En Windows las implementa OLE32 y OLEAUT32 las reexporta pidiéndoselas a OLE32. El objeto de
// CreateErrorInfo da dos interfaces: ICreateErrorInfo en +0 e IErrorInfo en +8 (this - 8 = objeto).

namespace {

const ComGuid IID_IErrorInfo = {0x1CF2B120, 0x547D, 0x101B, {0x8E, 0x65, 0x08, 0x00, 0x2B, 0x2B, 0xD1, 0x19}};
const ComGuid IID_ICreateErrorInfo = {0x22F03340, 0x547D, 0x101B, {0x8E, 0x65, 0x08, 0x00, 0x2B, 0x2B, 0xD1, 0x19}};

struct ErrInfo { u8 guid[16] = {}; std::u16string source, desc, helpfile; u32 helpctx = 0; };
std::map<u32, ErrInfo> g_errinfo;   // por objeto
thread_local u32 g_cur_errinfo = 0;
u32 g_vt_cei = 0, g_vt_ei = 0;

std::u16string wget(u32 p) {
    std::u16string s;
    if (p) for (u32 i = 0;; i++) { char16_t c = (char16_t)rd16(p + 2 * i); if (!c) break; s.push_back(c); }
    return s;
}
// BSTR: longitud en bytes delante; se libera con SysFreeString (IMalloc de CoGetMalloc)
u32 bstr(const std::u16string &s) {
    u32 n = 2 * (u32)s.size();
    u32 p = heap_alloc(4 + n + 2);
    wr32(p, n);
    for (u32 i = 0; i < s.size(); i++) wr16(p + 4 + 2 * i, s[i]);
    wr16(p + 4 + n, 0);
    return p + 4;
}

u32 ei_query(u32 obj, u32 iid, u32 out) {
    if (com_guid_eq(iid, IID_IUnknown) || com_guid_eq(iid, IID_ICreateErrorInfo)) { com_addref(obj); wr32(out, obj); return S_OK; }
    if (com_guid_eq(iid, IID_IErrorInfo)) { com_addref(obj); wr32(out, obj + 8); return S_OK; }
    wr32(out, 0);
    return E_NOINTERFACE;
}
u32 ei_release(u32 obj) {
    u32 n = com_release(obj);
    if (!n) { g_errinfo.erase(obj); heap_free(obj); }
    return n;
}
// ICreateErrorInfo (this = objeto)
void cei_QueryInterface() { TRACE_COM(); RET(ei_query(arg(0), arg(1), arg(2)), 3); }
void cei_AddRef() { TRACE_COM(); RET(com_addref(arg(0)), 1); }
void cei_Release() { TRACE_COM(); RET(ei_release(arg(0)), 1); }
void cei_SetGUID() { TRACE_COM(); std::memcpy(g_errinfo[arg(0)].guid, MEM + arg(1), 16); RET(S_OK, 2); }
void cei_SetSource() { TRACE_COM(); g_errinfo[arg(0)].source = wget(arg(1)); RET(S_OK, 2); }
void cei_SetDescription() { TRACE_COM(); g_errinfo[arg(0)].desc = wget(arg(1)); RET(S_OK, 2); }
void cei_SetHelpFile() { TRACE_COM(); g_errinfo[arg(0)].helpfile = wget(arg(1)); RET(S_OK, 2); }
void cei_SetHelpContext() { TRACE_COM(); g_errinfo[arg(0)].helpctx = arg(1); RET(S_OK, 2); }
// IErrorInfo (this = objeto + 8)
void ei_QueryInterface() { TRACE_COM(); RET(ei_query(arg(0) - 8, arg(1), arg(2)), 3); }
void ei_AddRef() { TRACE_COM(); RET(com_addref(arg(0) - 8), 1); }
void ei_Release() { TRACE_COM(); RET(ei_release(arg(0) - 8), 1); }
void ei_GetGUID() { TRACE_COM(); std::memcpy(MEM + arg(1), g_errinfo[arg(0) - 8].guid, 16); RET(S_OK, 2); }
void ei_GetSource() { TRACE_COM(); wr32(arg(1), bstr(g_errinfo[arg(0) - 8].source)); RET(S_OK, 2); }
void ei_GetDescription() { TRACE_COM(); wr32(arg(1), bstr(g_errinfo[arg(0) - 8].desc)); RET(S_OK, 2); }
void ei_GetHelpFile() { TRACE_COM(); wr32(arg(1), bstr(g_errinfo[arg(0) - 8].helpfile)); RET(S_OK, 2); }
void ei_GetHelpContext() { TRACE_COM(); wr32(arg(1), g_errinfo[arg(0) - 8].helpctx); RET(S_OK, 2); }

// llamada a un método COM del invitado (el objeto puede ser de VB, no nuestro)
u32 com_call(u32 obj, u32 slot, std::initializer_list<u32> rest) {
    std::vector<u32> a{obj};
    a.insert(a.end(), rest.begin(), rest.end());
    u32 fn = rd32(rd32(obj) + 4 * slot);
    switch (a.size()) {
    case 1: return guest_call(fn, {a[0]});
    case 2: return guest_call(fn, {a[0], a[1]});
    default: return guest_call(fn, {a[0], a[1], a[2]});
    }
}

}  // namespace

STUB(ext_ole32_dll_CreateErrorInfo) {
    if (!g_vt_cei) {
        g_vt_cei = com_vtbl({cei_QueryInterface, cei_AddRef, cei_Release, cei_SetGUID, cei_SetSource, cei_SetDescription,
                             cei_SetHelpFile, cei_SetHelpContext});
        g_vt_ei = com_vtbl({ei_QueryInterface, ei_AddRef, ei_Release, ei_GetGUID, ei_GetSource, ei_GetDescription,
                            ei_GetHelpFile, ei_GetHelpContext});
    }
    u32 o = com_object(g_vt_cei, 8);
    wr32(o + 8, g_vt_ei);
    g_errinfo[o];
    wr32(arg(0), o);
    RET(S_OK, 1);
}
STUB(ext_ole32_dll_SetErrorInfo) {   // (reservado, IErrorInfo*)
    u32 nu = arg(1);
    if (nu) com_call(nu, 1, {});                     // AddRef
    if (g_cur_errinfo) com_call(g_cur_errinfo, 2, {});   // Release
    g_cur_errinfo = nu;
    RET(S_OK, 2);
}
STUB(ext_ole32_dll_GetErrorInfo) {   // (reservado, IErrorInfo**): se lleva la referencia y lo borra
    wr32(arg(1), g_cur_errinfo);
    u32 had = g_cur_errinfo;
    g_cur_errinfo = 0;
    RET(had ? S_OK : S_FALSE, 2);
}

// VB y OLEAUT32 las piden con GetProcAddress(ole32.dll, …)
void register_dynamic_export(const char *dll, const char *name, void (*f)(void));
namespace {
struct Registrar {
    Registrar() {
        register_dynamic_export("ole32.dll", "CreateErrorInfo", ext_ole32_dll_CreateErrorInfo);
        register_dynamic_export("ole32.dll", "SetErrorInfo", ext_ole32_dll_SetErrorInfo);
        register_dynamic_export("ole32.dll", "GetErrorInfo", ext_ole32_dll_GetErrorInfo);
    }
} g_registrar;
}  // namespace
