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

BOARD_LIBS_IN="${1:?usage: stage-qt.sh <board-libs.txt>}"
# The list is captured from the board over the serial console, which leaves
# CRLF endings. "grep -x libxcb.so.1" then never matches "libxcb.so.1\r", the
# exclusion silently does nothing, and every library the board already has gets
# duplicated into /opt/qt5/lib - where it is paged off SD instead of mapped
# from XIP at zero RSS. Strip them.
BOARD_LIBS=$(mktemp)
tr -d '\r' < "$BOARD_LIBS_IN" > "$BOARD_LIBS"
trap 'rm -f "$BOARD_LIBS"' EXIT
TARGET=/src/build/buildroot/target
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
# Libraries post-build.sh replaces with slim shim versions in the target. Kept
# in step with that list by hand; a mismatch shows up as a missing symbol at
# dlopen time, not at link time.
SLIM="libfontconfig.so.1 libXrender.so.1 libXft.so.2 libX11.so.6 libXext.so.6 \
      libXcursor.so.1 libICE.so.6 libSM.so.6"
OUT=/src/images/qt5-stage.tar.gz
STAGE=$(mktemp -d)

[ -d "$TARGET" ] || { echo "no buildroot target at $TARGET" >&2; exit 1; }

# Where Buildroot put Qt. Discovered, not assumed - the layout has moved
# between Qt versions (lib/qt/plugins vs lib/qt5/plugins).
PLUGINS=$(find "$TARGET/usr/lib" -type d -name platforms -path "*plugins*" | head -1)
[ -n "$PLUGINS" ] || { echo "no Qt platforms plugin dir under $TARGET" >&2; exit 1; }
PLUGROOT=$(dirname "$PLUGINS")
echo "plugins: $PLUGROOT"

# The apps. Qt's own examples are the off-the-shelf clients here - unmodified
# upstream code, which is the point: nothing about them is tuned for this board.
#
#   gallery       every common widget on one form - the visual correctness test
#   analogclock   continuous repaint, so it measures frame cost
#   basicdrawing  QPainter primitives, the raster paint engine under load
APPS=""
for cand in gallery analogclock basicdrawing; do
	f=$(find "$TARGET/usr/lib" "$TARGET/usr/bin" -type f -name "$cand" -perm -u+x 2>/dev/null | head -1)
	[ -n "$f" ] && APPS="$APPS $f"
done
[ -n "$APPS" ] || { echo "no Qt example binary found - is QT5BASE_EXAMPLES on?" >&2; exit 1; }
APP=$(echo $APPS | cut -d' ' -f1)
echo "apps:$APPS"

mkdir -p "$STAGE/lib" "$STAGE/plugins" "$STAGE/bin"

# Transitive DT_NEEDED closure over the app and every plugin, resolved against
# the target sysroot. A plugin is dlopened, so nothing records a dependency on
# it and a naive ldd of the binary alone misses libqxcb and everything under it.
# readelf is NOT on PATH in the build container, and a missing one fails
# silently here: needed() returns nothing, the closure is empty, and the
# tarball ships plugins and binaries with no libraries at all. Resolve it
# explicitly and fail loudly if it is absent.
READELF=""
for r in /src/build/buildroot/host/bin/riscv32-*-readelf \
	 /src/toolchain/riscv32-esp-linux-musl/bin/riscv32-*-readelf; do
	[ -x "$r" ] && { READELF="$r"; break; }
done
[ -n "$READELF" ] || { echo "no target readelf found" >&2; exit 1; }
echo "readelf: $READELF"

needed() { "$READELF" -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

SEEN=""
QUEUE="$APPS $(find "$PLUGROOT" -name '*.so')"
while [ -n "$QUEUE" ]; do
	NEXT=""
	for f in $QUEUE; do
		for n in $(needed "$f"); do
			case " $SEEN " in *" $n "*) continue;; esac
			SEEN="$SEEN $n"
			# Already on the board? Then it must come from the board -
			# UNLESS the board's copy is one of the deliberately slim
			# replacements. post-build.sh swaps a dozen X libraries
			# for hand-written ones carrying only the symbols the
			# existing clients call (libfontconfig is 5,360 bytes
			# against 423,636 upstream). That is exactly right for
			# this desktop and wrong for Qt, which calls symbols the
			# slim ones never needed to export - the failure is
			# "Error relocating libQt5XcbQpa.so.5: FcPatternAdd:
			# symbol not found", at dlopen of the platform plugin,
			# long after everything appears to have linked.
			#
			# For those, take the FULL library out of the staging
			# sysroot into /opt/qt5/lib. Qt then gets a complete
			# implementation and the rest of the desktop keeps the
			# slim one, because LD_LIBRARY_PATH applies to Qt only.
			case " $SLIM " in
			*" $n "*)
				src=$(find "$SYSROOT/usr/lib" -name "$n" 2>/dev/null | head -1)
				[ -n "$src" ] && { cp -aL "$src" "$STAGE/lib/$n"
					NEXT="$NEXT $STAGE/lib/$n"; }
				continue;;
			esac
			if grep -qx "$n" "$BOARD_LIBS"; then continue; fi
			# Target first - that is what a real image ships. The
			# external toolchain's sysroot is a fallback because
			# Buildroot does NOT copy libstdc++ into target here
			# even with BR2_INSTALL_LIBSTDCPP=y (its layout differs
			# from what the copy step expects), and Qt is C++, so
			# without this the whole stack ships unloadable.
			src=$(find "$TARGET/usr/lib" "$TARGET/lib" -name "$n" 2>/dev/null | head -1)
			[ -n "$src" ] || src=$(find /src/toolchain/*/*/sysroot/lib \
				/src/toolchain/*/*/sysroot/usr/lib \
				-name "$n" 2>/dev/null | head -1)
			[ -n "$src" ] || { echo "  MISSING: $n" >&2; continue; }
			cp -aL "$src" "$STAGE/lib/$n"
			NEXT="$NEXT $STAGE/lib/$n"
		done
	done
	QUEUE="$NEXT"
done

[ -n "$(ls -A "$STAGE/lib")" ] || {
	echo "dependency closure is EMPTY - readelf or the exclusion list is wrong" >&2
	exit 1
}

for a in $APPS; do cp -aL "$a" "$STAGE/bin/"; done
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
