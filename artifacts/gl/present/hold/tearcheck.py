#!/usr/bin/env python3
"""tearcheck.py <rec.mjpeg>: frames of the tear client with more than one
solid palette colour inside the window are torn presents."""
import io, sys
from PIL import Image
PAL = [(255,0,0),(0,255,0),(0,0,255),(255,255,0),(0,255,255),(255,0,255)]
data = open(sys.argv[1], 'rb').read()
frames, i = [], 0
while True:
    a = data.find(b'\xff\xd8', i)
    if a < 0: break
    b = data.find(b'\xff\xd9', a)
    if b < 0: break
    frames.append(data[a:b+2]); i = b + 2
torn = 0
for k, f in enumerate(frames):
    im = Image.open(io.BytesIO(f)).convert('RGB')
    w, h = im.size
    px = im.load()
    cnt = [0]*6
    blk = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            if r < 25 and g < 25 and b < 25:
                blk += 1
                continue
            for j, (pr, pg, pb) in enumerate(PAL):
                if abs(r-pr) < 60 and abs(g-pg) < 60 and abs(b-pb) < 60:
                    cnt[j] += 1
                    break
    tot = sum(cnt)
    big = [j for j in range(6) if tot and cnt[j] > 0.02 * tot]
    if len(big) > 1 or (tot and blk > 0.02 * tot and len(sys.argv) > 2):
        torn += 1
        if torn <= 5:
            print("frame %d torn: %s" % (k, cnt))
print("%d frames, %d torn" % (len(frames), torn))
