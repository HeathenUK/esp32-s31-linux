#!/usr/bin/env python3
"""trace.py - read gltrace files (tools/glref/gltrace/gltrace.h).

  trace.py summary TRACE        per-swap lines (frame, mode, live hash,
                                records and bytes of the frame), then the
                                totals and the heaviest entry points
  trace.py hashes TRACE         "frame hash" for every full frame
  trace.py names TRACE          the entry points the records use
  trace.py pngcmp RAW PNG [W H] RGB565 raw frame vs a glref PNG capture
                                (capture.c's 565 -> 888 expansion): exact?
  trace.py raw2png RAW PNG W H  RGB565 raw -> PNG
  trace.py texmem TRACE         texture storage our library holds, from the
                                uploads (RGB565 + an A8 plane unless the
                                internal format is RGB/luminance; level 0
                                and stored levels > 0 apart), after the load
                                phase (the first full frame) and at the end;
                                "sub-updated" = textures glTexSubImage2D
                                writes (QuakeSpasm's lightmaps)
s31, MIT.
"""
import struct
import sys

TR_MAGIC = 0x52544c47
SWAP, CTX, NEWCTX, DELCTX, UNH, END = 0xFFF, 0xFFE, 0xFFD, 0xFFC, 0xFFB, 0xFFA
PSEUDO = {SWAP: 'SWAP', CTX: 'CTX', NEWCTX: 'NEWCTX', DELCTX: 'DELCTX', UNH: 'UNHANDLED', END: 'END'}


def read(path):
    b = open(path, 'rb').read()
    magic, ver, n = struct.unpack_from('<III', b, 0)
    if magic != TR_MAGIC:
        sys.exit('%s: not a gltrace file' % path)
    off = 12
    names = []
    for _ in range(n):
        ln, = struct.unpack_from('<I', b, off)
        off += 4
        names.append(b[off:off + ln].decode())
        off += (ln + 3) & ~3
    return b, off, names, ver


def records(b, off):
    end = len(b)
    while off + 4 <= end:
        h, = struct.unpack_from('<I', b, off)
        i, nw = h & 0xFFF, h >> 12
        if nw == 0xFFFFF:
            nw, = struct.unpack_from('<I', b, off + 4)
        if nw == 0:
            sys.exit('corrupt record at %d' % off)
        yield i, off, nw
        off += nw * 4


def words(b, off, k, n):
    return struct.unpack_from('<%dI' % n, b, off + 4 * k)


def summary(path):
    b, off, names, ver = read(path)
    per = {}
    fr_rec = fr_bytes = 0
    tot = 0
    nfull = nstate = 0
    unh = 0
    print('# %s: version %d, %d names, %d bytes' % (path, ver, len(names), len(b)))
    print('# frame flags hash w h window records bytes')
    for i, o, nw in records(b, off):
        tot += 1
        if i in PSEUDO:
            name = PSEUDO[i]
        else:
            name = names[i] if i < len(names) else '?%d' % i
        c = per.setdefault(name, [0, 0])
        c[0] += 1
        c[1] += nw * 4
        fr_rec += 1
        fr_bytes += nw * 4
        if i == SWAP:
            f, fl, hsh, w, h, win = words(b, o, 1, 6)
            mode = 'count' if fl & 2 else ('warm' if fl & 1 else 'state')
            if fl & 1:
                nfull += 1
            else:
                nstate += 1
            print('%d %s %08x %d %d %d %d %d' % (f, mode, hsh, w, h, win, fr_rec, fr_bytes))
            fr_rec = fr_bytes = 0
        elif i == CTX:
            a = words(b, o, 1, 8)
            print('# CTX ctx %d drawable %d %dx%d depth %d stencil %d db %d rgb %06x' % a)
        elif i == NEWCTX:
            print('# NEWCTX ctx %d share %d' % words(b, o, 1, 2))
        elif i == DELCTX:
            print('# DELCTX ctx %d' % words(b, o, 1, 1))
        elif i == UNH:
            unh += 1
            print('# UNHANDLED %s' % names[words(b, o, 1, 1)[0]])
        elif i == END:
            print('# END frames %d unhandled %d' % words(b, o, 1, 2))
    print('# top entry points by bytes:')
    for name, (n, by) in sorted(per.items(), key=lambda kv: -kv[1][1])[:25]:
        print('#   %-28s %9d calls %11d bytes' % (name, n, by))
    print('# %d records, %d full frames, %d state-only, %d unhandled' % (tot, nfull, nstate, unh))


