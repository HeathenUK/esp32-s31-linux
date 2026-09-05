#!/usr/bin/env bash
# Ground truth: what an X client is SUPPOSED to look like.
#
#   scripts/xref.sh xcalc [outfile.png]
#
# Builds the app from the tarball Buildroot already downloaded, against REAL
# libXt and libXaw in a throwaway Debian container, runs it under Xvfb with its
# own app-defaults installed, and captures the window.
#
# This exists because appearance has twice been "settled" by reading source and
# been wrong both times (see the s31-xvfb-ground-truth note). Our libX11, libXt
# and libXaw are all replacements - xlite, xtlite and an EMPTY libXaw7 whose
# symbols resolve to xtlite - so "is this how the app should look?" cannot be
# answered from our own stack at all. It has to come from a real one.
#
# First use settled a real question: xcalc's button grid ends a few pixels short
# of its display bevel on the right, and that is AUTHENTIC - the reference shows
# the identical gap. What is ours is that xtlite's rubber-sheet scale_tree()
# multiplies that gap by the resize factor, so maximising turns ~6 px into ~28.
#
# The container is disposable and mounts the repo read-only, so it cannot touch
# the build volumes. Nothing here runs against the board.
set -euo pipefail
APP="${1:?usage: xref.sh <app> [out.png]}"
OUT="${2:-references/${APP}-real.png}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TAR=$(ls "$REPO"/buildroot/dl/xapp_"$APP"/"$APP"-*.tar.* 2>/dev/null | head -1 || true)
[ -n "$TAR" ] || { echo "no Buildroot tarball for $APP under buildroot/dl/xapp_$APP" >&2; exit 1; }
echo "--- reference build of $(basename "$TAR") against real Xaw ---"

mkdir -p "$REPO/$(dirname "$OUT")"
# The work directory must live INSIDE the repo. On macOS `mktemp -d` returns a
# /var/folders path that Docker Desktop does not share, so the bind mount
# silently creates a DIRECTORY at the target and the container dies with
# "/run.sh: Is a directory" - which reads like a script bug, not a mount one.
WORK="$REPO/.xref-work"
rm -rf "$WORK"; mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/run.sh" <<'INNER'
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq build-essential pkg-config xutils-dev libxaw7-dev \
    libxt-dev libx11-dev libxext-dev libxmu-dev libxkbfile-dev libxft-dev \
    xvfb x11-apps x11-utils xfonts-base imagemagick >/dev/null 2>&1
mkdir -p /b /usr/share/X11/app-defaults && cd /b
tar -xf /tarball
cd */
./configure --prefix=/usr >/tmp/conf.log 2>&1 || { echo "CONFIGURE FAILED"; tail -12 /tmp/conf.log; exit 1; }
make -j"$(nproc)" >/tmp/make.log 2>&1 || { echo "BUILD FAILED"; tail -12 /tmp/make.log; exit 1; }
# The app-defaults ARE the layout spec; without them the app lays out wrong and
# the reference is worthless.
for f in app-defaults/*; do
    [ -f "$f" ] && install -m644 "$f" "/usr/share/X11/app-defaults/$(basename "$f")"
done
Xvfb :99 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 3
export DISPLAY=:99
./"$APPNAME" >/dev/null 2>&1 &
sleep 8
ID=$(xwininfo -root -tree | awk '/^ +0x/ && !/has no name/ {print $1; exit}')
echo "geometry: $(xwininfo -id "$ID" | awk '/Width:|Height:/ {printf "%s ", $2}')"
xwd -id "$ID" -out /tmp/x.xwd
convert /tmp/x.xwd /out/ref.png
INNER

docker run --rm --platform linux/arm64 \
    -v "$TAR:/tarball:ro" -v "$WORK:/out" -v "$WORK/run.sh:/run.sh:ro" \
    -e APPNAME="$APP" debian:12 bash /run.sh
cp "$WORK/ref.png" "$REPO/$OUT"
echo "--- reference: $OUT ($(stat -f%z "$REPO/$OUT" 2>/dev/null || stat -c%s "$REPO/$OUT") bytes) ---"
