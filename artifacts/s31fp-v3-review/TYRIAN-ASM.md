# Tyrian arithmetic and hardware assessment, 2026-09-27

Review the existing implementation before adding another acceleration route.
This review does not claim the handwritten code is optimal.

## Evidence and current candidate

The existing title-song instruction profile attributes 52.5% to multiply,
16.4% to the remaining adlib code, 6.1% to signed-int conversion, 4.3% to
conversion back to int and 4.6% to addition. These are historical instrumented
counts, not current quiet-run timings. The final v2 report records roughly
53.7 multiplies/sample: 24.5 power-of-two, 10 integer/float-operand, 17 general,
and 1.3 zero. Division is absent from this workload. Optimizing division would
therefore not address title music.

v2 already uses Zbb clz, Zbs bit insertion, caller-saved registers without a
stack frame, exact power-of-two exponent adjustment, and narrower products for
integer/float-derived operands. The normal path preserves libgcc's result,
rounding mode and sticky flags; exceptional inputs fall back. Replacing double
with hardware float is not a bit-exact substitution.

A concrete simplification is in `.Lm_pow2a`: t6 contains x xor y and a3 still
contains y. Recover x with xor, isolate its sign with two shifts, and xor that
sign into the adjusted y. This saves two instructions versus separately
isolating the xor sign and y sign. It changes no exponent, rounding or fallback
logic. The isolated Makefile build passed relocation checks; QEMU v2check1
passed 1,000,000 operands plus 249,873 directed-mode cases, zero mismatches
against libgcc (bits and flags). See mul-exactness.txt. Board validation and
performance measurement remain pending; do not claim an audible improvement.

RISC-V recommends MULHU followed by MUL with identical ordered sources and a
high-result destination distinct from either source, allowing optional fusion:
https://docs.riscv.org/reference/isa/_attachments/riscv-unprivileged.pdf
The current general product uses the reverse order. Some pairs can be reordered
without extra instructions; the last overwrites a source and needs different
register allocation. S31 fusion support has NOT been established. Do not add
moves or claim a benefit just because the ISA permits fusion. This remains a
bounded experiment after the existing candidates, not a reason to replace the
entire multiply algorithm.

## BitScrambler: verified S31 capability, unsuitable helper granularity

Read the actual /opt/esp-idf tree in the build container, including:
- components/esp_hal_dma/esp32s31/include/hal/bitscrambler_peri_select.h
- components/esp_driver_bitscrambler/include/driver/bitscrambler_loopback.h
- components/esp_driver_bitscrambler/src/bitscrambler_loopback.c
- docs/en/api-reference/peripherals/bitscrambler.rst

The public S31 documentation agrees:
https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s31/api-reference/peripherals/bitscrambler.html

The documented data path routes bits from a DMA input, counters, comparison
results, previous output and LUT into a 32-bit output. It is useful for stream
formatting/lookup, not a native wide integer or floating-point multiply unit.
A custom multi-step arithmetic emulation is not ruled out mathematically, but
is a poor match for individual dependent soft-double calls. Such calls supply
only two operands and require their answer synchronously; they cannot be
batched across application dependencies by a generic helper interceptor.

Loopback exists, so 'only works attached to an active peripheral' would be an
incorrect rejection. It occupies both BitScrambler channels and a selected
peripheral's DMA functionality. The existing IDF driver synchronizes cached
buffers, resets/mounts DMA descriptors, starts both channels and waits for
completion. Linux would additionally need an ownership and buffer-sharing
interface. Those costs cannot be ignored or called free parallel execution.
No board offload experiment or speed claim was made. Retain BitScrambler as a
possible bulk format/LUT transform tool when a measured workload presents one;
do not build a new service merely to offload an 8-byte arithmetic helper.

## Placement and audible acceptance

Copied helpers execute in colour-selected application RAM; moving the source
library from flash to SD did not meaningfully improve the colour-copy title
benchmark (19.66 versus 19.84 us/sample medians). This does not settle placement
for interposed strings or other library-resident code. See placement-result.txt
in ../s31fp-copy-fix and REVIEW.md.

The latest 60-second native Tyrian window still logged 1116 underrun reports.
No matched baseline establishes a gain. Synth sample cost and output hashes
are useful gates, but sustained underruns and the owner's listening report
remain necessary acceptance evidence. A two-instruction helper reduction is
not grounds to promise crackle-free music, especially on the heavier songs.

## Board result for the two-instruction reduction

New library d538cddf versus hardened pre-change d59b210e, both on SD,
colour-copy enabled, CPU0 OPL benchmark. Round 0 excluded as warm-up; five
interleaved paired rounds, reversed order on alternate rounds:

| Song | Old median (range), us/sample | New median (range), us/sample |
| --- | --- | --- |
| 36 title | 20.21 (19.79–29.62) | 20.01 (19.90–29.40) |
| 5 heavy | 28.66 (28.41–28.73) | 28.23 (28.13–28.64) |

All 24 synth invocations exited zero and retained the matching hashes
9979b782 (title), 23d6ac74 (heavy). Small improvement direction; overlapping
spread means no robust application/audible speedup claim. Heavy synthesis
still misses real-time at 44.1 kHz. Keep the reduction for its smaller exact
instruction sequence, not a claimed cure for crackling.

The first in-process preload checker was insufficient: signature matching can
patch its renamed oracle too (28 matches). Correct validation uses the existing
binary dump in separate S31FP=0 and copy-enabled processes. New candidate,
100,000 multiply sets with seed 173 on CPU0 and CPU1: dumps identical, both
ARITHMETIC_EXACT markers and ARITHMETIC_EXACT_DONE recorded. Host static
v2check's oracle is not patched and its earlier million-case result remains
valid. See mul-board-exact.txt and mul-result.txt.
