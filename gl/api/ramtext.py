#!/usr/bin/env python3
"""ramtext.py - build side of lever L1 (tinygl/source/s31_ramtext.c). s31, MIT.

  ramtext.py rename OBJCOPY LIST OBJDIR [ORDER]
      Move the sections of the functions api/ramtext.list names into
      "s31hot_text" (objcopy --rename-section: the code is unchanged).
      With ORDER (api/hotorder.list) the range is laid out in its order
      (see cmd_rename).
  ramtext.py fix ANALYSIS TARGET
      ANALYSIS is the link with -Wl,-q (the linker's relocations kept),
      TARGET the same link without it (what ships). Lists every PC-relative
      reference from inside [__s31hot_start, __s31hot_end) to outside it,
      proves by simulating the move that each one still reaches its target
      from the RAM slot, fails on anything that cannot be moved (absolute
      relocations, a compressed or conditional jump out of the range, a TLS
      access), checks that TARGET is ANALYSIS byte for byte, and writes the
      table into TARGET's s31_ramtext_fix[].
  ramtext.py fix ELF
      The same on one file linked with -q (the static test programs).

No dependencies beyond the standard library: it runs in the Buildroot build
and in the build containers as they are.
"""
import os
import re
import struct
import subprocess
import sys

MAGIC, UNSET, HDR = 0x52313353, 0xffffffff, 4
# s31_ramtext.c's table kinds: an auipc (K_RV_HI) is followed by its low
# halves (K_RV_LO_I: addi, loads, jalr; K_RV_LO_S: stores)
K_RV_LO_S, K_RV_HI, K_RV_JAL, K_A64_ADRP, K_A64_B26, K_A64_ADR, K_A64_LDLIT, K_RV_LO_I = range(8)
KNAME = {1: 'auipc', 2: 'jal', 3: 'adrp', 4: 'b/bl', 5: 'adr', 6: 'ldr-literal'}
GRAIN = 64                      # s31_ramtext.c RT_GRAIN
EM_RISCV, EM_AARCH64 = 243, 183
SHT_SYMTAB, SHT_RELA, SHT_NOBITS = 2, 4, 8
SHF_ALLOC = 2


def die(msg):
    sys.stderr.write('ramtext.py: ERROR: %s\n' % msg)
    sys.exit(1)


class Elf:
    def __init__(self, path):
        self.path = path
        self.d = d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[5] != 1:
            die('%s: not a little-endian ELF file' % path)
        self.b64 = d[4] == 2
        self.machine, = struct.unpack_from('<H', d, 18)
        if self.b64:
            shoff, = struct.unpack_from('<Q', d, 0x28)
            shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
        else:
            shoff, = struct.unpack_from('<I', d, 0x20)
            shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
        self.sh = []
        for i in range(shnum):
            o = shoff + i * shentsize
            if self.b64:
                n, t, fl, a, off, sz, ln, inf, al, es = struct.unpack_from('<IIQQQQIIQQ', d, o)
            else:
                n, t, fl, a, off, sz, ln, inf, al, es = struct.unpack_from('<IIIIIIIIII', d, o)
            self.sh.append(dict(name=n, type=t, flags=fl, addr=a, off=off, size=sz,
                                link=ln, info=inf, entsize=es))
        strtab = self.sh[shstrndx]
        for s in self.sh:
            s['name'] = self.cstr(strtab['off'] + s['name'])
        self._syms = None

    def cstr(self, o):
        return self.d[o:self.d.index(b'\0', o)].decode()

    def data(self, s):
        return b'' if s['type'] == SHT_NOBITS else self.d[s['off']:s['off'] + s['size']]

    def syms(self):
        if self._syms is None:
            self._syms = []
            for s in self.sh:
                if s['type'] != SHT_SYMTAB:
                    continue
                st = self.sh[s['link']]
                es = 24 if self.b64 else 16
                for o in range(s['off'], s['off'] + s['size'], es):
                    if self.b64:
                        n, info, other, shndx, v, sz = struct.unpack_from('<IBBHQQ', self.d, o)
                    else:
                        n, v, sz, info, other, shndx = struct.unpack_from('<IIIBBH', self.d, o)
                    self._syms.append(dict(name=self.cstr(st['off'] + n), value=v, size=sz,
                                           shndx=shndx, type=info & 15))
        return self._syms

    def sym(self, name):
        hits = [s for s in self.syms() if s['name'] == name and s['shndx'] != 0]
        if len(hits) != 1:
            return None
        return hits[0]

    def relas(self):
        """(target section, [(offset, type, symbol index, addend)]) for every
        non-allocated RELA section (the relocations -q kept)"""
        es = 24 if self.b64 else 12
        for s in self.sh:
            if s['type'] != SHT_RELA or s['flags'] & SHF_ALLOC:
                continue
            out = []
            for o in range(s['off'], s['off'] + s['size'], es):
                if self.b64:
                    off, info, add = struct.unpack_from('<QQq', self.d, o)
                    out.append((off, info & 0xffffffff, info >> 32, add))
                else:
                    off, info, add = struct.unpack_from('<IIi', self.d, o)
                    out.append((off, info & 0xff, info >> 8, add))
            yield self.sh[s['info']], s['link'], out

    def read(self, addr, n):
        for s in self.sh:
            if s['flags'] & SHF_ALLOC and s['type'] != SHT_NOBITS and \
                    s['addr'] <= addr and addr + n <= s['addr'] + s['size']:
                o = s['off'] + addr - s['addr']
                return self.d[o:o + n]
        die('%s: address 0x%x is not in a section with contents' % (self.path, addr))

    def foff(self, addr):
        for s in self.sh:
            if s['flags'] & SHF_ALLOC and s['type'] != SHT_NOBITS and \
                    s['addr'] <= addr < s['addr'] + s['size']:
                return s['off'] + addr - s['addr']
        die('%s: address 0x%x has no file offset' % (self.path, addr))


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


