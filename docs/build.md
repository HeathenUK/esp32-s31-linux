# Build System for ESP32-S31 Linux

This project uses a unified `Makefile` at the root directory to manage downloading the toolchain, out-of-tree builds for all components, and flashing the firmware to the board. All build artifacts are cleanly separated into the `build/` directory.

## Building in the container

Builds run inside the CI-equivalent container via `docker/build.sh`, which
bind-mounts the repo at `/src` and keeps the build tree and toolchain on Docker
volumes. Build output is therefore **not visible on the host** - copy artifacts
out explicitly.

```bash
./docker/build.sh                    # make all
./docker/build.sh 'cd /src/build/buildroot && make fltk'
./docker/build.sh bash               # interactive shell
```

### Native versus emulated - the one thing to get right

`PLATFORM` selects the image, and it now **defaults to the host architecture**:
`linux/arm64` on Apple Silicon, `linux/amd64` elsewhere. Set it explicitly only
to cross-check against the other image.

This default used to be `linux/amd64` unconditionally, which on an Apple Silicon
machine silently selected the emulated image. Nothing ever failed - builds just
took minutes where the native image takes about ninety seconds - so there was no
symptom pointing at the cause. If a build feels inexplicably slow, check first:

```bash
docker ps --format '{{.Image}}'      # ...-arm64 is native on Apple Silicon
docker exec <id> uname -m            # aarch64, not x86_64
```

The two platforms use **separate volumes** (`esp32-s31-*` versus
`esp32-s31-arm64-*`). A toolchain built for one host cannot run on the other, so
they are deliberately not shared; switching platforms means rebuilding, not
resuming. Never run two builds against the same volume concurrently - it
corrupts Buildroot's `.cmd` and `.d` files and costs a full rebuild.

### Downloads

`BR2_WGET` carries `--read-timeout=30` as well as `--connect-timeout`. Without
the read timeout a server that accepts the connection and then stops sending
hangs forever: `-t 3` never retries and the mirror fallback never runs. Note
several Buildroot primary URLs use the `http+` prefix, forcing plain HTTP, which
is what stalls; the same files over HTTPS are usually fine. To seed a tarball by
hand, drop it in `buildroot/dl/<pkg>/` and the hash check will pick it up.

## Build Targets

### Default Target
- **`make all`** (or just **`make`**)
  The default target. It downloads and verifies the pinned prebuilt toolchain,
  then executes `download`, `opensbi`, `linux`, and `initramfs`. It never
  builds the compiler from source.

### Download & Toolchain
- **`make download`**
  Updates git submodules recursively. `make all` installs the prebuilt S31
  toolchain before running this target.
- **`make toolchain`**
  Downloads the pinned ESP32-S31 Linux GCC release from
  `GrieferPig/crosstool-NG-s31`, verifies its SHA256 checksum, and installs the
  compiler as
  `toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc`.
- **`make toolchain-source`**
  Uses the sibling `../crosstool-NG` checkout and
  `configs/riscv32-esp-linux-musl.config` to rebuild the same GCC 15.2,
  binutils 2.45, and musl toolchain from source. This target is intended for
  toolchain development; normal project builds should use `make toolchain`.

### Components (Out-of-Tree Builds)
- **`make opensbi`**
  Builds OpenSBI and dynamically compiles the device tree (DTB) from the Linux source. The FW_JUMP binary and DTB are concatenated and padded to match the bootloader's partition size limit. Output is placed in `build/fw_payload.bin`.
- **`make linux`**
  Builds the Linux kernel (`xipImage`) out-of-tree into `build/linux/`. Outputs `xipImage` and the compiled `esp32s31_generic.dtb` directly to the `build/` root.
- **`make rootfs`**
  Uses the pinned Buildroot submodule and the ESP32-S31 br2-external tree to
  build a complete S31-optimized RV32IMAFBC/musl userspace with HWLoop and PIE
  assembler support. It includes BusyBox, BlueZ tools,
  Dropbear, iproute2, tcpdump, memtester, CoreMark, and the project
  diagnostics. The generated SquashFS is copied to `build/rootfs.sqfs` and
  padded to the rootfs partition size. BusyBox `wget` supports HTTPS and
  HTTP-to-HTTPS redirects using its size-optimized internal TLS client; this
  client encrypts transfers but does not validate CA certificates.
