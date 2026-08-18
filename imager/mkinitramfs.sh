#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Assemble the imager initramfs from the Buildroot target tree. Kept minimal on
# purpose: it is RAM-resident and this board has ~14 MB, so it carries busybox,
# the C library and the receiver, and nothing else.
#
#   imager/mkinitramfs.sh <buildroot-target-dir> <staging-dir> <sdrecv-binary>
set -eu

TARGET=$1
STAGE=$2
SDRECV=$3

rm -rf "$STAGE"
mkdir -p "$STAGE"/bin "$STAGE"/sbin "$STAGE"/lib "$STAGE"/proc "$STAGE"/sys "$STAGE"/dev "$STAGE"/tmp

cp -a "$TARGET"/bin/busybox "$STAGE"/bin/
cp "$SDRECV" "$STAGE"/bin/sdrecv
cp "$(dirname "$0")"/init "$STAGE"/init
chmod +x "$STAGE"/init "$STAGE"/bin/sdrecv

# The C library, installed at whatever path busybox names as its interpreter.
# On musl the loader and libc are the same file, and Buildroot leaves /lib/
# holding only a symlink into /usr/lib, which a copy would not preserve.
mkdir -p "$STAGE"/usr/lib
cp -a "$TARGET"/usr/lib/libc.so "$STAGE"/usr/lib/
interp=$("$TARGET"/../host/bin/*-readelf -l "$TARGET"/bin/busybox 2>/dev/null |
	sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
if [ -n "$interp" ]; then
	mkdir -p "$STAGE$(dirname "$interp")"
	ln -sf /usr/lib/libc.so "$STAGE$interp"
	echo "interpreter: $interp -> /usr/lib/libc.so"
else
	echo "WARNING: could not determine busybox's interpreter" >&2
fi

# Applets the init script actually calls.
for applet in sh mount umount dd sync sleep echo cat cut stty dmesg md5sum gunzip awk head; do
	ln -sf /bin/busybox "$STAGE"/bin/"$applet"
done

echo "initramfs staged at $STAGE:"
du -sh "$STAGE" | cut -f1
