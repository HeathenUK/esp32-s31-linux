# Working rules

- Read `CLAUDE.md` and the newest dated status document in `docs/` before
  board work. Newer recorded state supersedes historical summaries.
- Use the existing Makefile recipes for builds, through `docker/build.sh`
  and `$S31_MAKE` in the container. Inspect the recipe and its inputs first;
  an old standalone `.sh` file is not evidence of the current build path.
- Reuse `scripts/board/` for console, reset, deployment and capture. Do not
  invent another runner. Never compete with a running board measurement.
- The harness is a workload: polling, forks, logging, screenshots, memory
  use and profiling/affinity changes can alter the result. Keep diagnostic
  runs distinct from quiet performance comparisons.
- For quiet detached jobs, prefer `runsh.py --done TOKEN --done-timeout N`
  and a completion line emitted only after workload exit. This is passive
  serial reading, not board polling; collect and validate results afterwards.
  A missing token is not proof the workload stopped.
- **A detached launcher saying STARTED is not proof the application ran.**
  Report attempted launch, verified execution and completed result separately.
  Require the application's completion evidence and exit status for a pass.
- **Older board images lack `timeout`.** `CONFIG_TIMEOUT=y` is now requested
  in the BusyBox fragment, but a source/config change is not a deployment.
  Before using it, require `command -v timeout` on that board. Use BusyBox
  syntax (`timeout -s KILL SECONDS COMMAND`); do not assume GNU exit codes.
  If absent, use `runsh.py`'s existing watchdog or its sleep/reap pattern for
  detached work. A missing command is a harness failure, never board failure.
- The build volume held a rejected test kernel at the 2026-09-27 checkpoint.
  Do not run bare `make sync-images`; verify the latest status and copy only
  intended artifacts. Do not overwrite the known-good #401 kernel.

For details, see `scripts/board/README.md` and
`docs/status-and-todo-2026-09-27.md`.
