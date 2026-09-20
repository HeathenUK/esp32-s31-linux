#!/usr/bin/env python3
"""What is the board actually doing right now?

    alive.py                 # passive: poke the console and report
    alive.py --reset         # reset, then watch the whole boot through

Written because "NO_SHELL" was answering a different question than the one
being asked. It means "I did not see a prompt", and that is true of at least
five completely different situations:

    the board is unpowered or held in reset
    hart0 is running but Linux never started
    Linux started and panicked or wedged part way
    Linux is up and sitting at login:, which emits nothing
    Linux is up at a shell prompt, which also emits nothing

The last two are *healthy*, and an idle healthy board looks byte-for-byte
identical to a dead one on a serial line. Reporting them the same way has
already produced two wrong diagnoses in one session - once "the kernel hangs",
which it did not, and once a real wedge that was nearly dismissed as the same
false alarm.

The two things that make this hard, both learned the expensive way:

  - **The board prints at two bauds.** hart0's second-stage bootloader talks at
    115200 and only then does the console switch to 1 Mbps. Watching a reset at
    1 Mbps shows the first seconds as framing garbage or nothing at all, so
    "silent early" proves nothing whatsoever. This reads each phase at the baud
    that phase actually uses.
  - **Silence is not evidence.** So this pokes: a newline at a login prompt
    redraws it, and at a shell prompt returns another prompt. If a poke gets an
    answer the board is alive no matter how quiet it was.
"""
import argparse
import os
import subprocess
import sys
import time

import re

import serial

import console

PORT = os.environ.get("S31_PORT", "/dev/cu.usbserial-130")

# Ordered: the furthest match wins, so a later stage implies the earlier ones.
STAGES = [
    ("HART0_ROM",    ("ESP-ROM:", "rst:0x", "boot:")),
    ("HART0_APP",    ("Loaded app from partition", "oct_psram", "esp_psram")),
    ("HANDOFF",      ("Linux mapped at", "OpenSBI mapped at", "s31_display:")),
    ("KERNEL_EARLY", ("Linux version", "Booting Linux", "riscv:")),
    ("DISPLAY_UP",   ("scanout started",)),
    ("ROOT_MOUNTED", ("Mounted root", "Run /init", "VFS: Pivoted")),
    ("USERSPACE",    ("Starting syslogd", "Growing root", "Starting udevd")),
    ("LOGIN",        (re.compile(r"login:\s*$", re.M),)),
    # A PROMPT, at the end of a line - not the substring "# ". hart0's loader
    # prints "## Label            Usage ..." above its partition table on every
    # boot, so the old needle called any board that reached the second-stage
    # loader SHELL, with Linux dead. It lied three times on 2026-09-20 and the
    # last one ("booted cleanly") went to the user while the panel sat on the
    # loader's splash.
    ("SHELL",        (re.compile(r"(^|\s)(~|/[\w/.-]*) #\s*$", re.M),)),
]

PROMPT_LINE = re.compile(r"^(~|/[\w/.-]*) #$")

# Stages hart0's own output may prove. Everything later must come from Linux.
HART0_STAGES = ("HART0_ROM", "HART0_APP", "HANDOFF")


def furthest(text, only=None):
    """Classify on the FILTERED lines, never the raw buffer: framing garbage
    from reading one baud at another contains '# ' often enough that the raw
    match declared STAGE SHELL on a board that never left the loader - an
    empty 'linux phase' followed by SHELL was believed for four resets."""
    clean = "\n".join(readable_lines(text))
    seen = None
    for name, needles in STAGES:
        if only is not None and name not in only:
            continue
        if any((n.search(clean) if hasattr(n, "search") else n in clean)
               for n in needles):
            seen = name
    return seen


def read_for(baud, secs, poke=None):
    try:
        console.take_port_lock(what='alive.py')
        s = serial.Serial(PORT, baud, timeout=0.2)
    except serial.SerialException as e:
        return "", "PORT_ERROR: %s" % e
    buf = b""
    try:
        if poke:
            s.write(poke)
        t0 = time.time()
        while time.time() - t0 < secs:
            d = s.read(8192)
            if d:
                buf += d
    finally:
        s.close()
    return buf.decode("utf-8", "replace"), None


