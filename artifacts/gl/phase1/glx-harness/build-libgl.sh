#!/bin/sh
# Build libGL.so.1 (core + gl/glx) with the core's own gl/api/build-lib.sh
# into this harness's libgl-out/, so testing never touches gl/out-host.
export GL=/src/gl CC=gcc NM=nm ARCHFLAGS= XINC= XLIBS="-lXext -lX11" \
	OBJ=/tmp/s31gl-harness OUT=/src/artifacts/gl/phase1/glx-harness/libgl-out/libGL.so.1
mkdir -p $(dirname $OUT)
sh /src/gl/api/build-lib.sh && ln -sf libGL.so.1 $(dirname $OUT)/libGL.so
