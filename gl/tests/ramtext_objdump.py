#!/usr/bin/env python3
"""ramtext_objdump.py ELF OBJDUMP - an independent check of the RAM-copy table
(lever L1) from the disassembly rather than the relocations ramtext.py
reads. s31, MIT.

For every instruction objdump prints inside [__s31hot_start, __s31hot_end):
  - a jump or branch (jal, j, c.j, c.jal, b*, c.b*) whose target lies
    outside the range must be a 4-byte jal and be in s31_ramtext_fix[] as a
    jal site: a compressed jump or a branch out of the range cannot move;
  - an auipc whose high part (pc + imm << 12, within 2 KiB of its target)
    points more than 2 KiB outside the range must be in the table as an
    auipc site, and one pointing well inside it must not be;
  - an instruction objdump annotates with a resolved address ("# addr
    <sym>", the low half of an auipc pair) outside the range must belong to
    an auipc in the table.
(A jump table left outside the range, which would send the copy back to
flash, is ramtext.py's check: it fails the build on one.) Prints one line
per problem and a summary; exit 1 on any problem.
In the build container:
  ./docker/build.sh 'python3 /src/gl/tests/ramtext_objdump.py /src/gl/out-rv32/libGL.so.1.unstripped \\
      /src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-objdump'
"""
import os
import re
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'api'))
from ramtext import Elf, K_RV_HI, K_RV_JAL, K_RV_LO_I, K_RV_LO_S  # noqa: E402

JUMPS = re.compile(r'^(jal|j|c\.j|c\.jal|beq|bne|blt|bge|bltu|bgeu|beqz|bnez|blez|bgez|'
                   r'bltz|bgtz|bgt|ble|bgtu|bleu|c\.beqz|c\.bnez)$')


def main(elf, objdump):
    e = Elf(elf)
    start, end = e.sym('__s31hot_start')['value'], e.sym('__s31hot_end')['value']
    tab = e.sym('s31_ramtext_fix')
    w = struct.unpack_from('<4I', e.read(tab['value'], 16))
    n = w[1]
    ent = struct.unpack_from('<%dI' % n, e.read(tab['value'] + 16, 4 * n))
    hi, jal, lo = set(), set(), set()
    for x in ent:
        o, k = start + (x >> 3), x & 7
        {K_RV_HI: hi, K_RV_JAL: jal, K_RV_LO_I: lo, K_RV_LO_S: lo}[k].add(o)
    out = subprocess.run([objdump, '-d', '--no-show-raw-insn', '--start-address=0x%x' % start,
                          '--stop-address=0x%x' % end, elf],
                         capture_output=True, text=True, check=True).stdout
    bad = []
    last_auipc = {}
    counts = dict(insn=0, jump_out=0, auipc=0, auipc_out=0, lo_out=0)
    for line in out.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$', line)
        if not m:
            continue
        pc, mn, ops = int(m.group(1), 16), m.group(2), m.group(3)
        counts['insn'] += 1
        if JUMPS.match(mn):
            t = re.search(r'\b([0-9a-f]+) <', ops)
            if t:
                tgt = int(t.group(1), 16)
                if not start <= tgt < end:
                    counts['jump_out'] += 1
                    if mn != 'jal' and mn != 'j':
                        bad.append('0x%x %s %s: leaves the range and cannot move' % (pc, mn, ops))
                    elif pc not in jal:
                        bad.append('0x%x %s %s: leaves the range, not in the table' % (pc, mn, ops))
        elif mn == 'auipc':
            counts['auipc'] += 1
            imm = int(ops.split(',')[1], 0)
            page = (pc + (imm << 12 if imm < 0x80000 else (imm - 0x100000) << 12)) & 0xffffffff
            if not start - 2048 <= page < end + 2048:
                counts['auipc_out'] += 1
                if pc not in hi:
                    bad.append('0x%x auipc to 0x%x: outside the range, not in the table' % (pc, page))
            elif start + 2048 <= page < end - 2048 and pc in hi:
                bad.append('0x%x auipc to 0x%x: inside the range, but in the table' % (pc, page))
        # the low half of a pair: an I/S-type instruction whose base register
        # an auipc wrote last (objdump also annotates lui constants: those
        # are not pc-relative and are skipped)
        args = [a.strip() for a in ops.split('#')[0].split(',')]
        base = None
        if args and len(args) >= 2:
            mm = re.match(r'^-?\w*\((\w+)\)$', args[-1])
            if mm:
                base = mm.group(1)                  # load/store/jalr: off(base)
            elif mn in ('addi', 'jalr') and len(args) >= 3:
                base = args[1]
        c = re.search(r'#\s*([0-9a-f]+)\s*<', ops)
        if c and base is not None and base in last_auipc and mn != 'auipc':
            a = int(c.group(1), 16)
            if not start <= a < end:
                counts['lo_out'] += 1
                if pc not in lo:
                    bad.append('0x%x %s %s: reaches outside, not in the table' % (pc, mn, ops))
        if mn == 'auipc':
            last_auipc[args[0]] = pc
        elif args and args[0] and not mn.startswith(('s', 'fs', 'c.s', 'c.fs', 'b', 'c.b', 'j')) \
                and mn not in ('fence', 'fence.i', 'ecall', 'ebreak', 'ret', 'c.jr', 'jr', 'nop', 'c.nop'):
            last_auipc.pop(args[0], None)           # rd overwritten
    for b in bad:
        print(b)
    print('ramtext_objdump: %d instructions in 0x%x-0x%x; %d jumps and %d auipc pairs leave '
          'the range, %d annotated low halves outside; table: %d auipc, %d low halves, %d jal; '
          '%d problems' % (counts['insn'], start, end, counts['jump_out'], counts['auipc_out'],
                           counts['lo_out'], len(hi), len(lo), len(jal), len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main(*sys.argv[1:3])
