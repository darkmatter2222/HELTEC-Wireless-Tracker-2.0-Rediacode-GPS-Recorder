#!/usr/bin/env python3
"""
elf_decode.py — one dependency-free reader for the ESP32-S3 firmware ELF.

Supersedes the 17 one-off scratch scripts that the v2.1 / v2.2 panic
investigations left at the repo root. Same information, argv-driven, no
hard-coded build paths.

Usage:
    python scripts/elf_decode.py header                 # class/endian/machine/entry
    python scripts/elf_decode.py sha256                 # match the ROM's "ELF file SHA256:" line
    python scripts/elf_decode.py sections [filter]      # section table (name/addr/size)
    python scripts/elf_decode.py sym <name|0xADDR>      # symbol lookup by name or address
    python scripts/elf_decode.py lines <0xADDR> [...]   # addr2line, via the toolchain

All commands accept a trailing --elf <path>; the default is the V2 build:
    .pio/build/heltec_tracker_v2/firmware.elf

The matching recipe is documented in AGENTS.md -> "LittleFS Mount With
block_count == 0 Panics on First File Open": confirm the sha256 against the
panic banner first, then resolve the backtrace addresses.
"""

import hashlib
import os
import struct
import subprocess
import sys

DEFAULT_ELF = os.path.join(".pio", "build", "heltec_tracker_v2", "firmware.elf")
TOOLCHAIN = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages",
    "toolchain-xtensa-esp-elf", "bin")


# ---------------------------------------------------------------------------
# ELF container (32-bit little-endian, which is all xtensa produces)
# ---------------------------------------------------------------------------

def _read(elf):
    with open(elf, "rb") as f:
        return f.read()


def _check(data):
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{data[:4]!r} is not an ELF magic")
    if data[4] != 1:
        raise SystemExit(f"ELF class {data[4]} unsupported (want 1 = 32-bit)")
    if data[5] != 1:
        raise SystemExit(f"endianness {data[5]} unsupported (want 1 = little)")
    return data


