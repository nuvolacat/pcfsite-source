// PCF7 — registro de Windows (ADVAPI32 Reg*): un árbol en memoria guardado en un fichero de texto.
//
// MANAGER solo mira si existe la clave de instalación; ProQuinielas (Visual Basic 5) lo usa de verdad:
// los controles OCX se registran (DllRegisterServer) escribiendo CLSID\{…}\InprocServer32, OLE32 los
// busca ahí, y VB guarda la configuración en HKCU\Software\VB and VBA Program Settings. El fichero
// ($PCF7_REGISTRY, por defecto <carpeta de trabajo del juego>/registro.txt) se reescribe en cada cambio.
//
// Formato: una clave por línea "[RUTA\COMPLETA]" y sus valores "nombre=tipo:hex" (nombre vacío = el
// valor por defecto). Las ramas: HKEY_CLASSES_ROOT es HKEY_LOCAL_MACHINE\Software\Classes.
#include "plat.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <strings.h>
#include <vector>

void set_last_error(u32 e);

namespace {

constexpr u32 ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_INVALID_HANDLE = 6, ERROR_MORE_DATA = 234,
              ERROR_NO_MORE_ITEMS = 259, ERROR_INVALID_PARAMETER = 87;
constexpr u32 HKCR = 0x80000000u, HKCU = 0x80000001u, HKLM = 0x80000002u, HKU = 0x80000003u, HKCC = 0x80000005u;

struct ILess {
    bool operator()(const std::string &a, const std::string &b) const {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    }
};
struct Value { u32 type = 1; std::vector<u8> data; };
struct Key {
    std::map<std::string, std::unique_ptr<Key>, ILess> sub;
    std::map<std::string, Value, ILess> values;
    std::vector<std::string> sub_order, value_order;   // orden de creación (RegEnum*)
};

Key g_root;                               // hijos: HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, HKEY_USERS…
std::map<u32, Key *> g_handles;
u32 g_next = 0x100;
bool g_loaded = false;

std::string reg_path() {
    if (const char *p = std::getenv("PCF7_REGISTRY")) return p;
#ifdef __EMSCRIPTEN__
    return "/pcf7/rw/registro.txt";   // copia del de partida (web.cpp)
#else
    return juego_trabajo() + "/registro.txt";
#endif
}

Key *child(Key *k, const std::string &name, bool create) {
    auto it = k->sub.find(name);
    if (it != k->sub.end()) return it->second.get();
    if (!create) return nullptr;
    k->sub_order.push_back(name);
    return (k->sub[name] = std::make_unique<Key>()).get();
}

std::vector<std::string> split(const std::string &path) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : path) {
        if (c == '\\') { if (!cur.empty()) r.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

Key *walk(Key *k, const std::string &path, bool create) {
    for (auto &p : split(path)) {
        k = child(k, p, create);
        if (!k) return nullptr;
    }
    return k;
}

void save();

void load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(reg_path());
    std::string line;
    Key *cur = nullptr;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '[' && line.back() == ']') { cur = walk(&g_root, line.substr(1, line.size() - 2), true); continue; }
        size_t eq = line.find('='), colon = line.find(':', eq);
        if (!cur || eq == std::string::npos || colon == std::string::npos) continue;
        Value v;
        v.type = (u32)std::strtoul(line.substr(eq + 1, colon - eq - 1).c_str(), nullptr, 10);
        for (size_t i = colon + 1; i + 1 < line.size(); i += 2) v.data.push_back((u8)std::strtoul(line.substr(i, 2).c_str(), nullptr, 16));
        std::string name = line.substr(0, eq);
        if (!cur->values.count(name)) cur->value_order.push_back(name);
        cur->values[name] = v;
    }
}

void dump(std::ostream &o, const Key &k, const std::string &path) {
    if (!path.empty() && (!k.values.empty() || k.sub.empty())) {
        o << "[" << path << "]\n";
        for (auto &n : k.value_order) {
            auto it = k.values.find(n);
            if (it == k.values.end()) continue;
            o << n << "=" << it->second.type << ":";
            char b[3];
            for (u8 c : it->second.data) { snprintf(b, sizeof b, "%02x", c); o << b; }
            o << "\n";
        }
    }
    for (auto &n : k.sub_order) {
        auto it = k.sub.find(n);
        if (it != k.sub.end()) dump(o, *it->second, path.empty() ? n : path + "\\" + n);
    }
}

