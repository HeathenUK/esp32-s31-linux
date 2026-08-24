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

## Status

The 6.18 kernel **builds**. It is 5,946,229 bytes against a 5,636,096 byte
partition - 310 KB over - because 6.18 is larger than 6.12 and this
configuration carries sound, Bluetooth and Wi-Fi.

The space exists: the userspace XIP image is 6.9 MB of Xorg, its modules and
X11 fonts, and the board now boots to text mode. Trimming `XIP_ROOTS` to what
a text system actually runs frees several megabytes to move into the kernel
partition.

**Not yet booted on hardware.** Building is not working; see
`docs/current-state.md` for what has to be re-verified after any kernel change.
