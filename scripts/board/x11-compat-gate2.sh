#!/bin/bash
# x11-compat-gate2.sh [libdir] - the FRESH-BOOT fullscreen half of the stock
# client compatibility pass: Chocolate Doom fullscreen (SDL2, 320x240 via
# RANDR + EWMH) and OpenTyrian (SDL2, fullscreen at panel size). ~5 minutes.
#
#   scripts/board/x11-compat-gate2.sh              # the shipped /usr/lib libs
#   scripts/board/x11-compat-gate2.sh /root/xtest  # test copies (LD_LIBRARY_PATH)
#
# Why a second script: x11-compat-gate.sh already spends ~7 min on one boot
# (five runsh steps, each with a 30 s pre-sleep and a 40-110 s budget, plus
# five screenshots). Two fullscreen steps of ~40 s + ~12 s plus screenshots
# would push it past 9 min, and these two are wanted on a FRESH boot (~85 s
# to a prompt), so they live here and each script stays under the 10-minute
# rule (memory: board-test-10-minute-rule).
#
# Pass rules (each printed as PASS/FAIL, exit status is the AND):
#   boot       after scripts/board/reset.py: lvdesk is up and its control
#              fifo exists (LVDESK_CTL=1 in the card's S40lvdesk). Records
#              the kernel build (#N) the pass ran on.
#   cdoomfs-a  ./chocolate-doom -fullscreen -extraconfig chocolate-fullscreen.cfg
#              (menu.conf:15, sound on, DAC floored): alive at 28 s, lvdesk
#              entered the 320x240 mode (`kms: fullscreen 320x240`), >= 100
#              puts in 28 s, >= 2 KeyPress after 4 taps of Escape.
#   cdoomfs-b  `close N` on the ctl fifo (= the title-bar X button's code
#              path): WM_DELETE_WINDOW sent, Chocolate Doom shows its quit
#              prompt and ignores it, DROPPED at 3 s, gone by 8 s, and the
#              video mode is back (`lvdesk: fullscreen off`).
#   tyrian-a   opentyrian --no-joystick from its data dir, HOME=/root
#              (menu.conf:17): alive at 25 s, fullscreen entered (any
#              `kms: fullscreen WxH` - the mode is recorded, not asserted),
#              >= 50 puts, >= 5 MotionNotify after `uinject park`, >= 2
#              KeyPress after 4 taps of Down.
#   tyrian-b   `close N`: asked, gone by 8 s, mode back. `dropped` is
#              recorded, not asserted: SDL2 turns the ask into SDL_QUIT and
#              OpenTyrian is expected to quit itself (dropped=0 is the good
#              case; dropped=1 still passes the gone-by-8-s rule).
#
# The close goes through the ctl fifo because a fullscreen window has no
# title bar to click: `echo "close N" > /tmp/lvdesk.ctl` runs exactly
# win_close() (lvdesk.c `close ` handler -> xshim_window_request_close, the
# 3 s drop in xshim_close_tick). N comes from `echo list > /tmp/lvdesk.ctl`,
# which prints `lvdesk: win N WxH+X+Y name` into /var/log/lvdesk.log; the
# lines are read back from a line-count offset taken before the command.
#
# Traps this encodes (all recorded in memory/docs): uinject costs ~2.6 s per
# invocation (device settle), so taps go in the background and one run
# carries all four codes (90 ms apart); `xlite: queue event type` never
# lists MotionNotify - count `xlite: motion` lines; KeyPress is counted
# with `queue event type 2$` ANCHORED: unanchored it also counts type 22
# (ConfigureNotify, 3 per Chocolate Doom launch and 5 per OpenTyrian on
# #377), so keys>=2 passed with no key delivered (the first shipped-image
# run, 2026-09-24, read keys=7 and 9 for 4 real presses each); XLITE_TRACE_INPUT must
# be in the app's OWN environment (xlite reads getenv once per process), so
# it is set inside the setsid sh -c string; fullscreen entry is proven by
# the log, not the panel; the screenshot after cdoomfs-a is 320x240 (the
# scanout geometry follows the mode); runsh's board-side watchdog kills a
# step at timeout-3 s and prints RS_TIMEKILL (board fine, effects
# half-applied); nothing here is run while a human is using the board
# (NEVER inject into a board in use, and never run this while another tool
# holds the console).
set -u
cd "$(dirname "$0")/../.."
LIBDIR=${1:-}
OUT=artifacts/x11-gate2-$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
S=$OUT/step.sh
fail=0
ENV="DISPLAY=:0"; [ -n "$LIBDIR" ] && ENV="LD_LIBRARY_PATH=$LIBDIR DISPLAY=:0"
T0=$SECONDS