void save() {
    {
        std::ofstream out(reg_path());
        dump(out, g_root, "");
    }
    web_sucio(reg_path());
}

Key *root_key(u32 h) {
    load();
    switch (h) {
    case HKLM: return child(&g_root, "HKEY_LOCAL_MACHINE", true);
    case HKCU: return child(&g_root, "HKEY_CURRENT_USER", true);
    case HKU: return child(&g_root, "HKEY_USERS", true);
    case HKCC: return child(&g_root, "HKEY_CURRENT_CONFIG", true);
    case HKCR: return walk(child(&g_root, "HKEY_LOCAL_MACHINE", true), "Software\\Classes", true);
    }
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

u32 new_handle(Key *k) { u32 h = g_next; g_next += 4; g_handles[h] = k; return h; }

std::string wstr(u32 p) {
    std::string s;
    for (u32 i = 0; p && i < 4096; i++) { u32 c = rd16(p + 2 * i); if (!c) break; s.push_back(c < 256 ? (char)c : '?'); }
    return s;
}
std::string sname(u32 p, bool wide) { return p ? (wide ? wstr(p) : gstr(p)) : std::string(); }

u32 open_key(u32 h, u32 sub, u32 out, bool create, bool wide) {
    Key *k = root_key(h);
    if (!k) return ERROR_INVALID_HANDLE;
    std::string path = sname(sub, wide);
    Key *r = path.empty() ? k : walk(k, path, create);
    if (!r) {
        static bool tr = std::getenv("PCF7_TRACE_REG") != nullptr;
        if (tr) plog("registro: no existe 0x%x\\%s", h, path.c_str());
        if (out) wr32(out, 0);
        return ERROR_FILE_NOT_FOUND;
    }
    if (out) wr32(out, new_handle(r));
    if (create) save();
    return ERROR_SUCCESS;
}

// datos de un valor como los devuelve la versión A o W (REG_SZ/EXPAND_SZ/MULTI_SZ se convierten)
std::vector<u8> as_returned(const Value &v, bool wide) {
    bool str = v.type == 1 || v.type == 2 || v.type == 7;
    if (!str || !wide) return v.data;
    std::vector<u8> r;
    for (u8 c : v.data) { r.push_back(c); r.push_back(0); }
    return r;
}

u32 query(Key *k, const std::string &name, u32 ptype, u32 pdata, u32 pcb, bool wide) {
    auto it = k->values.find(name);
    if (it == k->values.end()) {
        // RegQueryValue de una clave sin valor por defecto: cadena vacía
        if (!name.empty()) return ERROR_FILE_NOT_FOUND;
        Value v;
        v.data = {0};
        it = k->values.emplace(name, v).first;
        k->value_order.push_back(name);
    }
    std::vector<u8> d = as_returned(it->second, wide);
    if (ptype) wr32(ptype, it->second.type);
    u32 cap = pcb ? rd32(pcb) : 0;
    if (pcb) wr32(pcb, (u32)d.size());
    if (!pdata) return ERROR_SUCCESS;
    if (cap < d.size()) return ERROR_MORE_DATA;
    if (!d.empty()) std::memcpy(MEM + pdata, d.data(), d.size());
    return ERROR_SUCCESS;
}

u32 set_value(Key *k, const std::string &name, u32 type, u32 data, u32 cb, bool wide) {
    Value v;
    v.type = type;
    bool str = type == 1 || type == 2 || type == 7;
    if (str && wide) {
        for (u32 i = 0; i + 1 < cb || (cb == 0 && i < 8192); i += 2) {
            u32 c = rd16(data + i);
            v.data.push_back(c < 256 ? (u8)c : '?');
            if (!c && type != 7) break;
        }
        if (v.data.empty() || v.data.back()) v.data.push_back(0);
    } else if (str && cb == 0) {
        std::string s = gstr(data);
        v.data.assign(s.begin(), s.end());
        v.data.push_back(0);
    } else {
        v.data.assign(MEM + data, MEM + data + cb);
        if (str && (v.data.empty() || v.data.back())) v.data.push_back(0);
    }
    if (!k->values.count(name)) k->value_order.push_back(name);
    k->values[name] = v;
    save();
    return ERROR_SUCCESS;
}

bool delete_key(Key *parent, const std::string &path) {
    auto parts = split(path);
    if (parts.empty()) return false;
    std::string last = parts.back();
    parts.pop_back();
    Key *p = parent;
    for (auto &x : parts) { p = child(p, x, false); if (!p) return false; }
    auto it = p->sub.find(last);
    if (it == p->sub.end()) return false;
    Key *gone = it->second.get();
    for (auto h = g_handles.begin(); h != g_handles.end();) h = h->second == gone ? g_handles.erase(h) : std::next(h);
    for (auto o = p->sub_order.begin(); o != p->sub_order.end(); ++o)
        if (!strcasecmp(o->c_str(), last.c_str())) { p->sub_order.erase(o); break; }
    p->sub.erase(it);
    save();
    return true;
}

u32 put_name(u32 buf, u32 cap, const std::string &s, bool wide) {   // cap en caracteres, con el nulo
    if (s.size() + 1 > cap) return ERROR_MORE_DATA;
    for (size_t i = 0; i <= s.size(); i++) {
        u32 c = i < s.size() ? (u8)s[i] : 0;
        if (wide) wr16(buf + 2 * (u32)i, c); else wr8(buf + (u32)i, c);
    }
    return ERROR_SUCCESS;
}

const std::string *nth_sub(Key *k, u32 i) {
    if (i >= k->sub_order.size()) return nullptr;
    return &k->sub_order[i];
}

}  // namespace

