#!/bin/sh
# Listening check for the 2026-09-27 audio changes (s31route buffer fix,
# native 22.05/24/32 kHz codec rows, s31route's own resampler). Installed on
# the board as /root/audiocheck.sh. Run it from the board's console (or an
# ssh/serial shell) while standing by the speaker:
#
#     sh /root/audiocheck.sh            # every case, ~50 s
#     sh /root/audiocheck.sh 36000 2    # one case
#
# Each case plays /root/afp/wavgen's ~4.5 s signal through the normal
# "default" device (plug -> s31route -> codec), exactly as an application
# would, at DAC 143 (the owner's level). Per case:
#   0-1 s    a pure A440: concert A. Flat/sharp = the rate is wrong.
#   1-2.6 s  C-E-G-C plucked arpeggio, LEFT speaker only in the stereo cases
#   2.6-3.4  the same, faster, RIGHT only (stereo cases)
#   3.4-4.5  a C major chord
# Listen for: hiss or noise under the notes, a harsh/metallic edge on the
# bright arpeggio (converter images), clicks, dropouts or a stutter, and the
# A being out of tune from one case to the next. All cases should sound the
# same apart from the channel placement.
DAC=143
amixer -q -c 0 sset DACL $DAC 2>/dev/null
amixer -q -c 0 sset DACR $DAC 2>/dev/null
if [ -n "$1" ]; then CASES="$1:${2:-2}"; else
CASES="22050:1 22050:2 32000:1 32000:2 36000:1 36000:2 44100:1 44100:2 48000:1 48000:2"; fi
for c in $CASES; do
	r=${c%:*}; ch=${c#*:}
	[ "$ch" = 1 ] && name=mono || name=stereo
	echo "=== now playing: $r Hz $name ==="
	/root/afp/wavgen $r $ch > /tmp/audiocheck.wav || { echo "wavgen failed"; exit 1; }
	aplay -q /tmp/audiocheck.wav &
	p=$!
	sleep 1
	hw=$(grep -E '^rate' /proc/asound/card0/pcm0p/sub0/hw_params 2>/dev/null)
	wait $p
	echo "    codec ran at: ${hw:-?}"
	sleep 1
done
rm -f /tmp/audiocheck.wav
echo "=== done ==="
