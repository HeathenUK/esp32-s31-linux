#!/bin/bash
# x11-compat-gate.sh [libdir] - the stock-client compatibility pass, on ONE boot.
#
# Launches each X11 client the card carries, checks that it maps and keeps
# presenting (lvdesk's SIGUSR1 ShmPutImage counter), pushes one pointer
# action and one key through uinject, exercises the title-bar close (the
# WM_DELETE_WINDOW ask, then the 3 s drop), and captures a panel screenshot
# per client into artifacts/x11-gate-<stamp>/. ~7 minutes.
#
#   scripts/board/x11-compat-gate.sh              # the shipped /usr/lib libs
#   scripts/board/x11-compat-gate.sh /root/xtest  # test copies (LD_LIBRARY_PATH)
#
# Pass rules (each printed as PASS/FAIL, exit status is the AND):
#   xcalc      maps; alive after a button click; exits within 4 s of the
#              close button (it registers WM_DELETE_WINDOW).
#   st         maps; exits within 4 s of the close button.
#   prboom     -window: >= 100 puts in 22 s; after the close button it shows
#              its quit prompt, ignores the ask, is DROPPED at 3 s and must be
#              gone by 8 s (SDL 1.2 keeps two connections - the dropped one
#              must take the process down: xring.h `closed`).
#   cdoom      Chocolate Doom -window: >= 100 puts in 28 s; same drop rule.
#   quake      tyr-quake-x11 -fullscreen +map e1m1 -mem 20: alive at 48 s,
#              puts advancing, >= 5 MotionNotify after `uinject park`,
#              >= 2 KeyPress after 4 taps of W.
#   Fullscreen Chocolate Doom and OpenTyrian are in x11-compat-gate2.sh
#   (fresh boot, ~5 min); this script stays under the 10-minute rule on
#   one boot.
#
# Traps this encodes (all recorded in memory/docs): uinject costs ~2 s per
# invocation (device settle), so taps go in the background; `xlite: queue
# event type` never lists MotionNotify - count `xlite: motion` lines; the
# close button of a 320x240 window at the default place is at 462,70 and
# xcalc's at 368,70, st's at 626,70 - the desktop places the first window at
# 150,60; runsh windows are sized to each step; nothing here is run while a
# human is using the board (NEVER inject into a board in use).
set -u
cd "$(dirname "$0")/../.."
LIBDIR=${1:-}
OUT=artifacts/x11-gate-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/step.sh
fail=0
ENV="DISPLAY=:0"; [ -n "$LIBDIR" ] && ENV="LD_LIBRARY_PATH=$LIBDIR DISPLAY=:0"

step() { # name timeout wait
	python3 scripts/board/runsh.py "$S" "$2" "$3" 2>&1 | grep -av "^\s*$\|__rs_\|__rp\|RS_DONE\|Done(" | tee "$OUT/$1.txt"
	python3 scripts/board/screenshot.py "$OUT/$1.png" >/dev/null 2>&1 || true
}
verdict() { if grep -aq "$2" "$OUT/$1.txt"; then echo "PASS $1"; else echo "FAIL $1 (wanted: $2)"; fail=1; fi; }

cat > "$S" <<EOF
for p in \$(pidof xcalc) \$(pidof st) \$(pidof prboom) \$(pidof chocolate-doom) \$(pidof tyr-quake-x11) \$(pidof uinject); do kill -9 \$p; done
export $ENV; setsid sh -c 'exec xcalc >/tmp/xcalc.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 6; a=\$(pidof xcalc | wc -w); /root/uinject dragto 220 367 220 367 >/dev/null 2>&1; sleep 1; b=\$(pidof xcalc | wc -w)
/root/uinject dragto 368 70 368 70 >/dev/null 2>&1; sleep 4; c=\$(pidof xcalc | wc -w); echo "xcalc mapped=\$a afterclick=\$b afterclose=\$c"
EOF
step xcalc 60 30; verdict xcalc "mapped=1 afterclick=1 afterclose=0"

