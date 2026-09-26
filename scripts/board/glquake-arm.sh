#!/bin/bash
# glquake-arm.sh <label> <runs> [env=VAL ...] -- <quakespasm args> : one
# measurement arm of GLQuake (stock QuakeSpasm 0.96.3) on a FRESH BOOT.
#
#   scripts/board/glquake-arm.sh win12 2 GQ_BASE=/root/quake/td -- \
#       -mixspeed 11025 -heapsize 12288 -zone 384 -width 320 -height 240 -window
#
# reset.py, wait for 60 s of board uptime, then ONE runsh fires a board-side
# loop (setsid) that runs glquake-run.sh <runs> times back to back (a
# timedemo each, each a new process; run 1 is the warm-up to discard), then
# bounded runsh polls until it is done, then the results are cat'ed back
# into artifacts/gl/glquake/arms/<label>-<stamp>/. Budget: ~85 s boot +
# ~4.5 min per run at 4 fps - keep <runs> at 2 (the 10-minute rule).
# Timedemo basedir: /root/quake/td (see glquake-run.sh for why).
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}; N=${2:?runs}; shift 2
ENV=""; while [ $# -gt 0 ] && [ "$1" != "--" ]; do ENV="$ENV export $1;"; shift; done
[ "${1:-}" = "--" ] && shift
ARGS="$*"
OUT=artifacts/gl/glquake/arms/$L-$(date +%m%d-%H%M%S); mkdir -p "$OUT"
T=$(mktemp -d)
python3 scripts/board/reset.py > "$OUT/reset.log" 2>&1
cat > $T/fire.sh <<X
cat > /root/gq/arm-$L.sh <<'Y'
$ENV
i=1; while [ \$i -le $N ]; do sh /root/glquake-run.sh $L-r\$i ${GQ_MAX:-420} $ARGS; i=\$((i+1)); sleep 5; done
echo ARMDONE > /root/gq/arm-$L.done
Y
mkdir -p /root/gq; rm -f /root/gq/arm-$L.done
setsid sh /root/gq/arm-$L.sh </dev/null >/dev/null 2>&1 &
echo FIRED \$(uname -v) up \$(cut -d' ' -f1 /proc/uptime)
X
python3 scripts/board/runsh.py $T/fire.sh 60 60 | grep -a FIRED | tee "$OUT/fired.txt"
cat > $T/wait.sh <<X
i=0; while [ \$i -lt 38 ]; do [ -f /root/gq/arm-$L.done ] && break; sleep 15; i=\$((i+1)); done
[ -f /root/gq/arm-$L.done ] && echo ARM_FINISHED || echo ARM_RUNNING
X
while :; do
	r=$(python3 scripts/board/runsh.py $T/wait.sh 600 | grep -a "ARM_FINISHED\|ARM_RUNNING" | tail -1 | tr -d "\r")
	echo "poll: $r"; [ "$r" = ARM_FINISHED ] && break
	[ -z "$r" ] && { echo "poll returned nothing - check alive.py"; break; }
done
i=1; while [ $i -le $N ]; do
	printf 'cat /root/gq/%s-r%s.txt\n' "$L" "$i" > $T/cat.sh
	python3 scripts/board/runsh.py $T/cat.sh 60 | tr -d "\r" | sed -n "/^GQ $L-r$i/,/^GQDONE/p" > "$OUT/r$i.txt"
	grep -a "^RESULT" "$OUT/r$i.txt"
	i=$((i+1))
done
echo "$OUT"
