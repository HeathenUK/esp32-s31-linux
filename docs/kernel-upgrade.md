# Moving to a newer kernel

## The version is decided by XIP, not by what is newest

**RISC-V XIP kernel support was removed from mainline in v7.1.** It survives in
`arch/arm` and `arch/xtensa` only; `arch/riscv` has no reference to
`XIP_KERNEL` at all, and `vmlinux-xip.lds.S` is left behind orphaned. Checked
tag by tag:

	v6.13 .. v7.0   XIP_KERNEL present
	v7.1, v7.2      removed

This board executes its kernel from flash because it has 15.4 MB of RAM. So the
practical ceiling is v7.0 - and v7.0 is not in kernel.org's maintained list, so
it gets no fixes.

**Target: 6.18 LTS.** It keeps XIP, it is six releases newer than 6.12, and it
is maintained. Going to 7.x would mean re-adding and then carrying a feature
mainline deleted, which is a standing maintenance cost for no benefit here.

## The BSP is small, which is what makes this tractable

The vendor tree is a **single squashed import** of 6.12 - 86,729 files in one
commit - with the S31 work already mixed in. There is no upstream history to
rebase onto and no pristine base to diff against inside the repo.

But diffing it against a pristine 6.12 tarball shows the BSP is only:

	45 added paths        self-contained drivers, DTS, defconfigs
	101 modified files    ~3,100 changed lines
	0 deleted

and most of those 101 are one-line `obj-$(CONFIG_...)` registrations. The real
surface is `arch/riscv` (24 files: XIP, CLIC, custom extension state, cache and
DMA coherency), `drivers/usb`, `drivers/mmc` and `kernel/time`.

`scripts/port-kernel.sh` automates carrying that forward: diff old-pristine
against ours, apply to new-pristine as a patch, copy the purely-ours paths, and
carry anything upstream deleted.

**Count `.rej` files, not log lines.** `patch` writes "hunks failed" in lower
case; an early version of the script grepped for `FAILED` and cheerfully
reported a clean apply while rejects were being written to disk.

## What actually broke, 6.12 -> 6.18

Applying the BSP left 14 files with rejects, of which 10 were mechanical and 2
were stmmac (dropped - this board has no Ethernet PHY). The rest, plus what the
compiler then found, was genuine API churn:

| Change | Fix |
|---|---|
| `CACHEMAINT_FOR_DMA` became a menuconfig | move `select DMA_DIRECT_REMAP` to `SOC_ESP32S31`, or it closes a dependency loop with `ERRATA_STARFIVE_JH7100` |
| `folio->flags` -> `folio->flags.f` | mechanical |
| `struct function_desc.func` is now a `const struct pinfunction *` | drop the post-hoc fixup; `pinmux_generic_add_function()` already builds it |
| `PIN_CONFIG_OUTPUT` -> `PIN_CONFIG_LEVEL` | rename; this is the path `output-high`/`output-low` arrive on |
| `gpio_chip->set`/`set_multiple` now return `int` | return 0 |
| `platform_driver.remove_new` removed | rename to `.remove`, already the right shape |
| `hrtimer_init()` + assigning `.function` | `hrtimer_setup(timer, fn, clock, mode)` |
| `drm_fbdev_dma_setup()` removed | `DRM_FBDEV_DMA_DRIVER_OPS` in the driver + `drm_client_setup_with_fourcc()` |
| `cpufreq_generic_attr` removed | drop `.attr`; the core publishes it now |
| akcipher `.sign`/`.verify` removed | drop them; raw sign/verify moved to the `sig` alg type |
| IIO `write_event_config` state `int` -> `bool` | mechanical |
| `MODULE_IMPORT_NS(X)` -> `MODULE_IMPORT_NS("X")` | mechanical |
| `dw_mci_init_slot()` restructured | our `goto err_host_allocated` had no label; return directly |

All of it is in `patches/0001-esp32s31-6.18-api-fixes.patch`: 20 files, 387
lines.

## Making it fit: done

6.18 is bigger than 6.12, and this configuration carries sound, Bluetooth and
Wi-Fi. The space came from the userspace XIP image, which was 6.9 MB of Xorg,
its modules and X11 fonts for a board that now boots to text mode:

	XIP image   6,926,336 -> 3,596,288 bytes   (XIP_ROOTS trimmed to the text set)
	linux       0x560000  -> 0x620000          6,422,528 bytes
	rootfs      0x960000/0x6A0000 -> 0xA20000/0x5E0000

The 6.18 kernel is 5,966,709 bytes and now has 455 KB spare. The desktop root
set is kept as `XIP_ROOTS_DESKTOP` - it was tuned by measurement and should not
be re-derived.

**This layout is live and verified with the 6.12 kernel**: Wi-Fi, sound, both
cramfs XIP mounts and opkg all work on it.

