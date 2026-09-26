#!/usr/bin/env python3
"""ramtext_trace.py EXE [ARGS...] - where the hot range really runs (lever L1).

Runs a static RV32 test program (gl/build.sh's *.qemu, linked with
api/ramtext.ld and processed by api/ramtext.py) under qemu-user twice, with
S31GL_RAMTEXT=0 and =1, logging every translated block it executes, and
counts the blocks that ran inside the XIP hot range [__s31hot_start,
__s31hot_end) and inside its RAM copy. With the copy on, what still runs in
the XIP range is listed by the edge that entered it (caller -> callee):
those are call sites outside the range that the entry points (s31_rt) do not
cover, which is expected for cold paths (the clipper, glopEnd's polygons)
and a bug for anything per-vertex or per-pixel.

In the s31-glref-qemu image:
  docker run --rm -v $REPO:/src -w /tmp s31-glref-qemu \\
    python3 /src/gl/tests/ramtext_trace.py /src/gl/out-rv32/headless_gears.qemu 64 48 3 /tmp/h.ppm
s31, MIT.
"""
import bisect
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True      # nothing written into gl/api
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'api'))
from ramtext import Elf  # noqa: E402

QEMU = ['qemu-riscv32', '-cpu', 'rv32,zba=true,zbb=true,zbc=true,zbs=true']


def main(argv):
    exe = argv[0]
    e = Elf(exe)
    start, end = e.sym('__s31hot_start')['value'], e.sym('__s31hot_end')['value']
    slot = e.sym('s31_ramtext_slot')['value']
    d = slot + (start & 4095) - start
    fns = sorted((s['value'], s['name']) for s in e.syms()
                 if s['type'] == 2 and s['shndx'] != 0)      # STT_FUNC
    addrs = [a for a, _ in fns]

    def name(pc):
        if start + d <= pc < end + d:
            return 'RAM:' + name(pc - d)
        i = bisect.bisect_right(addrs, pc) - 1
        return fns[i][1] if i >= 0 else hex(pc)

    pcre = re.compile(r'\[[0-9a-f]+/([0-9a-f]+)/')
    for on in ('0', '1'):
        log = '/tmp/ramtext_trace.%d.%s.log' % (os.getpid(), on)
        env = dict(os.environ, S31GL_RAMTEXT=on)
        r = subprocess.run(QEMU + ['-d', 'exec,nochain', '-D', log, exe] + argv[1:],
                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        note = [l for l in r.stderr.decode().splitlines() if 'ramtext' in l]
        xip = ram = 0
        edges = {}
        prev = None
        with open(log) as f:
            for line in f:
                m = pcre.search(line)
                if not m:
                    continue
                pc = int(m.group(1), 16)
                if start <= pc < end:
                    xip += 1
                    if prev is not None and not start <= prev < end:
                        k = '%s -> %s' % (name(prev), name(pc))
                        edges[k] = edges.get(k, 0) + 1
                elif start + d <= pc < end + d:
                    ram += 1
                prev = pc
        os.unlink(log)
        tot = xip + ram
        print('S31GL_RAMTEXT=%s: exit %d, hot-range blocks %d: XIP %d, RAM %d (%.1f%% in RAM) %s'
              % (on, r.returncode, tot, xip, ram, 100.0 * ram / tot if tot else 0,
                 ' '.join(note)))
        if on == '1':
            for k, v in sorted(edges.items(), key=lambda kv: -kv[1])[:12]:
                print('    %7d entries into the XIP range: %s' % (v, k))


if __name__ == '__main__':
    main(sys.argv[1:])
