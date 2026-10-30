// PCF7 — WINMM (tiempo, mmio), COMDLG32. El joystick de WINMM está en user32.cpp (son mensajes de ventana).
#include "handles.h"

#include <fcntl.h>
#include <unistd.h>

u32 ticks_ms();

// ---------------------------------------------------------------- WINMM: tiempo

STUB(ext_winmm_dll_timeGetTime) { RET(ticks_ms(), 0); }
STUB(ext_winmm_dll_timeGetSystemTime) {
    u32 p = arg(0);
    wr32(p, 1); wr32(p + 4, ticks_ms());   // MMTIME { wType = TIME_MS, ms }
    RET(0, 2);
}
STUB(ext_winmm_dll_timeBeginPeriod) { RET(0, 1); }
STUB(ext_winmm_dll_timeEndPeriod) { RET(0, 1); }

// ---------------------------------------------------------------- WINMM: mmio (lectura de RIFF/WAV)
// HMMIO = handle de un KFile. MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags.

namespace {
u32 fourcc(const char *s) { return (u32)(u8)s[0] | (u32)(u8)s[1] << 8 | (u32)(u8)s[2] << 16 | (u32)(u8)s[3] << 24; }
}

STUB(ext_winmm_dll_mmioOpenA) {
    std::string w = gstr(arg(0));
    u32 flags = arg(2);
    if (flags & 0xFFFF0000u & ~0x00010000u) plog_once("mmioflags", "mmioOpenA: flags 0x%x", flags);
    int fd = ::open(((flags & 3) ? host_path_w(w) : host_path(w)).c_str(), ((flags & 3) ? O_RDWR : O_RDONLY) | O_BINARY);
    if (fd < 0) { plog("mmioOpenA(\"%s\") -> no existe", w.c_str()); RET(0, 3); }
    auto f = std::make_shared<KFile>();
    f->fd = fd;
    f->path = w;
    RET(handles().add(f), 3);
}
STUB(ext_winmm_dll_mmioClose) { handles().close(arg(0)); RET(0, 2); }
STUB(ext_winmm_dll_mmioRead) {
    auto f = handles().get<KFile>(arg(0));
    if (!f) RET(0xFFFFFFFFu, 3);
    ssize_t r = ::read(f->fd, MEM + arg(1), arg(2));
    RET(r < 0 ? 0xFFFFFFFFu : (u32)r, 3);
}
STUB(ext_winmm_dll_mmioSeek) {
    auto f = handles().get<KFile>(arg(0));
    if (!f) RET(0xFFFFFFFFu, 3);
    off_t r = ::lseek(f->fd, (i32)arg(1), arg(2) == 0 ? SEEK_SET : arg(2) == 1 ? SEEK_CUR : SEEK_END);
    RET((u32)r, 3);
}
STUB(ext_winmm_dll_mmioDescend) {
    auto f = handles().get<KFile>(arg(0));
    u32 ck = arg(1), parent = arg(2), flags = arg(3);
    if (!f) RET(256 + 5, 4);   // MMIOERR_CANNOTREAD
    u32 end = parent ? rd32(parent + 12) + rd32(parent + 4) : 0xFFFFFFFFu;
    u32 want_id = rd32(ck), want_type = rd32(ck + 8);
    for (;;) {
        off_t pos = ::lseek(f->fd, 0, SEEK_CUR);
        if ((u32)pos + 8 > end) RET(256 + 4, 4);   // MMIOERR_CHUNKNOTFOUND
        u8 h[12];
        if (::read(f->fd, h, 8) != 8) RET(256 + 4, 4);
        u32 id, size, type = 0;
        std::memcpy(&id, h, 4); std::memcpy(&size, h + 4, 4);
        bool list = id == fourcc("RIFF") || id == fourcc("LIST");
        if (list) { if (::read(f->fd, h + 8, 4) != 4) RET(256 + 4, 4); std::memcpy(&type, h + 8, 4); }
        u32 data = (u32)pos + 8;
        bool match = true;
        if (flags & 0x10) match = id == want_id;                                    // MMIO_FINDCHUNK
        else if (flags & 0x20) match = id == fourcc("RIFF") && type == want_type;    // MMIO_FINDRIFF
        else if (flags & 0x40) match = id == fourcc("LIST") && type == want_type;    // MMIO_FINDLIST
        if (match) {
            wr32(ck, id); wr32(ck + 4, size); wr32(ck + 8, type); wr32(ck + 12, data); wr32(ck + 16, 0);
            RET(0, 4);
        }
        ::lseek(f->fd, data + ((size + 1) & ~1u), SEEK_SET);
    }
}
STUB(ext_winmm_dll_mmioAscend) {
    auto f = handles().get<KFile>(arg(0));
    if (!f) RET(256 + 5, 3);
    u32 ck = arg(1);
    ::lseek(f->fd, rd32(ck + 12) + ((rd32(ck + 4) + 1) & ~1u), SEEK_SET);
    RET(0, 3);
}

// ---------------------------------------------------------------- COMDLG32 (preparar página para imprimir)

STUB(ext_comdlg32_dll_PageSetupDlgA) { RET(0, 1); }
STUB(ext_comdlg32_dll_CommDlgExtendedError) { RET(0, 0); }
