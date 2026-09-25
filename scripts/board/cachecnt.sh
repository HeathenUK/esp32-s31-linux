#!/bin/bash
# cachecnt.sh <label> "<board command>" - the L1 cache access counters around
# one workload. The PMU this board was documented as not having.
#
# CACHE_L1_CACHE_ACS_CNT_CTRL_REG at 0x2C000180 (DR_REG_CACHE_BASE 0x2C000000,
# verified in the container's cache_reg.h 2026-09-23): enable bits 0-3 IBUS0-3,
# 4-7 DBUS0-3; clear bits 16-19 IBUS, 20-23 DBUS. Per bus: HIT, MISS, CONFLICT,
# NXTLVL_RD (+ NXTLVL_WR for DBUS), 32-bit each:
#   IBUS0 0x184..0x190   IBUS1 0x194..0x1a0
#   DBUS0 0x1c4..0x1d4   DBUS1 0x1d8..0x1e8
# IBUS0/DBUS0 are hart 0 (Linux CPU1, the lent core); IBUS1/DBUS1 are hart 1
# (Linux CPU0). The D-cache is shared; the two DBUS ports are the two harts'
# sides of it. The I-side on hart 1 mixes user PSRAM fills with kernel flash
# fills, so the D-side numbers are the genuinely new ones.
#
# The counters wrap in ~14 s at full tilt, so the window is the workload only.
# Whole-block caveat: a write to the neighbouring autoload ENA in this block
# killed the hart twice from S-mode (memory s31-icache-autoload-wedges). The
# state of the block is printed BEFORE anything is written, reset.py is the
# recovery, and this script never touches 0x0-0x158.
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}; CMD=${2:?board command}
OUT=artifacts/perf-plan/cachecnt-$L-$(date +%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
cat > "$S" <<EOF
B=0x2C000000
echo "STATE ctrl=\$(devmem 0x2C000180 32) int_ena=\$(devmem 0x2C00015c 32) icache0_prelock=\$(devmem 0x2C00003c 32) dcache_prelock=\$(devmem 0x2C00007c 32)"
dump() { for off in 0x184 0x188 0x18c 0x190 0x194 0x198 0x19c 0x1a0 0x1c4 0x1c8 0x1cc 0x1d0 0x1d4 0x1d8 0x1dc 0x1e0 0x1e4 0x1e8; do printf "%s " \$(devmem \$((B+off)) 32); done; echo; }
devmem 0x2C000180 32 0x00330033
devmem 0x2C000180 32 0x00000033
echo "CNT_BEFORE \$(dump)"
T0=\$(cut -d' ' -f1 /proc/uptime)
$CMD
T1=\$(cut -d' ' -f1 /proc/uptime)
echo "CNT_AFTER \$(dump)"
echo "ELAPSED \$T0 \$T1"
devmem 0x2C000180 32 0x00000000
echo CC_DONE
EOF
python3 scripts/board/runsh.py "$S" 90 40 2>&1 | tr -d '\r' > "$OUT/run.log"
grep -aE "^(STATE|ELAPSED|CC_DONE)|NO_SHELL|KILLED" "$OUT/run.log"
python3 - "$OUT/run.log" <<'PY'
import sys, re
t = open(sys.argv[1]).read()
b = re.search(r'^CNT_BEFORE (.*)$', t, re.M); a = re.search(r'^CNT_AFTER (.*)$', t, re.M)
if not (b and a):
    sys.exit("no counter lines - see the log")
bv = [int(x, 16) for x in b.group(1).split()]; av = [int(x, 16) for x in a.group(1).split()]
d = [(y - x) & 0xffffffff for x, y in zip(bv, av)]
names = [("IBUS0 hart0/CPU1", d[0:4]), ("IBUS1 hart1/CPU0", d[4:8]),
         ("DBUS0 hart0/CPU1", d[8:13]), ("DBUS1 hart1/CPU0", d[13:18])]
for n, v in names:
    hit, miss, conf, rd = v[0], v[1], v[2], v[3]
    wr = v[4] if len(v) > 4 else None
    tot = hit + miss
    # "miss" counts STALL EVENTS, not missed lines (esp-idf 16078650, the S31
    # cache counter semantics), so miss/(hit+miss) is a stall share. The line
    # miss ratio is the next-level reads per hit.
    line = "%-18s hit %10d stall %9d (stall%% %.2f) conflict %7d nxtlvl_rd %8d (rd/hit %.2f%%)" % (n, hit, miss, 100.0 * miss / max(1, tot), conf, rd, 100.0 * rd / max(1, hit))
    if wr is not None:
        line += " nxtlvl_wr %8d" % wr
    print(line)
PY
echo "log: $OUT/run.log"
