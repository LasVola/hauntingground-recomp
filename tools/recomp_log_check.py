"""Offline checks of one generate (build/recomp.log + generated/) against the boot ELF and the boundaries CSV.

    python -I tools/recomp_log_check.py [--elf build/SLUS_210.75] [--generated generated] [--log build/recomp.log]

Reports (read-only):
  - the PS2Recomp report block and the warning/error categories of recomp.log;
  - every "unresolved JR/JALR" site classified by its instruction (jalr call, jr ra, MW virtual tail call
    lw t9,N(t9); jr t9, other jr);
  - per jr <reg> site in the code sections: a generated `switch (jumpTarget)` or an IndirectJump dispatch must exist
    (a site with neither would leave the guest stuck), and every switch-table target the analyzer read from the ELF
    must be a case label;
  - unhandled-instruction lines, if any.
Exit code 1 when a jr site has neither a switch nor a dispatch, or an error line is present.
"""
import collections
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def arg(name, default):
    return Path(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


def main():
    elf_path = arg("--elf", ROOT / "build" / "SLUS_210.75")
    gen = arg("--generated", ROOT / "generated")
    log = arg("--log", ROOT / "build" / "recomp.log")
    data = elf_path.read_bytes()
    phoff = struct.unpack_from("<I", data, 0x1C)[0]
    _t, off, vaddr, _pa, filesz, _ms, _f, _al = struct.unpack_from("<8I", data, phoff)

    def word(a):
        return struct.unpack_from("<I", data, off + a - vaddr)[0]

    code = []
    for line in (ROOT / "recomp" / "hauntingground_sections.csv").read_text(encoding="utf-8").splitlines()[1:]:
        line = line.split("#", 1)[0].strip()
        if line:
            n, s, e, k = [x.strip() for x in line.split(",")]
            if k == "code":
                code.append((int(s, 0), int(e, 0)))
    text = log.read_text(encoding="utf-8", errors="replace").splitlines()
    print("== report")
    in_report = False
    for line in text:
        if line.startswith("=========="):
            in_report = True
        if in_report and line.strip() == "Events:":
            break
        if in_report:
            print("  " + line)
    cats = collections.Counter(m.group(1) for m in (re.match(r"\s*\[(\w+)\] ([\w-]+)", l) for l in text) if m)
    kinds = collections.Counter(re.match(r"\s*\[(\w+)\] ([\w-]+)", l).group(0).strip()
                                for l in text if re.match(r"\s*\[(warning|error)\] ([\w-]+)", l))
    print("== event categories", dict(cats), dict(kinds))
    errors = [l for l in text if l.strip().startswith("[error]")]
    for l in errors[:20]:
        print("  " + l.strip())
    # Each fallback warning is printed twice in recomp.log (5168 lines = 2 x 2584 functions on 2026-10-18), and it lists
    # every jr/jalr of the function, the resolved switch sites included; so dedupe and classify.
    jt = ROOT.parent / "port_prep" / "boundaries" / "out" / "jump_tables.csv"
    jt_rows = [l.split(",") for l in jt.read_text(encoding="ascii").splitlines()[1:] if l] if jt.exists() else []
    switch_jr = {int(r[0], 16) for r in jt_rows}
    sites, funcs = set(), set()
    for l in text:
        m = re.search(r"function=(\S+) addr=\S+ - unresolved JR/JALR at ((?:0x[0-9a-f]+ ?)+);", l)
        if m:
            funcs.add(m.group(1))
            sites |= {int(x, 16) for x in m.group(2).split()}
    cls = collections.Counter()
    for a in sorted(sites):
        w = word(a)
        if (w >> 26) == 0 and (w & 0x3F) == 9:
            cls["jalr"] += 1
        elif (w >> 26) == 0 and (w & 0x3F) == 8:
            rs = (w >> 21) & 31
            prev = word(a - 4)
            if a in switch_jr:
                cls["jr switch (resolved, listed with its function)"] += 1
            elif rs == 31:
                cls["jr ra"] += 1
            elif rs == 25 and (prev >> 26) == 0x23 and ((prev >> 16) & 31) == 25 and ((prev >> 21) & 31) == 25:
                cls["jr t9 after lw t9,N(t9) (MW virtual tail call)"] += 1
            else:
                cls["jr other (0x%X)" % a] += 1
        else:
            cls["not a jr/jalr (0x%X)" % a] += 1
    print("== unresolved JR/JALR: %d functions, %d distinct sites %s" % (len(funcs), len(sites), dict(cls)))
    # Per jr <reg> site: switch or dispatch.
    cases, switch_sites, dispatch_sites = set(), set(), set()
    for path in gen.glob("*.cpp"):
        if path.name == "register_functions.cpp":
            continue
        body = path.read_text(encoding="utf-8", errors="replace")
        for chunk in body.split("const uint32_t jumpTarget")[1:]:
            m = re.search(r"dispatchGuestBranch\(rdram, ctx, jumpTarget, 0x([0-9A-Fa-f]+)u, [^)]*IndirectJump", chunk)
            if not m:
                continue
            site = int(m.group(1), 16)
            head = chunk[:m.start()]
            if "switch (jumpTarget)" in head:
                switch_sites.add(site)
                cases |= {int(x, 16) for x in re.findall(r"case 0x([0-9A-Fa-f]+)u:", head)}
            else:
                dispatch_sites.add(site)
    jr_sites = [a for s, e in code for a in range(s, e, 4)
                if (word(a) >> 26) == 0 and (word(a) & 0x3F) == 8 and ((word(a) >> 21) & 31) != 31]
    neither = [a for a in jr_sites if a not in switch_sites and a not in dispatch_sites]
    print("== jr <reg> sites in code: %d; with a switch %d; with an IndirectJump dispatch only %d; neither %d %s" % (
        len(jr_sites), sum(1 for a in jr_sites if a in switch_sites),
        sum(1 for a in jr_sites if a in dispatch_sites and a not in switch_sites), len(neither),
        " ".join("0x%X" % a for a in neither[:10])))
    if jt_rows:
        rows = jt_rows
        total = missing = 0
        miss_sites = set()
        for r in rows:
            site, table, n = int(r[0], 16), int(r[2], 16), int(r[5])
            for k in range(n):
                t = word(table + 4 * k)
                total += 1
                if t not in cases:
                    missing += 1
                    miss_sites.add(site)
        print("== switch tables %d (%d entries): %d entries without a case label, at %d jr sites %s" % (
            len(rows), total, missing, len(miss_sites), " ".join("0x%X" % a for a in sorted(miss_sites)[:10])))
    unhandled = [l.strip() for l in text if "unhandled-instruction" in l]
    print("== unhandled-instruction lines: %d" % len(unhandled))
    for l in unhandled[:40]:
        print("  " + l)
    return 1 if (neither or errors) else 0


if __name__ == "__main__":
    sys.exit(main())
