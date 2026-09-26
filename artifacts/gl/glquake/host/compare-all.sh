#!/bin/sh
# Diffs (tools/glref/compare.py) and luminance tables for every frame of
# the dark/ runs: mesa-full vs mesa-nocomb (path cost), mesa-nocomb vs ours
# (libGL cost, same QuakeSpasm path). Run from the Mac.
if [ ! -x /usr/bin/Xvfb ]; then
    REPO=$(cd "$(dirname "$0")/../../../.." && pwd)
    exec docker run --rm -v "$REPO":/src -w /src/artifacts/gl/glquake/dark s31-glref:latest \
        sh /src/artifacts/gl/glquake/host/compare-all.sh "$@"
fi
C=/src/tools/glref/compare.py
A=/src/artifacts/gl/glquake/host/analyze.py
mkdir -p diff sbs
for f in 250 400 550 700 850; do
    echo "== timedemo frame $f"
    printf 'path  (mesa-full vs mesa-nocomb): '; python3 $C mesa/mesa-full.f$f.png mesa/mesa-nocomb.f$f.png --diff diff/path.f$f.png
    printf 'libGL (mesa-nocomb vs ours):      '; python3 $C mesa/mesa-nocomb.f$f.png ours/ours.f$f.png --diff diff/libgl.f$f.png
    python3 $A sbs/timedemo.f$f.png mesa-full=mesa/mesa-full.f$f.png mesa-nocomb=mesa/mesa-nocomb.f$f.png ours=ours/ours.f$f.png
done
for f in 44 48; do
    echo "== plain start frame $f (the board screenshot is ~f44-48)"
    printf 'libGL (mesa-nocomb vs ours):      '; python3 $C mesa/plain-mesa-nocomb.f$f.png ours/plain-ours.f$f.png --diff diff/plain-libgl.f$f.png
    python3 $A sbs/plain.f$f.png board=board mesa-full=mesa/plain-mesa-full.f$f.png mesa-nocomb=mesa/plain-mesa-nocomb.f$f.png ours=ours/plain-ours.f$f.png
done