def hashes(path):
    b, off, names, ver = read(path)
    for i, o, nw in records(b, off):
        if i == SWAP:
            f, fl, hsh, w, h, win = words(b, o, 1, 6)
            if fl & 1:
                print(f, '%08x' % hsh, 'count' if fl & 2 else 'warm')


def used_names(path):
    b, off, names, ver = read(path)
    s = set()
    for i, o, nw in records(b, off):
        if i < len(names):
            s.add(names[i])
    for n in sorted(s):
        print(n)


def rgb(raw, w, h):
    px = struct.unpack('<%dH' % (w * h), raw[:w * h * 2])
    out = bytearray()
    for v in px:
        r, g, bb = v >> 11, (v >> 5) & 63, v & 31
        out += bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (bb * 255 + 15) // 31))
    return bytes(out)


def pngcmp(raw, png, w=320, h=240):
    from PIL import Image
    a = rgb(open(raw, 'rb').read(), w, h)
    im = Image.open(png).convert('RGB')
    if im.size != (w, h):
        print('pngcmp: size %s != %dx%d' % (im.size, w, h))
        return 1
    bb = im.tobytes()
    bad = sum(1 for k in range(0, len(a), 3) if a[k:k + 3] != bb[k:k + 3])
    print('pngcmp: %s vs %s: %d of %d pixels differ%s' % (raw, png, bad, w * h, ' (EXACT)' if not bad else ''))
    return 1 if bad else 0


def raw2png(raw, png, w, h):
    from PIL import Image
    Image.frombytes('RGB', (w, h), rgb(open(raw, 'rb').read(), w, h)).save(png)


RGB_FMTS = {1, 3, 0x1909, 0x803F, 0x8040, 0x8041, 0x8042, 0x2A10, 0x1907, 0x804F, 0x8050, 0x8051,
            0x8052, 0x8053, 0x8054}


def texmem(path):
    b, off, names, ver = read(path)
    idx = {n: i for i, n in enumerate(names)}
    I = lambda n: idx.get(n, -1)
    bind, gen, dele, ti, tsi = I('glBindTexture'), I('glGenTextures'), I('glDeleteTextures'), \
        I('glTexImage2D'), I('glTexSubImage2D')
    cur = 0
    tex = {}          # name -> {level: bytes}
    sub = set()
    snap = None
    first = None

    def totals():
        l0 = sum(v.get(0, 0) for v in tex.values())
        mips = sum(sum(x for k, x in v.items() if k > 0) for v in tex.values())
        lm = sum(sum(v.values()) for n, v in tex.items() if n in sub)
        return l0, mips, lm, len(tex), len([n for n in tex if n in sub])
    for i, o, nw in records(b, off):
        if i == bind:
            t, n = words(b, o, 1, 2)
            if t == 0x0DE1:
                cur = n
        elif i == ti:
            target, level, ifmt, w, h = words(b, o, 1, 5)
            if target == 0x0DE1:
                per = 2 if ifmt in RGB_FMTS else 3
                tex.setdefault(cur, {})[level] = w * h * per
        elif i == tsi:
            sub.add(cur)
        elif i == dele:
            n, = words(b, o, 1, 1)
            for nm in words(b, o, 3, n):
                tex.pop(nm, None)
        elif i == SWAP:
            f, fl = words(b, o, 1, 2)
            if fl & 1 and snap is None:
                snap = totals()
                first = f
    for label, t in (('after the load phase (frame %s)' % first, snap), ('at the end', totals())):
        if t:
            l0, mips, lm, n, nl = t
            print('texmem %s: %d textures, level 0 %d B, levels > 0 %d B, total %d B; '
                  '%d sub-updated textures (lightmaps) %d B' % (label, n, l0, mips, l0 + mips, nl, lm))


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == 'summary':
        summary(a[1])
    elif a[0] == 'hashes':
        hashes(a[1])
    elif a[0] == 'names':
        used_names(a[1])
    elif a[0] == 'pngcmp':
        sys.exit(pngcmp(a[1], a[2], *(int(x) for x in a[3:5])))
    elif a[0] == 'texmem':
        texmem(a[1])
    elif a[0] == 'raw2png':
        raw2png(a[1], a[2], int(a[3]), int(a[4]))
    else:
        sys.exit(__doc__)
