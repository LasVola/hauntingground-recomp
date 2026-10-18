"""Haunting Ground (USA, SLUS-21075) PC port: generate, configure, build, boot.

    python -I build.py generate [--iso X] [--toml F]  recompile SLUS_210.75 from your disc image into generated/
    python -I build.py configure [-DOPTION=...]       CMake configure (Ninja, RelWithDebInfo, no LTCG)
    python -I build.py build [target]                 compile hauntingground.exe (configures first if needed)
    python -I build.py all [--iso X]                  generate -> configure -> build
    python -I build.py boot [--seconds N] [--iso X]   one hidden, time-capped run; output in build/port/bin/logs
    python -I build.py run [args]                     a normal run in a window (arguments go to hauntingground.exe)
    python -I build.py status                         what exists so far

Needs Python 3.10+, Visual Studio 2022 with C++ (cl, CMake and Ninja come with it; VSROOT if it is not in the default
folder; tools/win/vsenv.cmd sets it up) and your own Haunting Ground (USA) disc image. The image is found by --iso,
then HG_ISO, then [Game] Iso= in hauntingground.ini (next to this file or next to hauntingground.exe), then the first
*.iso that boots SLUS_210.75 in game/ next to this file.
Adapted from the God Hand port's build.py (itself from the Urban Reign port; one file, no menu, release or bench).
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build" / "port"
BIN = BUILD / "bin"
EXE = BIN / "hauntingground.exe"
VSENV = ROOT / "tools" / "win" / "vsenv.cmd"
JOBS = os.environ.get("HG_JOBS", str(os.cpu_count() or 8))
GENERATED = ROOT / "generated"
TOML = Path(os.environ.get("HG_TOML", ROOT / "recomp" / "hauntingground.toml"))

SERIAL = "SLUS-21075"
BOOT_ELF = "SLUS_210.75"
BOOT_SHA256 = "3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8"
BOOT2 = b"cdrom0:\\SLUS_210.75;1"
INI = "hauntingground.ini"
# The recompiler's view of SLUS_210.75's single RWX section (code, VU microcode, data, rodata, MW static initializers
# and their data together): one row per block. generate writes a copy of the boot ELF with these section headers for
# ps2_recomp only (build/SLUS_210.75.recomp.elf); the runtime always loads the disc's own ELF.
SECTIONS = ROOT / "recomp" / "hauntingground_sections.csv"

EXIT_CODES = {
    0: "the runtime returned by itself",
    1: "initialisation failed or fatal exception",
    2: "no disc image found",
    3: "wrong disc or boot ELF hash mismatch",
    4: "CPU lacks SSE4.1/AVX2",
    5: "stopped on a missing function (MissingFunction=stop)",
    6: "crashed (unhandled exception or fatal signal; see the [crash] lines)",
    124: "time limit reached",
    125: "killed by build.py (did not stop by itself after the time limit)",
}


# Builds, the recompiler and test boots run at below-normal priority so the PC stays responsive (child processes such
# as ninja/cl inherit it) only when HG_LOW_PRIORITY=1 (2026-10-18: normal priority and all cores by default).
LOW_PRIORITY = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0) if os.environ.get("HG_LOW_PRIORITY") == "1" else 0


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    kwargs["creationflags"] = kwargs.get("creationflags", 0) | LOW_PRIORITY
    return subprocess.run([str(c) for c in cmd], **kwargs).returncode


def find_vsroot():
    """VSROOT for tools/win/vsenv.cmd, whose default is ...\\2022\\Community. Some PCs have only the 2022 Build Tools in
    Program Files (x86), so look: VSROOT, then vswhere, then the usual folders."""
    if os.environ.get("VSROOT"):
        return os.environ["VSROOT"]
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
        if out and (Path(out) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat").exists():
            return out
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
            root = Path(base) / "Microsoft Visual Studio" / "2022" / edition
            if (root / "VC" / "Auxiliary" / "Build" / "vcvars64.bat").exists():
                return str(root)
    return None


def in_vs(*cmd):
    """Runs a command inside the Visual Studio x64 environment (tools/win/vsenv.cmd)."""
    if not VSENV.exists():
        print(f"{VSENV} is missing (copy tools/win/vsenv.cmd from the Urban Reign port).")
        return 1
    env = dict(os.environ)
    vsroot = find_vsroot()
    if vsroot:
        env["VSROOT"] = vsroot
    return run(["cmd", "/c", VSENV, *cmd], env=env)


def game_running():
    if os.name != "nt":
        return False
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq hauntingground.exe", "/NH"], capture_output=True, text=True).stdout
    return "hauntingground.exe" in out.lower()


# ---- disc ---------------------------------------------------------------------------------------------------------

def iso_read_root_file(iso_path, name):
    """One file from the ISO9660 root directory (SYSTEM.CNF and SLUS_210.75 are both there), or None."""
    with open(iso_path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            return None
        root = pvd[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        f.seek(lba * 2048)
        data = f.read(size)
        i = 0
        while i < len(data):
            length = data[i]
            if length == 0:
                i = (i // 2048 + 1) * 2048
                continue
            entry = data[i:i + length]
            entry_name = entry[33:33 + entry[32]].decode("ascii", "replace").split(";")[0]
            if entry_name.upper() == name.upper():
                f.seek(struct.unpack_from("<I", entry, 2)[0] * 2048)
                return f.read(struct.unpack_from("<I", entry, 10)[0])
            i += length
    return None


def boots_game(iso_path):
    try:
        cnf = iso_read_root_file(iso_path, "SYSTEM.CNF")
    except OSError:
        return False
    return bool(cnf) and b"BOOT2" in cnf and BOOT2 in cnf


def ini_iso(ini_path):
    """[Game] Iso= of a hauntingground.ini (relative paths: against the ini's folder), or None."""
    try:
        text = ini_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    section = ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
        elif section == "game" and line.lower().startswith("iso=") and line[4:].strip():
            value = Path(line[4:].strip().strip('"'))
            return value if value.is_absolute() else (ini_path.parent / value)
    return None


def find_iso(arg=None):
    """--iso, then HG_ISO, then [Game] Iso= (hauntingground.ini next to this file, then next to the exe), then the
    first *.iso that boots SLUS_210.75 in game/ here. Nothing is hard-coded."""
    if arg:
        return Path(arg)
    if os.environ.get("HG_ISO"):
        return Path(os.environ["HG_ISO"])
    for ini in (ROOT / INI, BIN / INI):
        path = ini_iso(ini)
        if path and path.is_file():
            return path
    for folder in (ROOT / "game",):
        for path in sorted(folder.glob("*.iso")):
            if boots_game(path):
                return path
    return None


def extract_boot_elf(iso):
    """Writes build/SLUS_210.75 from the disc after checking its SHA-256. Returns its path or None."""
    elf = iso_read_root_file(iso, BOOT_ELF)
    if elf is None:
        print(f"{BOOT_ELF} not found on {iso}")
        return None
    digest = hashlib.sha256(elf).hexdigest()
    if digest != BOOT_SHA256:
        print(f"{BOOT_ELF} hash mismatch ({digest}); expected {BOOT_SHA256} ({SERIAL})")
        return None
    out = ROOT / "build" / BOOT_ELF
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.read_bytes() != elf:
        out.write_bytes(elf)
    return out


# ---- generate -----------------------------------------------------------------------------------------------------

def find_recompiler():
    """HG_RECOMP, then the recompiler built from runtime/ps2xRecomp into out/recomp, then the port build tree."""
    candidates = []
    if os.environ.get("HG_RECOMP"):
        candidates.append(Path(os.environ["HG_RECOMP"]))
    candidates += [ROOT / "out" / "recomp" / "ps2_recomp.exe",
                   BUILD / "runtime" / "ps2xRecomp" / "ps2_recomp.exe",
                   ROOT / "tools" / "ps2_recomp.exe"]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def stage_toml(toml_path, elf, staging):
    """recomp/hauntingground.toml with input/output pointed at build/ and its relative CSV paths made absolute."""
    text = toml_path.read_text(encoding="utf-8")
    text = re.sub(r'(?m)^input\s*=.*$', f'input = "{elf.as_posix()}"', text)
    text = re.sub(r'(?m)^output\s*=.*$', f'output = "{staging.as_posix()}"', text)

    def absolute(match):
        key, value = match.group(1), match.group(2)
        path = (toml_path.parent / value).resolve() if not Path(value).is_absolute() else Path(value)
        if path.exists():
            return f'{key} = "{path.as_posix()}"'
        # A named file is required: without the boundaries CSV the recompiler falls back to its own start scan, which
        # swallows SDK starts and vtable-only methods and silently drops them from the generated code.
        raise SystemExit(f"{key} {path} is missing (named in {toml_path})")

    text = re.sub(r'(?m)^(ghidra_output|runtime_relocations)\s*=\s*"([^"]+)"', absolute, text)
    staged = ROOT / "build" / "hauntingground.staging.toml"
    staged.write_text(text, encoding="utf-8")
    return staged


# ---- recompiler ELF (split section headers) -------------------------------------------------------------------------

SHT_PROGBITS, SHT_STRTAB, SHT_NOBITS = 1, 3, 8
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
SECTION_KINDS = {  # kind -> (type, flags); ps2_recomp treats SHF_EXECINSTR as EE code unless the name is .vutext*
    "code": (SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR),
    "vu": (SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR),
    "rodata": (SHT_PROGBITS, SHF_ALLOC),
    "data": (SHT_PROGBITS, SHF_ALLOC | SHF_WRITE),
    "ctors": (SHT_PROGBITS, SHF_ALLOC | SHF_WRITE),
    "bss": (SHT_NOBITS, SHF_ALLOC | SHF_WRITE),
}


def read_sections(path=SECTIONS):
    """recomp/hauntingground_sections.csv rows (header line, then Name,Start,End,Kind; '#' starts a comment)."""
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines()[1:]:
        line = line.split("#", 1)[0].strip()
        if line:
            name, start, end, kind = [x.strip() for x in line.split(",")]
            rows.append((name, int(start, 0), int(end, 0), kind))
    return rows


def split_section_elf(elf, out, rows):
    """Copy of the boot ELF whose section headers follow rows. Program headers and every loaded byte stay unchanged; a
    new .shstrtab and section header table are appended. Only ps2_recomp reads it. Raises SystemExit on a bad map."""
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit(f"{elf}: not a 32-bit little-endian ELF")
    phoff, shoff = struct.unpack_from("<II", data, 0x1C)
    phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHH", data, 0x2C)
    loads = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _pa, p_filesz, p_memsz, _fl, _al = struct.unpack_from("<8I", data, phoff + 32 * i)
        if p_type == 1 and p_memsz:
            loads.append((p_vaddr, p_offset, p_filesz, p_memsz))
    if len(loads) != 1:
        raise SystemExit(f"{elf}: expected one non-empty PT_LOAD, found {len(loads)}")
    vaddr, offset, filesz, memsz = loads[0]
    # The map must tile the segment exactly: PROGBITS rows over the file part, the NOBITS row over the rest.
    pos = vaddr
    for name, start, end, kind in rows:
        if kind not in SECTION_KINDS or start != pos or end <= start or start % 4 or end % 4:
            raise SystemExit(f"{SECTIONS}: bad row {name} 0x{start:X}-0x{end:X} {kind} (expected start 0x{pos:X})")
        if (kind == "bss") != (start >= vaddr + filesz) or (kind != "bss" and end > vaddr + filesz):
            raise SystemExit(f"{SECTIONS}: {name} crosses the file/BSS boundary 0x{vaddr + filesz:X}")
        if kind == "vu" and not name.startswith(".vutext"):
            raise SystemExit(f"{SECTIONS}: VU microcode row {name} must be named .vutext* (ps2_recomp skips only those)")
        if kind == "ctors" and name != ".ctors":
            raise SystemExit(f"{SECTIONS}: constructor table row {name} must be named .ctors")
        pos = end
    if pos != vaddr + memsz:
        raise SystemExit(f"{SECTIONS}: rows end at 0x{pos:X}, the segment at 0x{vaddr + memsz:X}")
    old = [struct.unpack_from("<10I", data, shoff + shentsize * i) for i in range(shnum)]
    old_names = data[old[shstrndx][4]:old[shstrndx][4] + old[shstrndx][5]]

    def old_name(sh):
        return old_names[sh[0]:old_names.index(b"\0", sh[0])].decode("ascii")

    strtab = bytearray(b"\0")

    def add_name(name):
        if not name:
            return 0
        at = len(strtab)
        strtab.extend(name.encode("ascii") + b"\0")
        return at

    # Keep every header that loads nothing (NULL first, .strtab/.symtab, .comment, .reginfo); the loaded ones are
    # replaced by the map. sh_link is renumbered (.symtab -> .strtab).
    kept = [i for i, sh in enumerate(old) if i != shstrndx and not (sh[2] & SHF_ALLOC)]
    if not kept or kept[0] != 0:
        raise SystemExit(f"{elf}: section 0 is not the NULL section")
    new_index = {old_i: 2 + n for n, old_i in enumerate(kept[1:])}
    new_index[0] = 0
    headers = [old[0], None]  # [1] = .shstrtab, filled in below
    for old_i in kept[1:]:
        sh = old[old_i]
        link = new_index.get(sh[6], 0) if sh[6] else 0
        headers.append((add_name(old_name(sh)), sh[1], sh[2], sh[3], sh[4], sh[5], link, sh[7], sh[8], sh[9]))
    for name, start, end, kind in rows:
        sh_type, flags = SECTION_KINDS[kind]
        file_off = offset + (start - vaddr) if kind != "bss" else offset + filesz
        headers.append((add_name(name), sh_type, flags, start, file_off, end - start, 0, 0,
                        128 if start == vaddr else 4, 0))
    shstrtab_name = add_name(".shstrtab")
    out_data = bytearray(data)
    str_off = len(out_data)
    out_data += strtab
    out_data += b"\0" * (-len(out_data) % 4)
    headers[1] = (shstrtab_name, SHT_STRTAB, 0, 0, str_off, len(strtab), 0, 0, 1, 0)
    sh_off = len(out_data)
    for sh in headers:
        out_data += struct.pack("<10I", *sh)
    struct.pack_into("<I", out_data, 0x20, sh_off)
    struct.pack_into("<HHH", out_data, 0x2E, 40, len(headers), 1)
    # Gate: what the PS2 loads (entry, program headers, segment bytes) is byte-identical to the disc ELF.
    if out_data[0x18:0x1C] != data[0x18:0x1C] or out_data[phoff:phoff + 32 * phnum] != data[phoff:phoff + 32 * phnum] \
            or out_data[offset:offset + filesz] != data[offset:offset + filesz]:
        raise SystemExit("split ELF changed loaded bytes")
    if not out.exists() or out.read_bytes() != bytes(out_data):
        out.write_bytes(bytes(out_data))
    return out


def sync_tree(src, dst):
    """Copies only changed files, so an incremental build recompiles only what changed; removes stale ones."""
    dst.mkdir(parents=True, exist_ok=True)
    wanted = {p.name for p in src.iterdir() if p.is_file()}
    changed = removed = 0
    for name in wanted:
        new = (src / name).read_bytes()
        target = dst / name
        if target.exists() and target.read_bytes() == new:
            continue
        target.write_bytes(new)
        changed += 1
    for target in dst.iterdir():
        if target.is_file() and target.name not in wanted:
            target.unlink()
            removed += 1
    print(f"generated sources: {len(wanted)} total, {changed} written, {removed} removed")


# ---- generate checks ------------------------------------------------------------------------------------------------

REGISTER_LINE = re.compile(r"g_ps2RecompiledFunctionTable\[\d+\] = (\w+); // 0x([0-9a-fA-F]+)")
REPORT_KEYS = ("Functions discovered", "Functions processed", "Additional entrypoints", "Generated functions",
               "Indirect fallback promotions", "Unhandled instructions", "Correctness-critical", "Warnings")


def check_generated(staging, toml_path, rows, log):
    """Gates after ps2_recomp: no registered address outside the code rows of the section map (no function in data),
    and every boundaries-CSV start registered. Prints the report lines of recomp.log. Returns the failed gate count."""
    failed = 0
    reg = (staging / "register_functions.cpp").read_text(encoding="utf-8")
    entries = {int(a, 16): n for n, a in REGISTER_LINE.findall(reg)}
    code = [(s, e) for _n, s, e, k in rows if k == "code"]
    if code:
        outside = sorted(a for a in entries if not any(s <= a < e for s, e in code))
        print(f"check: {len(entries)} registered addresses, {len(outside)} outside the code sections"
              + (f" (first {[hex(a) for a in outside[:8]]})" if outside else ""))
        failed += bool(outside)
    m = re.search(r'(?m)^ghidra_output\s*=\s*"([^"]+)"', toml_path.read_text(encoding="utf-8"))
    if m:
        starts = set()
        for line in (toml_path.parent / m.group(1)).read_text(encoding="ascii").splitlines()[1:]:
            if line:
                starts.add(int(line.split(",")[1], 0))
        missing = sorted(a for a in starts if a not in entries)
        print(f"check: {len(starts)} boundaries-CSV starts, {len(missing)} without a table entry"
              + (f" (first {[hex(a) for a in missing[:8]]})" if missing else ""))
        failed += bool(missing)
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(REPORT_KEYS):
            print("  " + line)
    return failed


def generate(args=()):
    args = list(args)
    iso_arg = args[args.index("--iso") + 1] if "--iso" in args and args.index("--iso") + 1 < len(args) else None
    toml_path = Path(args[args.index("--toml") + 1]) if "--toml" in args and args.index("--toml") + 1 < len(args) else TOML
    if not toml_path.exists():
        print(f"{toml_path} not found (the Haunting Ground recompiler config).")
        return 1
    if toml_path.name != "hauntingground.toml":
        # generate syncs into generated/ and deletes every file it did not write: another toml here would wipe the
        # boot code and force a full rebuild.
        print(f"generate takes only hauntingground.toml (got {toml_path})")
        return 1
    iso = find_iso(iso_arg)
    if not iso:
        print("No disc image found. Pass --iso, set HG_ISO or [Game] Iso= in hauntingground.ini, or put your own "
              "Haunting Ground (USA) image in game/.")
        return 1
    print(f"disc image: {iso}")
    elf = extract_boot_elf(iso)
    if not elf:
        return 1
    rows = []
    if SECTIONS.exists():
        rows = read_sections()
        elf = split_section_elf(elf, ROOT / "build" / f"{BOOT_ELF}.recomp.elf", rows)
        print(f"recompiler ELF: {elf} ({len(rows)} sections from {SECTIONS.name}; loaded bytes unchanged)")
    recompiler = find_recompiler()
    if not recompiler:
        print("Building the recompiler (ps2_recomp) first.")
        if configure(["-DHG_BUILD_RECOMP=ON"]) or in_vs("cmake", "--build", BUILD, "--target", "ps2_recomp", "--", "-j", JOBS):
            return 1
        recompiler = find_recompiler()
        if not recompiler:
            print("ps2_recomp.exe still not found after building it.")
            return 1
    staging = ROOT / "build" / "generated_staging"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    staged = stage_toml(toml_path, elf, staging)
    log = ROOT / "build" / "recomp.log"
    print(f"+ {recompiler} {staged}  (output in {log})", flush=True)
    with open(log, "w", encoding="utf-8") as out:
        code = subprocess.run([str(recompiler), str(staged)], cwd=ROOT / "build", stdout=out, stderr=subprocess.STDOUT,
                              creationflags=LOW_PRIORITY).returncode
    if code:
        print(f"ps2_recomp failed ({code}); see {log}")
        return code
    if not (staging / "register_functions.cpp").exists():
        print(f"ps2_recomp wrote no register_functions.cpp into {staging}; see {log}")
        return 1
    if check_generated(staging, toml_path, rows, log):
        print(f"generate gates failed; generated/ left unchanged (staging {staging}, log {log})")
        return 1
    sync_tree(staging, GENERATED)
    return 0


# ---- configure / build --------------------------------------------------------------------------------------------

# Optional offline FetchContent sources (raylib-src, imgui-src, rlimgui-src; for the recompiler also fmt, elfio,
# toml11, libdwarf, rabbitizer): deps/_deps inside this folder or one folder up, when one exists. Without one (a fresh
# clone) CMake's FetchContent downloads them on the first configure.
DEFAULT_DEPS = next((p for p in (ROOT / "deps" / "_deps", ROOT.parent / "deps" / "_deps") if p.is_dir()), None)


def configure(extra=()):
    extra = list(extra)
    args = ["cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            # No /GL over the generated code ever (src/CMakeLists.txt); HG_LTO adds it for the port and IOP only.
            "-DHG_LTO=OFF"]
    # HG_DEPS: an already downloaded FetchContent _deps folder (raylib-src, ...) to reuse instead of downloading.
    # Default: DEFAULT_DEPS (deps/_deps here or one folder up), when one exists.
    deps = os.environ.get("HG_DEPS") or (str(DEFAULT_DEPS) if DEFAULT_DEPS else "")
    if deps:
        for name in ("raylib", "sdl3", "imgui", "rlimgui", "nlohmann_json", "fmt", "elfio", "toml11", "libdwarf",
                     "rabbitizer"):
            src = Path(deps) / f"{name}-src"
            if src.is_dir():
                args.append(f"-DFETCHCONTENT_SOURCE_DIR_{name.upper()}={src.as_posix()}")
    # With a local deps folder, never download at configure time unless HG_ALLOW_DOWNLOADS=1: a missing dependency
    # fails instead. Without one (a fresh clone), FetchContent downloads what it needs. Passed both ways so that a
    # cached ON from an earlier configure does not stick.
    offline = bool(deps) and os.environ.get("HG_ALLOW_DOWNLOADS") != "1"
    args.append(f"-DFETCHCONTENT_FULLY_DISCONNECTED={'ON' if offline else 'OFF'}")
    # The recompiler is not part of the game build; only an explicit -DHG_BUILD_RECOMP=ON turns it on, and every
    # other configure resets it (a stale ON would pull elfio, toml11, fmt, libdwarf and rabbitizer).
    if not any(a.startswith("-DHG_BUILD_RECOMP=") for a in extra):
        args.append("-DHG_BUILD_RECOMP=OFF")
    return in_vs(*args, *extra)


def has_generated_code():
    return (GENERATED / "register_functions.cpp").exists()


def build(target="hauntingground"):
    if not has_generated_code():
        print("The recompiled game code is missing: run 'python -I build.py generate' first.")
        return 1
    if game_running():
        print("hauntingground.exe is running: close it first (the link would fail).")
        return 1
    # build.ninja, not CMakeCache.txt: CMake writes the cache even when configure fails.
    if not (BUILD / "build.ninja").exists() and configure():
        return 1
    # The output is also written to build/port/last_build.log.
    log_path = BUILD / "last_build.log"
    env = dict(os.environ)
    vsroot = find_vsroot()
    if vsroot:
        env["VSROOT"] = vsroot
    cmd = ["cmd", "/c", VSENV, "cmake", "--build", BUILD, "--target", target, "--", "-j", JOBS]
    print("+", " ".join(str(c) for c in cmd), f"(log {log_path})", flush=True)
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen([str(c) for c in cmd], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace", creationflags=LOW_PRIORITY)
        for line in proc.stdout:
            sys.stdout.write(line)
            log.write(line)
        code = proc.wait()
    return code


def build_all(args=()):
    return generate(args) or configure() or build()


# ---- run / boot ---------------------------------------------------------------------------------------------------

def iso_args(args):
    """--iso <image> for hauntingground.exe when the caller gave none and find_iso() resolves one (HG_ISO, the ini or
    game/); otherwise nothing, and the exe applies its own search."""
    if "--iso" in args:
        return []
    iso = find_iso()
    return ["--iso", str(iso)] if iso and iso.is_file() else []


def play(args=()):
    if not EXE.exists():
        print("hauntingground.exe not built yet: run 'python -I build.py build' first.")
        return 1
    args = list(args) + iso_args(args)
    # Session header (sweep L2): play_last.log had elapsed seconds only, no wall-clock time or exe identity.
    digest = hashlib.sha256(EXE.read_bytes()).hexdigest()
    print(f"[session] start {time.strftime('%Y-%m-%d %H:%M:%S %z')} exe {EXE.name} sha256 {digest}", flush=True)
    # --time-limit 0: a normal run is not capped (the ini default TimeLimitSeconds is for hidden boots).
    code = run([EXE, "--window", "--time-limit", "0", *args], cwd=BIN)
    print(f"[session] end {time.strftime('%Y-%m-%d %H:%M:%S %z')} exit {code}", flush=True)
    return code


def keep_play_log(args=()):
    """Start-Haunting-Ground.cmd, before it overwrites logs/play_last.log: keep the previous session as
    logs/play_<its time>.log (nothing is deleted; each is ~1 MB). Sweep 2026-10-17 L2: two sessions' logs were lost."""
    logs = BIN / "logs"
    last = logs / "play_last.log"
    if last.is_file() and last.stat().st_size > 0:
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(last.stat().st_mtime))
        target = logs / f"play_{stamp}.log"
        if not target.exists():
            shutil.copy2(last, target)
    return 0


def summarize(log_path, code, stamp):
    print(f"exit code {code}: {EXIT_CODES.get(code, 'unknown')}")
    text = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    picks = [line for line in text if "missing-target" in line or "time limit" in line
             or " ERROR " in line or "[crash]" in line or "runtime exited" in line]
    for line in picks[:60]:
        print("  " + line[:240])
    if len(picks) > 60:
        print(f"  ... {len(picks) - 60} more")
    print(f"log: {log_path}")
    for pattern in (f"*{stamp}*",):
        for path in sorted(log_path.parent.glob(pattern)):
            if path != log_path:
                print(f"     {path}")


def boot(args=()):
    """One hidden run with a wall-clock cap. Nothing appears on screen (no console window, hidden game window, muted).
    hauntingground.exe stops itself at the cap (exit 124); only if it is still alive 60 s later is this one process killed."""
    args = list(args)
    seconds = 120
    extra = []
    i = 0
    while i < len(args):
        if args[i] == "--seconds" and i + 1 < len(args):
            seconds = int(args[i + 1])
            i += 2
        elif args[i] == "--iso" and i + 1 < len(args):
            extra += ["--iso", args[i + 1]]
            i += 2
        else:
            extra.append(args[i])
            i += 1
    extra += iso_args(extra)
    # A hidden run is always capped: 0 would mean "no limit" to the exe. Default maximum 180 s (HG_BOOT_MAX_SECONDS).
    max_seconds = int(os.environ.get("HG_BOOT_MAX_SECONDS", "180"))
    if not 1 <= seconds <= max_seconds:
        print(f"--seconds {seconds}: a hidden boot needs 1..{max_seconds} s (HG_BOOT_MAX_SECONDS raises the maximum)")
        return 1
    if not EXE.exists():
        print("hauntingground.exe not built yet: run 'python -I build.py build' first.")
        return 1
    if game_running():
        print("hauntingground.exe is already running; not starting another one.")
        return 1
    logs = BIN / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = logs / f"boot_{stamp}.log"
    env = dict(os.environ)
    env["HG_HEADLESS"] = "1"
    env["HG_TIME_LIMIT_SECONDS"] = str(seconds)
    env.setdefault("HG_MISSING_FUNCTION", "stop")
    env.setdefault("PS2X_DUMP_RAM_ON_MISSING", str(logs / f"missing_ram_{stamp}.bin"))
    env.setdefault("HG_DUMP_RAM", str(logs / f"timeout_ram_{stamp}.bin"))
    # Hidden runs never read or write the player's controls.ini: a fresh default-layout file per run.
    env.setdefault("HG_CONTROLS_FILE", str(logs / f"controls_{stamp}.ini"))
    # ... nor the player's memory card (sweep 2026-10-17 L3/CR-2): 30 earlier hidden boots opened bin/memcards/Mcd001.ps2
    # read/write and a time-limit stop can land mid-save. Default: a per-run copy (the run still sees the player's saves).
    def resolved(value):
        path = Path(value)
        return (path if path.is_absolute() else BIN / path).resolve()
    if resolved(env["HG_CONTROLS_FILE"]) == (BIN / "controls.ini").resolve():
        print("refusing: HG_CONTROLS_FILE is the player's controls.ini; hidden runs use a scratch file.")
        return 1
    if env.get("HG_MC_DIR"):
        if resolved(env["HG_MC_DIR"]) == (BIN / "memcards").resolve():
            print("refusing: HG_MC_DIR is the player's memory card folder; hidden runs use a copy.")
            return 1
    else:
        mc_copy = logs / f"mc_{stamp}"
        mc_copy.mkdir(parents=True, exist_ok=True)
        for name in ("Mcd001.ps2", "Mcd002.ps2"):
            if (BIN / "memcards" / name).is_file():
                shutil.copy2(BIN / "memcards" / name, mc_copy / name)
        env["HG_MC_DIR"] = str(mc_copy)
    print(f"memory cards: {env['HG_MC_DIR']}")
    # Read by the runtime before main() (a static initializer), so it must come from the real environment.
    env.setdefault("PS2X_BRANCH_TRACE", "256")
    env.setdefault("PS2X_BRANCH_TRACE_FILE", str(logs / f"branch_trace_{stamp}.txt"))
    cmd = [str(EXE), "--headless", "--time-limit", str(seconds), *extra]
    print("+", " ".join(cmd), f"> {log_path}", flush=True)
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | LOW_PRIORITY
    with open(log_path, "w", encoding="utf-8") as out:
        proc = subprocess.Popen(cmd, cwd=BIN, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                creationflags=flags)
        print(f"hauntingground.exe pid {proc.pid}, cap {seconds} s")
        try:
            code = proc.wait(timeout=seconds + 60)
        except subprocess.TimeoutExpired:
            print(f"pid {proc.pid} did not stop by itself within {seconds + 60} s; killing that process only")
            proc.kill()
            proc.wait()
            code = 125  # TerminateProcess exit code is 1, which would read as "initialisation failed"
    summarize(log_path, code, stamp)
    return code


def status(args=()):
    iso = find_iso()
    rows = [
        ("recompiler config", TOML),
        ("disc image", iso or ROOT / "game" / "<none>.iso"),
        ("boot ELF copy", ROOT / "build" / BOOT_ELF),
        ("recompiler section map", SECTIONS),
        ("recompiler ELF (split sections)", ROOT / "build" / f"{BOOT_ELF}.recomp.elf"),
        ("generated code", GENERATED / "register_functions.cpp"),
        ("CMake cache", BUILD / "CMakeCache.txt"),
        ("hauntingground.exe", EXE),
        ("tools/win/vsenv.cmd", VSENV),
    ]
    for label, path in rows:
        print(f"  [{'x' if Path(path).exists() else ' '}] {label}: {path}")
    print(f"  Visual Studio (VSROOT): {find_vsroot() or 'not found'}")
    return 0


COMMANDS = {
    "generate": generate,
    "configure": lambda a: configure(a),
    "build": lambda a: build(a[0] if a else "hauntingground"),
    "all": build_all,
    "boot": boot,
    "run": play,
    "play": play,
    "keep-play-log": keep_play_log,
    "status": status,
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        return 0 if len(sys.argv) < 2 else 2
    return COMMANDS[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
