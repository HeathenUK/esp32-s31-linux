#!/bin/sh
# Diagnostic libGL variants, built from a COPY of gl/ (gl/ itself is never
# edited): exp-base = unchanged (the baseline for the same sources); exp-round = texture.c's T565 rounds to nearest instead of
# truncating (all textures); exp-lmround = rounds only for 256-wide
# internal-format-4 textures (QuakeSpasm's 256x256 lightmap blocks).
# Output: host/exp-*/gl/out-host/libGL.so.1 (the path keeps capture.c's
# "/gl/out-host/" rule). Run from the Mac.
set -e
REPO=$(cd "$(dirname "$0")/../../../.." && pwd)
H=$REPO/artifacts/gl/glquake/host
for v in base round lmround; do
    D=$H/exp-$v
    rm -rf "$D"; mkdir -p "$D"
    rsync -a --exclude ref-apps --exclude 'out-*' "$REPO/gl/" "$D/gl/"
    F=$D/gl/tinygl/source/texture.c
    case $v in
    base) ;;
    round)
        perl -0pi -e 's/#define T565\(r, g, b\) .*\n/#define RND8(v, m) ((v) + (m) > 255 ? 255 : (v) + (m))\n#define T565(r, g, b) ((unsigned short)(((RND8(r, 4) & 0xF8) << 8) | ((RND8(g, 2) & 0xFC) << 3) | (RND8(b, 4) >> 3)))\n/' "$F" ;;
    lmround)
        perl -0pi -e 's/#define T565\(r, g, b\) .*\n/#define RND8(v, m) ((v) + (m) > 255 ? 255 : (v) + (m))\n#define T565(r, g, b) (lmr ? (unsigned short)(((RND8(r, 4) & 0xF8) << 8) | ((RND8(g, 2) & 0xFC) << 3) | (RND8(b, 4) >> 3)) : (unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))\n/' "$F"
        perl -0pi -e 's/(  int cls = tex_class\(ifmt, &lum\);\n)/$1  int lmr = ws == 8 \&\& ifmt == 4;\n/' "$F" ;;
    esac
    grep -n "define T565\|int lmr" "$F" | head -2
    docker run --rm -e S31GL_IN_RIG=1 -e S31GL_NO_GLX= -v "$D":/src -w /src s31-glref:latest \
        sh /src/gl/host-build.sh 2>&1 | grep -iE "error|warning|libGL.so.1" | head -5
done
