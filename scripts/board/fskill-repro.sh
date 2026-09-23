#!/bin/bash
# fskill-repro.sh - does lvdesk keep painting after a FULLSCREEN client dies by
# SIGKILL? On 2026-09-23 a gate run straight after apps-smoke.sh had killed a
# fullscreen OpenTyrian with -9 scored "restore+raise repaints: nothing was
# drawn". lvdesk.c has a "fullscreen off (client gone)" path; this checks it
# fires. Booted, idle desktop; ~1 min; ends with the smoke checks.
set -u
cd "$(dirname "$0")/../.."
OUT=artifacts/perf-plan/fskill-$(date +%H%M%S); mkdir -p "$OUT"
cat > "$OUT/run.sh" <<'EOS'
L=$(ps | awk '/[l]vdesk/ {print $1}' | head -1)
N0=$(grep -ac "fullscreen" /var/log/lvdesk.log)
cd /root/oty/usr/share/opentyrian/data && HOME=/root DISPLAY=:0 setsid /root/oty/usr/bin/opentyrian --no-joystick </dev/null >/root/oty.log 2>&1 &
sleep 12
echo "FS_LINES_DURING $(grep -a 'fullscreen' /var/log/lvdesk.log | tail -2 | tr '\n' '|')"
kill -9 $(ps | awk '/[o]pentyrian/ {print $1}'); sleep 3
echo "FS_LINES_AFTER $(grep -a 'fullscreen' /var/log/lvdesk.log | tail -2 | tr '\n' '|')"
echo "LVDESK_ALIVE $(ps | grep -c '[l]vdesk') fs_lines_added=$(( $(grep -ac fullscreen /var/log/lvdesk.log) - N0 ))"
echo FK_DONE
EOS
python3 scripts/board/runsh.py "$OUT/run.sh" 60 40 2>&1 | tr -d '\r' | grep -aE "^(FS_LINES|LVDESK_ALIVE|FK_DONE)|NO_SHELL"
python3 scripts/board/screenshot.py "$OUT/after-kill.png" >/dev/null 2>&1 && echo "  panel: $OUT/after-kill.png"
python3 scripts/board/smoke.py 2>&1 | grep -aE "PASS|FAIL" | head -12
