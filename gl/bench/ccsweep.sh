#!/bin/bash
# ccsweep.sh OUT LISTFILE [JOBS] - cache-model arms of one qsr image in
# parallel (phase 6 tier 4, cache colouring). Each LISTFILE line is
#   LABEL|QSR_ENV|CS_ARGS
# run as QSR_ENV=... CS_ARGS=... cachesim.sh OUT LABEL; prints the
# per-frame lines in list order. s31, MIT.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "$1" = --one ]; then
	IFS='|' read -r l e a < <(sed -n "$4p" "$3")
	QSR_ENV="$e" CS_ARGS="$a" "$HERE/cachesim.sh" "$2" "$l" > /dev/null 2>&1
	rm -rf "$2/qsr/cs-run-$l"
	exit 0
fi
OUT=$(cd "$1" && pwd); LIST=$2; J=${3:-10}
# build the plugin once, not in every job
PL=$HERE/qemu/cachesim.dylib
if [ ! -f "$PL" ] || [ "$HERE/qemu/cachesim.c" -nt "$PL" ]; then
	cc -O2 -shared -fPIC -undefined dynamic_lookup $(pkg-config --cflags glib-2.0) \
		-I"$(brew --prefix qemu)/include" "$HERE/qemu/cachesim.c" -o "$PL" || exit 1
fi
seq 1 "$(wc -l < "$LIST")" | xargs -P "$J" -n 1 "$0" --one "$OUT" "$LIST"
while IFS='|' read -r l e a; do
	[ -z "$l" ] && continue
	cat "$OUT/qsr/cs-$l.txt" 2>/dev/null || echo "cachesim $l: FAILED"
done < "$LIST"
