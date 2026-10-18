"""Checks the recompiler ELF that build.py generate writes (build/SLUS_210.75.recomp.elf) against the disc ELF.

    python -I tools/recomp_elf_check.py [disc_elf] [recomp_elf]

Defaults: build/SLUS_210.75 and build/SLUS_210.75.recomp.elf next to this port. Prints both section tables and checks
that the entry, the program headers and every loaded byte are identical, that the new sections tile the PT_LOAD segment
in the order of recomp/hauntingground_sections.csv, and that only .text/.sinit (and .vutext, which ps2_recomp skips by
name) carry SHF_EXECINSTR. Exit code 0 only when every check passes. Read-only.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def sections(data):
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    raw = [struct.unpack_from("<10I", data, shoff + shentsize * i) for i in range(shnum)]
    names = data[raw[shstrndx][4]:raw[shstrndx][4] + raw[shstrndx][5]]
    out = []
    for sh in raw:
        name = names[sh[0]:names.index(b"\0", sh[0])].decode("ascii")
        out.append((name,) + sh[1:])
    return out


def loads(data):
    phoff = struct.unpack_from("<I", data, 0x1C)[0]
    phnum = struct.unpack_from("<H", data, 0x2C)[0]
    return [struct.unpack_from("<8I", data, phoff + 32 * i) for i in range(phnum)]


def show(title, secs):
    print(title)
    for i, (name, typ, flags, addr, off, size, link, info, align, ent) in enumerate(secs):
        fl = "".join(c for c, bit in (("W", 1), ("A", 2), ("X", 4)) if flags & bit)
        print(f"  [{i:2}] {name or '<unnamed>':22} type {typ:2} {fl:3} 0x{addr:08X}-0x{addr + size:08X} off 0x{off:06X}"
              f" size 0x{size:06X} link {link}")


def main():
    disc = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "SLUS_210.75"
    recomp = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "build" / "SLUS_210.75.recomp.elf"
    a, b = disc.read_bytes(), recomp.read_bytes()
    fails = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what)
        if not ok:
            fails.append(what)

    show(f"disc ELF {disc} ({len(a)} bytes)", sections(a))
    show(f"recompiler ELF {recomp} ({len(b)} bytes)", sections(b))
    check(a[0x18:0x1C] == b[0x18:0x1C], "entry point unchanged (0x%08X)" % struct.unpack_from("<I", b, 0x18)[0])
    check(loads(a) == loads(b), "program headers unchanged")
    segs = [p for p in loads(a) if p[0] == 1 and p[5]]
    check(len(segs) == 1, "one non-empty PT_LOAD")
    _t, off, vaddr, _pa, filesz, memsz, _f, _al = segs[0]
    check(a[off:off + filesz] == b[off:off + filesz], "loaded bytes unchanged (0x%X bytes at file 0x%X)" % (filesz, off))
    check(b[:len(a)][0x34:] == a[0x34:], "every disc byte after the ELF header unchanged (only appended data)")
    rows = []
    for line in (ROOT / "recomp" / "hauntingground_sections.csv").read_text(encoding="utf-8").splitlines()[1:]:
        line = line.split("#", 1)[0].strip()
        if line:
            n, s, e, k = [x.strip() for x in line.split(",")]
            rows.append((n, int(s, 0), int(e, 0), k))
    alloc = [s for s in sections(b) if s[2] & 2]
    check([(s[0], s[3], s[3] + s[5]) for s in alloc] == [(n, s, e) for n, s, e, _k in rows],
          "allocated sections = the %d rows of hauntingground_sections.csv, in order" % len(rows))
    pos = vaddr
    for s in alloc:
        check(s[3] == pos, f"{s[0]} starts where the previous section ends (0x{pos:08X})")
        if s[1] != 8:
            check(s[4] == off + (s[3] - vaddr), f"{s[0]} file offset 0x{s[4]:X} matches its address")
        pos = s[3] + s[5]
    check(pos == vaddr + memsz, "sections end at the segment end 0x%08X" % (vaddr + memsz))
    execs = [s[0] for s in alloc if s[2] & 4]
    check(execs == [".text", ".vutext", ".sinit"], "SHF_EXECINSTR only on .text, .vutext (skipped by name), .sinit: %s"
          % execs)
    print("RESULT " + ("OK" if not fails else f"FAILED ({len(fails)})"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
