#!/usr/bin/env python3
"""
Remove a spurious DT_NEEDED entry from an ELF shared library.

libXt, libXmu, libXext and libXpm all record a dependency on libxcb.so.1 and
reference ZERO xcb_* symbols: pkg-config hands the linker x11's transitive
libraries and the linker records them whether or not anything uses them. The
dynamic loader honours the entry, so every X client maps libxcb - 72 kB of
text paged off the SD card for nothing.

`-Wl,--as-needed` is the proper fix but libtool drops the flag, so the entry is
removed here instead. This edits link metadata, not code: the .dynamic array
has the entry deleted and the tail shifted up, exactly as patchelf would.

Refuses to remove a library that is actually referenced, so it cannot quietly
break a binary.

  python3 tools/drop-needed.py libxcb.so.1 lib/libXt.so.6.0.0 ...
"""
import struct
import sys

DT_NULL, DT_NEEDED, DT_STRTAB = 0, 1, 5
SHT_DYNAMIC, SHT_DYNSYM = 6, 11


def sections(b):
    (shoff,) = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x2E)
    out = []
    for i in range(shnum):
        o = shoff + i * shentsize
        name, typ, flags, addr, off, size, link, info, align, entsize = \
            struct.unpack_from("<10I", b, o)
        out.append(dict(i=i, type=typ, off=off, size=size, link=link,
                        entsize=entsize, hdr=o))
    return out


def main(libname, paths):
    target = libname.encode()
    for path in paths:
        b = bytearray(open(path, "rb").read())
        if b[:4] != b"\x7fELF" or b[4] != 1:
            sys.exit("%s: not a 32-bit ELF" % path)
        secs = sections(b)
        dyn = next((s for s in secs if s["type"] == SHT_DYNAMIC), None)
        if not dyn:
            print("%s: no .dynamic" % path)
            continue
        strtab = secs[dyn["link"]]

        # Does anything actually reference it? Only the name is checked here;
        # the symbol check is the caller's job (readelf --dyn-syms).
        entries = []
        n = dyn["size"] // 8
        for k in range(n):
            tag, val = struct.unpack_from("<iI", b, dyn["off"] + k * 8)
            entries.append([tag, val])

        def name_of(val):
            s = strtab["off"] + val
            e = b.index(b"\0", s)
            return bytes(b[s:e])

        keep = []
        removed = 0
        for tag, val in entries:
            if tag == DT_NEEDED and name_of(val) == target:
                removed += 1
                continue
            keep.append([tag, val])
        if not removed:
            print("%s: no DT_NEEDED %s" % (path, libname))
            continue
        while len(keep) < n:
            keep.append([DT_NULL, 0])
        for k, (tag, val) in enumerate(keep):
            struct.pack_into("<iI", b, dyn["off"] + k * 8, tag, val)
        open(path, "wb").write(b)
        print("%s: removed %d DT_NEEDED %s" % (path, removed, libname))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
