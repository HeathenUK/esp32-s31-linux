#!/bin/sh
# Build stock QuakeSpasm 0.96.3 for the host GL rig (s31-glref container),
# the same switches as rootfs/build-quakespasm.sh uses for the board:
# USE_SDL2=0 (SDL 1.2 = the rig's sdl12-compat), no MP3/Vorbis, WAV on.
# Source: a copy of tyrquake/quakespasm-quakespasm-0.96.3 in ./src (the
# original tree holds the board's riscv objects; not touched).
#   artifacts/gl/glquake/host/build-host.sh      (from the Mac)
set -e
if [ ! -x /usr/bin/Xvfb ]; then
    REPO=$(cd "$(dirname "$0")/../../../.." && pwd)
    exec docker run --rm -v "$REPO":/src -w /src s31-glref:latest \
        sh /src/artifacts/gl/glquake/host/build-host.sh "$@"
fi
H=/src/artifacts/gl/glquake/host
[ -d $H/src/Quake ] || { echo "copy the source to $H/src first"; exit 1; }
cd $H/src/Quake
make -j$(nproc) USE_SDL2=0 USE_CODEC_MP3=0 USE_CODEC_VORBIS=0 USE_CODEC_WAVE=1 \
    CPUFLAGS="-fsingle-precision-constant" 2>&1 | grep -iE "error|warning: implicit|quakespasm" | tail -15
cp quakespasm $H/quakespasm
ls -la $H/quakespasm
ldd $H/quakespasm | grep -iE 'sdl|libGL'
