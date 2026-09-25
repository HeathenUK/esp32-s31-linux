#!/usr/bin/env python3
"""Check every GL_* enum value in TinyGL's internal gl.h against the Khronos
headers. TinyGL compiles against its own header, the ABI layer against
Khronos: a value that differs would silently mean a different thing on each
side of the boundary. Exit status 1 on any mismatch.

usage: enumcmp.py tinygl/include/GL/gl.h include/GL/gl.h include/GL/glext.h
"""
import re, sys
tg = open(sys.argv[1]).read()
kh = {}
for f in sys.argv[2:]:
    for m in re.finditer(r'#define\s+(GL_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b', open(f).read()):
        kh.setdefault(m.group(1), int(m.group(2), 0))
bad = 0
for m in re.finditer(r'\b(GL_\w+)\s*=\s*(?:\(int\))?\s*(0x[0-9A-Fa-f]+|\d+)', tg):
    n, v = m.group(1), int(m.group(2), 0)
    if n in kh and kh[n] != v:
        print("enumcmp: %s tinygl=0x%x khronos=0x%x" % (n, v, kh[n]))
        bad += 1
print("enumcmp: %d mismatches" % bad)
sys.exit(1 if bad else 0)
