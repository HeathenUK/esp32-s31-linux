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
import subprocess
import sys

ESPTOOL = ('/Users/gadyke/.espressif/python_env/idf6.0_py3.12_env/bin/esptool')
PORT = '/dev/cu.usbserial-130'


def reset(port=PORT):
    subprocess.run([ESPTOOL, '-p', port, '--after', 'hard-reset', 'chip-id'],
                   capture_output=True, timeout=60)


if __name__ == '__main__':
    reset(sys.argv[1] if len(sys.argv) > 1 else PORT)
    print('reset %s' % PORT)