// consulta directa para OLE32 (ole32.cpp): valor por defecto de <raíz>\<ruta>
std::string reg_query_default(u32 root, const std::string &path, bool *found) {
    Key *k = root_key(root);
    if (k) k = walk(k, path, false);
    *found = k != nullptr;
    if (!k) return "";
    auto it = k->values.find("");
    if (it == k->values.end()) return "";
    std::string s(it->second.data.begin(), it->second.data.end());
    while (!s.empty() && !s.back()) s.pop_back();
    return s;
}

// escritura directa del valor por defecto (cadena) de <raíz>\<ruta>, creando la clave
void reg_set_default(u32 root, const std::string &path, const std::string &value) {
    Key *k = root_key(root);
    if (k) k = walk(k, path, true);
    if (!k) return;
    Value v;
    v.data.assign(value.begin(), value.end());
    v.data.push_back(0);
    if (!k->values.count("")) k->value_order.push_back("");
    k->values[""] = v;
    save();
}

// ---------------------------------------------------------------- claves

STUB(ext_advapi32_dll_RegOpenKeyExA) { RET(open_key(arg(0), arg(1), arg(4), false, false), 5); }
STUB(ext_advapi32_dll_RegOpenKeyExW) { RET(open_key(arg(0), arg(1), arg(4), false, true), 5); }
STUB(ext_advapi32_dll_RegOpenKeyA) { RET(open_key(arg(0), arg(1), arg(2), false, false), 3); }
STUB(ext_advapi32_dll_RegOpenKeyW) { RET(open_key(arg(0), arg(1), arg(2), false, true), 3); }
STUB(ext_advapi32_dll_RegCreateKeyA) { RET(open_key(arg(0), arg(1), arg(2), true, false), 3); }
STUB(ext_advapi32_dll_RegCreateKeyW) { RET(open_key(arg(0), arg(1), arg(2), true, true), 3); }
STUB(ext_advapi32_dll_RegCreateKeyExA) {   // (hkey, sub, reserved, class, options, sam, sa, phk, disposition)
    Key *k = root_key(arg(0));
    bool existed = k && walk(k, gstr(arg(1)), false);
    u32 r = open_key(arg(0), arg(1), arg(7), true, false);
    if (arg(8)) wr32(arg(8), existed ? 2 : 1);   // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
    RET(r, 9);
}
STUB(ext_advapi32_dll_RegCreateKeyExW) {
    Key *k = root_key(arg(0));
    bool existed = k && walk(k, wstr(arg(1)), false);
    u32 r = open_key(arg(0), arg(1), arg(7), true, true);
    if (arg(8)) wr32(arg(8), existed ? 2 : 1);
    RET(r, 9);
}
STUB(ext_advapi32_dll_RegCloseKey) { g_handles.erase(arg(0)); RET(ERROR_SUCCESS, 1); }
STUB(ext_advapi32_dll_RegFlushKey) { RET(ERROR_SUCCESS, 1); }
STUB(ext_advapi32_dll_RegDeleteKeyA) { Key *k = root_key(arg(0)); RET(k && delete_key(k, gstr(arg(1))) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND, 2); }
STUB(ext_advapi32_dll_RegDeleteKeyW) { Key *k = root_key(arg(0)); RET(k && delete_key(k, wstr(arg(1))) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND, 2); }

