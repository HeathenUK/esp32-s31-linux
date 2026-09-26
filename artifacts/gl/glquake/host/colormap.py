#!/usr/bin/env python3
"""Software Quake's lighting response (gfx/colormap.lmp) against
QuakeSpasm's GL overbright response (2 * lightmap / 255, r_brush.c
R_BuildLightMap >>8 + glBlendFunc(DST_COLOR, SRC_COLOR)), from pak0.pak.
Software (r_surf.c R_BuildLightMap): row = ((255*256 - lm*256) >> 2) >> 8,
clamped to >= 1<<6 before the shift, so lightmap lm maps to row (255-lm)/4."""
import struct, sys
import numpy as np
pak = open(sys.argv[1], "rb").read()
_, dofs, dlen = struct.unpack("<4sii", pak[:12])
files = {}
for i in range(dlen // 64):
    name, ofs, ln = struct.unpack("<56sii", pak[dofs + 64 * i: dofs + 64 * i + 64])
    files[name.split(b"\0")[0].decode()] = pak[ofs:ofs + ln]
pal = np.frombuffer(files["gfx/palette.lmp"], np.uint8).reshape(256, 3).astype(float)
cm = np.frombuffer(files["gfx/colormap.lmp"][:64 * 256], np.uint8).reshape(64, 256)
L = pal @ [0.299, 0.587, 0.114]
ok = (np.arange(256) < 224) & (L > 8)          # not fullbrights, not black
print("lightmap  sw_row  software_gain  gl_overbright_gain  gl_no_overbright  sw/gl")
for lm in (16, 32, 48, 64, 96, 128, 160, 192, 224, 255):
    t = max((255 * 256 - lm * 256) >> 2, 1 << 6)
    row = t >> 8
    gain = np.median(L[cm[row]][ok] / L[ok])
    gl = 2 * lm / 255
    print("%8d %7d %14.3f %19.3f %17.3f %6.2f" % (lm, row, gain, gl, min(gl, 1), gain / gl))
