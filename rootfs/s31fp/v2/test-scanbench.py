#!/usr/bin/env python3
"""Validate current full-body inventory against positive and deceptive ELF fixtures."""
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

header = pathlib.Path(__file__).with_name('sigs3.h').read_text()
def body(name):
    data = re.search(r'body_' + name + r'\[\d+\] = \{([^}]+)\}', header)[1]
    return bytes(int(v, 16) for v in data.split(','))

def elf(parts):
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    start = 52 + 32 * len(parts)
    eh = struct.pack('<16sHHIIIIIHHHHHH', ident, 3, 243, 1, 0, 52, 0, 0, 52, 32, len(parts), 0, 0, 0)
    ph, payload = b'', b''
    for data, flags in parts:
        off = start + len(payload)
        ph += struct.pack('<IIIIIIII', 1, off, off, 0, len(data), len(data), flags, 2)
        payload += data
    return eh + ph + payload

with tempfile.TemporaryDirectory() as tmp:
    count = 0
    def check(name, data, expected=None):
        global count
        p = pathlib.Path(tmp) / name
        p.write_bytes(data)
        r = subprocess.run([sys.argv[1], str(p)], capture_output=True, text=True)
        if expected is None:
            assert r.returncode != 0, (name, r.stdout, r.stderr)
        else:
            assert r.returncode == 0, (name, r.stderr)
            assert len(re.findall(r'^MATCH ', r.stdout, re.M)) == expected, (name, r.stdout)
        count += 1
    mul = body('muldf3')
    check('exact-at-segment-end', elf([(mul, 5)]), 1)
    corrupt = bytearray(mul); corrupt[-1] ^= 1
    check('prefix-only-rejected', elf([(corrupt, 5)]), 0)
    div = bytearray(body('divdf3'))
    for off in (250, 254): div[off:off+4] = b'\xff'*4
    check('relocation-masked', elf([(div, 5)]), 1)
    check('multiple-executable-segments', elf([(mul, 5), (body('floatunsidf'), 5)]), 2)
    check('non-executable-excluded', elf([(mul, 4)]), 0)
    check('truncated-body', elf([(mul[:-2], 5)]), 0)
    check('truncated-header', b'\x7fELF')
    check('truncated-segment', elf([(mul, 5)])[:-1])
    wrong = bytearray(elf([(mul, 5)])); wrong[4] = 2
    check('wrong-class', wrong)
    print(f'PASS scanbench {count} ELF/matcher fixtures')
