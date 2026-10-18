"""GS VRAM dump (4 MB, runtime layout) -> PNG of one PSMCT32 buffer.

Usage: python -I vram2png.py <vram.bin> <out.png> <fbp hex> [fbw=8] [w=512] [h=448]
Addressing copied from runtime/ps2xRuntime/include/runtime/gs/ps2_gs_psmct32.h (addrPSMCT32, block = fbp*32).
Alpha is forced to 255 (the present shader does the same); RGB is written as stored.
"""
import struct
import sys
import zlib

BLOCK = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23],
         [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15],
          [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
          [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47],
          [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]


def addr32(block, width, x, y):
    page = (block >> 5) + (y >> 5) * (width or 1) + (x >> 6)
    bid = (block & 0x1F) + BLOCK[(y >> 3) & 3][(x >> 3) & 7]
    return (page << 13) + ((bid >> 5) << 13) + (bid & 0x1F) * 256 + COLUMN[y & 7][x & 7] * 4


def main():
    src, dst, fbp = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    fbw, w, h = (int(a) for a in (sys.argv[4:7] + ["8", "512", "448"][len(sys.argv[4:7]):]))
    vram = open(src, "rb").read()
    rows = bytearray()
    nonzero = 0
    for y in range(h):
        rows.append(0)
        for x in range(w):
            a = addr32(fbp * 32, fbw, x, y) & (len(vram) - 1)
            r, g, b = vram[a], vram[a + 1], vram[a + 2]
            nonzero += (r | g | b) != 0
            rows += bytes((r, g, b, 255))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    print(f"{dst}: fbp 0x{fbp:x} fbw {fbw} {w}x{h}, non-black pixels {nonzero}/{w * h}")


if __name__ == "__main__":
    main()