// ---------------------------------------------------------------- valores

STUB(ext_advapi32_dll_RegQueryValueExA) {   // (hkey, name, reserved, type, data, cb)
    Key *k = root_key(arg(0));
    RET(k ? query(k, sname(arg(1), false), arg(3), arg(4), arg(5), false) : ERROR_INVALID_HANDLE, 6);
}
STUB(ext_advapi32_dll_RegQueryValueExW) {
    Key *k = root_key(arg(0));
    RET(k ? query(k, sname(arg(1), true), arg(3), arg(4), arg(5), true) : ERROR_INVALID_HANDLE, 6);
}
STUB(ext_advapi32_dll_RegQueryValueA) {     // (hkey, sub, data, pcb): valor por defecto de la subclave
    Key *k = root_key(arg(0));
    if (k && arg(1)) k = walk(k, gstr(arg(1)), false);
    RET(k ? query(k, "", 0, arg(2), arg(3), false) : ERROR_FILE_NOT_FOUND, 4);
}
STUB(ext_advapi32_dll_RegQueryValueW) {
    Key *k = root_key(arg(0));
    if (k && arg(1)) k = walk(k, wstr(arg(1)), false);
    RET(k ? query(k, "", 0, arg(2), arg(3), true) : ERROR_FILE_NOT_FOUND, 4);
}
STUB(ext_advapi32_dll_RegSetValueExA) {     // (hkey, name, reserved, type, data, cb)
    Key *k = root_key(arg(0));
    RET(k ? set_value(k, sname(arg(1), false), arg(3), arg(4), arg(5), false) : ERROR_INVALID_HANDLE, 6);
}
STUB(ext_advapi32_dll_RegSetValueExW) {
    Key *k = root_key(arg(0));
    RET(k ? set_value(k, sname(arg(1), true), arg(3), arg(4), arg(5), true) : ERROR_INVALID_HANDLE, 6);
}
STUB(ext_advapi32_dll_RegSetValueA) {       // (hkey, sub, type REG_SZ, data, cb): valor por defecto de la subclave
    Key *k = root_key(arg(0));
    if (k && arg(1)) k = walk(k, gstr(arg(1)), true);
    RET(k ? set_value(k, "", 1, arg(3), 0, false) : ERROR_INVALID_HANDLE, 5);
}
STUB(ext_advapi32_dll_RegSetValueW) {
    Key *k = root_key(arg(0));
    if (k && arg(1)) k = walk(k, wstr(arg(1)), true);
    RET(k ? set_value(k, "", 1, arg(3), 0, true) : ERROR_INVALID_HANDLE, 5);
}
STUB(ext_advapi32_dll_RegDeleteValueA) {
    Key *k = root_key(arg(0));
    std::string n = sname(arg(1), false);
    bool ok = k && k->values.erase(n);
    if (ok) save();
    RET(ok ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND, 2);
}
STUB(ext_advapi32_dll_RegDeleteValueW) {
    Key *k = root_key(arg(0));
    std::string n = sname(arg(1), true);
    bool ok = k && k->values.erase(n);
    if (ok) save();
    RET(ok ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND, 2);
}

// ---------------------------------------------------------------- enumeración

