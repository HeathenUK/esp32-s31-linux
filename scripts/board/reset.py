#!/usr/bin/env python3
"""Reset the board, properly.

Never hand-roll the DTR/RTS sequence. On this board DTR drives EN and RTS
drives IO0, both inverted, and a sequence that never drives EN low leaves the
reset dependent on the pins' prior state: sometimes it resets, sometimes it
does nothing, sometimes it parks the board in download mode where the ROM
prints a few bytes and goes silent forever. That made "no output" mean nothing,
and poisoned four consecutive diagnoses.

esptool already does it correctly. Use esptool.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console

ESPTOOL = ('/Users/gadyke/.espressif/python_env/idf6.0_py3.12_env/bin/esptool')
PORT = '/dev/cu.usbserial-130'


def reset(port=PORT):
    """Take the port lock first.

    esptool opens the tty as a subprocess and knows nothing about our flock, so
    without this a reset can land in the middle of somebody else's measurement
    - which is worse than a contending read, because it REBOOTS THE BOARD under
    them and the run they were timing silently becomes a run of something else.
    Taking the lock here makes that collision impossible rather than unlikely.
    """
    console.take_port_lock(what='reset.py (esptool hard-reset)')
    try:
        subprocess.run([ESPTOOL, '-p', port, '--after', 'hard-reset',
                        'chip-id'], capture_output=True, timeout=60)
    finally:
        console.release_port_lock()


if __name__ == '__main__':
    reset(sys.argv[1] if len(sys.argv) > 1 else PORT)
    print('reset %s' % PORT)