# ---- RISC-V decoders: the target of a PC-relative instruction at pc
def rv_auipc(w, pc):
    return (pc + sext(w & 0xfffff000, 32)) & 0xffffffff


def rv_jal(w, pc):
    imm = ((w >> 31) << 20) | (((w >> 21) & 0x3ff) << 1) | (((w >> 20) & 1) << 11) | \
        (((w >> 12) & 0xff) << 12)
    return (pc + sext(imm, 21)) & 0xffffffff


def rv_branch(w, pc):
    imm = ((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 0x3f) << 5) | \
        (((w >> 8) & 0xf) << 1)
    return (pc + sext(imm, 13)) & 0xffffffff


def rv_cj(h, pc):
    imm = (((h >> 12) & 1) << 11) | (((h >> 11) & 1) << 4) | (((h >> 9) & 3) << 8) | \
        (((h >> 8) & 1) << 10) | (((h >> 7) & 1) << 6) | (((h >> 6) & 1) << 7) | \
        (((h >> 3) & 7) << 1) | (((h >> 2) & 1) << 5)
    return (pc + sext(imm, 12)) & 0xffffffff


def rv_cb(h, pc):
    imm = (((h >> 12) & 1) << 8) | (((h >> 10) & 3) << 3) | (((h >> 5) & 3) << 6) | \
        (((h >> 3) & 3) << 1) | (((h >> 2) & 1) << 5)
    return (pc + sext(imm, 9)) & 0xffffffff


def rv_jal_patch(w, d):
    imm = sext(((w >> 31) << 20) | (((w >> 21) & 0x3ff) << 1) | (((w >> 20) & 1) << 11) |
               (((w >> 12) & 0xff) << 12), 21) - d
    if not -(1 << 20) <= imm < (1 << 20):
        return None
    return (w & 0xfff) | (((imm >> 20) & 1) << 31) | (((imm >> 1) & 0x3ff) << 21) | \
        (((imm >> 11) & 1) << 20) | (((imm >> 12) & 0xff) << 12)


# ---- AArch64
def a64_adr(w, pc):
    imm = sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
    if w >> 31:
        return ((pc & ~0xfff) + (imm << 12)) & 0xffffffffffffffff
    return (pc + imm) & 0xffffffffffffffff


def a64_b26(w, pc):
    return (pc + 4 * sext(w & 0x3ffffff, 26)) & 0xffffffffffffffff


def a64_lo19(w, pc):
    return (pc + 4 * sext((w >> 5) & 0x7ffff, 19)) & 0xffffffffffffffff


