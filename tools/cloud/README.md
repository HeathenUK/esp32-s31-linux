# Board-less development host (`tools/cloud/`)

For a machine with **no board attached**: a Claude Code cloud session, or any
x86_64 Ubuntu 24.04 host. It cross-compiles for the S31 and runs the result
under QEMU user-mode. It is for **correctness and instruction counts**. It can
never answer a timing question.

    make cloud-setup     # provision (idempotent; ~2.5 min cold on 4 cores, ~1 s warm)
    make cloud-check     # environment self-test, including negative controls
    make cloud-test      # s31fp v2 exactness + v3 strings/clock under QEMU (~10 s)
    . tools/cloud/env.sh # s31-cc, s31-qemu, qemu-riscv32, toolchain on PATH

On Claude Code on the web, `.claude/hooks/session-start.sh` runs the setup
automatically and loads `env.sh` into the session. The hook exits at once
anywhere `CLAUDE_CODE_REMOTE` is not `true`, so the Mac with the board is
never touched.

## What you get

| | |
|---|---|
| `toolchain/` | The pinned release toolchain `esp32s31-linux-gcc-15.2.0-4`, via the existing `make toolchain` (sha256-checked). |
| `build/cloud/qemu/` | `qemu-riscv32` **10.0.6** with TCG plugins. `plugins/libinsn.so` is also linked at `/plugins`, the path `icount.sh`/`oplcount.sh` expect. V2-REPORT's counts were taken on 10.0. Ubuntu's own qemu-user is 8.2 and has no plugin support. |
| `build/cloud/sysroot/` | The **QEMU-only** musl 1.2.5 and libgcc described below. |
| `/src` | A symlink to the checkout: the Docker build contract's path, which the s31fp scripts hardcode. |
| `s31-cc` | Compiles a QEMU binary: toolchain gcc, `-march` without `xesp*`, linked against the QEMU sysroot. |
| `s31-qemu` | Runs it. The default CPU is the **board's ISA** (`rv32imafc` + Zba/Zbb/Zbc/Zbs, **no D**), so a double that reaches the FPU is SIGILL here rather than a silent pass. `S31_QEMU_CPU=max` is for the v2 oracle (`hw.c`), which needs D. |

Instruction counts:

    s31-qemu -plugin /plugins/libinsn.so -d plugin ./prog args

## Why there is a separate QEMU sysroot

The release toolchain's GCC carries Espressif's **`xesploop`** patch, and its
musl the **`xespv`** string patch (`toolchain/.../s31-patch-sha256.txt`).
QEMU implements neither:

- Its libgcc `__addtf3`/`__subtf3`/`__multf3` contain `esp.lp.setup`
  hardware loops. musl's `printf` uses them for `long double`, so even
  `printf("%g")` dies with SIGILL.
- Its musl `memcpy` (`__riscv32_xespv_memcpy64`) and `memchr` use
  `esp.vld/vst.128` PIE vector ops.
- Any loop in **your own code** compiled with the default `-march` may become
  a hardware loop. So QEMU builds must pass an explicit
  `-march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs` (the s31fp scripts
  already do).

The sysroot is upstream musl 1.2.5, plus those three libgcc members rebuilt
from the same Espressif GCC revision (`0dbf5849`, from the toolchain's
crosstool-ng.config), for plain rv32imafc. `setup.sh` fails if any `esp.*`
instruction survives in it. It is for QEMU **only**: nothing linked against it
belongs on the board. For board binaries use `$S31_CC`.

Three s31fp scripts take optional overrides that `env.sh` sets. With them
unset, the Docker build behaves exactly as before:

- `S31_CC`: the compiler, in place of Buildroot's wrapper, for
  `build-preload3.sh` and `build-v3.sh`. The scripts pass `-march` themselves
  and use no Buildroot libraries, so the wrapper adds nothing they rely on.
- `S31_TARGET_SYSROOT`: the loader for v3's QEMU root.
- `S31_QEMU_LDFLAGS`: the link flags for v2's two **QEMU** test binaries
  (`v2check*`, `v2test*`). The `-board` binaries in the same script never
  get it.

## Measured here (2026-09-27)

- `make cloud-test`: all 14 v2 helpers, 50 k operand sets each (+25%
  under a directed rounding mode), **0 mismatches** in bits and fflags. v3
  strings: 100 k cases / 1.3 M calls, 0 mismatches, both through the preload
  and through libc. v3 clock ABI: 0 failures.
- Negative control: a `FENV=0` v2 build checked without `V2_NOFENV` reports
  **11,525** mismatches in 20 k sets, so the harness does fail when it should.

## The release toolchain is NOT what built the board's userspace

The deployed `overlay/opt/s31/libs31fp.so` (MD5 `9767181c…`, the #402 library)
has `.comment` `GCC: (crosstool-NG UNKNOWN) 15.2.0` and an arch tag with
`xespv2p2` but **no `xesploop`**. The same sources built here with the pinned
release toolchain give `bf4cc877…`, with `.comment`
`crosstool-NG esp-16.0.0_00000000.32-8c983cc` and `xesploop` in the arch tag.
The disassembly differs: our own C is code-generated differently.

What does agree: `build-preload3.sh` regenerates `sigs3.h` from the
toolchain's libgcc on every build, and the release toolchain reproduces the
committed file byte for byte. So the libgcc soft-double bodies that s31fp
matches in applications are identical, and exactness results transfer.

What does not transfer: **instruction counts and code size of our own C**.
Treat an icount delta measured here as a delta between two builds from this
toolchain, not as the number the board's build would give. Only the source-built
toolchain (`make toolchain-source`, from a `../crosstool-NG` checkout that is
not in this repo) reproduces board binaries.

## What this host cannot do

- **Anything timing-related**: fusion, latency, cache or flash-XIP effects,
  CSR cost, PSRAM bandwidth. QEMU has no pipeline model. Hand those to the
  board (`scripts/board/`) with the prepared binaries.
- **Build the kernel**: `linux-71-port/` is gitignored and not in the
  repository, so neither `make linux` nor OpenSBI's builtin DTB can be built.
- **Build the rootfs or the game ports**: those need a Buildroot tree
  (`build/buildroot`, ~an hour cold, and `esp-hosted-fg` uses an SSH-only
  submodule URL).
- **Talk to a board**: there is no serial port. Every `scripts/board/` tool
  will report NO_SHELL or PortBusy here, and that says nothing about the board.
