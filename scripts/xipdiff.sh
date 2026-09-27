#!/bin/sh
# xipdiff.sh <old.cramfs> <new.cramfs> [path ...] - list the files that differ
# between two XIP cramfs images, then md5 each named path in both. Run inside
# the build container, e.g.
#   ./docker/build.sh 'sh /src/scripts/xipdiff.sh /src/images/old.cramfs \
#       /src/build/rootfs-xip.cramfs usr/bin/lvdesk usr/lib/libs31fp.so'
set -e
CK=/src/build/buildroot/host/bin/cramfsck
W=$(mktemp -d)
$CK -x $W/a "$1" >/dev/null
$CK -x $W/b "$2" >/dev/null
shift 2
echo "entries: old $(find $W/a | wc -l) new $(find $W/b | wc -l)"
diff -rq --no-dereference $W/a $W/b | sed "s|$W/a|OLD|g; s|$W/b|NEW|g" || true
echo "(end of differences)"
for p in "$@"; do
	echo "$p old $(md5sum < $W/a/$p 2>/dev/null | cut -c1-8) new $(md5sum < $W/b/$p 2>/dev/null | cut -c1-8)"
done
rm -rf $W
