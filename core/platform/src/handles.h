// PCF7 — tabla de objetos del núcleo (HANDLE de Windows): ficheros, búsquedas, eventos, mapeos…
#pragma once
#include "plat.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct KObj { virtual ~KObj() = default; };

struct KFile : KObj {
    int fd = -1;
    std::string path;      // ruta del host
    bool escribe = false;  // abierto para escribir (web: la partida se guarda al cerrarlo)
    ~KFile() override;
};

struct KFindEntry { std::string name; u32 attrs; u64 size; i64 mtime; };
struct KFind : KObj {
    std::vector<KFindEntry> entries;
    size_t next = 0;
};

struct KEvent : KObj {
    bool manual = false, signaled = false;
    std::string name;
};

struct KMapping : KObj {
    std::shared_ptr<KFile> file;
    u32 size = 0;
    bool escribe = false;   // PAGE_READWRITE: las vistas de escritura se vuelcan al fichero al desmapear
};

class Handles {
public:
    u32 add(std::shared_ptr<KObj> o) {
        u32 h = next_;
        next_ += 4;
        objs_[h] = std::move(o);
        return h;
    }
    template <class T> std::shared_ptr<T> get(u32 h) {
        auto it = objs_.find(h);
        return it == objs_.end() ? nullptr : std::dynamic_pointer_cast<T>(it->second);
    }
    void close(u32 h) { objs_.erase(h); }

private:
    u32 next_ = 0x100;
    std::map<u32, std::shared_ptr<KObj>> objs_;
};

Handles &handles();
void set_last_error(u32 e);
u32 ticks_ms();
u64 unix_to_filetime(i64 secs);
u8 cp1252_upper(u8 c);
u8 cp1252_lower(u8 c);
