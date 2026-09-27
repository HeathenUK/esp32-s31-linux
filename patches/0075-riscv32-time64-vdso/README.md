# RV32 kernel-backed time64 vDSO

Apply `time64-vdso.patch` to the Linux 7.1 port after the existing board patches.
This enables the existing generic Linux timekeeper/vvar implementation on RV32,
exports versioned time64 clock_gettime/clock_getres, reads TIMEH/TIME/TIMEH across
32-bit counter rollover, and uses time64 syscall fallbacks. RV64 keeps its ABI.
No timer driver, application, musl, or dynamic-loader changes are required.

The vDSO clock object uses -O2 -mno-save-restore: -Os emitted __lshrdi3, and the
board kernel's -msave-restore emitted out-of-object register-save helpers. A
standalone vDSO cannot resolve those. The existing build's relocation check
rejects such a library. The successful RV32 vDSO has no relocations, stays within
one 4 KiB RAM page (vdso_start=c0802000, vdso_end=c0803000), and its time state is
maintained by the kernel's standard sequence-count protocol. There is no
userspace calibration, writer lock, periodic resynchronization or guessed rate.

RV32 musl 1.2.5 does not bind clock_gettime to vDSO. s31fp v3 resolves these two
versioned symbols using musl's existing MIT-licensed ELF lookup algorithm and
interposes external clock/gettimeofday/time calls. Older kernels and S31CLK=0
use libc; unsupported clock IDs use the normal syscall ABI. Internal libc calls
are not intercepted. gettimeofday/time derive from kernel REALTIME, preserving
clock steps and NTP corrections. This is not an approximate clock.

Build through the existing recipe, in an isolated output copied from verified
#401 (1e579994cc0d326eeb0ab032324db746):

```
./docker/build.sh '$S31_MAKE linux LINUX_OUT=/src/build/linux-s31-vdso XIP_IMAGE=/src/build/s31-vdso-release/xipImage FDT_DTB=/src/build/s31-vdso-release/esp32s31_generic.dtb'
./docker/build.sh '$S31_MAKE s31fp-v3'
```

Only two .config additions relative to #401: GENERIC_TIME_VSYSCALL and
GENERIC_GETTIMEOFDAY. Image MD5 7685a9d76275a511d701bb70a3bbab2f, 5,164,669 bytes.
Actual-board acceptance and deployment evidence is in artifacts/s31fp-vdso.

## Actual-board validation and enabled deployment

Kernel #402 and libs31fp MD5 9767181c1c32d35d8eabc09108f87eff are installed.
The library is `/opt/s31/libs31fp.so` on SD, selected for login and desktop via
`/etc/s31fp.env`; copy, strings and S31CLK=1 are enabled. Reset verification
confirmed #402, lvdesk PID 249 and active vDSO with ABI checks passing.

`clock-board-full.txt`: exact syscall brackets for realtime/monotonic/raw/coarse/
boottime and process/thread CPU clocks, matching resolutions, invalid-clock errno,
fork, ~1 kHz signal re-entry, both CPUs, S31CLK=0 fallback and combined shipping
copy/string policy all passed. Three threads with CPU migration made 6,555,804
calls over 35 s (multiple low-counter wraps), zero monotonicity violations.
A forward REALTIME step and restoration were immediately reflected correctly.

Hot calls, 200,000 iterations per operation, no applications alongside:

| CPU | monotonic | raw | realtime | gettimeofday | raw syscall |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0 | 456 ns | 471 ns | 429 ns | 470 ns | 2514 ns |
| 1 | 564 ns | 593 ns | 559 ns | 558 ns | 3049 ns |

About 5x lower hot overhead; not a measured game FPS gain. Call frequency varies
by application and was not instrumented during quiet application runs. The
adapter's SD-backed pages can be reclaimed, so these are not worst-case latency
bounds. Kernel vDSO code/data cannot be swapped to SD.

Initial clock-board.txt records a harness failure (`taskset` absent), not a
clock failure. The corrected reusable runner requires the existing oncpu helper.
BusyBox taskset/CPU-list support is configured and build-verified but not yet
installed on the board; gate and post-build now enforce this dependency.