cat > "$S" <<EOF
export $ENV; setsid sh -c 'exec st >/tmp/st.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 8; a=\$(pidof st | wc -w); /root/uinject dragto 626 70 626 70 >/dev/null 2>&1; sleep 4; c=\$(pidof st | wc -w); echo "st mapped=\$a afterclose=\$c"; for p in \$(pidof st); do kill -9 \$p; done
EOF
step st 40 30; verdict st "mapped=1 afterclose=0"

cat > "$S" <<EOF
export $ENV; L=\$(pidof lvdesk); [ -z "\$L" ] && L=\$(pidof lvdesk.new); kill -USR1 \$L; sleep 1; n0=\$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print \$2}')
cd /root/doom/wads && setsid sh -c 'exec /root/doom/prboom -width 320 -height 240 -window >/tmp/prboom.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 22; kill -USR1 \$L; sleep 1; n1=\$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print \$2}'); a=\$(pidof prboom | wc -w)
/root/uinject dragto 462 70 462 70 >/dev/null 2>&1; sleep 8; c=\$(pidof prboom | wc -w); echo "prboom mapped=\$a puts=\$((n1-n0)) after8s=\$c"; for p in \$(pidof prboom); do kill -9 \$p; done
EOF
step prboom 70 30; verdict prboom "mapped=1 puts=[1-9][0-9][0-9]* after8s=0"

cat > "$S" <<EOF
export $ENV; L=\$(pidof lvdesk); [ -z "\$L" ] && L=\$(pidof lvdesk.new); kill -USR1 \$L; sleep 1; n0=\$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print \$2}')
cd /root/doom && setsid sh -c 'exec ./chocolate-doom -window -width 320 -height 200 -1 -nosound -nomusic >/tmp/cdoom.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 28; kill -USR1 \$L; sleep 1; n1=\$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print \$2}'); a=\$(pidof chocolate-doom | wc -w)
/root/uinject dragto 462 70 462 70 >/dev/null 2>&1; sleep 8; c=\$(pidof chocolate-doom | wc -w); echo "cdoom mapped=\$a puts=\$((n1-n0)) after8s=\$c"; for p in \$(pidof chocolate-doom); do kill -9 \$p; done
EOF
step cdoom 80 30; verdict cdoom "mapped=1 puts=[1-9][0-9][0-9]* after8s=0"

cat > "$S" <<EOF
export $ENV HOME=/root/quake; cd /root/quake; rm -f gate.log
setsid sh -c 'XLITE_TRACE_INPUT=1 exec ./tyr-quake-x11 -basedir /root/quake -mem 20 -sndspeed 11025 -width 320 -height 240 -fullscreen +map e1m1 >/root/quake/gate.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 48; a=\$(pidof tyr-quake-x11 | wc -w); L=\$(pidof lvdesk); [ -z "\$L" ] && L=\$(pidof lvdesk.new); kill -USR1 \$L; sleep 1; n0=\$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print \$2}')
setsid sh -c '/root/uinject park' </dev/null >/dev/null 2>&1 & sleep 5; m=\$(grep -ac 'xlite: motion' /root/quake/gate.log); for p in \$(pidof uinject); do kill \$p; done
setsid sh -c 'i=0; while [ \$i -lt 4 ]; do /root/uinject key 17; i=\$((i+1)); done' </dev/null >/dev/null 2>&1 & sleep 14; k=\$(grep -ac 'queue event type 2' /root/quake/gate.log)
kill -USR1 \$L; sleep 1; n1=\$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print \$2}'); echo "quake alive=\$a puts=\$((n1-n0)) motion=\$m keys=\$k"; for p in \$(pidof tyr-quake-x11); do kill -9 \$p; done
EOF
step quake 110 30; verdict quake "alive=1 puts=[1-9][0-9]* motion=\([5-9]\|[1-9][0-9]\+\) keys=\([2-9]\|[1-9][0-9]\)"

echo "artifacts in $OUT"; exit $fail
