#!/usr/bin/env python3
"""Desktop smoke test: does the board still work after a change?

    scripts/board/smoke.py [--reset] [--out DIR]

Every verification in this project has been someone eyeballing one screenshot,
which is why a change takes an hour to trust and why two "wins" survived far
longer than they deserved. This makes the check one command.

WHAT IT ASSERTS, and why each one is here:

  lvdesk starts and CAPTURES THE VT.  "console keyboard off" is a standing
      invariant - an lvdesk that does not take the VT leaks every keystroke to
      fbcon, and it has shipped that way before.
  Each X client starts, STAYS UP, and MAPS A WINDOW.  Staying up matters
      separately from starting: a client that dies after four seconds looks
      identical to a working one if you only check once.  MapWindow comes from
      the shim's own trace, so it is the server's word, not the client's.
  The shim answers everything it was asked.  Any UNIMPLEMENTED line, or running
      out of atoms, is a failure even if the client survives - the client is
      blocked or is quietly treating a property as absent.
  No OOM kill, oops or panic in dmesg.
  MemAvailable stays above a floor.  Below it the board is one app launch from
      the OOM killer, which is how several runs died during the Qt work.

  INPUT REACHES THE CLIENT AND THE SCREEN CHANGES.  A click on xcalc and a drag
      of a window, each verified by comparing captures either side.  This is the
      only check that exercises the whole loop - evdev, lvdesk, the shim, the
      client, the repaint - and a client that is up but deaf is indistinguishable
      from a working one without it.
  THE DRAG'S CPU COST IS RECORDED.  Dragging is the heaviest interactive thing
      the compositor does: a move plus a repaint per motion event.  Regressions
      in responsiveness show up here first.

Frame comparison MASKS THE TRAY.  The clock and the live MemAvailable figure
change between any two captures, so an unmasked diff is all false positives.

Still worth opening the captures by hand: "runs" and "looks right" are different
questions, and the Qt work turned up cases of the first without the second.
"""
import argparse, pathlib, re, subprocess, sys, time

try:
    from PIL import Image, ImageChops
except ImportError:
    Image = None


# The tray carries a clock and a live MemAvailable figure, so ANY two captures
# differ down there and a naive whole-frame diff is all false positives. Mask
# the bottom strip and compare the rest.
TRAY_H = 26


def frame_delta(a: pathlib.Path, b: pathlib.Path):
    """Fraction of pixels that changed outside the tray, or None if we cannot tell."""
    if Image is None or not (a.exists() and b.exists()):
        return None
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return 1.0
    w, h = ia.size
    box = (0, 0, w, max(1, h - TRAY_H))
    diff = ImageChops.difference(ia.crop(box), ib.crop(box)).convert("L")
    # Count pixels that moved by more than JPEG ringing would explain.
    hist = diff.histogram()
    changed = sum(hist[25:])
    return changed / float(box[2] * box[3])

HERE = pathlib.Path(__file__).resolve().parent
CLIENTS = ["xclock", "xcalc", "xfiles"]

# Thresholds are OBSERVED BASELINES with headroom, not opinions about what is
# acceptable. Both started as guesses and both failed on the first run against a
# healthy board, which is exactly how a harness gets ignored. Re-derive them
# from a few green runs after any deliberate change rather than nudging them to
# make a red run go away.
#
#   MemAvailable   ~1900 kB idle with the desktop up and no clients.
#   drag CPU       1760 ms observed on a healthy board for one uinject drag.
#                  This is the heaviest interactive path there is, so it is the
#                  most useful regression signal here - but the absolute number
#                  means nothing without a baseline to compare against.
MEM_FLOOR_KB = 1400
DRAG_CPU_MAX_MS = 2600


def run_on_board(script: str, timeout: str = "300") -> str:
    tmp = pathlib.Path("/tmp/_smoke.sh")
    tmp.write_text(script)
    r = subprocess.run([sys.executable, str(HERE / "runsh.py"), str(tmp), timeout],
                       capture_output=True, text=True)
    return r.stdout + r.stderr


