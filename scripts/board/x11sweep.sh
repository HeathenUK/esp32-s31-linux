#!/bin/sh
# The X11 regression sweep. Run ON THE BOARD (via runsh.py) after ANY change
# to lvdesk, xshim, or the xlite family, BEFORE flashing it into XIP.
#
# This exists because a change that made xfiles fast shipped with a blank
# xclock: the check that ran was "did four windows appear", and a window
# existing says nothing about its content. A blank window, a missing border
# and a wrong clock face all pass that check. So this script launches every
# supported app, INTERACTS where interaction is the point (xcalc must
# actually compute), and leaves the screen in a state where one hardware
# screenshot shows all four - and the operator of the host-side harness MUST
# LOOK AT that screenshot. The sweep is the easy half; the looking is the
# check.
#
#   host$ python3 scripts/board/runsh.py scripts/board/x11sweep.sh 300
#   host$ python3 scripts/board/screenshot-hw.py sweep.jpg   # then EXAMINE it
#
# Expected in the screenshot, per app:
#   xclock  face: tick ring, hour + minute hands (time may lag NTP at boot)
#   xcalc   black display bevel, "7" in the display (this script presses 7),
#           1 px border around EVERY button
#   xfiles  icon grid with folder/file icons and filename labels
#   oclock  jewel dot at 12, hour + minute hands (square window: SHAPE stubbed)
set -e
X11RUN=/root/x11run
UI=/root/uinject

fail() { echo "SWEEP FAIL: $1"; exit 1; }

[ -x "$X11RUN" ] || fail "x11run missing"
[ -x "$UI" ] || fail "uinject missing"
pidof lvdesk >/dev/null || fail "lvdesk not running"

killall xclock xcalc xfiles oclock 2>/dev/null || true
sleep 2

$X11RUN xclock  >/tmp/sweep-xclock.log 2>&1 & sleep 5
$X11RUN xcalc   >/tmp/sweep-xcalc.log  2>&1 & sleep 6
$X11RUN xfiles  >/tmp/sweep-xfiles.log 2>&1 & sleep 9
$X11RUN oclock  >/tmp/sweep-oclock.log 2>&1 & sleep 5

for a in xclock xcalc xfiles oclock; do
	pidof "$a" >/dev/null || fail "$a died: $(tail -2 /tmp/sweep-$a.log)"
done

# Server-side gaps are regressions too.
# grep -c prints the count AND exits 1 on zero matches, so `|| echo 0`
# yields "0\n0" and a bad-number error. || true keeps the printed count.
N=$(grep -c UNIMPLEMENTED /var/log/lvdesk.log 2>/dev/null || true)
N=${N:-0}
[ "$N" -eq 0 ] || { grep UNIMPLEMENTED /var/log/lvdesk.log | tail -4;
		    fail "$N UNIMPLEMENTED requests"; }

# Interact: raise xcalc via its taskbar button and press 7. The display
# showing "7" in the screenshot is the proof the whole input->layout->draw
# path works; a dead click here has caught real bugs before.
UINJECT_SETTLE=2600 $UI click 242 469
sleep 2
UINJECT_SETTLE=2600 $UI click 248 398
sleep 2
# Raise xclock last so its face is visible in the screenshot.
UINJECT_SETTLE=2600 $UI click 147 469
sleep 2

echo "SWEEP OK: 4 apps up, 0 unimplemented - now take the screenshot AND LOOK AT IT"
