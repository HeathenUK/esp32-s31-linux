#!/bin/sh
# Collect Qt and ONLY the libraries the board does not already have, into a
# tarball for /opt/qt5 on the SD card.
#
# Runs inside the build container (paths are container paths).
#
#   ./docker/build.sh '/src/rootfs/stage-qt.sh /src/rootfs/board-libs.txt'
#
# Why not just re-image the card with the new rootfs: Qt is an experiment, the
# card holds state the repo does not, and a re-image loses it. This stages Qt
# beside the existing system instead, at a path that is NOT part of the XIP
# overlay, so nothing about the shipping image changes.
#
# Why an exclusion list rather than bundling everything: /usr/lib is a cramfs
# XIP image, and a binary mapped from it costs ZERO RSS. Bundling our own copy
# of, say, libX11 would put a second copy in /opt/qt5/lib, LD_LIBRARY_PATH
# would prefer it, and it would be paged off the SD card at full RSS instead of
# mapped from flash for free. So anything already on the board must come from
# the board. Pass the list captured from the running system.
set -e

BOARD_LIBS="${1:?usage: stage-qt.sh <board-libs.txt>}"
TARGET=/src/build/buildroot/target
OUT=/src/images/qt5-stage.tar.gz
STAGE=$(mktemp -d)

[ -d "$TARGET" ] || { echo "no buildroot target at $TARGET" >&2; exit 1; }

# Where Buildroot put Qt. Discovered, not assumed - the layout has moved
# between Qt versions (lib/qt/plugins vs lib/qt5/plugins).
PLUGINS=$(find "$TARGET/usr/lib" -type d -name platforms -path "*plugins*" | head -1)
[ -n "$PLUGINS" ] || { echo "no Qt platforms plugin dir under $TARGET" >&2; exit 1; }
PLUGROOT=$(dirname "$PLUGINS")
echo "plugins: $PLUGROOT"

# The app. Qt's own examples are the off-the-shelf clients here.
APP=""
for cand in analogclock calculator digitalclock widgets; do
	f=$(find "$TARGET/usr/lib" "$TARGET/usr/bin" -type f -name "$cand" -perm -u+x 2>/dev/null | head -1)
	[ -n "$f" ] && { APP="$f"; break; }
done
[ -n "$APP" ] || { echo "no Qt example binary found - is QT5BASE_EXAMPLES on?" >&2; exit 1; }
echo "app: $APP"

mkdir -p "$STAGE/lib" "$STAGE/plugins" "$STAGE/bin"

# Transitive DT_NEEDED closure over the app and every plugin, resolved against
# the target sysroot. A plugin is dlopened, so nothing records a dependency on
# it and a naive ldd of the binary alone misses libqxcb and everything under it.
needed() { riscv32-esp-linux-musl-readelf -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

SEEN=""
QUEUE="$APP $(find "$PLUGROOT" -name '*.so')"
while [ -n "$QUEUE" ]; do
	NEXT=""
	for f in $QUEUE; do
		for n in $(needed "$f"); do
			case " $SEEN " in *" $n "*) continue;; esac
			SEEN="$SEEN $n"
			# Already on the board? Then it must come from the board.
			if grep -qx "$n" "$BOARD_LIBS"; then continue; fi
			src=$(find "$TARGET/usr/lib" "$TARGET/lib" -name "$n" | head -1)
			[ -n "$src" ] || { echo "  MISSING: $n" >&2; continue; }
			cp -aL "$src" "$STAGE/lib/$n"
			NEXT="$NEXT $STAGE/lib/$n"
		done
	done
	QUEUE="$NEXT"
done

cp -aL "$APP" "$STAGE/bin/"
cp -a "$PLUGROOT"/* "$STAGE/plugins/"
# Plugins keep their own DT_NEEDED on Qt libs; those are in lib/ already.

cat > "$STAGE/qt-env.sh" <<'ENV'
# Source this, or use it as a prefix, to run the staged Qt.
# LD_LIBRARY_PATH puts /opt/qt5/lib FIRST but it holds only libraries the board
# lacks, so everything already in the XIP overlay still maps from flash at zero
# RSS - see stage-qt.sh for why that matters.
export LD_LIBRARY_PATH=/opt/qt5/lib
export QT_PLUGIN_PATH=/opt/qt5/plugins
export QT_QPA_PLATFORM=xcb
export DISPLAY=:0
ENV

echo "--- staged ---"
du -sk "$STAGE"/lib "$STAGE"/plugins "$STAGE"/bin
ls "$STAGE/lib"
tar czf "$OUT" -C "$STAGE" .
ls -la "$OUT"
rm -rf "$STAGE"
