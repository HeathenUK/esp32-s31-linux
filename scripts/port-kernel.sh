#!/bin/bash
# Re-create the ESP32-S31 kernel tree on a new upstream base.
#
# The vendor tree is a single squashed import of 6.12 with the S31 work already
# mixed in, so there is no upstream history to rebase onto. What there is, is a
# small BSP: diffing against a pristine tarball of the same base yields 101
# modified files (~3,100 lines) and 45 added paths. That is the thing to carry
# forward.
#
# Usage: port-kernel.sh <pristine-old-tree> <pristine-new-tree> <our-tree> <out>
set -euo pipefail
OLD=$1 NEW=$2 OURS=$3 OUT=$4

# Files that differ only because macOS is case-insensitive and these upstream
# names differ only in case - xt_DSCP.c/xt_dscp.c and friends. Not our changes.
CASE_TWINS='xt_DSCP|xt_HL|xt_RATEEST|xt_TCPMSS|xt_CONNMARK|xt_MARK|ipt_ECN|ipt_TTL|ip6t_HL|litmus'

rm -rf "$OUT"; cp -a "$NEW" "$OUT"

diff -rq --no-dereference -x '.git*' "$OLD" "$OURS" > /tmp/portq.txt 2>/dev/null || true
grep '^Files ' /tmp/portq.txt | sed "s|Files $OLD/||; s| and .*||" | grep -vE "$CASE_TWINS" > /tmp/portmod.txt

# Modified upstream files: apply as a patch so upstream churn shows up as a
# conflict instead of being silently overwritten.
: > /tmp/port.patch
while read -r f; do
	[ -e "$NEW/$f" ] || { echo "GONE UPSTREAM: $f"; continue; }
	diff -u --label "a/$f" --label "b/$f" "$OLD/$f" "$OURS/$f" >> /tmp/port.patch || true
done < /tmp/portmod.txt

( cd "$OUT" && patch -p1 --forward --no-backup-if-mismatch < /tmp/port.patch ) \
	> /tmp/port-apply.txt 2>&1 || true
# patch(1) writes "hunks failed" in lower case; grepping for FAILED reports a
# clean apply while rejects are being written to disk. Count the .rej files -
# they are the ground truth, not the log.
REJ=$(find "$OUT" -name '*.rej' | wc -l | tr -d ' ')
echo "files with rejected hunks: $REJ"
find "$OUT" -name '*.rej' | sed "s|$OUT/||" | sort

# Files upstream deleted but we still need: carry ours verbatim.
while read -r f; do
	[ -e "$NEW/$f" ] || { mkdir -p "$OUT/$(dirname "$f")"; cp -a "$OURS/$f" "$OUT/$f"; echo "carried: $f"; }
done < /tmp/portmod.txt

# Purely-ours files and directories: copy wholesale, nothing to conflict with.
grep '^Only in ' /tmp/portq.txt | grep -v "$OLD" | sed "s|Only in $OURS/*||; s|: |/|" | while read -r p; do
	[ -z "$p" ] && continue
	mkdir -p "$OUT/$(dirname "$p")"
	cp -a "$OURS/$p" "$OUT/$p"
done
echo "port written to $OUT"
