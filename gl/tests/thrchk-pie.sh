#!/bin/sh
# thrchk-pie.sh - phase 6 (S31GL_THREADS): the second rasteriser thread may
# run on Linux CPU1 (hart 0), which has no PIE, and musl's strcmp, memcmp,
# memchr, memrchr and memcpy (from 64 bytes) are PIE code - a call there
# traps and bounces the worker to CPU0. This lists every call through the
# PLT (any libc function) in the code the worker can reach: the direct-call
# closure of s31_thr.c's worker() and of every span stage, fused filler and
# texel/LOD function it calls through the pipe's pointers (the stage-name
# prefixes below; the selection functions under them are included too, so
# the list is a superset). Expected: only syscall (futex) - from worker()
# itself. Run after gl/build.sh, in the build container:
#   ./docker/build.sh "sh /src/gl/tests/thrchk-pie.sh"
# s31, MIT.
OD=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-objdump
LIB=${1:-/src/gl/out-rv32/libGL.so.1.unstripped}
$OD -d --no-show-raw-insn "$LIB" > /tmp/thr-d.txt
python3 - <<'P'
import re
calls = {}; fn = None
for l in open('/tmp/thr-d.txt'):
    m = re.match(r'^[0-9a-f]+ <(.*)>:', l)
    if m:
        fn = m.group(1); calls.setdefault(fn, set()); continue
    m = re.search(r'\s(jal|call|tail|j)\s+(?:ra,)?\s*[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>', l)
    if m and fn:
        calls[fn].add(m.group(2))
roots = [f for f in calls if f == 'worker' or f.startswith('ZB_fillBody') or
         re.match(r'(zd|zc|zt|ze|zf|zs|zo|zw|zv|zst|zx|zx8|zf8|zf1|zp|zpx|zpf8?)_', f)]
seen = set(); todo = list(roots)
while todo:
    f = todo.pop()
    if f in seen: continue
    seen.add(f)
    for g in calls.get(f, ()):
        if g not in seen: todo.append(g)
plt = {}
for f in sorted(seen):
    for g in calls.get(f, ()):
        if g.endswith('@plt'):
            plt.setdefault(g, []).append(f)
print("thrchk-pie: %d roots, %d functions reachable" % (len(roots), len(seen)))
for g in sorted(plt):
    print("PLT", g, "from", " ".join(sorted(plt[g])))
bad = [g for g in plt if not g.startswith('syscall')]
print("thrchk-pie: %d PLT callees other than syscall" % len(bad))
P