## Why upstream removed it, and why that matters to us

Commit `9b3a2be84803` ("riscv: Remove support for XIP kernel", Nam Cao,
2026-04-04):

> XIP has a history of being broken for long periods of time. In 2023, it was
> broken for 18 months before getting fixed. In 2024 it was 4 months. And now
> it is broken again since commit a44fb5722199 ("riscv: Add runtime constant
> support"), 10 months ago. These are clear signs that XIP feature is not being
> used. [...] Remove XIP support. Revert is possible if someone shows up
> complaining.

So it went for lack of users, not because it is unfixable - and the middle
sentence is the operative one for us.

**Runtime constants are incompatible with XIP, and that is what stopped our
first 6.18 boot dead.** `runtime_const_init()` rewrites the `lui`/`addi`
parcels of an instruction *in place*; under `CONFIG_XIP_KERNEL` that memory is
the read-only flash window, so the fixup faults or is silently dropped. Its
only user is `fs/dcache.c`, from `dcache_init_early()` - early enough in
`start_kernel()` that there is no console yet, which is exactly why the board
was silent rather than printing an oops.

The fix is three lines: under `CONFIG_XIP_KERNEL`, include
`asm-generic/runtime-const.h` instead, which just uses the symbols directly.
`arch/riscv/include/asm/runtime-const.h` in the port does this. With it, 6.18
boots through DRM, the hosted radio, `wlan0`, `hci0`, microSD and EXT4.

## Reaching 7.1 / 7.2

Two routes, and they are genuinely different in kind:

1. **Revert the removal.** `9b3a2be84803` (17 files, -351/+37) and
   `8b5b048277e2`-era "riscv: further remove XIP" (4 files, -148/+3) are both
   published, so XIP can be restored onto 7.2 mechanically - roughly 500 lines -
   plus the runtime-const fix above, which upstream never made. We would then be
   maintaining a feature mainline deleted, but the commit explicitly invites it:
   "Revert is possible if someone shows up complaining."
2. **Stop using XIP.** `annoyedmilk/esp32-s31-linux` runs 7.1 on this same SoC by
   copying the kernel into PSRAM at `0x50000000` and booting a BusyBox
   initramfs - so the removal costs them nothing. That is the cheaper path in
   maintenance and the expensive one in RAM: our kernel is ~6 MB of the 15.4 MB
   total, and memory is the binding constraint on this board. It would also
   *gain* speed, since kernel code from flash is ~6x slower than from RAM.

Route 2 is worth measuring rather than assuming, now that the desktop is gone
and text mode has ~5 MB free.

## Status: boots to userspace, init segfaults

With the runtime-const fix, 6.18 boots through the whole kernel - DRM, the
hosted radio transport, `wlan0`, `hci0`, microSD, EXT4 - and then:

	init[1]: unhandled signal 11 code 0x1 at 0x00000000
	epc : 00000000 ra : 00000000   cause: 0000000c
	Kernel panic - not syncing: Attempted to kill init!

`epc` and `ra` both zero means the process entered userspace with a zeroed
context, i.e. the ELF entry point came out as 0. Userspace here is PIE (see
`s31-pie-cases`), so the interpreter's load bias is the thing to suspect.

Ruled out already: the `esp32s31_ext` field is in `struct thread_struct` at the
same position as 6.12, and `flush_icache_pte()`'s signature and callers are
identical between the two, so neither the custom extension state nor the
I-cache path explains it.

Earlier, before the runtime-const fix, it produced no output at all. Also
established then:

- **Not the partition geometry.** The flashed table decodes exactly as intended,
  and the 6.12 kernel boots from the same layout.
- **Not a loader restart loop.** One boot banner, no loader error, no reset.
- **Not the image header.** Both images carry the same `RISCV` magic and header
  layout; only the size field differs.
- **Not a missing device tree - but that *was* one bug.**
  `CONFIG_BUILTIN_DTB_SOURCE` was renamed `CONFIG_BUILTIN_DTB_NAME`, so the
  Makefile's `--set-str` set a symbol that no longer exists and the first
  attempt linked **no DTB at all**. The Makefile now sets both names. Fixing it
  grew the image by the size of the DTB, and changed nothing about the silence.
- **It dies before `earlycon`.** Adding
  `earlycon=esp32s31uart,mmio32,0x2038a000,1000000n8` produced no output either,
  so it is failing in early assembly or the XIP page-table setup, before console
  init.

The next place to look is the XIP early-boot path - `head.S`, and the 4K
leaf-mapping code in `create_kernel_page_table()` that this board needs because
RV32 folds the PMD. A `.text.fast`-style direct UART poke in `head.S` would
localise it, since there is no console to print from yet.