step() { # name timeout wait
	python3 scripts/board/runsh.py "$S" "$2" "$3" 2>&1 | grep -av "^\s*$\|__rs_\|__rp\|RS_DONE\|Done(" | tee "$OUT/$1.txt"
	python3 scripts/board/screenshot.py "$OUT/$1.png" >/dev/null 2>&1 || true
}
verdict() { if grep -aq "$2" "$OUT/$1.txt"; then echo "PASS $1"; else echo "FAIL $1 (wanted: $2)"; fail=1; fi; }

# --- boot: hard reset, then wait for lvdesk and its control fifo -----------
# runsh's third argument is a fixed pre-sleep before the port is opened;
# console.wait_for_shell then tracks the rest of the boot (~85 s to a
# prompt from reset, HARD_CAP 180 s). The 150 s budget covers the 120 s
# board-side poll (watchdog fires at 147 s).
python3 scripts/board/reset.py >/dev/null 2>&1
cat > "$S" <<'EOF'
i=0; while [ $i -lt 60 ]; do [ -n "$(pidof lvdesk)" ] && [ -p /tmp/lvdesk.ctl ] && grep -aq 'lvdesk: control fifo' /var/log/lvdesk.log && break; sleep 2; i=$((i+1)); done
sleep 5
L=$(pidof lvdesk | awk '{print $1}')
nofs=0; [ -n "$L" ] && nofs=$(tr '\0' '\n' < /proc/$L/environ 2>/dev/null | grep -ac '^LVDESK_NOFULLSCREEN=')
echo "boot lvdesk=$(pidof lvdesk | wc -w) ctl=$([ -p /tmp/lvdesk.ctl ] && echo 1 || echo 0) nofs=$nofs uptime=$(cut -d' ' -f1 /proc/uptime) kernel=$(uname -r) build=$(uname -v | awk '{print $1}') mem=$(grep -a MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2)"
EOF
step boot 150 60; verdict boot "lvdesk=1 ctl=1"

# --- cdoomfs-a: Chocolate Doom fullscreen 320x240, sound on ---------------
# KEY_ESC=1: during the attract demo any key opens the Doom menu and Escape
# closes it, so four taps leave it closed for the WM_DELETE ask in the next
# step (Enter would start a game and a level load). 28 s is the windowed
# cdoom step's proven map-to-demo window (922 puts on 2026-09-24).
echo "export $ENV" > "$S"; cat >> "$S" <<'EOF'
L=$(pidof lvdesk); [ -z "$L" ] && L=$(pidof lvdesk.new)
amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null
f0=$(grep -ac 'kms: fullscreen [0-9]' /var/log/lvdesk.log)
kill -USR1 $L; sleep 1; n0=$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print $2}')
cd /root/doom && setsid sh -c 'XLITE_TRACE_INPUT=1 exec ./chocolate-doom -fullscreen -extraconfig /etc/lvdesk/chocolate-fullscreen.cfg >/tmp/cdoomfs.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 28; a=$(pidof chocolate-doom | wc -w)
f1=$(grep -ac 'kms: fullscreen [0-9]' /var/log/lvdesk.log); mode=$(grep -a 'lvdesk: fullscreen [0-9]' /var/log/lvdesk.log | tail -n 1 | awk '{print $3}')
kill -USR1 $L; sleep 1; n1=$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print $2}')
setsid sh -c '/root/uinject key 1 1 1 1' </dev/null >/dev/null 2>&1 & sleep 6; k=$(grep -ac 'queue event type 2$' /tmp/cdoomfs.log)
echo "cdoomfs alive=$a fs=$((f1-f0)) mode=${mode:-NONE} puts=$((n1-n0)) keys=$k"
EOF
step cdoomfs-a 70 2; verdict cdoomfs-a "alive=1 fs=[1-9] mode=320x240 puts=[1-9][0-9][0-9][0-9]* keys=\([2-9]\|[1-9][0-9]\)"