STUB(ext_advapi32_dll_RegEnumKeyA) {   // (hkey, index, name, cch)
    Key *k = root_key(arg(0));
    const std::string *n = k ? nth_sub(k, arg(1)) : nullptr;
    RET(n ? put_name(arg(2), arg(3), *n, false) : ERROR_NO_MORE_ITEMS, 4);
}
STUB(ext_advapi32_dll_RegEnumKeyW) {
    Key *k = root_key(arg(0));
    const std::string *n = k ? nth_sub(k, arg(1)) : nullptr;
    RET(n ? put_name(arg(2), arg(3), *n, true) : ERROR_NO_MORE_ITEMS, 4);
}
STUB(ext_advapi32_dll_RegEnumKeyExA) {   // (hkey, index, name, pcch, reserved, class, pcchclass, filetime)
    Key *k = root_key(arg(0));
    const std::string *n = k ? nth_sub(k, arg(1)) : nullptr;
    if (!n) RET(ERROR_NO_MORE_ITEMS, 8);
    u32 r = put_name(arg(2), rd32(arg(3)), *n, false);
    if (r == ERROR_SUCCESS) wr32(arg(3), (u32)n->size());
    if (arg(6)) wr32(arg(6), 0);
    if (arg(5)) wr8(arg(5), 0);
    if (arg(7)) { wr32(arg(7), 0); wr32(arg(7) + 4, 0); }
    RET(r, 8);
}
static u32 enum_value(bool wide) {   // (hkey, index, name, pcchname, reserved, type, data, pcbdata)
    Key *k = root_key(arg(0));
    if (!k || arg(1) >= k->value_order.size()) return ERROR_NO_MORE_ITEMS;
    const std::string &n = k->value_order[arg(1)];
    u32 r = put_name(arg(2), rd32(arg(3)), n, wide);
    if (r != ERROR_SUCCESS) return r;
    wr32(arg(3), (u32)n.size());
    if (!arg(6) && !arg(7) && !arg(5)) return ERROR_SUCCESS;
    return query(k, n, arg(5), arg(6), arg(7), wide);
}
STUB(ext_advapi32_dll_RegEnumValueA) { RET(enum_value(false), 8); }
STUB(ext_advapi32_dll_RegEnumValueW) { RET(enum_value(true), 8); }
STUB(ext_advapi32_dll_RegQueryInfoKeyA) {   // (hkey, class, pcclass, reserved, psubkeys, pmaxsub, pmaxclass, pvalues, pmaxname, pmaxdata, psec, ptime)
    Key *k = root_key(arg(0));
    if (!k) RET(ERROR_INVALID_HANDLE, 12);
    u32 maxsub = 0, maxname = 0, maxdata = 0;
    for (auto &s : k->sub_order) maxsub = std::max(maxsub, (u32)s.size());
    for (auto &[n, v] : k->values) { maxname = std::max(maxname, (u32)n.size()); maxdata = std::max(maxdata, (u32)v.data.size()); }
    if (arg(1)) wr8(arg(1), 0);
    if (arg(2)) wr32(arg(2), 0);
    if (arg(4)) wr32(arg(4), (u32)k->sub.size());
    if (arg(5)) wr32(arg(5), maxsub);
    if (arg(6)) wr32(arg(6), 0);
    if (arg(7)) wr32(arg(7), (u32)k->values.size());
    if (arg(8)) wr32(arg(8), maxname);
    if (arg(9)) wr32(arg(9), maxdata);
    if (arg(10)) wr32(arg(10), 0);
    if (arg(11)) { wr32(arg(11), 0); wr32(arg(11) + 4, 0); }
    RET(ERROR_SUCCESS, 12);
}

// ---------------------------------------------------------------- seguridad y registro de eventos (no hay)

STUB(ext_advapi32_dll_OpenProcessToken) { if (arg(2)) wr32(arg(2), 0x3000); RET(1, 3); }
STUB(ext_advapi32_dll_LookupPrivilegeValueA) { if (arg(2)) { wr32(arg(2), 20); wr32(arg(2) + 4, 0); } RET(1, 3); }
STUB(ext_advapi32_dll_AdjustTokenPrivileges) { RET(1, 6); }
STUB(ext_advapi32_dll_InitializeSecurityDescriptor) { if (arg(0)) std::memset(MEM + arg(0), 0, 20); RET(1, 2); }
STUB(ext_advapi32_dll_SetSecurityDescriptorDacl) { RET(1, 4); }
STUB(ext_advapi32_dll_RegisterEventSourceA) { RET(0x3004, 2); }
STUB(ext_advapi32_dll_DeregisterEventSource) { RET(1, 1); }
STUB(ext_advapi32_dll_ReportEventA) { RET(1, 9); }
