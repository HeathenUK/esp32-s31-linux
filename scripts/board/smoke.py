#!/usr/bin/env python3
"""Desktop regression suite. ONE round trip, no screenshots, ~15 s.

The previous version took up to half an hour: 25 console round trips and 14
fixed sleeps, plus JPEG transfers to the host for pixel diffs that were flaky
anyway - they could not tell a window that failed to repaint from one that
merely changed stacking order.

Three rules keep it fast and honest:

  * ONE runsh call. The console handshake, not the board, is the expensive
    part. Everything runs on the board and prints one line per check.
  * POLL, never sleep-and-hope. Waiting for a window to appear takes as long
    as it takes, up to a bound; it does not cost a fixed four seconds.
  * The content check never leaves the board. mjpegrec records frames ON
    DAMAGE through the hardware JPEG encoder, so recording across a maximise
    answers both questions at once: frames captured at all means it repainted,
    and the byte count means real content rather than a blank fill. A bare
    desktop is ~20 kB, a maximised xcalc ~60-85 kB, an empty window ~10 kB.

Usage: smoke.py [--reset]
"""
import pathlib, re, subprocess, sys

HERE = pathlib.Path(__file__).resolve().parent

BOARD = r'''
export DISPLAY=:0
CTL=/tmp/lvdesk.ctl
LOG=/var/log/lvdesk.log
ok() { echo "CHK|$1|PASS|$2"; }
no() { echo "CHK|$1|FAIL|$2"; }

LV=$(ps | awk '/lvdesk/ && !/awk/ {print $1}' | head -1)
[ -n "$LV" ] || { no "lvdesk running" "no process"; echo "CHK|END|"; exit 0; }
ok "lvdesk running" "pid $LV"

grep -qi "console keyboard off" $LOG && ok "VT captured" "" \
                                     || no "VT captured" "vt_takeover missing"

# A write to the control FIFO BLOCKS FOREVER if lvdesk is not reading it -
# a wedged desktop then hangs the suite until the console timeout, which is
# what made this take two minutes to report nothing. Bound every write.
ctl() {
	( echo "$1" > $CTL ) & w=$!
	n=0
	while kill -0 $w 2>/dev/null && [ $n -lt 20 ]; do usleep 100000; n=$((n+1)); done
	if kill -0 $w 2>/dev/null; then kill $w 2>/dev/null; return 1; fi
	return 0
}
# Read the log by OFFSET, never by truncating it: the VT line is written once
# at startup, and a harness that empties the log to read its own output
# destroys the evidence for every other check. This failed "VT captured" on a
# perfectly healthy desktop.
logmark() { wc -c < $LOG; }
logsince() { tail -c +$(( $1 + 1 )) $LOG 2>/dev/null; }
wins() {
	m=$(logmark)
	ctl list || { echo 0; return; }
	sleep 1
	logsince "$m" | grep -c '^lvdesk: win '
}
ticks() { read -r a b c d e f g h i j k l m ut st r < /proc/$LV/stat; echo $((ut+st)); }

for p in $(ps | awk '/xclock|xcalc|xfiles/ && !/awk/ {print $1}'); do kill $p 2>/dev/null; done
BASE=$(wins)

# --- clients start and map, polled ---------------------------------------
WANT=$BASE
for c in xclock xcalc xfiles; do
	$c >/dev/null 2>&1 &
	WANT=$((WANT + 1))
	n=0
	while [ $n -lt 20 ]; do
		[ "$(wins)" -ge "$WANT" ] && break
		n=$((n + 1))
	done
	if [ "$(wins)" -ge "$WANT" ]; then
		ok "$c maps a window" "${n}s"
	else
		no "$c maps a window" "no window after ${n}s"
	fi
done

# --- repaint + content, recorded on the board ----------------------------
# mjpegrec captures on damage, so this covers the maximise that follows.
TOP=$(( $(wins) - 1 ))
rm -f /tmp/sm.mjpeg
if [ -x /root/mjpegrec ]; then
	A=$(ticks)
	ctl "max $TOP"
	sleep 3
	B=$(ticks)
	# RECORD THE RESTORE, NOT THE MAXIMISE. During a maximise the client is
	# busy redrawing, so anything that depends on client traffic still works
	# and a broken build passes - measured, with the Expose-flush bug
	# reintroduced on purpose. The failure needs a RESIZE (the client must
	# redraw at the new size) followed by a RAISE while the client is idle:
	# the Expose is queued with no traffic behind it to flush it, and the
	# window never comes back. That is the sequence a human hit.
	# Two attempts, not one. On a COLD boot xfiles pages its text in from
	# the card on first use and the restore repaint can land after a 3 s
	# recording window has closed: 0 frames, which reads exactly like "the
	# desktop never repainted" and failed the gate twice on 2026-09-19 while
	# the same board passed 12/12 a minute later. A second attempt after the
	# client has had its first repaint separates "slow once" (reported as
	# INFO) from "never" (still a FAIL).
	SZ=0; ATT=0
	while [ "$SZ" -le 0 ] && [ $ATT -lt 2 ]; do
		ATT=$((ATT + 1))
		rm -f /tmp/sm.mjpeg /tmp/sm.mjpeg.txt
		/root/mjpegrec /tmp/sm.mjpeg 6 8 90 256 >/dev/null 2>&1 &
		REC=$!
		usleep 300000
		ctl "max $TOP"                  # restore - a resize
		sleep 1
		ctl "raise $TOP"                # ...then a raise, client idle
		sleep 1
		wait $REC 2>/dev/null
		SZ=$(wc -c < /tmp/sm.mjpeg 2>/dev/null || echo 0)
		if [ "$SZ" -le 0 ] && [ $ATT -lt 2 ]; then
			echo "CHK|repaint attempt 1 empty|INFO|retrying once (cold start?)"
			ctl "max $TOP"; sleep 3     # back to maximised for the retry
		fi
	done
	# Judge CONTENT on the largest single frame, not the total: the total
	# is frames x size and a cold client that repaints once fails it while
	# painting perfectly. Calibrated 2026-09-19 on the board (q90, 800x480):
	#   bare desktop            5-10 kB per frame
	#   restored window, BLANK  19-23 kB
	#   restored xfiles PAINTED 54-65 kB
	# 40000 splits blank from painted with margin on both sides.
	SIZES=$(awk 'NF==3{print $2}' /tmp/sm.mjpeg.txt 2>/dev/null | tr '\n' ' ')
	MAXF=$(printf '%s\n' $SIZES | sort -n | tail -1); MAXF=${MAXF:-0}
	[ "$SZ" -gt 0 ] && ok "restore+raise repaints" "frames: ${SIZES}bytes" \
	                || no "restore+raise repaints" "no frames in 6 s - nothing was drawn"
	[ "$MAXF" -gt 40000 ] && ok "window has content" "largest frame ${MAXF} B" \
	                      || no "window has content" "largest frame ${MAXF} B - a painted xfiles is 54-65 kB, a blank one 19-23"
	echo "CHK|maximise CPU|INFO|$(( (B - A) * 10 )) ms"
	# The recording ring and its output are the harness's own
	# footprint - 640 kB of ring plus the file on tmpfs read as a
	# memory regression and failed this on a healthy board.
	rm -f /tmp/sm.mjpeg /tmp/sm.mjpeg.txt
else
	no "restore+raise repaints" "/root/mjpegrec missing"
fi

# --- the shim answered everything ----------------------------------------
[ "$(grep -c UNIMPLEMENTED $LOG 2>/dev/null)" = 0 ] \
	&& ok "no unimplemented requests" "" \
	|| no "no unimplemented requests" "a client is BLOCKED on a reply"
[ "$(grep -c 'out of atoms' $LOG 2>/dev/null)" = 0 ] \
	&& ok "atom table sufficient" "" || no "atom table sufficient" "overflow"

# --- the system survived --------------------------------------------------
dmesg | grep -qiE "Out of memory|oom-kill" && no "no OOM kills" "OOM in dmesg" \
                                           || ok "no OOM kills" ""
dmesg | grep -qiE "segfault|Oops|kernel panic" && no "no crashes" "see dmesg" \
                                               || ok "no crashes" ""
for p in $(ps | awk '/xclock|xcalc|xfiles/ && !/awk/ {print $1}'); do kill $p 2>/dev/null; done
sleep 2
M=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
[ "$M" -ge 2400 ] && ok "MemAvailable >= 2400 kB (idle)" "$M kB" \
                  || no "MemAvailable >= 2400 kB (idle)" "$M kB"
echo "CHK|END|"
'''


def main() -> int:
    if "--reset" in sys.argv:
        subprocess.run([sys.executable, str(HERE / "alive.py"), "--reset"],
                       capture_output=True, text=True)
    tmp = pathlib.Path("/tmp/_smoke.sh")
    tmp.write_text(BOARD)
    r = subprocess.run([sys.executable, str(HERE / "runsh.py"), str(tmp), "120"],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    rows = re.findall(r"^CHK\|([^|]*)\|([^|]*)\|?(.*)$", out, re.M)
    if not any(n == "END" for n, _s, _d in rows):
        print("smoke: the board did not finish - console output follows")
        print(out[-600:])
        return 2
    bad = 0
    for name, status, detail in rows:
        if name == "END":
            continue
        if status == "INFO":
            print(f"  ....  {name}: {detail}")
            continue
        bad += status != "PASS"
        print(f"  {status}  {name}" + (f"   {detail}" if detail else ""))
    total = sum(1 for n, s, _ in rows if n != "END" and s != "INFO")
    print(f"{total - bad}/{total} passed" + ("" if bad else "  - all green"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