# --- cdoomfs-b: the polite close through the ctl fifo, then the 3 s drop --
echo "export $ENV" > "$S"; cat >> "$S" <<'EOF'
n=$(wc -l < /var/log/lvdesk.log); echo list > /tmp/lvdesk.ctl; sleep 1
idx=$(tail -n +$((n+1)) /var/log/lvdesk.log | grep -a 'lvdesk: win [0-9]' | grep -ai 'doom' | head -n 1 | awk '{print $3}')
[ -z "$idx" ] && idx=$(tail -n +$((n+1)) /var/log/lvdesk.log | grep -a 'lvdesk: win [0-9]' | tail -n 1 | awk '{print $3}')
c0=$(grep -ac 'lvdesk: ctl close' /var/log/lvdesk.log); w0=$(grep -ac 'sent WM_DELETE_WINDOW' /var/log/lvdesk.log); d0=$(grep -ac 'ignored WM_DELETE_WINDOW' /var/log/lvdesk.log); o0=$(grep -ac 'lvdesk: fullscreen off' /var/log/lvdesk.log)
[ -n "$idx" ] && echo "close $idx" > /tmp/lvdesk.ctl
sleep 8; c=$(pidof chocolate-doom | wc -w)
c1=$(grep -ac 'lvdesk: ctl close' /var/log/lvdesk.log); w1=$(grep -ac 'sent WM_DELETE_WINDOW' /var/log/lvdesk.log); d1=$(grep -ac 'ignored WM_DELETE_WINDOW' /var/log/lvdesk.log); o1=$(grep -ac 'lvdesk: fullscreen off' /var/log/lvdesk.log)
echo "cdoomfs-close idx=${idx:-NONE} ctl=$((c1-c0)) asked=$((w1-w0)) dropped=$((d1-d0)) fsoff=$((o1-o0)) after8s=$c"
for p in $(pidof chocolate-doom) $(pidof uinject); do kill -9 $p; done
EOF
step cdoomfs-b 30 2; verdict cdoomfs-b "asked=1 dropped=1 fsoff=[1-9] after8s=0"

