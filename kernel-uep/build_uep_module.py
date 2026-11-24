#!/usr/bin/env python3
"""Build an unsigned ARM32 loadable kernel module (.ko) for the CU8000 5.4.77
kernel without kernel headers, by cloning the verified skeleton of the stock
arc4.ko and grafting clang-assembled code sections.

Empirical facts this relies on (see evidence/lightway/uep-toggle-review.txt and
evidence/firmware/stock-arc4.ko):
  sizeof(struct module) = 512, name at +12, init at +0xd8, exit at +0x1d8
  vermagic = "12344917, release SMP preempt mod_unload ARMv7 p2v8 "
  no CONFIG_MODVERSIONS (__versions absent), no module signatures.

Usage: build_uep_module.py <raw_clang.o> <output.ko> <module_name>
"""
import struct
import sys

ARC4 = None  # set in main

SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS, SHT_REL, SHT_NOTE = 1, 2, 3, 4, 8, 9, 7


def parse_elf(path):
    d = open(path, "rb").read()
    assert d[:5] == b"\x7fELF\x01" and d[5] == 1, "want ELF32 LE"
    hdr = struct.unpack("<HHIIIIIHHHHHH", d[16:52])
    (_t, _m, _v, _e, _p, shoff, flags, _eh, _pe, _pn, shentsize, shnum, shstrndx) = hdr
    secs = []
    for i in range(shnum):
        o = shoff + i * shentsize
        vals = struct.unpack("<10I", d[o:o + 40])
        secs.append(dict(zip(
            ("name_off", "type", "flags", "addr", "offset", "size", "link", "info", "align", "entsize"), vals)))
    shstr = secs[shstrndx]
    for s in secs:
        b = d[shstr["offset"] + s["name_off"]:]
        s["name"] = b[:b.index(b"\x00")].decode()
        s["data"] = d[s["offset"]:s["offset"] + s["size"]] if s["type"] != SHT_NOBITS else b""
    out = dict(data=d, secs=secs, flags=flags, shentsize=shentsize)
    # symbols
    for s in secs:
        if s["type"] == SHT_SYMTAB:
            strtab = secs[s["link"]]
            syms = []
            for j in range(s["size"] // 16):
                e = s["data"][j * 16:(j + 1) * 16]
                nm, val, sz, info, other, shndx = struct.unpack("<IIIBBH", e)
                b = strtab["data"][nm:]
                name = b[:b.index(b"\x00")].decode()
                syms.append(dict(name=name, value=val, size=sz, info=info, shndx=shndx))
            out["syms"] = syms
    return out


def relocs(elf, sec):
    """relocs of section `sec`: list of (offset, sym_idx, type, addend_in_place)."""
    out = []
    for s in elf["secs"]:
        if s["type"] == SHT_REL and elf["secs"][s["info"]]["name"] == sec:
            for j in range(s["size"] // 8):
                r_off, r_info = struct.unpack("<II", s["data"][j * 8:j * 8 + 8])
                out.append((r_off, r_info >> 8, r_info & 0xFF))
    return out


def build(raw_path, out_path, modname):
    arc4 = parse_elf(ARC4)
    raw = parse_elf(raw_path)

    def arc4sec(n):
        for s in arc4["secs"]:
            if s["name"] == n:
                return s
        raise KeyError(n)

    def rawsec(n):
        for s in raw["secs"]:
            if s["name"] == n:
                return s
        raise KeyError(n)

    vermagic = arc4sec(".modinfo")["data"]
    i = vermagic.index(b"vermagic=")
    vermagic = vermagic[i + 9:vermagic.index(b"\x00", i)]
    # the numeric prefix differs per firmware build (12344917 on the 0080 unit,
    # 13432690 on the 0090 unit); take it verbatim from the template and only
    # check the structure
    assert vermagic.endswith(b", release SMP preempt mod_unload ARMv7 p2v8 "), vermagic

    # ---- compose sections ------------------------------------------------
    layout = []  # dicts: name,type,flags,align,data,link,info,entsize
    add = lambda **k: layout.append(k)
    add(name="", type=0, flags=0, align=0, data=b"")  # ELF NULL section 0

    for n in (".note.Linux",):
        s = arc4sec(n)
        add(name=n, type=s["type"], flags=s["flags"], align=s["align"], data=s["data"])
    # .note.gnu.build-id intentionally NOT cloned: carrying stock arc4's
    # BuildID is misleading for provenance and the loader does not need it.
    add(name=".text", type=SHT_PROGBITS, flags=0x6, align=4, data=struct.pack("<I", 0xE12FFF1E))
    for n in (".init.text", ".exit.text", ".rodata"):
        s = rawsec(n)
        add(name=n, type=SHT_PROGBITS, flags=s["flags"], align=max(4, s["align"]), data=s["data"])
    for n in (".plt", ".init.plt"):
        s = arc4sec(n)
        add(name=n, type=s["type"], flags=s["flags"], align=s["align"], data=s["data"])

    modinfo = b"license=GPL\x00" + f"name={modname}\x00".encode() + b"vermagic=" + vermagic + b"\x00"
    add(name=".modinfo", type=SHT_PROGBITS, flags=0x2, align=4, data=modinfo)

    this = bytearray(512)
    this[12:12 + len(modname) + 1] = modname.encode() + b"\x00"
    add(name=".gnu.linkonce.this_module", type=SHT_PROGBITS, flags=0x3, align=4, data=bytes(this))

    # ---- symbols ----------------------------------------------------------
    syms = [dict(name="", value=0, size=0, info=0, shndx=0)]  # null
    sec_index = {s["name"]: i for i, s in enumerate(layout) if s["name"]}

    def new_sym(name, value, size, bind, typ, shndx):
        syms.append(dict(name=name, value=value, size=size, info=(bind << 4) | typ, shndx=shndx))
        return len(syms) - 1

    for s in layout:
        if not s["name"] or s["name"].startswith(".rel") or s["name"] in (".symtab", ".strtab", ".shstrtab"):
            continue
        new_sym("", 0, 0, 0, 3, sec_index[s["name"]])  # SECTION sym

    init_sym = new_sym("init_module", 0, rawsec(".init.text")["size"], 1, 2, sec_index[".init.text"])
    exit_sym = new_sym("cleanup_module", 0, rawsec(".exit.text")["size"], 1, 2, sec_index[".exit.text"])

    # remap clang's UNDEF syms (e.g. printk)
    raw_syms = raw["syms"]
    undef_map = {}
    for s in raw_syms:
        if s["shndx"] == 0 and s["name"]:
            undef_map[s["name"]] = new_sym(s["name"], 0, 0, 1, 0, 0)

    def map_raw_sym(idx):
        s = raw_syms[idx]
        if s["shndx"] == 0:
            return undef_map[s["name"]]
        # local section-relative symbol -> our section symbol for that section
        raw_name = raw["secs"][s["shndx"]]["name"]
        return sec_index[raw_name]

    # ---- relocations -------------------------------------------------------
    rel_sections = []  # (target_name, entries=[(off, sym, type)])
    for tgt in (".init.text", ".exit.text"):
        ents = [(off, map_raw_sym(si), t) for (off, si, t) in relocs(raw, tgt)]
        if ents:
            rel_sections.append((tgt, ents))
    rel_sections.append((".gnu.linkonce.this_module", [(0xD8, init_sym, 2), (0x1D8, exit_sym, 2)]))

    for tgt, ents in rel_sections:
        add(name=f".rel{tgt}", type=SHT_REL, flags=0, align=4, entsize=8,
            data=b"".join(struct.pack("<II", o, (si << 8) | t) for (o, si, t) in ents))
        layout_idx = sec_index[tgt]
        # link/info are finalized after symtab position is known
        layout[-1]["info"] = layout_idx

    # ---- string tables / symtab --------------------------------------------
    strtab = b"\x00"
    str_off = {}
    for s in syms:
        if s["name"] and s["name"] not in str_off:
            str_off[s["name"]] = len(strtab)
            strtab += s["name"].encode() + b"\x00"
    symdata = b"".join(
        struct.pack("<IIIBBH", str_off.get(s["name"], 0), s["value"], s["size"], s["info"], 0, s["shndx"])
        for s in syms)
    add(name=".symtab", type=SHT_SYMTAB, flags=0, align=4, data=symdata, entsize=16)
    add(name=".strtab", type=SHT_STRTAB, flags=0, align=1, data=strtab)
    add(name=".shstrtab", type=SHT_STRTAB, flags=0, align=1, data=b"")
    symtab_sec = layout[-3]
    symtab_sec["link"] = len(layout) - 2          # -> .strtab
    symtab_sec["info"] = next(i for i, s in enumerate(syms) if s["info"] >> 4)  # first global

    # ---- write ELF ----------------------------------------------------------
    shstrtab = b"\x00"
    name_off = {}
    for s in layout:
        if s["name"] and s["name"] not in name_off:
            name_off[s["name"]] = len(shstrtab)
            shstrtab += s["name"].encode() + b"\x00"
    layout[-1]["data"] = shstrtab

    symtab_idx = len(layout) - 3
    for s in layout:
        if s["name"].startswith(".rel."):
            s["link"] = symtab_idx
    ehsize, shentsize, phentsize = 52, 40, 32
    off = ehsize
    for s in layout:
        s.setdefault("link", 0)
        s.setdefault("info", 0)
        s.setdefault("entsize", 0)
        if s["type"] != SHT_NOBITS:
            s["file_off"] = off
            off += len(s["data"])
            pad = (-off) % s["align"] if s["align"] else 0
            off += pad
        else:
            s["file_off"] = off
    shoff = off
    total = shoff + len(layout) * shentsize
    buf = bytearray(total)
    ehdr = struct.pack("<4sBBBBB7sHHIIIIIHHHHHH",
                       b"\x7fELF", 1, 1, 1, 0, 0, b"\x00" * 7,
                       1, 40, 1, 0, 0, shoff, arc4["flags"], ehsize, phentsize, 0,
                       shentsize, len(layout), len(layout) - 1)
    buf[:52] = ehdr
    for i, s in enumerate(layout):
        if s["type"] != SHT_NOBITS:
            buf[s["file_off"]:s["file_off"] + len(s["data"])] = s["data"]
        ho = shoff + i * shentsize
        buf[ho:ho + 40] = struct.pack("<10I", name_off.get(s["name"], 0), s["type"], s["flags"], 0,
                                      s["file_off"], len(s["data"]), s["link"], s["info"],
                                      s["align"], s["entsize"])
    open(out_path, "wb").write(buf)
    print(f"wrote {out_path} ({len(buf)} bytes), {len(syms)} symbols")


def main():
    global ARC4
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    ARC4 = "evidence/firmware/stock-arc4.ko"
    build(sys.argv[1], sys.argv[2], sys.argv[3])


if __name__ == "__main__":
    main()