def _hdr(data):
    (etype, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = struct.unpack_from(
        "<HHIIIIIHHHHHH", data, 16)
    return dict(type=etype, machine=machine, version=version, entry=entry,
                phoff=phoff, shoff=shoff, flags=flags, ehsize=ehsize,
                phentsize=phentsize, phnum=phnum, shentsize=shentsize,
                shnum=shnum, shstrndx=shstrndx)


def _sections(data, hdr):
    """Return [(name, type, addr, offset, size, link, entsize)].

    Elf32_Shdr layout -- note sh_flags sits between sh_type and sh_addr, so the
    six words are NOT contiguous from the start of the record:
        0 sh_name | 4 sh_type | 8 sh_flags | 12 sh_addr | 16 sh_offset |
        20 sh_size | 24 sh_link | 28 sh_info | 32 sh_addralign | 36 sh_entsize
    """
    out = []
    strtab_off = 0
    for i in range(hdr["shnum"]):
        base = hdr["shoff"] + i * hdr["shentsize"]
        name, stype = struct.unpack_from("<II", data, base)
        addr, offset, size, link = struct.unpack_from("<IIII", data, base + 12)
        entsize = struct.unpack_from("<I", data, base + 36)[0]
        if i == hdr["shstrndx"]:
            strtab_off = offset
        out.append([name, stype, addr, offset, size, link, entsize])
    def sname(n):
        end = data.index(b"\x00", strtab_off + n)
        return data[strtab_off + n:end].decode("ascii", "replace")
    for row in out:
        row[0] = sname(row[0])
    return out


SHT_SYMTAB, SHT_PROGBITS = 2, 1


def _symbols(data, sections):
    """[(value, size, name)] from .symtab, falling back to .dynsym."""
    for row in sections:
        if row[1] not in (SHT_SYMTAB,):
            continue
        _, _, _, offset, size, link, entsize = row
        strtab_off = _strtab_off(sections, link)
        out = []
        n = size // entsize if entsize else 0
        for i in range(n):
            base = offset + i * entsize
            st_name, st_value, st_size, st_info = struct.unpack_from(
                "<IIIB", data, base)
            if st_value == 0:
                continue
            end = data.index(b"\x00", strtab_off + st_name)
            name = data[strtab_off + st_name:end].decode("ascii", "replace")
            if name:
                out.append((st_value, st_size, name))
        if out:
            return out
    return []


def _strtab_off(sections, index):
    # sh_link points at the section index of the related string table;
    # row layout is [name, type, addr, offset, size, link, entsize].
    return sections[index][3]


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_header(data, hdr):
    print(f"magic      : {data[:16].hex()}")
    print(f"class      : 32-bit little-endian")
    print(f"type       : {hdr['type']} (1=REL 2=EXEC 3=DYN)")
    print(f"machine    : {hdr['machine']} (94 = EM_XTENSA)")
    print(f"entry      : 0x{hdr['entry']:08x}")
    print(f"sections   : {hdr['shnum']}   program headers: {hdr['phnum']}")
    print(f"flags      : 0x{hdr['flags']:08x}")


def cmd_sections(sections, filt):
    for name, stype, addr, offset, size, _link, _ent in sections:
        if filt and filt not in name:
            continue
        kind = {SHT_PROGBITS: "PROGBITS", SHT_SYMTAB: "SYMTAB",
                8: "NOBITS", 3: "STRTAB"}.get(stype, f"type{stype}")
        print(f"{addr:#010x}  {size:>9}  {kind:<9} {name}")


def cmd_sym(rows, want):
    as_num = None
    try:
        as_num = int(want, 16)
    except ValueError:
        pass
    hits = [(v, sz, n) for (v, sz, n) in rows
            if (as_num is not None and v == as_num) or (as_num is None and n == want)]
    if not hits:
        # substring match is far more useful for C++ mangled/qualified names
        hits = [(v, sz, n) for (v, sz, n) in rows if want in n]
    for value, size, name in hits[:40]:
        print(f"{value:#010x}  size={size:<6} {name}")
    if not hits:
        print(f"no symbol matching {want!r} ({len(rows)} in table)")


def cmd_lines(elf, addrs):
    a2l = os.path.join(TOOLCHAIN, "xtensa-esp32s3-elf-addr2line.exe")
    if not os.path.exists(a2l):
        a2l = os.path.join(TOOLCHAIN, "xtensa-esp32s3-elf-addr2line")
    if not os.path.exists(a2l):
        print("toolchain addr2line not found; expected at")
        print(f"  {a2l}")
        return 1
    out = subprocess.run([a2l, "-e", elf, "-f", "-C", *addrs],
                         capture_output=True, text=True)
    print(out.stdout.strip() or out.stderr.strip())
    return 0


def main(argv):
    args = list(argv[1:])
    if not args:
        print(__doc__)
        return 2
    elf = DEFAULT_ELF
    if "--elf" in args:
        i = args.index("--elf")
        elf = args[i + 1]
        del args[i:i + 2]
    if not os.path.exists(elf):
        print(f"no such ELF: {elf}")
        return 1

    cmd, rest = args[0], args[1:]
    if cmd == "sha256":
        with open(elf, "rb") as f:
            print(hashlib.sha256(f.read()).hexdigest())
        return 0

    data = _check(_read(elf))
    hdr = _hdr(data)
    if cmd == "header":
        cmd_header(data, hdr)
        return 0
    sections = _sections(data, hdr)
    if cmd == "sections":
        cmd_sections(sections, rest[0] if rest else "")
        return 0
    rows = _symbols(data, sections)
    if cmd == "sym":
        if not rest:
            print("usage: sym <name|0xADDR>")
            return 2
        cmd_sym(rows, rest[0])
        return 0
    if cmd == "lines":
        if not rest:
            print("usage: lines <0xADDR> [<0xADDR> ...]")
            return 2
        return cmd_lines(elf, rest)
    print(f"unknown command {cmd!r}; see the module docstring")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