def readable_lines(text):
    """Drop the framing garbage produced by reading one baud at another."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        printable = sum(1 for c in line if 32 <= ord(c) < 127)
        # A bare busybox prompt is 3 characters; the 4-character floor below
        # threw it away, and only the old sloppy '# ' needle ever "saw" a shell.
        if PROMPT_LINE.match(line) or printable >= max(4, int(len(line) * 0.8)):
            out.append(line)
    return out


def report(stage, lines, extra=""):
    print("STAGE %s" % (stage or "NOTHING"))
    if extra:
        print(extra)
    for l in lines[-40:]:
        print("  | %s" % l)
    return 0 if stage in ("SHELL", "LOGIN") else 1


def passive():
    """Is it alive right now? Poke it, because idle is silent.

    THE POKE IS A NONCE, NOT A NEWLINE.
    =====================================================================
    A bare newline at a shell prompt gets `# ` back - two printable
    characters - and readable_lines() drops anything under four, so the
    SHELL needle could never match the one reply a healthy idle board
    actually sends. It appeared to work only because hart0's Wi-Fi
    chatter supplied longer lines that matched an EARLIER stage.

    So the moment a client was rendering and hart0 went quiet, a
    perfectly healthy board reported STAGE SILENT. That false negative
    cost most of a day on 2026-09-06: a black-window bug at 640x400 was
    diagnosed as a capture-path wedge, then a GEM wedge, then a PPA
    wedge, then a CMA wedge - four wrong causes, a kernel change and a
    DTS revert - and the board was reachable by runsh.py the whole time.

    Echoing a nonce fixes both halves. It cannot be forged by framing
    garbage, so it can be matched against the RAW text without
    reintroducing the false-SHELL problem the filter exists for; and a
    shell that echoes it is provably executing commands, which is a
    stronger statement than "something printed".

    A busy board is SLOW, not dead: this waits longer and retries before
    it will say SILENT.
    """
    nonce = "A%dZ" % (time.time_ns() % 100000000)
    poke = ("\r\necho " + nonce + "\r\n").encode()

    for attempt in (1, 2, 3):
        text, err = read_for(1000000, 4.0, poke=poke)
        if err:
            print(err)
            return 2
        lines = readable_lines(text)
        # The nonce comes back twice (echo of the typed line, then its
        # output); either occurrence proves a live shell.
        if nonce in text:
            return report("SHELL", lines,
                          "shell echoed the nonce %s (attempt %d)"
                          % (nonce, attempt))
        stage = furthest(text)
        if stage:
            return report(stage, lines,
                          "poked the console and it answered (attempt %d)"
                          % attempt)
    text, err = read_for(1000000, 4.0, poke=poke)
    lines = readable_lines(text)
    stage = furthest(text)
    if stage:
        return report(stage, lines, "poked the console and it answered")
    # Nothing at 1 Mbps. Is hart0 talking at 115200 - i.e. stuck pre-handoff?
    text2, _ = read_for(115200, 3.0)
    lines2 = readable_lines(text2)
    if furthest(text2):
        return report(furthest(text2), lines2,
                      "answering at 115200 only: hart0 is up, Linux is not")
    print("STAGE SILENT")
    print("  no answer to a nonce echo at either baud, after 4 tries.")
    print("  NOTE: a heavily loaded board is slow, not dead - if this")
    print("  disagrees with runsh.py, believe runsh.py and fix this.")
    print("  That is a wedged or unpowered board, OR a board mid-boot -")
    print("  run with --reset to watch a boot from the beginning.")
    return 2


def watch_reset(timeout):
    subprocess.run([sys.executable,
                    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "reset.py")],
                   check=False, capture_output=True)
    # hart0 first, at its own baud.
    early, _ = read_for(115200, 12.0)
    early_lines = readable_lines(early)
    early_stage = furthest(early, only=HART0_STAGES)
    print("--- hart0 phase (115200, %d raw bytes, %d readable lines) ---"
          % (len(early), len(early_lines)))
    for l in early_lines:
        print("  | %s" % l)
    if not early_stage:
        print("STAGE NOTHING")
        print("  hart0 printed nothing at 115200. The board is unpowered,")
        print("  held in reset, or parked in download mode.")
        return 2

    # Then Linux, at the baud the console switches to.
    print("--- linux phase (1 Mbps, %ds) ---" % timeout)
    late, _ = read_for(1000000, timeout, poke=b"\r\n")
    late_lines = readable_lines(late)
    print("(%d raw bytes, %d readable lines)" % (len(late), len(late_lines)))
    stage = furthest(late) or early_stage
    if stage in ("LOGIN", "SHELL"):
        # Seeing a prompt scroll past is not the same as a board that answers.
        nonce = "A%dZ" % (int(time.time() * 1000) % 100000000)
        text, _ = read_for(1000000, 4.0, poke=("\r\necho %s\r\n" % nonce).encode())
        if nonce in text or re.search(r"login:\s*$", text, re.M):
            return report(stage, late_lines, "confirmed: the console answered a poke")
        print("STAGE %s_SEEN_BUT_SILENT" % stage)
        print("  a %s prompt went past during boot, but the console did NOT answer a" % stage.lower())
        print("  poke afterwards. Do not call this board up.")
        return 2
    print("  (%d kernel lines seen)" % sum(1 for l in late_lines if l.startswith("[")))
    return report(stage, late_lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--timeout", type=int, default=70)
    a = ap.parse_args()
    return watch_reset(a.timeout) if a.reset else passive()


if __name__ == "__main__":
    sys.exit(main())
