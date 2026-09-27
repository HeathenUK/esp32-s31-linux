# s31fp copy investigation, 2026-09-27

Work in progress. No shipping library, overlay or flash image replaced.
Kernel #401 and shipped libs31fp 077a697a were verified before the runs.

## Evidence and limits

- Original report: one prboom SIGSEGV at level load after OpenTyrian, not a
  captured fault PC. Prior 25-second attempts are not full timedemo passes.
- prboom 2.5.0 on the board is md5 9aa9beaa. Its source unconditionally
  replaces SIGSEGV with I_SignalHandler; the prboom-plus -devparm exception
  does not apply. segvtrap now has opt-in SEGVTRAP_HOLD=1, verified on the board
  with a program that replaces the handler then faults (99 without hold,
  SIGSEGV/139 with PC and maps with hold). No stdio in the reporter.
- The first three launcher attempts did NOT run prboom: timeout was absent.
  They are recorded as failures in repro-old-trap.txt. PB_STARTED was wrongly
  reported to the owner as confirmed execution. That assertion was retracted.
- capture1: full 5026-gametic demo, 5 copied helpers, 2 coloured pages, no
  fault report. Exit 255 is NORMAL: g_game.c calls I_Error for the timedemo
  result and lprintf.c calls I_SafeExit(-1). The initial harness's rc=0 check
  was corrected. A mid-run console check contaminated performance; its fps
  is not used as performance evidence.
- quiet-copy-3: fresh boot, three explicitly enabled copy+colour attempts,
  no board tools until the complete 3 x 180 s bound elapsed. All three
  completed 5026 gametics, copied five helpers and coloured two pages, with
  expected exit 255 and no fault reports (quiet3-result.txt).

## Demonstrated unsafe error paths (not yet attributed to original crash)

linux-71-port/mm/mremap.c:mremap_to unmaps MREMAP_FIXED's destination BEFORE
vrm_set_new_addr/move_vma can fail. preload3.c assumed a failure left the
original text alone, marked the pool page used, and carried on. A later pool
allocation can even reuse the hole. Continuing after RX mprotect failure was
also unsafe: it retried blindly, then entered an application with NX text.

Compile-time fault injections (absent in production), through the Makefile:

    $S31_MAKE s31fp-v2 S31FP_TEST_OUT=/src/build/s31fp-fault-CASE \
      S31FP_TEST_CFLAGS=-DS31FP_TEST_REMAP_FAIL

Use RX_FAIL or RW_FAIL for the other cases. REMAP_FAIL removes the actual
mapping before returning ENOMEM, matching the kernel's destructive-error
possibility. It supplies synthetic page colours only in that test build, so
QEMU can exercise the path despite not exposing guest PFNs.

Host QEMU before: destructive remap failure -> SIGILL (132); RX failure -> SIGSEGV
(139); early RW failure -> original program passes. After: destructive remap
or RX failure -> explicit unconditional diagnostic and exit 125 BEFORE main;
early RW failure -> restore RX and run unpatched, passes. This is fail-safe
startup handling, not recovery from an already destructive mapping failure.

Other corrections: set mmap2's sixth syscall argument to zero; do not read
uninitialised pagemap entries after a short/failed read; request RW before
colouring mutates mappings; never ignore a failed icache-flush syscall.

Host arithmetic: fixed-qemu.txt, 14 helpers x 1,000,000 operand sets plus
random directed-rounding cases, zero mismatches vs libgcc (bits and fflags).
QEMU is a helper/fault-path test, not evidence of board cache correctness.
Board validation of the candidate is still required.

## Tooling follow-up requested by owner

CONFIG_TIMEOUT=y in busybox.fragment; make br-reconfigure-busybox succeeds.
Generated .config changes only CONFIG_TIMEOUT (plus timestamp). Post-build
requires the applet link, gate.py checks availability/normal exit. AGENTS.md
and CLAUDE.md explicitly require Makefile builds, prerequisite checks, and
completion evidence after quiet runs. New BusyBox is built, NOT installed.

Use the existing board scripts and Makefile targets. Do not run sync-images:
the build volume contains the rejected kernel. No v3 change has been made.

## Real-board fault injection and candidate validation

faults-board.txt: kernel #401, all six before/after cases completed. Old
REMAP, RX **and RW** cases exited 139 with captured fault PCs. The old RW
case differs from QEMU because real board colouring had already replaced
text with non-executable pages before the RW request failed. Candidate
REMAP/RX cases exited 125 with explicit diagnostics; RW ran unpatched and
returned 0 with the expected 12,000-byte dump. This strengthens the error-path
fix evidence but does not attribute the historical crash.

At 16:57 UTC the existing b5 launcher accepted the candidate exactness-only
run (14 operations, three arms, then 280 fresh copy processes). Collection
is deferred until after the six-minute window; launch is not a pass.

Candidate b5-result.txt: completed on #401, library d59b210e. All 14
operations x 100,000 inputs produced identical result/flag dumps for
unpatched/copy/trampoline. Hammer: 280 passed, zero failed, CPU affinity
0/1/either and alternating scan/cache. B5_DONE verified after six minutes
without board observation.

Candidate original-sequence launcher accepted after fresh reset, 17:05 UTC.
No collection before 17:13 UTC (77 s warmup + two 180 s bounds + margin).

Candidate application sequence completed: tyrian and gears survived their
60/17-second windows until deliberate SIGKILL; both subsequent prboom runs
completed all 5026 gametics, copied five helpers, coloured two pages, and
returned expected 255, no fault report. `PB_DONE runs=2 failures=0` in
candidate-result.txt. Windowed fps 48.4/47.7 are not comparable with the
earlier fullscreen shipped-library trials. Historical crash remains unproven;
this bounded investigation now moves to placement and broader v3 work.

Owner clarified that waiting the full bound wastes time after early completion.
runsh now optionally receives a passive serial completion token, without any
commands during a workload. Real-board fast-completion control passed; the
placement run uses it. No new serial runner or board polling was introduced.

## Placement comparison

Identical shipped bytes (077a697a) at XIP and SD, same OPL title workload,
three interleaved rounds with reversed paired order in round two; no debug
reporter/counters or observers. All 21 runs exited 0, identical audio hash
9979b782. Median/range us per sample:

- xip-tramp: 21.54 (21.18–21.93)
- sd-tramp: 21.13 (20.97–21.17)
- xip-copy: 19.66 (19.60–19.74)
- sd-copy: 19.84 (19.55–19.85)
- xip-nocol: 20.57 (20.16–29.32)
- sd-nocol: 19.78 (19.64–24.20)
- candidate-copy: 19.65 (19.56–19.79)

Coloured copy shows no useful SD advantage in this workload. Both placements
have uncoloured outliers; SD does not replace frame colouring. SD trampoline
was modestly faster, but do not extrapolate to library-resident strings or
Quake/GL. Candidate SD is a separate build and only corroborates no obvious
copy-performance regression. No extra fast-text RAM is justified by this arm.
Passive completion returned promptly, verified in placement-launch.txt.
