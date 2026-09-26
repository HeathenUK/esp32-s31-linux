#!/bin/bash
# mem.sh OUT IMAGE... - memory traffic of the counted frames by ADDRESS
# (review 3a M3), with the QEMU plugin qemu/memclass.c: loads, stores and
# bytes to the colour buffers, the depth buffer, the stack and everything
# else, plus the distinct 64-byte lines dirtied, per frame, averaged and for
# the dearest frame. Needs images built by this tree's q_ui.c (q_bufs,
# q_fmark). Writes OUT/mem_IMAGE.txt and prints a summary line per image:
#   IMAGE: col st/B/lines  z st/B/lines  stack st/B  other st/B (per frame)
# No -icount: the plugin counts accesses, not time. s31, MIT.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$1" && pwd); shift
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
NM=$TC/bin/riscv32-esp-elf-nm
PL=$HERE/qemu/memclass.dylib
if [ ! -f "$PL" ] || [ "$HERE/qemu/memclass.c" -nt "$PL" ]; then
	cc -O2 -shared -fPIC -undefined dynamic_lookup $(pkg-config --cflags glib-2.0) \
		-I"$(brew --prefix qemu)/include" "$HERE/qemu/memclass.c" -o "$PL"
fi
cd "$OUT"
for d in "$@"; do
	(
	a() { $NM "$d.elf" | awk -v s="$1" '$3==s{print "0x"$1}'; }
	M=$(a q_mark); F=$(a q_fmark); Q=$(a q_bufs)
	[ -n "$M" ] && [ -n "$F" ] && [ -n "$Q" ] || { echo "$d: no q_mark/q_fmark/q_bufs (old q_ui?)" > mem_$d.txt; exit 0; }
	qemu-system-riscv32 -machine virt -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
		-bios none -m 512M -nographic -semihosting-config enable=on,target=native \
		-plugin "$PL,mark=$M,fmark=$F,bufs=$Q" -d plugin -D mem_$d.txt -kernel $d.elf >/dev/null 2>&1 || true
	) &
done
wait
for d in "$@"; do
	python3 - "$OUT/mem_$d.txt" "$d" <<'PY'
import sys, re
t = open(sys.argv[1]).read()
def row(tag):
    m = re.search(r'^%s (\d+):(.*)$' % tag, t, re.M)
    if not m: return None, None
    v = {}
    for part in m.group(2).split('|'):
        f = part.split()
        if len(f) < 13: continue
        v[f[0]] = dict(st=float(f[2]), B=float(f[4]), dl=float(f[6]), ld=float(f[8]), lB=float(f[10]), rl=float(f[12]))
    return int(m.group(1)), v
n, a = row('memtotal'); k, x = row('memmax')
if a is None:
    print("%s: no memtotal (%s)" % (sys.argv[2], t.strip()[:80])); sys.exit(0)
fb = lambda v, c: "%s st %.1fk %.0f kB (%.0f kB lines) ld %.0f kB" % (c, v[c]['st']/1e3, v[c]['B']/1e3, v[c]['dl']*64/1e3, v[c]['lB']/1e3)
print("%s (%d frames): %s | %s | %s | %s || buffers %.0f kB stored, %.0f kB in dirty lines; dearest frame %d: %.0f kB (%.0f kB lines)" % (
    sys.argv[2], n, fb(a, 'col'), fb(a, 'z'), fb(a, 'stack'), fb(a, 'other'),
    (a['col']['B'] + a['z']['B'])/1e3, (a['col']['dl'] + a['z']['dl'])*64/1e3,
    k, (x['col']['B'] + x['z']['B'])/1e3, (x['col']['dl'] + x['z']['dl'])*64/1e3))
PY
done