def a64_patch(w, kind, d):
    if kind in (K_A64_ADRP, K_A64_ADR):
        imm = sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21) - \
            (d // 4096 if kind == K_A64_ADRP else d)
        if not -(1 << 20) <= imm < (1 << 20):
            return None
        return (w & 0x9f00001f) | ((imm & 3) << 29) | (((imm >> 2) & 0x7ffff) << 5)
    if kind == K_A64_B26:
        imm = sext(w & 0x3ffffff, 26) - d // 4
        if not -(1 << 25) <= imm < (1 << 25):
            return None
        return (w & 0xfc000000) | (imm & 0x3ffffff)
    imm = sext((w >> 5) & 0x7ffff, 19) - d // 4
    if not -(1 << 18) <= imm < (1 << 18):
        return None
    return (w & 0xff00001f) | ((imm & 0x7ffff) << 5)


# RISC-V relocation types
RV_BRANCH, RV_JAL, RV_CALL, RV_CALL_PLT, RV_GOT_HI20 = 16, 17, 18, 19, 20
RV_PCREL_HI20, RV_PCREL_LO12_I, RV_PCREL_LO12_S = 23, 24, 25
RV_ALIGN, RV_RVC_BRANCH, RV_RVC_JUMP, RV_RELAX = 43, 44, 45, 51
RV_DIFF = {33, 34, 35, 36, 37, 38, 39, 40, 52, 53, 54, 55, 56}   # ADDn, SUBn, SUB6, SETn
# AArch64
A_LD_PREL_LO19, A_ADR_PREL_LO21, A_ADR_PREL_PG_HI21, A_ADR_PREL_PG_HI21_NC = 273, 274, 275, 276
A_TSTBR14, A_CONDBR19, A_JUMP26, A_CALL26, A_ADR_GOT_PAGE = 279, 280, 282, 283, 311
A_LO12 = {277, 278, 284, 285, 286, 299, 312, 313}


def analyse(e):
    """-> start, end, [(offset, kind, target, what)], outside-in counts"""
    s0, s1 = e.sym('__s31hot_start'), e.sym('__s31hot_end')
    if s0 is None or s1 is None:
        die('%s: no __s31hot_start/__s31hot_end (not linked with api/ramtext.ld?)' % e.path)
    start, end = s0['value'], s1['value']
    if end <= start:
        die('%s: the hot range is empty (api/ramtext.list matched nothing?)' % e.path)
    syms = e.syms()             # the one .symtab (-q relocations index it)
    fixes, bad, inside, into = [], [], {}, []
    seen = 0
    # RISC-V: the low halves of each auipc, by the auipc's address (a
    # PCREL_LO12 relocation points at its auipc, not at the target)
    lomap = {}
    if e.machine == EM_RISCV:
        for sec, symtab, rels in e.relas():
            for off, typ, si, add in rels:
                if start <= off < end and typ in (RV_PCREL_LO12_I, RV_PCREL_LO12_S):
                    lomap.setdefault(syms[si]['value'] + add, []).append(
                        (off - start, K_RV_LO_I if typ == RV_PCREL_LO12_I else K_RV_LO_S))
    for sec, symtab, rels in e.relas():
        lo, hi = sec['addr'], sec['addr'] + sec['size']
        overlaps = lo < end and start < hi
        for off, typ, si, add in rels:
            S = syms[si]['value'] if si else 0
            undef = si and syms[si]['shndx'] == 0
            name = syms[si]['name'] if si else ''
            if not (overlaps and start <= off < end):
                # a reference INTO the range from outside: fine (it reaches
                # the XIP copy); counted for the report - except a jump
                # table's label difference, whose table stayed outside: the
                # copy would jump through it back into the XIP code
                T = S + add
                if not undef and start <= T < end:
                    inside[typ] = inside.get(typ, 0) + 1
                    into.append((off, typ, T, sec['flags']))
                    if e.machine == EM_RISCV and typ in RV_DIFF:
                        bad.append('0x%x: a jump table outside the range points into it '
                                   '(%s): its .rodata.<function> did not move' % (off, name))
                continue
            seen += 1
            where = '0x%x (+0x%x)' % (off, off - start)
            if e.machine == EM_RISCV:
                if typ in (0, RV_PCREL_LO12_I, RV_PCREL_LO12_S, RV_ALIGN, RV_RELAX):
                    continue
                if typ in RV_DIFF:
                    # a jump table entry, label - table: fine when both
                    # ends move, which is every label inside the range
                    if undef or not start <= S + add < end:
                        bad.append('%s: jump-table entry against %s outside the range'
                                   % (where, name))
                    continue
                w, = struct.unpack('<I', e.read(off, 4)) if typ != RV_RVC_JUMP and \
                    typ != RV_RVC_BRANCH else (0,)
                parts = []
                if typ == RV_JAL:
                    T, kind = rv_jal(w, off), K_RV_JAL
                elif typ in (RV_CALL, RV_CALL_PLT):
                    w2, = struct.unpack('<I', e.read(off + 4, 4))
                    T, kind = (rv_auipc(w, off) + sext(w2 >> 20, 12)) & 0xffffffff, K_RV_HI
                    parts = [(off + 4 - start, K_RV_LO_I)]      # its jalr
                elif typ == RV_PCREL_HI20:
                    T, kind = (S + add) & 0xffffffff, K_RV_HI
                    parts = lomap.get(off, [])
                    if undef:
                        bad.append('%s: PCREL_HI20 against undefined %s' % (where, name))
                        continue
                    if not -2048 <= T - rv_auipc(w, off) <= 2047:
                        bad.append('%s: auipc does not match its relocation (%s)' % (where, name))
                        continue
                elif typ == RV_GOT_HI20:
                    T, kind = rv_auipc(w, off), K_RV_HI
                    parts = lomap.get(off, [])
                    if start <= T < end:
                        bad.append('%s: GOT access into the range' % where)
                        continue
                    T = None        # always outside: the GOT slot
                elif typ in (RV_BRANCH, RV_RVC_BRANCH, RV_RVC_JUMP):
                    if typ == RV_BRANCH:
                        T = rv_branch(w, off)
                    else:
                        h, = struct.unpack('<H', e.read(off, 2))
                        T = rv_cj(h, off) if typ == RV_RVC_JUMP else rv_cb(h, off)
                    if not start <= T < end:
                        bad.append('%s: %s leaves the range (to 0x%x %s): it cannot be moved'
                                   % (where, {16: 'branch', 44: 'c.b*', 45: 'c.j/c.jal'}[typ],
                                      T, name))
                    continue
                elif w & 3 == 3 and w & 0x7f in (0x03, 0x07, 0x13, 0x23, 0x27, 0x67) and \
                        (w >> 15) & 31 == 3:
                    # a static link's gp relaxation (auipc gone, the access
                    # now gp-relative: binutils' internal GPREL types): the
                    # address comes from gp, not the pc, so it moves freely
                    continue
                else:
                    bad.append('%s: relocation type %d (%s) cannot be moved' % (where, typ, name))
                    continue
            elif e.machine == EM_AARCH64:
                if typ == 0 or typ in A_LO12:
                    continue
                parts = []
                w, = struct.unpack('<I', e.read(off, 4))
                if typ in (A_JUMP26, A_CALL26):
                    T, kind = a64_b26(w, off), K_A64_B26
                elif typ == A_ADR_PREL_LO21:
                    T, kind = a64_adr(w, off), K_A64_ADR
                elif typ == A_LD_PREL_LO19:
                    T, kind = a64_lo19(w, off), K_A64_LDLIT
                elif typ in (A_ADR_PREL_PG_HI21, A_ADR_PREL_PG_HI21_NC):
                    T, kind = S + add, K_A64_ADRP
                    if undef or (T & ~0xfff) != a64_adr(w, off):
                        bad.append('%s: adrp does not match its relocation (%s)' % (where, name))
                        continue
                elif typ == A_ADR_GOT_PAGE:
                    T, kind = None, K_A64_ADRP
                elif typ in (A_CONDBR19, A_TSTBR14):
                    T = a64_lo19(w, off) if typ == A_CONDBR19 else \
                        (off + 4 * sext((w >> 5) & 0x3fff, 14))
                    if not start <= T < end:
                        bad.append('%s: conditional branch leaves the range' % where)
                    continue
                else:
                    bad.append('%s: relocation type %d (%s) cannot be moved' % (where, typ, name))
                    continue
            else:
                die('%s: machine %d is not supported' % (e.path, e.machine))
            if T is not None and start <= T < end:
                continue            # moves with the range
            if kind == K_RV_HI and not parts:
                bad.append('%s: auipc (to %s) without its low half' % (where, name))
                continue
            fixes.append((off - start, kind, T, name, sorted(parts)))
    if seen == 0:
        die('%s: no relocations inside the hot range: link with -Wl,-q' % e.path)
    if bad:
        die('%s: the hot range cannot be moved:\n  ' % e.path + '\n  '.join(bad))
    return start, end, fixes, inside, into


def lo_get(w, k):
    return sext(w >> 20, 12) if k == K_RV_LO_I else sext(((w >> 25) << 5) | ((w >> 7) & 31), 12)


def lo_put(w, k, lo):
    u = lo & 0xfff
    if k == K_RV_LO_I:
        return (w & 0x000fffff) | (u << 20)
    return (w & 0x01fff07f) | ((u >> 5) << 25) | ((u & 31) << 7)


def simulate(e, start, end, fixes, dst):
    """apply the fixes as s31_ramtext.c does, to a copy at dst, and check
    that every site still reaches its old target"""
    d = dst - start
    img = bytearray(e.read(start, end - start))
    for off, kind, T, name, parts in fixes:
        w, = struct.unpack_from('<I', img, off)
        pc_old, pc_new = start + off, dst + off
        if kind == K_RV_HI:
            los = [lo_get(struct.unpack_from('<I', img, po)[0], pk) for po, pk in parts]
            if len(set(los)) != 1:
                die('site +0x%x (auipc to %s): its low halves disagree' % (off, name))
            old = (rv_auipc(w, pc_old) + los[0]) & 0xffffffff
            rel = (old - pc_new) & 0xffffffff
            hi = (rel + 0x800) & 0xfffff000
            lo = sext(rel - hi, 12)
            nw = (w & 0xfff) | hi
            struct.pack_into('<I', img, off, nw)
            for po, pk in parts:
                pw, = struct.unpack_from('<I', img, po)
                struct.pack_into('<I', img, po, lo_put(pw, pk, lo))
                nlo = lo_get(struct.unpack_from('<I', img, po)[0], pk)
                if (rv_auipc(nw, pc_new) + nlo) & 0xffffffff != old:
                    die('site +0x%x (auipc to %s) does not reach its target from 0x%x'
                        % (off, name, dst))
            continue
        if kind == K_RV_JAL:
            nw = rv_jal_patch(w, d)
            ok = nw is not None and rv_jal(nw, pc_new) == rv_jal(w, pc_old)
        elif kind == K_A64_ADRP:
            tp = a64_adr(w, pc_old)
            imm = (tp - (pc_new & ~0xfff)) // 4096
            ok = -(1 << 20) <= imm < (1 << 20)
            nw = (w & 0x9f00001f) | ((imm & 3) << 29) | (((imm >> 2) & 0x7ffff) << 5)
            ok = ok and a64_adr(nw, pc_new) == tp
        else:
            nw = a64_patch(w, kind, d)
            if nw is None:
                ok = False
            elif kind == K_A64_B26:
                ok = a64_b26(nw, pc_new) == a64_b26(w, pc_old)
            elif kind == K_A64_LDLIT:
                ok = a64_lo19(nw, pc_new) == a64_lo19(w, pc_old)
            else:
                ok = a64_adr(nw, pc_new) == a64_adr(w, pc_old)
        if not ok:
            die('site +0x%x (%s to %s) does not reach its target from 0x%x'
                % (off, KNAME[kind], name, dst))
        struct.pack_into('<I', img, off, nw)
    return img


SHT_DYNSYM = 11
SHT_PROGBITS = 1
SHF_WRITE, SHF_EXECINSTR = 1, 4
PT_GNU_RELRO = 0x6474e552
DW_MAGIC = 0x57313353           # "S31W": s31_ramtext.c's data-word table
DW_HDR = 5                      # magic, count, link start, relro page lo, hi
RV_32, A_ABS64 = 1, 257
DYN_REL = {EM_RISCV: (3, 1, 2), EM_AARCH64: (1027, 257, 1025)}   # RELATIVE, 32/ABS64, GLOB_DAT


def phdrs(e):
    d = e.d
    if e.b64:
        phoff, = struct.unpack_from('<Q', d, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    else:
        phoff, = struct.unpack_from('<I', d, 0x1c)
        phentsize, phnum = struct.unpack_from('<HH', d, 0x2a)
    for i in range(phnum):
        o = phoff + i * phentsize
        if e.b64:
            t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, o)
        else:
            t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<IIIIIIII', d, o)
        yield t, va, ms


def data_words(a, t, start, end, into):
    """the words of the writable image that hold an address in the range:
    function-pointer tables (-q's absolute relocations in data) and, in a
    shared library, what the dynamic relocations put there (RELATIVE, the
    GOT). s31_ramtext.c rewrites each to the copy -> sorted vaddrs"""
    ws = set()
    absr = RV_32 if a.machine == EM_RISCV else A_ABS64
    for off, typ, T, fl in into:
        if typ == absr and fl & SHF_ALLOC and not fl & SHF_EXECINSTR:
            ws.add(off)
    rel, abs_, glob = DYN_REL[a.machine]
    es = 24 if t.b64 else 12
    dsyms = None
    for s in t.sh:
        if s['type'] != SHT_RELA or not s['flags'] & SHF_ALLOC:
            continue
        if dsyms is None:
            dsyms = []
            ds = t.sh[s['link']]
            ses = 24 if t.b64 else 16
            for o in range(ds['off'], ds['off'] + ds['size'], ses):
                if t.b64:
                    n, info, other, shndx, v, sz = struct.unpack_from('<IBBHQQ', t.d, o)
                else:
                    n, v, sz, info, other, shndx = struct.unpack_from('<IIIBBH', t.d, o)
                dsyms.append((v, shndx))
        for o in range(s['off'], s['off'] + s['size'], es):
            if t.b64:
                off, info, add = struct.unpack_from('<QQq', t.d, o)
                typ, si = info & 0xffffffff, info >> 32
            else:
                off, info, add = struct.unpack_from('<IIi', t.d, o)
                typ, si = info & 0xff, info >> 8
            if typ == rel:
                T = add
            elif typ in (abs_, glob) and si and dsyms[si][1] != 0:
                T = dsyms[si][0] + (add if typ == abs_ else 0)
            else:
                continue
            if start <= T < end:
                ws.add(off)
    out, ro = [], 0
    for w in sorted(ws):
        if w & 3:
            die('data word 0x%x holding a hot address is not aligned' % w)
        sec = [x for x in t.sh if x['flags'] & SHF_ALLOC and x['addr'] <= w < x['addr'] + x['size']]
        if not sec or not sec[0]['flags'] & SHF_WRITE:
            ro += 1         # a static link's const table in .rodata: it
            continue        # keeps the XIP address (correct, not covered)
        out.append(w)
    return out, ro


def cold_refs(a, start, end, into):
    """what still reaches the XIP copy: code outside the range that calls
    into it or takes the address of something in it -> report lines"""
    fns = sorted((s['value'], s['name']) for s in a.syms() if s['type'] == 2 and s['shndx'] != 0)
    import bisect
    addrs = [v for v, _ in fns]

    def name(x):
        i = bisect.bisect_right(addrs, x) - 1
        return fns[i][1] if i >= 0 else hex(x)
    calls, taken = {}, {}
    for off, typ, T, fl in into:
        if not fl & SHF_EXECINSTR:
            continue
        k = '%s -> %s' % (name(off), name(T))
        if a.machine == EM_RISCV and typ in (RV_CALL, RV_CALL_PLT, RV_JAL) or \
                a.machine == EM_AARCH64 and typ in (A_JUMP26, A_CALL26):
            calls[k] = calls.get(k, 0) + 1
        elif a.machine == EM_RISCV and typ in (RV_PCREL_HI20, RV_GOT_HI20) or \
                a.machine == EM_AARCH64 and typ in (A_ADR_PREL_PG_HI21, A_ADR_PREL_PG_HI21_NC,
                                                    A_ADR_PREL_LO21, A_ADR_GOT_PAGE):
            taken[k] = taken.get(k, 0) + 1
    out = ['cold call into the range: %s' % k for k in sorted(calls)]
    out += ['cold address of a hot function: %s' % k for k in sorted(taken)]
    return out, len(calls), len(taken)


def alloc_image(e):
    img = {}
    for s in e.sh:
        if not s['flags'] & SHF_ALLOC or s['size'] == 0 or s['name'] == '.note.gnu.build-id':
            continue            # (-q keeps an empty .tm_clone_table; the build
                                # id hashes the -q file's extra sections too)
        data = e.data(s)
        if s['type'] == SHT_DYNSYM:
            es = 24 if e.b64 else 16
            rows = []
            for o in range(0, len(data), es):
                row = bytearray(data[o:o + es])
                at = 6 if e.b64 else 14
                shndx, = struct.unpack_from('<H', row, at)
                struct.pack_into('<H', row, at, 0)
                rows.append((bytes(row), e.sh[shndx]['name'] if 0 < shndx < 0xff00 else shndx))
            data = rows
        img[(s['name'], s['addr'])] = (s['type'], s['flags'], s['size'], data)
    return img


def fnv1a(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xffffffff
    return h


def cmd_fix(apath, tpath):
    a = Elf(apath)
    start, end, fixes, inside, into = analyse(a)
    t = Elf(tpath) if tpath != apath else a
    if tpath != apath:
        # what ships must be what was analysed: every allocated section, at
        # the same address with the same bytes. (.dynsym only differs in
        # st_shndx, as -q adds sections: compared by section name.)
        sa, st = alloc_image(a), alloc_image(t)
        if sa != st:
            diff = sorted(set(k for k in set(sa) | set(st) if sa.get(k) != st.get(k)))
            die('%s differs from its -q twin %s in %s' % (tpath, apath,
                                                          ', '.join(str(k) for k in diff)))
    tab, slot = t.sym('s31_ramtext_fix'), t.sym('s31_ramtext_slot')
    if tab is None or slot is None:
        die('%s: no s31_ramtext_fix / s31_ramtext_slot' % tpath)
    fixes.sort()
    entries = []
    for o, k, _, _, parts in fixes:
        entries.append((o << 3) | k)
        entries += [(po << 3) | pk for po, pk in parts]
    fixmax = tab['size'] // 4
    if len(entries) > fixmax - HDR:
        die('%d table entries, s31_ramtext_fix holds %d: raise RT_FIXMAX in s31_ramtext.c'
            % (len(entries), fixmax - HDR))
    grain = 4096 if t.machine == EM_AARCH64 else GRAIN     # s31_ramtext.c RT_GRAIN
    length, off = end - start, start & (grain - 1)
    span = (off + length + 4095) & ~4095
    page = (slot['value'] + 4095) & ~4095       # s31_ramtext.c: its whole pages
    if page + span > slot['value'] + slot['size']:
        die('slot 0x%x+%d cannot hold %d bytes at offset %d: raise RT_SLOT'
            % (slot['value'], slot['size'], length, off))
    dst = page + off
    simulate(a, start, end, fixes, dst)
    code = a.read(start, length)
    fo = t.foff(tab['value'])
    old = struct.unpack_from('<II', t.d, fo)
    if old != (MAGIC, UNSET):
        die('%s: s31_ramtext_fix is not the unprocessed table (%08x %08x)' % ((tpath,) + old))
    words = [MAGIC, len(entries), length, fnv1a(code)] + entries
    with open(tpath, 'r+b') as f:
        f.seek(fo)
        f.write(struct.pack('<%dI' % len(words), *words))
    dws, dro = data_words(a, t, start, end, into)
    dtab = t.sym('s31_ramtext_dw')
    if dtab is None:
        die('%s: no s31_ramtext_dw' % tpath)
    rlo = rhi = 0
    for pt, va, ms in phdrs(t):
        if pt == PT_GNU_RELRO:
            rlo, rhi = va & ~4095, (va + ms) & ~4095    # as ld.so protects it
    if len(dws) > dtab['size'] // 4 - DW_HDR:
        die('%d data words, s31_ramtext_dw holds %d: raise S31GL_RAMTEXT_DWMAX'
            % (len(dws), dtab['size'] // 4 - DW_HDR))
    fo = t.foff(dtab['value'])
    if struct.unpack_from('<II', t.d, fo) != (DW_MAGIC, UNSET):
        die('%s: s31_ramtext_dw is not the unprocessed table' % tpath)
    dwords = [DW_MAGIC, len(dws), start & 0xffffffff, rlo & 0xffffffff, rhi & 0xffffffff] + \
        [w & 0xffffffff for w in dws]
    with open(tpath, 'r+b') as f:
        f.seek(fo)
        f.write(struct.pack('<%dI' % len(dwords), *dwords))
    rep, ncall, ntaken = cold_refs(a, start, end, into)
    rp = os.environ.get('S31GL_RAMTEXT_REPORT')
    if rp:
        with open(rp, 'w') as f:
            f.write('\n'.join(rep) + '\n')
    kinds = {}
    for _, k, _, _, _ in fixes:
        kinds[KNAME[k]] = kinds.get(KNAME[k], 0) + 1
    print('ramtext: %s: %d bytes at 0x%x-0x%x, %d pages in RAM, %d sites (%s), '
          '%d table entries; slot 0x%x, displacement %+d; %d references into the '
          'range from outside: %d data words moved to the copy, %d cold call sites, '
          '%d cold address-takes, %d read-only words left'
          % (os.path.basename(tpath), length, start, end, span // 4096, len(fixes),
             ', '.join('%s %d' % kv for kv in sorted(kinds.items())), len(entries),
             page, dst - start, sum(inside.values()), len(dws), ncall, ntaken, dro))


# a function's sections: its own, its clones, its cold part - and its
# .rodata.<name>, which is where GCC puts its jump tables: they must move
# with the code, as a PIC table holds label - table offsets and the code
# jumps to table + entry
CLONE = r'(\.(part|isra|constprop|cold|lto_priv)\.?\d*)*$'


def section_re(fn):
    return re.compile(r'^\.text(\.unlikely)?\.%s' % re.escape(fn) + CLONE)


def rodata_re(fn):
    return re.compile(r'^\.rodata\.%s' % re.escape(fn) + CLONE)


def cmd_rename(objcopy, listfile, objdir, orderfile=None):
    """With ORDERFILE (phase 6 tier 6: api/hotorder.list, lines "object
    function", hottest first) each function it names goes to
    "s31hot_text.NNNN", NNNN its line, whether or not ramtext.list names it;
    ramtext.list's other functions go to "s31hot_text.8000" and the
    .text.unlikely parts of all of them to "s31hot_text.9000".
    api/ramtext.ld sorts the range by those names, so the order is the
    file's and the code is still exactly what the compiler made."""
    order = {}
    if orderfile:
        for i, line in enumerate(l.split('#', 1)[0].split() for l in open(orderfile)):
            if line:
                order.setdefault((line[0], line[1]), len(order))
    want = {}                   # object path -> [(function, from the list)]
    def obj_of(o, where):
        for cand in ('tgl_%s.o' % o, '%s.o' % o):
            path = os.path.join(objdir, cand)
            if os.path.exists(path):
                return path
        die('%s: no object for %s' % (where, o))
    for line in open(listfile):
        line = line.split('#', 1)[0].split()
        if not line:
            continue
        obj = obj_of(line[0], listfile)
        for fn in line[1:]:
            want.setdefault(obj, []).append((line[0], fn))
    for (o, fn) in order:
        obj = obj_of(o, orderfile)
        if (o, fn) not in want.get(obj, []):
            want.setdefault(obj, []).append((o, fn))
    total = 0
    for obj, fns in want.items():
        names = [s['name'] for s in Elf(obj).sh]
        args, done = [], set()
        for o, fn in fns:
            r, rd = section_re(fn), rodata_re(fn)
            hit = [n for n in names if r.match(n) and n not in done]
            if not hit and not any(r.match(n) for n in names):
                die('%s: no section for %s in %s' % (listfile, fn, obj))
            for n in hit:
                done.add(n)
                if not order:
                    dst = 's31hot_text'
                elif n.startswith('.text.unlikely.'):
                    dst = 's31hot_text.9000'
                elif (o, fn) in order:
                    dst = 's31hot_text.%04d' % order[(o, fn)]
                else:
                    dst = 's31hot_text.8000'
                args += ['--rename-section', '%s=%s' % (n, dst)]
            for n in names:
                if rd.match(n) and n not in done:
                    done.add(n)
                    args += ['--rename-section', '%s=s31hot_rodata' % n]
        if args:
            subprocess.check_call([objcopy] + args + [obj])
        total += len(args) // 2
    print('ramtext: %d sections moved to s31hot_text%s' % (total, ' (ordered: %d functions from %s)' % (len(order), orderfile) if order else ''))


def main(a):
    if len(a) in (4, 5) and a[0] == 'rename':
        cmd_rename(a[1], a[2], a[3], a[4] if len(a) == 5 else None)
    elif len(a) in (2, 3) and a[0] == 'fix':
        cmd_fix(a[1], a[-1])
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main(sys.argv[1:])