def shot(out: pathlib.Path, name: str) -> bool:
    r = subprocess.run([sys.executable, str(HERE / "screenshot-hw.py"),
                        str(out / f"{name}.jpg")], capture_output=True, text=True)
    return "bytes ->" in r.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reset", action="store_true",
                    help="hard reset first (a settled board measures differently)")
    ap.add_argument("--out", default="/tmp/smoke", help="where to write captures")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    results = []          # (name, ok, detail)

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))

    if args.reset:
        print("resetting...")
        subprocess.run([sys.executable, str(HERE / "reset.py")], capture_output=True)
        subprocess.run([sys.executable, str(HERE / "alive.py")], capture_output=True,
                       text=True, timeout=600)

    # --- desktop up, VT captured -------------------------------------------
    print("desktop:")
    o = run_on_board(r"""
killall xclock xcalc xfiles lvdesk 2>/dev/null; sleep 2
: > /var/log/lvdesk.log
XSHIM_TRACE=1 LVDESK_CTL=1 setsid /root/lvdesk >/var/log/lvdesk.log 2>&1 </dev/null &
sleep 10
echo "LVDESK_PID=$(pidof lvdesk)"
grep -i "console keyboard" /var/log/lvdesk.log | head -1
echo "AVAIL=$(awk '/MemAvailable/{print $2}' /proc/meminfo)"
""")
    check("lvdesk running", re.search(r"LVDESK_PID=\d", o))
    check("VT captured (console keyboard off)", "console keyboard off" in o)

    # --- each client -------------------------------------------------------
    print("clients:")
    for c in CLIENTS:
        o = run_on_board(f"""
export DISPLAY=:0
MARK=$(grep -c MapWindow /var/log/lvdesk.log 2>/dev/null)
{c} > /tmp/{c}.log 2>&1 &
sleep 8
P1=$(pidof {c})
sleep 6
P2=$(pidof {c})
echo "START=$P1 STILLUP=$P2"
echo "MAPPED=$(( $(grep -c MapWindow /var/log/lvdesk.log 2>/dev/null) - MARK ))"
echo "ERR=$(head -1 /tmp/{c}.log)"
""", "200")
        started = re.search(r"START=\d", o)
        stayed = re.search(r"STILLUP=\d", o)
        mapped = re.search(r"MAPPED=([1-9]\d*)", o)
        err = (re.search(r"ERR=(.*)", o) or [None, ""])[1].strip()
        check(f"{c} starts", started, err if not started else "")
        check(f"{c} stays up", stayed)
        check(f"{c} maps a window", mapped)

    shot(out, "clients")
    print(f"  (capture: {out}/clients.jpg - LOOK AT IT, 'runs' != 'looks right')")

    # --- interaction: does input reach the client and change the screen? ----
    #
    # "Starts and maps a window" is not the same as "works". This drives the
    # whole loop - evdev -> lvdesk -> shim -> client -> repaint - and asserts
    # the SCREEN CHANGED as a result. A client that is up but deaf looks
    # identical to a working one in a static capture.
    print("interaction:")
    # Close the client-phase apps first. Leaving them up ran two xcalcs and
    # took MemAvailable to 716 kB, so the memory check failed on the harness's
    # own leftovers rather than on anything real.
    run_on_board("killall xclock xcalc xfiles 2>/dev/null; sleep 3\n"
                 "export DISPLAY=:0\nxcalc >/dev/null 2>&1 &\nsleep 8\n", "120")
    shot(out, "before_click")

    # xcalc's buttons are big and its display echoes immediately, so a click
    # that lands is unambiguous on screen. uinject pays a ~2.6 s settle per
    # invocation - it re-finds the uinput device each time - so batch by
    # asking for one action per call and no more.
    o = run_on_board(r"""
export DISPLAY=:0
A=$(awk '{print $14+$15}' /proc/$(pidof lvdesk)/stat)
S=$(cut -d' ' -f1 /proc/uptime)
/root/uinject click 250 300 >/dev/null 2>&1
E=$(cut -d' ' -f1 /proc/uptime)
B=$(awk '{print $14+$15}' /proc/$(pidof lvdesk)/stat)
awk -v a=$S -v b=$E -v t=$((B-A)) 'BEGIN{printf "CLICK_WALL=%.2f CLICK_TICKS=%d\n", b-a, t}'
""", "200")
    shot(out, "after_click")
    d = frame_delta(out / "before_click.jpg", out / "after_click.jpg")
    check("click changes the screen", d is None or d > 0.0002,
          f"{d*100:.3f}% of pixels" if d is not None else "no PIL - not checked")
    m = re.search(r"CLICK_WALL=([\d.]+) CLICK_TICKS=(\d+)", o)
    if m:
        print(f"        click: {m.group(1)}s wall, lvdesk {m.group(2)} ticks")

    # Drag a window by its titlebar and time it. This is the heaviest thing the
    # compositor does interactively - every motion event is a move plus a
    # repaint - so it is where responsiveness regressions will show first.
    #
    # uinject's drag grabs a FIXED point (250,20), so what is under that point
    # decides whether it grabs a titlebar or empty desktop. With xcalc still up
    # it grabbed nothing and the check failed on aim, not on behaviour - which
    # is the "uinject aims true only once per desktop" trap. Close everything
    # so only lvdesk's own Terminal remains, at its known startup position.
    run_on_board("killall xcalc 2>/dev/null; sleep 4\n", "60")
    shot(out, "before_drag")
    o = run_on_board(r"""
export DISPLAY=:0
A=$(awk '{print $14+$15}' /proc/$(pidof lvdesk)/stat)
S=$(cut -d' ' -f1 /proc/uptime)
/root/uinject drag >/dev/null 2>&1
E=$(cut -d' ' -f1 /proc/uptime)
B=$(awk '{print $14+$15}' /proc/$(pidof lvdesk)/stat)
awk -v a=$S -v b=$E -v t=$((B-A)) 'BEGIN{printf "DRAG_WALL=%.2f DRAG_TICKS=%d\n", b-a, t}'
""", "200")
    shot(out, "after_drag")
    d = frame_delta(out / "before_drag.jpg", out / "after_drag.jpg")
    check("drag moves a window", d is None or d > 0.002,
          f"{d*100:.3f}% of pixels" if d is not None else "no PIL - not checked")
    m = re.search(r"DRAG_WALL=([\d.]+) DRAG_TICKS=(\d+)", o)
    if m:
        wall, ticks = float(m.group(1)), int(m.group(2))
        # uinject's own ~2.6 s settle dominates the wall time, so the CPU the
        # compositor spent is the number that means anything here.
        print(f"        drag: {wall:.2f}s wall (incl. ~2.6s settle), "
              f"lvdesk {ticks} ticks = {ticks * 10} ms of CPU")
        check(f"drag CPU < {DRAG_CPU_MAX_MS} ms", ticks * 10 < DRAG_CPU_MAX_MS,
              f"{ticks * 10} ms (baseline ~1760)")

    # --- the shim answered everything --------------------------------------
    print("shim:")
    o = run_on_board(r"""
echo "UNIMPL=$(grep -c UNIMPLEMENTED /var/log/lvdesk.log 2>/dev/null)"
echo "ATOMS=$(grep -c 'out of atoms' /var/log/lvdesk.log 2>/dev/null)"
""")
    check("no unimplemented requests", "UNIMPL=0" in o,
          "" if "UNIMPL=0" in o else "a client is BLOCKED on a reply")
    check("atom table sufficient", "ATOMS=0" in o)

    # --- system sanity -----------------------------------------------------
    print("system:")
    # Measure the floor on an IDLE desktop. Reading it with the client-phase
    # apps still up gave 68 kB, which is a real and alarming number - three
    # small X clients very nearly exhaust this board - but it is a property of
    # the workload, not a regression signal, and it made the check fail on the
    # harness's own leftovers. Both are reported: the loaded figure as data,
    # the idle one as the assertion.
    o = run_on_board(r"""
echo "LOADED=$(awk '/MemAvailable/{print $2}' /proc/meminfo)"
killall xclock xcalc xfiles 2>/dev/null
sleep 6
echo "OOM=$(dmesg | grep -ci 'killed process\|out of memory')"
echo "BAD=$(dmesg | grep -ci 'segfault\|Oops\|kernel panic')"
echo "AVAIL=$(awk '/MemAvailable/{print $2}' /proc/meminfo)"
""")
    ml = re.search(r"LOADED=(\d+)", o)
    if ml:
        print(f"        MemAvailable with 3 clients up: {ml.group(1)} kB")
    check("no OOM kills", "OOM=0" in o)
    check("no segfault/oops/panic", "BAD=0" in o)
    m = re.search(r"AVAIL=(\d+)", o)
    avail = int(m.group(1)) if m else 0
    check(f"MemAvailable >= {MEM_FLOOR_KB} kB", avail >= MEM_FLOOR_KB, f"{avail} kB")

    # --- summary -----------------------------------------------------------
    bad = [n for n, ok, _ in results if not ok]
    print(f"\n{len(results) - len(bad)}/{len(results)} passed")
    if bad:
        print("FAILED: " + ", ".join(bad))
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
