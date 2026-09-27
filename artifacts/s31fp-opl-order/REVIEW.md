# OPL multiply-order experiment — closed, not shipped

Changed five independent mul/mulhu pairs to mulhu/mul, with identical instruction
counts; left source-overwriting pairs unchanged. Candidate 682ea1094d4dfa56bac459195def81a8
versus installed #402 library 9767181c1c32d35d8eabc09108f87eff.

Existing Make s31fp-v3 built isolated outputs. Static QEMU oracle: 1M sets plus
249873 directed-rounding cases, zero bits/flags mismatches. Actual board:
separate unpatched/patched dumps agree on CPU0 and CPU1. Existing b10 arithmetic
harness: six alternating-order pairs for title/heavy songs, round zero warmup.
All 24 synth runs exit zero with identical audio hashes. No board observations
or transfers during the run; passive serial completion triggered collection.

Five measured pairs, median (range), microseconds/sample:
- Title: baseline 19.52 (19.47–19.80), candidate 19.48 (19.40–19.56).
- Heavy: baseline 27.95 (27.81–28.79), candidate 28.06 (27.84–28.28).

No repeatable workload benefit: title difference is tiny and heavy median is
worse. This does not prove S31 cannot fuse multiplies; it rejects the proposed
ordering as a demonstrated OPL improvement. Do not add register moves to chase
fusion without stronger evidence. Source restored to the deployed ordering;
rejected-order.patch preserves the exact experiment. Nothing deployed globally.

Source review: the NX comment's ordinary-signal example is too broad. The actual
S31 save_fp_state/restore_fp_state in arch/riscv/kernel/signal.c saves/restores
fcsr across signal return. Merely clearing NX in a returning handler therefore
does not alter the interrupted context. Deliberately editing saved ucontext is
separate and remains outside the existing exactness tests; no new runtime fix
is justified by the old comment alone. Preserve current rounding/flag semantics.

Next: measured memory matrix and selective library coverage, as the plan states.
Board libc hash 352e9417 and SDL1 hash dd6fe3c1 match the inspected local files.
Local prboom/OpenTyrian/libGL variants do not match installed hashes and must not
be treated as installed images. Installed memset is already scalar/unrolled,
so no PIE migration-avoidance benefit is available by replacing it.
