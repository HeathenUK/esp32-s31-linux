#!/bin/bash
# c45-handoff.sh [reps] - re-price the kworker hand-off (C45), same boot, A/B/A.
#
# 2026-08-27 measured the deferred damage copy at 7.4 ms of flush_work() wait
# against 1.62 ms inline (docs/current-state.md "The kworker hand-off costs
# ~8 ms"), HZ-independent, mechanism never found. That constant is what
# forbids handing lvdesk's per-frame expand-and-present to the idle CPU1.
# Since then: the CLIC-level bug (patches/0057, a task switched in from an IRQ
# stayed deaf until some sret), OpenSBI's trap path in SRAM (context switch
# 280.7 -> 57.2 us), softirq.o in RAM, SMP. If the hand-off is ~1 ms now, the
# SMP plan's step 5 reopens.
#
# dirtybench takes DRM master, so lvdesk is stopped for the run and restarted
# after; it does an atomic commit per rep and times DIRTYFB. defer_copy=1
# hands the damage copy to a workqueue (the 7.4 ms arm), 0 does it inline.
# A/B/A: the A-to-A gap is the error bar (dead-end #19: same-boot arms are
# position-biased). ~2 min. The panel goes dark while lvdesk is down.
#
# LVDESK_DIRECT=0 LVDESK_NOPRESENT=1 is load-bearing: kms.c now defaults to
# direct scanout + the PRESENT ioctl, where there is no per-frame copy at all
# and defer_copy never engages (first run 2026-09-23: every arm 0.05-0.4 ms).
# The 7.4 ms was measured on the dumb-buffer DIRTYFB copy path; this forces it.
set -u
cd "$(dirname "$0")/../.."
REPS=${1:-40}
OUT=artifacts/perf-plan/c45-$(date +%H%M%S); mkdir -p "$OUT"
S=$OUT/run.sh
cat > "$S" <<EOF
chmod +x /root/dirtybench
P=/sys/module/esp32s31_lcd/parameters
echo "PARAMS defer_copy=\$(cat \$P/defer_copy) ppa_async=\$(cat \$P/ppa_async) force_eng=\$(cat \$P/force_eng)"
/etc/init.d/S40lvdesk stop >/dev/null 2>&1; sleep 2
for arm in A1 B A2; do
	case \$arm in B) echo 1 > \$P/defer_copy;; *) echo 0 > \$P/defer_copy;; esac
	echo "ARM \$arm defer_copy=\$(cat \$P/defer_copy)"
	LVDESK_DIRECT=0 LVDESK_NOPRESENT=1 /root/dirtybench $REPS 2>&1 | sed "s/^/  \$arm /"
done
echo 0 > \$P/defer_copy
/etc/init.d/S40lvdesk start >/dev/null 2>&1
sleep 3; echo "LVDESK \$(ps | grep -c '[l]vdesk') console_keyboard=\$(cat /sys/class/tty/tty0/active 2>/dev/null)"
echo C45_DONE
EOF
python3 scripts/board/runsh.py "$S" 150 60 2>&1 | tr -d '\r' > "$OUT/run.log"
grep -aE "^(PARAMS|ARM|  A1|  B|  A2|LVDESK|C45_DONE)|NO_SHELL|KILLED" "$OUT/run.log"
echo "log: $OUT/run.log"
