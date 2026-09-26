#!/bin/bash
# gl-arms.sh <outdir> <arm>... - host driver for gl-arm.sh, FRESH BOOT PER ARM.
# An arm is name:render_scale:shmbufs:fs|win[:runs], e.g. g04:1:1:fs:6.
# Per arm: reset, wait for uptime >= 75 s, ship gl-arm.sh, start it setsid
# (output to /root/glarm-<name>.txt on the card), leave, poll for DONE,
# collect. ~3.5 min per arm at 6 runs. GLLIB=/root/g04 selects a test libGL,
# LVBIN a desktop binary (default /root/lvdesk.new), LVENV its extra env,
# GLENV the client's.
set -u
cd "$(dirname "$0")/../.."
OUT=${1:?outdir}; shift; mkdir -p "$OUT"
T=$(mktemp -d)
for arm in "$@"; do
	IFS=: read -r name rs nb mode runs <<< "$arm"; runs=${runs:-6}
	python3 scripts/board/reset.py >/dev/null 2>&1
	{ echo "cat > /root/gl-arm.sh <<'GLARM_EOF'"; cat scripts/board/gl-arm.sh; echo "GLARM_EOF"
	  echo "rm -f /root/glarm-$name.txt; GLLIB=${GLLIB:-} LVENV='${LVENV:-}' GLENV='${GLENV:-}' setsid sh /root/gl-arm.sh $name $rs $nb $mode $runs ${LVBIN:-} </dev/null >/dev/null 2>&1 &"
	  echo "echo LAUNCHED"; } > "$T/l.sh"
	python3 scripts/board/runsh.py "$T/l.sh" 30 75 > "$T/l.log" 2>&1
	grep -q "^LAUNCHED" "$T/l.log" || { echo "arm $name: no launch"; tail -3 "$T/l.log"; continue; }
	echo 'grep -c "^RUN" /root/glarm-'$name'.txt; grep -q "^DONE" /root/glarm-'$name'.txt && echo ARM_DONE' > "$T/p.sh"
	for i in $(seq 1 30); do
		sleep 15
		python3 scripts/board/runsh.py "$T/p.sh" 20 > "$T/p.log" 2>&1
		grep -q "^ARM_DONE" "$T/p.log" && break
	done
	echo "cat /root/glarm-$name.txt" > "$T/c.sh"
	python3 scripts/board/runsh.py "$T/c.sh" 20 2>&1 | python3 -c '
import sys
t=sys.stdin.read(); a=t.find("echo RS_DONE"); b=t.rfind("RS_DONE")
print(t[a+12:b].strip("\r\n") if a>=0 and b>a else t)' | tr -d '\r' > "$OUT/$name.txt"
	echo "== $name"; grep "^ARM\|^RUN\|DONE" "$OUT/$name.txt"
done
rm -rf "$T"
