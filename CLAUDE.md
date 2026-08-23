# Working on this board

Linux 6.12 on an ESP32-S31-Korvo-1 V1.1: dual hart (hart0 runs ESP-IDF and owns
the radios and audio, hart1 runs Linux), RV32 soft-float, **15.4 MB of usable
RAM**, kernel executing XIP from 80 MHz flash, rootfs on microSD.

Every rule below cost hours to learn. Read `docs/current-state.md` first after a
context reset - it records what is true now and what has already failed.

## Never hand-roll the tooling

**The scripts already exist, in `scripts/board/`. Use them.** Every one of them
has been re-written from scratch mid-task at least twice, badly, while the
original sat a directory away:

    scripts/board/reset.py                      # reset the board
    scripts/board/runsh.py  <script.sh> [t] [w] # run a shell script on it
    scripts/board/deploy_bin.py <f.b64> <dest>  # ship a binary to it
    scripts/board/screenshot.py <out.png>       # capture the real panel

**Do not write a DTR/RTS reset sequence.** Not once, not "just quickly". DTR
drives EN and RTS drives IO0, both inverted, and any sequence that fails to
drive EN low leaves the reset dependent on the pins' prior state - sometimes it
resets, sometimes it does nothing, sometimes it parks the board in download
mode where the ROM emits a few bytes and then goes silent forever. That makes
"no output" mean nothing at all, and it has already poisoned four consecutive
diagnoses. `scripts/board/reset.py` calls esptool, which does it correctly.

**Do not write another serial-console runner.** `runsh.py` ships the script as
a file (flattening it into `; ` one-liners breaks every multi-line construct
and yields empty output that looks like a hardware fault), and it tolerates the
login race - the LCD driver prints mode-set messages exactly when getty shows
its prompt, so a naive matcher reports NO_SHELL on a healthy board. **Retry
before concluding the board is dead.**

**Do not re-derive the screenshot path.** The scanout address is allocated, not
fixed, and `/dev/fb0` is fbdev emulation rather than what Xorg actually paints.
`screenshot.py` handles both.

There is a correct tool for each of these and improvising has repeatedly wasted
whole afternoons:

- **Build**: `./docker/build.sh 'cd /src && $S31_MAKE <target>'`. It defaults to
  the host architecture (arm64 here); an emulated amd64 build is ~10x slower.
  **Never run two builds against the same output volume** - the tree ends up
  unbuildable and the only honest fix is a full rebuild.
- **Flash**: IDF's esptool **5.3.1** at `-b 2000000` on `/dev/cu.usbserial-130`
  (`~/.espressif/python_env/idf6.0_py3.12_env/bin/esptool`). Homebrew's 5.2.0
  has no S31 stub and fails in confusing ways. Build output lives in a Docker
  volume - copy it to `images/` first.
- **Reset**: esptool, never a hand-rolled DTR/RTS toggle. A bad reset makes
  silence non-deterministic and poisons every diagnosis downstream.
- **Run something on the board**: `scratchpad/runsh2.py`, which ships a script
  as a file. Do not flatten multi-line scripts into `; ` one-liners. The login
  prompt races with driver messages, so retry rather than concluding the board
  is dead.
- **Timeouts**: size them to the work (flash ~40 s, `make linux` ~200 s, a board
  script ~60 s). A long default looks like progress while nothing happens.

Pick the **narrowest** build target that reaches the goal. `make linux` alone
for kernel work; the rootfs only needs rebuilding when userspace changes.

## Things that silently do the wrong thing

- **The Makefile overrides the kernel defconfig.** It runs `kconfig-tweak
  --disable PROFILING`, `--disable IPV6` and others after applying the
  defconfig. Setting those in the defconfig does nothing.
- **Partition geometry lives in THREE places** that must agree:
  `bootloader/partitions.csv`, the `*_PARTITION_SIZE` vars in `Makefile`, and
  constants compiled into `bootloader/main/main.c` (**twice**, ~line 71 and
  ~line 346). The loader validates and resets ~350 ms in when they disagree,
  which reads as the loader dying during PSRAM init.
- **The loader app is `hello_world.bin` at 0x20000**, not `bootloader.bin`.
  Those `boot:` log lines come from it. Repartitioning means reflashing all
  three.
- **A DTS edit needs both `make linux` and `make opensbi`** - the kernel uses a
  builtin DTB, so rebuilding only OpenSBI silently leaves the old tree in force.
- **Buildroot ignores unknown defconfig symbols.** Always grep the generated
  `.config` to confirm a package is actually enabled.
- **The SD card holds state the repo does not.** `/etc/init.d/S40xorg` and
  friends have been edited in place. Re-imaging loses it.

## Measuring anything

The single most expensive mistake in this project has been believing a
single-run difference.

- **Establish the noise floor before believing a result.** Repeat the identical
  measurement 5+ times and report the spread. Two separate changes here looked
  like 12-18% wins and were both inside a ±22-40% band.
- **Fresh boot per arm.** Performance decays run-over-run as memory pressure
  builds, so two arms sharing a boot measure the decay.
- **Discard warm-up.** The first runs after boot can read 0.00 fps while
  everything pages in.
- **Set a root cursor** (`xsetroot -cursor_name left_ptr`) before measuring
  anything pointer-related, or the pointer damages nothing and every run reads
  zero - which looks exactly like dead input.
- **Watch the tail, not the median.** The complaint is almost always the worst
  case; medians hide it.

There is **no perf, no ftrace, no PMU** in the shipping kernel. See
`docs/cursor-latency.md` for the instruments that do work: an `LD_PRELOAD`
ioctl timer (`rootfs/ioctlprof.c`), driver-side ktime counters in debugfs, and
a diagnostic kernel with `CONFIG_PROFILING=y` for `/proc/profile`.

## Standing constraints

- **Memory is the binding constraint**, not CPU. 15.4 MB total, of which ~4.3 MB
  is unreclaimable slab. X wants ~4.7 MB and each client costs the server
  another 0.4-2 MB. Clients page out under a full desktop, and that is what
  makes clicks and app launches slow.
- **Userspace binaries belong in XIP flash** - they then cost **zero RSS**
  (`/usr/bin/Xorg` maps 1,832 kB at Rss 0). Running the same binary from SD
  measured 4x worse. Kernel code is the opposite: it already runs from flash,
  where it is ~6x slower than RAM, and `.text.fast` moves it out.
- **Off-the-shelf software only.** Do not patch Xorg, mesa, or mature kernel
  drivers. A new loadable driver of our own is fine; patching theirs is not.
- **Do not disable Bluetooth** (or other features) to buy performance.
- Prefer a **runtime toggle** (module param, config file) over rebuilding once
  per hypothesis - most of the useful comparisons here are `echo x >
  /sys/module/...`.

## Where things are written down

- `docs/current-state.md` - what is true now, and the dead ends
- `docs/cursor-latency.md` - the pointer-latency investigation and open leads
- `docs/accel-plan.md` - which engine moves pixels and why (CPU vs PPA vs GDMA)
- `etc/s31-swap.conf` on the card - swap decisions with their measurements
- `docs/hot-text-plan.md` - the XIP flash penalty and `.text.fast`

When something is measured and rejected, record it **with its numbers** next to
the code it concerns. Several ideas here have been tried twice because the first
rejection was only in a commit message.