# --- tyrian-a: OpenTyrian fullscreen at panel size, motion and keys -------
# 25 s is apps-smoke's 22 s title window plus slack for its three >= 400 ms
# loading pauses (162 lvdesk frames in 22 s on #363; puts >= 50 is a floor
# with a 3x margin). KEY_DOWN=108 moves the menu highlight (Escape/Enter
# at the OpenTyrian menu can start or quit things). park sits at 400,200,
# above the menu rows (y >= ~262), so the hover does not re-steer it.
echo "export $ENV" > "$S"; cat >> "$S" <<'EOF'
L=$(pidof lvdesk); [ -z "$L" ] && L=$(pidof lvdesk.new); rm -f /root/oty.log
amixer -q sset 'DACL' 110 2>/dev/null; amixer -q sset 'DACR' 110 2>/dev/null
f0=$(grep -ac 'kms: fullscreen [0-9]' /var/log/lvdesk.log)
kill -USR1 $L; sleep 1; n0=$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print $2}')
cd /root/oty/usr/share/opentyrian/data && setsid sh -c 'HOME=/root XLITE_TRACE_INPUT=1 exec /root/oty/usr/bin/opentyrian --no-joystick >/root/oty.log 2>&1' </dev/null >/dev/null 2>&1 &
sleep 25; a=$(pidof opentyrian | wc -w)
f1=$(grep -ac 'kms: fullscreen [0-9]' /var/log/lvdesk.log); mode=$(grep -a 'kms: fullscreen [0-9]' /var/log/lvdesk.log | tail -n 1 | awk '{print $3}' | tr -d ,)
setsid sh -c '/root/uinject park' </dev/null >/dev/null 2>&1 & sleep 5; m=$(grep -ac 'xlite: motion' /root/oty.log); for p in $(pidof uinject); do kill $p; done
setsid sh -c '/root/uinject key 108 108 108 108' </dev/null >/dev/null 2>&1 & sleep 6; k=$(grep -ac 'queue event type 2$' /root/oty.log)
kill -USR1 $L; sleep 1; n1=$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print $2}')
echo "tyrian alive=$a fs=$((f1-f0)) mode=${mode:-NONE} puts=$((n1-n0)) motion=$m keys=$k"
EOF
step tyrian-a 70 2; verdict tyrian-a "alive=1 fs=[1-9] mode=[0-9][0-9]*x[0-9][0-9]* puts=\([5-9][0-9]\|[1-9][0-9][0-9][0-9]*\) motion=\([5-9]\|[1-9][0-9][0-9]*\) keys=\([2-9]\|[1-9][0-9]\)"

# --- tyrian-b: the polite close; OpenTyrian is expected to quit itself ----
echo "export $ENV" > "$S"; cat >> "$S" <<'EOF'
n=$(wc -l < /var/log/lvdesk.log); echo list > /tmp/lvdesk.ctl; sleep 1
idx=$(tail -n +$((n+1)) /var/log/lvdesk.log | grep -a 'lvdesk: win [0-9]' | grep -ai 'tyrian' | head -n 1 | awk '{print $3}')
[ -z "$idx" ] && idx=$(tail -n +$((n+1)) /var/log/lvdesk.log | grep -a 'lvdesk: win [0-9]' | tail -n 1 | awk '{print $3}')
c0=$(grep -ac 'lvdesk: ctl close' /var/log/lvdesk.log); w0=$(grep -ac 'sent WM_DELETE_WINDOW' /var/log/lvdesk.log); d0=$(grep -ac 'ignored WM_DELETE_WINDOW' /var/log/lvdesk.log); o0=$(grep -ac 'lvdesk: fullscreen off' /var/log/lvdesk.log)
[ -n "$idx" ] && echo "close $idx" > /tmp/lvdesk.ctl
sleep 8; c=$(pidof opentyrian | wc -w)
c1=$(grep -ac 'lvdesk: ctl close' /var/log/lvdesk.log); w1=$(grep -ac 'sent WM_DELETE_WINDOW' /var/log/lvdesk.log); d1=$(grep -ac 'ignored WM_DELETE_WINDOW' /var/log/lvdesk.log); o1=$(grep -ac 'lvdesk: fullscreen off' /var/log/lvdesk.log)
echo "tyrian-close idx=${idx:-NONE} ctl=$((c1-c0)) asked=$((w1-w0)) dropped=$((d1-d0)) fsoff=$((o1-o0)) after8s=$c"
EOF
step tyrian-b 30 2; verdict tyrian-b "asked=1 .*fsoff=[1-9] after8s=0"

# --- clean: nothing left running, DAC back to 143 (-24 dB), where apps-smoke leaves it -----
cat > "$S" <<'EOF'
for p in $(pidof opentyrian) $(pidof chocolate-doom) $(pidof uinject); do kill -9 $p; done
amixer -q sset 'DACL' 143 2>/dev/null; amixer -q sset 'DACR' 143 2>/dev/null
echo CLEAN
EOF
python3 scripts/board/runsh.py "$S" 20 2 >/dev/null 2>&1

echo "gate2 wall=$((SECONDS - T0))s artifacts in $OUT"; exit $fail
