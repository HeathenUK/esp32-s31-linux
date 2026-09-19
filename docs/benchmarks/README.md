# September 13 SDL measurement records

See [the report](../sdlbench-results-2026-09-13.md) for conclusions and limitations.

- `sdlbench-2026-09-13.jsonl`: individual CLI records, prefixed with collection names to keep run IDs unique. Selected memory snapshots are retained; repetitive mapping dumps are in local raw artifacts.
- `sdlbench-2026-09-13.csv`: measurement records with explicit accepted/failed status. Acceptance means the invocation passed its checks, not complete visual conformance or measured panel FPS.
- `sdlbench-manifest-2026-09-13.json`: source/binary SHA256s and unchanged installed-library MD5s for the final clients.
- JPEG/MJPEG files: separate hardware capture validation, excluded from timing runs.

`gated_control` records form the repeated futex comparison. `gated_path_smoke`
records establish final path coverage with one run per mode. `exploratory`
records include earlier measurements and the failed 640x400 run; do not pool
these evidence classes. Times are nanoseconds, CPU accounting is microseconds,
and compositor ticks use each record's `clk_tck`. Per-run p95 values are not a
pooled p95. No actual-game FPS or numerical observer-effect result is claimed.
