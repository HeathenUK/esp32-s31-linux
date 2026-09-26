# s31-glref: the host-side GL correctness rig (docs/gl-plan-2026-09-25.md).
# Xvfb + reference Mesa (llvmpipe/softpipe) + X11/GL/GLU/freeglut dev headers,
# so the same stock app can be rendered by Mesa and by our libGL and compared
# by pixels. Never used for board builds. Mount the repo under /Users (Docker
# on this Mac discards writes to /private/tmp mounts).
FROM debian:stable
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    xvfb xauth x11-utils x11-apps x11-xserver-utils mesa-utils \
    libgl1-mesa-dri libglx-mesa0 libgl-dev libglu1-mesa-dev freeglut3-dev \
    libx11-dev libxext-dev libxi-dev libxrandr-dev libxxf86vm-dev \
    build-essential meson ninja-build pkg-config cmake python3 python3-pil python3-numpy \
    imagemagick wget ca-certificates xz-utils bzip2 git file binutils \
    libsdl1.2-dev libsdl2-dev \
 && rm -rf /var/lib/apt/lists/*
# Stage-5a game corpus (tools/glref/build-games.sh): rRootage links SDL_mixer
# 1.2, GLtron libpng. A separate layer so the Mesa layer above is unchanged
# (suite.sh stamps the Mesa version; check it after a rebuild). rRootage reads
# its data from a compiled-in /usr/share/games/rRootage/; the link resolves
# once the repo is mounted at /src.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libsdl-mixer1.2-dev libpng-dev \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /usr/share/games \
 && ln -s /src/gl/ref-apps/rrootage-0.23a/rr_share /usr/share/games/rRootage
