# Working rules

- **Keep executing an authorized active plan.** A completed milestone, a plan,
  or a promise to continue is not a reason to end the turn. Commit milestones
  and continue with progress updates. Stop only when the requested work is
  complete, the user asks to stop, or a concrete blocker genuinely requires user
  input; identify that blocker explicitly. Never substitute a final promise for
  the next available action.

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
- Completion waits require a successful launcher (`RS_EXIT:0`). `runsh`
  rejects setup failures immediately; never describe a pending host wait as
  proof of board execution. CLI completion mode streams passive evidence to
  stderr. The shipped BusyBox `tar` lacks `-z`: use checked `gzip -dc`, then
  `tar xf`, instead of assuming desktop command options exist on the board.
- **A detached launcher saying STARTED is not proof the application ran.**
  Report attempted launch, verified execution and completed result separately.
  Require the application's completion evidence and exit status for a pass.
- **Older board images lack `timeout`.** `CONFIG_TIMEOUT=y` is now requested
  in the BusyBox fragment, but a source/config change is not a deployment.
  Before using it, require `command -v timeout` on that board. Use BusyBox
  syntax (`timeout -s KILL SECONDS COMMAND`); do not assume GNU exit codes.
  If absent, use `runsh.py`'s existing watchdog or its sleep/reap pattern for
  detached work. A missing command is a harness failure, never board failure.
- **Older board images also lack `taskset`.** Before any affinity workload,
  require `command -v taskset` and CPU-list support, or check executable
  `/root/afp2/oncpu` and use its hexadecimal mask argument (1=CPU0, 2=CPU1).
  Do not assume a config edit installed an applet. BusyBox now requests
  CONFIG_TASKSET=y and CONFIG_FEATURE_TASKSET_FANCY=y; post-build and gate check
  installation/support. Preflight dependencies before launching any workload.
- Keep kernel variants isolated. The 2026-09-27 fast-clock work verified the
  default build image against known-good #401 and built its candidate in
  `build/linux-s31-vdso`. Never use bare `make sync-images` to ship a variant:
  verify hashes, copy the intended artifact explicitly and pass its path to
  `make flash-linux XIP_IMAGE=...`. Preserve `images/ship-401-xipImage` for rollback.

For details, see `scripts/board/README.md` and
`docs/status-and-todo-2026-09-27.md`.
