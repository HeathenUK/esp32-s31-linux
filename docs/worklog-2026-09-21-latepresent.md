# Late present - the flag never applied, and once it did it was +13% on Doom

Every late-present test before 2026-09-21 ~20:30 was INVALID. S40lvdesk does
`. /etc/lvdesk.env`, so a bare `XSHIM_LATEPRESENT=1` line only set a shell
variable, and lvdesk never saw it. Found by checking /proc/<pid>/environ - the
first time anyone did. The "neutral" canary A/B and the Doom 34.2/34.4 "late
present" run both compared off with off.

With `export XSHIM_LATEPRESENT=1` in the file, lvdesk restarted (no reboot),
and the flag confirmed in lvdesk's environment:

| | off | on |
|---|---|---|
| fullscreen Doom timedemo | 34.6 fps | **39.1 fps (+13%)** |
| frames < 25 ms | 1219 | **2626** |
| 25-50 ms | 3729 | 2359 |
| 50-100 ms | 74 | **39** |
| >= 100 ms | 7 | 5 |

The user saw it by eye before the instrument confirmed it - the throughput
mean could not show jitter, and the earlier "neutral" read came from a broken
toggle, not from the workload.

Also retracted: "fullscreen Doom bypasses the xshim present path" - it does not.
Frame-gap histogram cost: one clock read per presented frame, always compiled
in, identical in both arms.