- **`make initramfs`**
  Compatibility name for `make rootfs`; this is the preferred rootfs build
  command for this project.
- **`make buildroot-menuconfig`**
  Opens Buildroot configuration using the ESP32-S31 defconfig. Persist useful
  changes by updating `buildroot-external/configs/esp32s31_rootfs_defconfig`.
- **`make buildroot-clean`**
  Removes only the Buildroot output tree. Use it after changing toolchain or
  package selections; downloaded source archives are retained.

### Cleaning
- **`make clean`**
  Removes the `build/` directory and all out-of-tree Linux, OpenSBI, CoreMark,
  and Buildroot artifacts.
- **`make fullclean`**
  Executes the `clean` target and additionally removes the downloaded `toolchain/` directory, reverting the repository to its freshly-cloned state.

### Flashing
*(Note: These targets dynamically parse the partition table (`bootloader/partitions.csv`) to determine the correct offset for flashing via `esptool`.)*
- **`make flash-opensbi`**
  Flashes the OpenSBI payload (`fw_payload.bin`) to the ESP32-S31.
- **`make flash-linux`**
  Flashes the Linux kernel (`xipImage`) to the ESP32-S31.
- **`make flash-rootfs`**
  Flashes `build/rootfs.sqfs` to the ESP32-S31 rootfs partition.
- **`make persist` / `make flash-persist`**
  Builds and flashes an empty 8-KiB-eraseblock JFFS2 image for `persist`.
  This resets overlay upper files and should only be used when initializing
  or intentionally clearing persistent data.
- **`make bootloader`**
  Dynamically searches for your ESP-IDF installation (looking for `export.sh` up to 5 levels deep in your home folder), sources the environment, and invokes `idf.py build` inside the `bootloader/` directory.
- **`make flash-bootloader`**
  Similar to the above, but invokes `idf.py flash -p /dev/ttyUSB0 -b 2000000` to flash the bootloader.
- **`make erase`**
  Completely erases the entire flash using `esptool erase_flash`.

### Quick Start Example
```bash
# 1. Clean up and build everything from scratch
make fullclean
make toolchain
make all

# 2. Source your ESP-IDF environment (required for esptool)
source ~/.espressif/export.sh

# 3. Build and flash the bootloader
make bootloader
make flash-bootloader

# 4. Flash all firmware partitions
make flash-opensbi flash-linux flash-rootfs
```

## Reproducing a flashed kernel byte-for-byte (verified 2026-08-31)

`images/xipImage` (build #66) was reproduced to an identical md5 from the
tree, which is what proves `patches/0021` captures the display-stack source
the board actually runs. Three inputs are not source and must be pinned:

- **The banner.** With no `KBUILD_BUILD_*` set, `init/version.o` carries a
  placeholder banner (`"# \n"`) and the real `#N + date` is linked in via
  `version-timestamp.o`. Setting `KBUILD_BUILD_TIMESTAMP` (or even just
  `KBUILD_BUILD_VERSION`) bakes the string into version.o's rodata instead,
  which grows it and shifts every later address - 712 KB of relocation diffs
  from one env var meant to *help* reproducibility. Leave both unset; pin
  only `KBUILD_BUILD_USER=builder KBUILD_BUILD_HOST=<container id>` (the
  container hostname is random per run), set `.version` to N-1
  (`scripts/build-version` increments before use), and shim `date` on PATH
  to the original link time (`init/Makefile` uses `$(shell LC_ALL=C date)`).
- **The initramfs.** `gen_init_cpio` stamps mtimes with `time()` directly -
  no shim reaches it - and the flashed cpio keeps the date of whatever old
  build last regenerated it. Extract the 512-byte cpio from the flashed
  image (`070701` magic near `__initramfs_start`) and drop it over
  `usr/initramfs_data.cpio`; the generation rule does not re-run when only
  its target changed.
- **The build-id** is a hash of the link inputs and matches by itself once
  the above do.
