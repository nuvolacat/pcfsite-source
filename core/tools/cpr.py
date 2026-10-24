"""Extrae los contenedores de instalación de PCF7 (`*.CPR`).

Un `.CPR` es una secuencia de entradas ZIP locales (`PK\\x03\\x04` + deflate), sin directorio
central: el directorio va aparte en el `.HDR` hermano. Se recorren las cabeceras locales en orden.

Uso: python3 cpr.py <fichero.CPR> <carpeta_destino> [-l]
"""
import os
import struct
import sys
import zlib


def entries(data):
    off = 0
    while off + 30 <= len(data) and data[off:off + 4] == b"PK\x03\x04":
        (_, _, flags, method, _, _, crc, csize, usize, nlen, xlen) = struct.unpack_from("<4sHHHHHIIIHH", data, off)
        name = data[off + 30:off + 30 + nlen].decode("cp850")
        start = off + 30 + nlen + xlen
        if flags & 8:
            raise ValueError(f"{name}: tamaño en descriptor de datos (no soportado)")
        yield name, method, crc, data[start:start + csize], usize
        off = start + csize


def main():
    src, dst = sys.argv[1], sys.argv[2]
    only_list = "-l" in sys.argv[3:]
    data = open(src, "rb").read()
    n = 0
    for name, method, crc, raw, usize in entries(data):
        n += 1
        if only_list:
            print(f"{usize:10d}  {name}")
            continue
        if method == 8:
            out = zlib.decompress(raw, -15)
        elif method == 0:
            out = raw
        else:
            raise ValueError(f"{name}: método {method} no soportado")
        if len(out) != usize or zlib.crc32(out) != crc:
            raise ValueError(f"{name}: CRC o tamaño incorrecto")
        path = os.path.join(dst, *name.replace("\\", "/").split("/"))
        if name.endswith(("/", "\\")):   # entrada de carpeta
            os.makedirs(path, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(path) or dst, exist_ok=True)
        open(path, "wb").write(out)
    print(f"{src}: {n} ficheros")


if __name__ == "__main__":
    main()
