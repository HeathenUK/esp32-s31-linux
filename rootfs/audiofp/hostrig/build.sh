#!/bin/sh
# Host rig (aarch64 glibc, Colima VM with snd-dummy as card 0 = "codec" and
# snd-aloop as card 1 = the Bluetooth loopback). Builds s31route from the
# tree, the stuck test sink and sdltone (SDL2), installs asound.conf.
#
# One-time VM setup (Colima's Ubuntu kernel has both drivers as modules):
#   colima ssh -- sudo apt-get install -y linux-modules-extra-$(uname -r)
#   colima ssh -- sudo modprobe snd-dummy index=0 pcm_substreams=2 hrtimer=1 [model=ac97]
#   colima ssh -- sudo modprobe snd-aloop index=1
#   docker build -t s31route-hosttest rootfs/audiofp/hostrig
# model=ac97 makes the "codec" 48 kHz-only, which exercises s31resample.
# Run (from rootfs/audiofp/hostrig):
#   docker run --rm --device /dev/snd -v $REPO:/src:ro -v $PWD:/w \
#       s31route-hosttest sh -c '/w/build.sh && /w/rapid.sh'
#   run.sh <rate> <ch> <samples> <secs> "<t:action ...>"  - one scheduled run
#   (actions: sink=<pcm>, ready / unready / deadready = fake /run/s31-bt-sink)
# SRC=<file> builds another s31route.c, for an A/B against the old one.
set -e
D=/usr/lib/aarch64-linux-gnu/alsa-lib
mkdir -p $D
SRC=${SRC:-/src/rootfs/s31route.c}
gcc -O2 -Wall -Wdouble-promotion -fsingle-precision-constant -fPIC -DPIC -shared -I/src/rootfs "$SRC" -o $D/libasound_module_pcm_s31route.so -lasound
gcc -O2 -Wall -fPIC -DPIC -shared /w/stuck.c -o $D/libasound_module_pcm_stuck.so -lasound
gcc -O2 -Wall -I/usr/include/SDL2 -o /usr/local/bin/sdltone /src/rootfs/audiofp/sdltone.c -lSDL2 -lpthread -lm
cp /w/asound.conf /etc/asound.conf
mkdir -p /run
