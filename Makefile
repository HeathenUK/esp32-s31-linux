# Makefile for ESP32-S31 Linux

TOOLCHAIN_DIR := $(CURDIR)/toolchain
CROSSTOOL_NG_DIR ?= $(abspath $(CURDIR)/../crosstool-NG)
CROSSTOOL_CONFIG := $(CURDIR)/configs/riscv32-esp-linux-musl.config
TOOLCHAIN_PREFIX := $(TOOLCHAIN_DIR)/riscv32-esp-linux-musl
TOOLCHAIN_RELEASE_TAG ?= latest
TOOLCHAIN_RELEASE_ASSET := riscv32-esp-linux-musl.tar.xz
TOOLCHAIN_RELEASE_REPOSITORY ?= GrieferPig/crosstool-NG-s31
TOOLCHAIN_RELEASE_API ?= https://api.github.com/repos/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/latest
TOOLCHAIN_RELEASE_DOWNLOAD_BASE ?= https://github.com/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/download
CROSS_COMPILE := $(TOOLCHAIN_PREFIX)/bin/riscv32-esp-linux-musl-
CC := $(CROSS_COMPILE)gcc
CPP := $(CROSS_COMPILE)cpp
DTC := dtc
JOBS ?= $(shell nproc)

# S31 supports F and the stateful Espressif HWLoop/PIE extensions, but firmware
# and kernel C code must not borrow task coprocessor state.  Use every safe
# integer code-generation extension there.
S31_SAFE_ISA := rv32imabc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
# Userspace keeps F and PIE, but NOT xesploop. Linux does save the hardware-loop
# CSRs across a context switch, yet they are M-mode only, so any trap that
# reaches hart0 - which saves coprocessor state per trap, not per task - can
# return with a loop counter that is no longer the one the thread set up. The
# symptom is a long loop silently computing the wrong answer perhaps 0.5% of the
# time, which is how md5sum came to report false SD corruption. Keep this in
# step with BR2_RISCV_ISA_EXTRA and BR2_TARGET_OPTIMIZATION.
S31_USER_ISA := rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
S31_COMMON_FLAGS := -mabi=ilp32 -mtune=esp-base
S31_USER_FLAGS := -march=$(S31_USER_ISA) $(S31_COMMON_FLAGS)

BUILD_DIR := $(CURDIR)/build
OPENSBI_DIR := $(CURDIR)/opensbi-esp32-s31
# The 7.1 port is the kernel this board runs.  linux-esp32-s31/ is the old 6.12
# tree, kept only for reference; building it against the shared output volume
# silently relinks 6.12 objects into a 7.1-named image.
LINUX_DIR := $(CURDIR)/linux-71-port
BUILDROOT_DIR := $(CURDIR)/buildroot
BUILDROOT_EXTERNAL := $(CURDIR)/buildroot-external

# Out-of-tree build dirs
OPENSBI_OUT := $(BUILD_DIR)/opensbi
LINUX_OUT := $(BUILD_DIR)/linux
BUILDROOT_OUT := $(BUILD_DIR)/buildroot
BUILDROOT_DL_DIR := $(BUILD_DIR)/buildroot-dl
TOOLCHAIN_ARCHIVE := $(BUILD_DIR)/downloads/$(TOOLCHAIN_RELEASE_ASSET)

PARTITIONS_CSV := $(CURDIR)/bootloader/partitions.csv
OPENSBI_OFFSET := $(shell awk -F, '/opensbi/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
LINUX_OFFSET := $(shell awk -F, '/linux/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
ROOTFS_OFFSET := $(shell awk -F, '/rootfs/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
XIP2_OFFSET := $(shell awk -F, '/xip2/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))

FW_PAYLOAD := $(BUILD_DIR)/fw_payload.bin
XIP_IMAGE := $(BUILD_DIR)/xipImage
ROOTFS_IMG := $(BUILD_DIR)/rootfs.sqfs
XIP2_ROOTFS_IMG := $(BUILD_DIR)/rootfs-xip2.cramfs

# The S31-capable ESP-IDF lives at /opt/esp-idf in the build container, which is
# outside $HOME and so invisible to the search below. Check it first: searching
# $HOME on a developer machine tends to turn up several unrelated ESP-IDF
# checkouts, and the first one found is usually not the one that knows about
# esp32s31 - it fails late, at "toolchain-esp32s31.cmake not found".
IDF_EXPORT := $(shell test -f /opt/esp-idf/export.sh && echo /opt/esp-idf/export.sh || \
	find $(HOME) -maxdepth 5 -type f -name export.sh 2>/dev/null | grep esp-idf | head -n 1)

.PHONY: all download toolchain toolchain-source opensbi linux coremark rootfs initramfs s31-pie-cases \
	buildroot-menuconfig buildroot-clean clean fullclean flash-opensbi flash-linux  \
	xip-rootfs flash-xip-rootfs xip2-stage xip2-image \
	flash-rootfs xip2-rootfs flash-xip2-rootfs xip-fast xip-image bootloader flash-bootloader erase \
	imager flash-imager reset

all: toolchain download opensbi linux initramfs

$(BUILD_DIR) $(OPENSBI_OUT) $(LINUX_OUT) $(BUILDROOT_OUT):
	mkdir -p $@

download: toolchain
	@echo "--- Download ---"
	git submodule update --init --recursive

toolchain: | $(BUILD_DIR)
	@set -eu; \
	if [ -x "$(CC)" ] && { [ -f "$(TOOLCHAIN_PREFIX)/.source-build" ] || [ ! -f "$(TOOLCHAIN_PREFIX)/.release" ]; }; then \
		echo "Using locally built toolchain at $(TOOLCHAIN_PREFIX)"; \
		exit 0; \
	fi; \
	mkdir -p "$(dir $(TOOLCHAIN_ARCHIVE))" "$(TOOLCHAIN_DIR)"; \
	release_tag="$(TOOLCHAIN_RELEASE_TAG)"; \
	if [ "$$release_tag" = latest ]; then \
		release_tag=$$(curl --fail --location --retry 3 --silent --show-error "$(TOOLCHAIN_RELEASE_API)" | sed -n 's/^[[:space:]]*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p'); \
	fi; \
	if [ -z "$$release_tag" ]; then \
		echo "ERROR: failed to resolve the latest toolchain release tag" >&2; exit 1; \
	fi; \
	release_url="$(TOOLCHAIN_RELEASE_DOWNLOAD_BASE)/$$release_tag/$(TOOLCHAIN_RELEASE_ASSET)"; \
	release_sha256_url="$$release_url.sha256"; \
	installed_tag=$$(cat "$(TOOLCHAIN_PREFIX)/.release" 2>/dev/null || true); \
	if [ -z "$$installed_tag" ] && [ -d "$(TOOLCHAIN_PREFIX)" ]; then \
		installed_tag=$$(find "$(TOOLCHAIN_PREFIX)" -maxdepth 1 -type f -name '.release-*' -printf '%f\n' 2>/dev/null | sed 's/^\.release-//' | head -n 1); \
	fi; \
	if [ "$$installed_tag" = "$$release_tag" ]; then \
		echo "Toolchain release $$release_tag is already installed"; \
		exit 0; \
	fi; \
	echo "Installing toolchain release $$release_tag"; \
	curl --fail --location --retry 3 --output "$(TOOLCHAIN_ARCHIVE).part" "$$release_url"; \
	curl --fail --location --retry 3 --output "$(TOOLCHAIN_ARCHIVE).sha256.part" "$$release_sha256_url"; \
	expected_hash=$$(awk 'NR == 1 { print $$1; exit }' "$(TOOLCHAIN_ARCHIVE).sha256.part"); \
	printf '%s\n' "$$expected_hash" | grep -Eq '^[0-9a-fA-F]{64}$$' || { echo "ERROR: invalid release checksum" >&2; exit 1; }; \
	printf '%s  %s\n' "$$expected_hash" "$(TOOLCHAIN_ARCHIVE).part" | sha256sum --check -; \
	mv "$(TOOLCHAIN_ARCHIVE).part" "$(TOOLCHAIN_ARCHIVE)"; \
	rm -f "$(TOOLCHAIN_ARCHIVE).sha256.part"; \
	staging=$$(mktemp -d "$(TOOLCHAIN_DIR)/.riscv32-esp-linux-musl.XXXXXX"); \
	trap 'chmod -R u+w "$$staging" 2>/dev/null || true; rm -rf "$$staging"' EXIT; \
	tar -xJf "$(TOOLCHAIN_ARCHIVE)" -C "$$staging"; \
	test -x "$$staging/bin/riscv32-esp-linux-musl-gcc"; \
	printf '%s\n' "$$release_tag" > "$$staging/.release"; \
	printf '%s\n' "$$release_tag" > "$$staging/.release-$$release_tag"; \
	chmod u-w "$$staging"; \
	if [ -e "$(TOOLCHAIN_PREFIX)" ]; then \
		backup="$(TOOLCHAIN_PREFIX).previous.$$(date -u +%Y%m%d%H%M%S)"; \
		mv "$(TOOLCHAIN_PREFIX)" "$$backup"; \
		echo "Previous toolchain retained at $$backup"; \
	fi; \
	mv "$$staging" "$(TOOLCHAIN_PREFIX)"; \
	trap - EXIT; \
	"$(CC)" --version | head -n 1

toolchain-source:
	python3 $(CURDIR)/build_linux_toolchain.py --ct-ng-dir "$(CROSSTOOL_NG_DIR)" --jobs "$(JOBS)" --force

FW_TEXT_START ?= 0x40380000
FW_RW_START ?= 0x50FF0000
# SV32 XIP uses a 4-MiB leaf/megapage boundary.
LINUX_XIP_ADDR ?= 0x40400000
FW_JUMP_ADDR ?= $(LINUX_XIP_ADDR)
OPENSBI_PARTITION_SIZE ?= 524288

FDT_SRC := $(LINUX_DIR)/arch/riscv/boot/dts/espressif/esp32s31_generic.dts
FDT_DTB := $(BUILD_DIR)/esp32s31_generic.dtb
OPENSBI_FW_JUMP_BIN := $(OPENSBI_OUT)/platform/generic/firmware/fw_jump.bin

opensbi: toolchain | $(OPENSBI_OUT)
	@echo "--- OpenSBI ---"
	$(CPP) -x assembler-with-cpp -nostdinc -undef -D__DTS__ \
		-I $(dir $(FDT_SRC)) \
		-I $(LINUX_DIR)/include \
		-I $(LINUX_DIR)/arch/riscv/boot/dts \
		$(FDT_SRC) | $(DTC) -@ -O dtb -i $(dir $(FDT_SRC)) -o $(FDT_DTB)
	$(MAKE) -C $(OPENSBI_DIR) O=$(OPENSBI_OUT) \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		PLATFORM=generic \
		PLATFORM_RISCV_XLEN=32 \
		PLATFORM_RISCV_ISA=$(S31_SAFE_ISA) \
		FW_TEXT_START=$(FW_TEXT_START) \
		FW_RW_START=$(FW_RW_START) \
		FW_JUMP=y \
		FW_JUMP_FDT_OFFSET= \
		FW_JUMP_ADDR=$(FW_JUMP_ADDR) \
		-j$(JOBS)
	@cp $(OPENSBI_FW_JUMP_BIN) $(BUILD_DIR)/staged_fw_jump.bin
	@truncate -s 262144 $(BUILD_DIR)/staged_fw_jump.bin
	@FDT_OFFSET=262144; \
	cat $(BUILD_DIR)/staged_fw_jump.bin $(FDT_DTB) > $(FW_PAYLOAD); \
	PAYLOAD_SIZE=$$(stat -c%s $(FW_PAYLOAD)); \
	MAX_PAYLOAD_SIZE=$$(( $(OPENSBI_PARTITION_SIZE) - 4 )); \
	if [ $$PAYLOAD_SIZE -gt $$MAX_PAYLOAD_SIZE ]; then echo "ERROR: Payload exceeds limit"; exit 1; fi; \
	python3 -c "import sys, struct; sys.stdout.buffer.write(struct.pack('<I', $$FDT_OFFSET))" > $(BUILD_DIR)/offset.bin; \
	truncate -s $$MAX_PAYLOAD_SIZE $(FW_PAYLOAD); \
	cat $(BUILD_DIR)/offset.bin >> $(FW_PAYLOAD); \
	rm -f $(BUILD_DIR)/staged_fw_jump.bin $(BUILD_DIR)/offset.bin

DEFCONFIG ?= esp32s31_defconfig

# Diagnostic surfaces, on by default.
#
# DIAG=0 builds the shipping kernel with debugfs compiled out. Measured on this
# board, debugfs_inode_cache alone is 311 kB and it backs a large share of the
# 9,108 kernfs nodes (783 kB) - see docs/where-the-ram-goes.md. The cost of
# turning it off is real: the LCD driver's counters, the PPA registers and
# deskbench's scanout discovery all live under /sys/kernel/debug. Same trade
# already made for CONFIG_PROFILING - develop with it, ship without it.
# Profiling kernel: `make linux PROF=1`.
#
# /proc/profile needs CONFIG_PROFILING, which *selects* PERF_EVENTS - together
# ~541 KB - and the linux partition has ~53 KB spare, so the radios and sound
# come out to make room. That is a deliberate, reversible trade and it is the
# only way to profile this kernel: there is no perf, no ftrace and no PMU.
#
# It is a switch rather than a hand-edit because the documented procedure used
# to be "delete the --disable PROFILING line and keep a backup of the Makefile
# in a scratchpad", which is one forgotten step away from shipping a kernel
# with no Bluetooth and no sound. PROF defaults to 0; a shippable kernel is
# what you get unless you ask otherwise.
#
# **This switch does NOT touch the radios or sound**, deliberately. Turning
# features off to make room is a decision for whoever owns the board, not a
# silent side effect of asking for a profile: it has to be requested
# explicitly, on the command line, and put back afterwards. If the image no
# longer fits, the size check below fails loudly - which is the correct
# outcome, because it surfaces the trade instead of making it.
#
# Clearing PROFILING alone does NOT clear PERF_EVENTS: PROFILING selects it,
# and olddefconfig keeps it because it is user-selectable in its own right.
# Both have to be named, in both directions.
# Tick rate: `make linux KHZ=250`.
#
# A switch because the tree and the notes disagree. docs/hot-text-plan.md says
# HZ 250 -> 100 was tried, "made SD worse (12.07 -> 16.93 ms per request)" and
# was reverted - yet the build has been forcing HZ=100 ever since. Whichever is
# right, it should be one flag to test rather than an edit.
#
# It matters more than a tick rate usually would, because this kernel is
# PREEMPT_NONE: a woken kthread or kworker cannot preempt a running task and
# waits for a scheduling point, so a jiffy is the granularity of every thread
# hand-off. At HZ=100 that is 10 ms.
KHZ ?= 100
ifeq ($(KHZ),250)
HZ_TWEAKS := --enable HZ_250 --disable HZ_100 --set-val HZ 250
else
HZ_TWEAKS := --disable HZ_250 --enable HZ_100 --set-val HZ 100
endif

PROF ?= 0
ifeq ($(PROF),1)
PROF_TWEAKS := --enable PROFILING --enable PERF_EVENTS
else
PROF_TWEAKS := --disable PROFILING --disable PERF_EVENTS
endif

# debugfs OFF by default. It is not mounted at runtime anyway, but compiling
# it in costs 1.14 MB of RAM on a 15.4 MB machine - measured 2026-09-02,
# MemFree 1892 kB with it against 3032 kB without, Slab 4260 against 3840 -
# and 232 KB of the linux partition. Build with DIAG=1 when a driver's
# debugfs knobs are actually needed; screenshot.py does not need it.
DIAG ?= 0
ifeq ($(DIAG),0)
DIAG_TWEAKS := --disable DEBUG_FS
else
DIAG_TWEAKS := --enable DEBUG_FS
endif
LINUX_TARGET ?= xipImage

# An oversized kernel is fatal, because it silently runs past its partition into
# rootfs. The SD imager is the one deliberate exception: it is flashed over the
# normal kernel only for as long as it takes to write the card, and the restore
# step reflashes rootfs anyway. See docs/sd-imager.md.
LINUX_SIZE_FATAL ?= 1

linux: toolchain | $(LINUX_OUT)
	@echo "--- Linux ---"
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" $(DEFCONFIG)
	$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
		--set-str BUILTIN_DTB_SOURCE "espressif/esp32s31_generic" \
		--set-str BUILTIN_DTB_NAME "espressif/esp32s31_generic" \
		--enable RISCV_ISA_C \
		--enable PROFILING \
		$(HZ_TWEAKS) \
		--disable RISCV_ISA_V \
		--disable RISCV_ISA_V_DEFAULT_ENABLE \
		--enable RISCV_ISA_ZBA \
		--enable RISCV_ISA_ZBB \
		--enable RISCV_ISA_ZBC \
		--enable DRM \
		--enable DRM_ESP32S31_LCD \
		--enable BACKLIGHT_CLASS_DEVICE \
		--enable DRM_PANEL \
		--enable DRM_PANEL_SIMPLE \
		--enable VT \
		--enable VT_CONSOLE \
		--enable FB \
		--enable FRAMEBUFFER_CONSOLE \
		--enable INPUT \
		--enable INPUT_EVDEV \
		--enable INPUT_KEYBOARD \
		--enable CFG80211 \
		--disable CFG80211_WEXT \
		--disable MAC80211 \
		--enable CFG80211_CERTIFICATION_ONUS \
		--disable CFG80211_REQUIRE_SIGNED_REGDB \
		--disable CFG80211_USE_KERNEL_REGDB_KEYS \
		--disable CFG80211_CRDA_SUPPORT \
		--enable HID \
		--enable HID_GENERIC \
		--enable USB_HID \
		--enable HIDRAW \
		--enable DEBUG_FS \
		--enable USB_MON \
		$(DIAG_TWEAKS) \
		--set-val LOG_BUF_SHIFT 14 \
		--enable CMA \
		--enable DMA_CMA \
		--set-val CMA_SIZE_MBYTES 0 \
		--disable DYNAMIC_DEBUG \
		--disable USB_DWC2_DEBUG \
		--disable USB_DWC2_DEBUG_PERIODIC \
		--enable HID_SUPPORT \
		--enable REGULATOR \
		--enable REGULATOR_FIXED_VOLTAGE \
		--enable DRM_CLIENT_SELECTION \
		--enable DRM_FBDEV_EMULATION \
		--enable FRAMEBUFFER_CONSOLE \
		--disable DRM_DEBUG_MODESET_LOCK \
		--enable INPUT_MISC \
		--enable INPUT_UINPUT \
		--enable INPUT_TOUCHSCREEN \
		--enable TOUCHSCREEN_GT1158_POLLED \
		--enable BT_HIDP \
		--enable HIGH_RES_TIMERS \
		--enable NO_HZ_IDLE \
		--enable FILE_LOCKING \
		--enable CRAMFS \
		--enable CRAMFS_MTD \
		--disable CRAMFS_BLOCKDEV \
		--enable DRM_ESP32S31_PPA \
		--disable FTRACE \
		--disable ENABLE_DEFAULT_TRACERS \
		--disable BLK_DEV_IO_TRACE \
		$(PROF_TWEAKS) \
		--disable BPF_SYSCALL \
		--disable BPF_JIT \
		--disable PREEMPT_LAZY \
		--disable PREEMPT \
		--disable PREEMPT_VOLUNTARY \
		--enable PREEMPT_NONE \
		--enable DRM_FBDEV_EMULATION \
		--disable IPV6
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" olddefconfig
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" \
		KCFLAGS="-march=$(S31_SAFE_ISA) $(S31_COMMON_FLAGS)" -j$(JOBS) $(LINUX_TARGET) dtbs
	cp -v $(LINUX_OUT)/arch/riscv/boot/$(LINUX_TARGET) $(XIP_IMAGE)
	cp -v $(LINUX_OUT)/arch/riscv/boot/dts/espressif/esp32s31_generic.dtb $(FDT_DTB)
	cp -v $(LINUX_OUT)/System.map $(BUILD_DIR)/System.map
	@XIP_SIZE=$$(stat -c%s $(XIP_IMAGE)); \
	if [ $$XIP_SIZE -gt $(LINUX_PARTITION_SIZE) ]; then \
		OVER=$$(($$XIP_SIZE - $(LINUX_PARTITION_SIZE))); \
		if [ "$(LINUX_SIZE_FATAL)" = "0" ]; then \
			echo "WARNING: $(notdir $(XIP_IMAGE)) ($$XIP_SIZE bytes) exceeds the linux partition by $$OVER bytes."; \
			echo "         Flashing it will overwrite the first $$OVER bytes of the rootfs"; \
			echo "         partition, which holds XIP image 1. Reflash it afterwards:"; \
			echo "             make flash-linux flash-xip-rootfs"; \
		else \
			echo "ERROR: xipImage ($$XIP_SIZE bytes) exceeds the linux partition ($(LINUX_PARTITION_SIZE) bytes)"; \
			exit 1; \
		fi; \
	else \
		echo "$(notdir $(XIP_IMAGE)) $$XIP_SIZE bytes, $$(($(LINUX_PARTITION_SIZE) - $$XIP_SIZE)) bytes free in the linux partition"; \
	fi

coremark: rootfs
	@test -x "$(BUILDROOT_OUT)/target/usr/bin/coremark"
	@echo "Buildroot CoreMark: $(BUILDROOT_OUT)/target/usr/bin/coremark"

# Keep this decimal because POSIX test(1) and truncate(1) do not accept the
# partition table's 0x-prefixed value.
ROOTFS_PARTITION_SIZE ?= 6422528
XIP2_PARTITION_SIZE ?= 1507328
# The XIP kernel must start on a 4-MiB Sv32 megapage boundary, so the linux
# partition stays at 0x400000 and rootfs takes every byte the kernel does not
# need. Keep this in step with bootloader/partitions.csv.
# 256 KB moved from linux to rootfs, 2026-09-02. The kernel had permanent
# slack once debugfs was compiled out (5,894,105 bytes in a 6,422,528 byte
# partition) and the XIP userspace image had 8 KB, which is not enough to
# add anything. Geometry lives in THREE places and all three are changed
# together: this file, bootloader/partitions.csv, and the constants in
# bootloader/main/main.c - which appear TWICE there.
LINUX_PARTITION_SIZE ?= 6160384

# Flashing knobs. These were hardcoded to /dev/ttyUSB0 and a bare `esptool`,
# which is a Linux-only assumption: on macOS the adapter is /dev/cu.usbserial-*
# and a bare `esptool` picks up whatever is on PATH. Homebrew's 5.2.0 fails on
# this part in ways that do not name the cause, so point at the IDF one.
# Override either on the command line.
SERIAL_PORT ?= $(firstword $(wildcard /dev/cu.usbserial-* /dev/ttyUSB0))
ESPTOOL ?= $(firstword $(wildcard $(HOME)/.espressif/python_env/idf6*/bin/esptool) esptool)
ESPTOOL_BAUD ?= 2000000
ESPFLASH = $(ESPTOOL) -p $(SERIAL_PORT) -b $(ESPTOOL_BAUD) write-flash

# Where a flashable artifact actually is. Builds run in a container and the
# build tree is a Docker volume, so on the host $(BUILD_DIR) is empty and the
# artifacts live in images/ after the documented copy-out step (see
# docker/build.sh). Prefer the build tree when it is populated - that is the
# in-container case, and during a build - and fall back to images/ otherwise,
# so `make flash-*` works from the host without overriding paths by hand.
# Deliberately applied only to the flash rules: applying it to the build rules
# would let a target be written to images/ when its build-tree copy is merely
# not created yet.
flashfile = $(if $(wildcard $(1)),$(1),$(CURDIR)/images/$(notdir $(1)))
BUILDROOT_MAKE = $(MAKE) -C $(BUILDROOT_DIR) O=$(BUILDROOT_OUT) \
	BR2_EXTERNAL=$(BUILDROOT_EXTERNAL) BR2_DL_DIR=$(BUILDROOT_DL_DIR)

s31-pie-cases:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	bash -c "source $(IDF_EXPORT) >/dev/null && $(CURDIR)/rootfs/gen_s31_pie_cases.sh $(CURDIR)/rootfs/s31_pie_cases.inc"

rootfs: toolchain s31-pie-cases | $(BUILDROOT_OUT)
	@echo "--- Buildroot rootfs ---"
	$(BUILDROOT_MAKE) esp32s31_rootfs_defconfig
	$(BUILDROOT_MAKE) toolchain-external-custom-rebuild
	$(BUILDROOT_MAKE) toolchain-external-rebuild
	$(BUILDROOT_MAKE) s31-tools-rebuild
	$(BUILDROOT_MAKE)
	cp -v $(BUILDROOT_OUT)/images/rootfs.squashfs $(ROOTFS_IMG)
	@# The root filesystem lives on the microSD card (rootfs.ext2, written with
	@# imager/send_image.py). The flash `rootfs` partition belongs to the
	@# userspace XIP image - see the xip-rootfs target - so the squashfs is not
	@# sized against it and must not be flashed there. It is kept only as a
	@# self-contained image for bring-up without a card.
	@echo "SD image:   $(BUILDROOT_OUT)/images/rootfs.ext2"
	@echo "squashfs:   $(ROOTFS_IMG) ($$(stat -c%s $(ROOTFS_IMG)) bytes, not flashed)"

# Userspace XIP image: the compositor's dependency closure, executed in place
# from the always-mapped flash window instead of faulting off the SD card.
#
# Two things this must get right, both of which were wrong when the image was
# built by hand and cost most of a session to find:
#
#   * Paths are preserved, not flattened. The runtime overlays this image over
#     /usr/lib, /usr/bin and /usr/libexec, so an object staged anywhere else is
#     dead weight - present in flash at a path nothing loads.
#   * dlopen()ed plugins are named explicitly. Weston loads its backend and
#     shell by absolute path, so a DT_NEEDED walk alone misses drm-backend.so
#     and desktop-shell.so, which are exactly the objects that run every frame.
XIP_STAGE := $(BUILD_DIR)/xipstage
XIP_ROOTFS_IMG := $(BUILD_DIR)/rootfs-xip.cramfs
#
# Deliberately narrow. An XIP cramfs cannot compress text - it has to be
# executable in place - so the closure has to fit the rootfs partition
# uncompressed.
#
# These are the X11 desktop's hot binaries; mkxipstage.py resolves each one's
# shared-library closure, so listing the executables is enough. Only text
# benefits, which is why fonts and the xkb rule files are not here: they are
# data, read once at startup, and belong on the card.
#
# Weston's roots are kept below rather than deleted, because Xfbdev reaching the
# DRM plane update path through fbdev emulation is still unconfirmed. To fall
# back, override on the command line rather than editing:
#
#   make xip-rootfs XIP_ROOTS='usr/bin/weston usr/lib/libweston-15/*.so \
#       usr/lib/weston/desktop-shell.so usr/libexec/weston-desktop-shell'
# xterm is deliberately NOT here. Measured, its exclusive closure - the binary
# plus libXt, libXaw7, libXpm and libncursesw, none of which anything else here
# needs - is 1,731,680 bytes, and staging it pushed this image to 6,889,472
# against a 6,291,456 partition. The server is the right thing to spend flash
# on: it is shared by every client, whereas xterm is one client among several.
#
# libXft is a root in its own right, not because the server needs it - Xfbdev
# does not - but because it is the shared half of text rendering. Its closure
# is libXft, libfontconfig and libexpat, 499,264 bytes, and every client that
# draws antialiased text goes through it: xterm and every FLTK application
# alike. Staging it beside FLTK instead would put it in the smaller image and
# duplicate nothing, but leave the shared stack in the partition with the least
# room. Measured: this is what makes both images fit.
# Text mode: the desktop is put aside, so the XIP image holds what a text
# system actually runs. This dropped the image from 6,926,336 to 3,596,288
# bytes, and that 3.3 MB is what let the kernel partition grow enough for 6.18.
#
# To go back to X, use the desktop set below - it is kept because it was tuned
# by measurement, not guesswork (see the note above about which binaries go in
# which image, and why libXft belongs here).
XIP_ROOTS ?= bin/busybox usr/sbin/wpa_supplicant usr/sbin/iw usr/bin/lvdesk \
	usr/bin/s31-a2dp \
	usr/libexec/bluetooth/bluetoothd \
	usr/bin/xfilesctl usr/bin/s31-open usr/bin/s31-thumb usr/bin/xfilesthumb usr/bin/s31-thumbs

# In the closure but deliberately left on the card. NEEDED is not the same as
# hot: lvdesk links libasound for the volume mixer and occasional PCM writes,
# which happen when a human moves a slider. 943,548 bytes of flash for that,
# against bluetoothd which never exits, is the wrong trade - and without this
# the two images total 8,205,052 against 7,602,176 of partition.
# libblkid is 329,436 bytes and udevd only touches it during coldplug, to probe
# filesystems for /dev/disk/by-uuid links nothing here uses. Those are clean
# file pages, so once boot is over the kernel can drop them and the steady-state
# cost of leaving it on the card is nothing - whereas in XIP it permanently
# occupied a third of image 2. Trading it for the X client chain is trading
# boot-time-only pages for pages that are resident the whole session.
# pcre2 is glib's regex engine and nothing on this system runs a regex
# through glib; it left XIP when bluetoothd grew 427 KB of classic-BT
# profiles (hid/hog/audio/avrcp/client support). From the SD lower layer
# its pages are simply never faulted.
# `iw` goes to the SD layer, not bluetoothd. Evicting bluetoothd to pay for
# the static busybox was a bad trade and is reverted: it put the daemon on
# the card, where an Aug-23 copy WITHOUT the a2dp plugin was still sitting,
# and A2DP broke in a way that looked exactly like the old LE-only plugin
# bug. Anything that runs wants to be in XIP; `iw` is a hand-run CLI that
# nothing on the boot path touches, so its pages are the ones to give up.
XIP_SKIP ?= libasound.so.2.0.0 libblkid.so.1.1.0 libpcre2-8.so.0.15.0 iw

# XIP_ROOTS_DESKTOP was defined here and referenced NOWHERE - dead since the
# desktop was set aside for text mode, so anything listed in it was silently
# not staged. lvdesk goes in XIP_ROOTS above, which is the variable the rule
# actually reads. Check `grep -c` on a make variable before trusting it.
#
# The whole X stack used to be listed here - the server, its modesetting and
# evdev modules, libXft, st, xsetroot and a dozen PCF fonts. It is one binary
# now. lvdesk statically carries LVGL and its fonts, so its exclusive closure
# is the binary plus libc, and putting it in flash takes its ~600 kB of text
# out of RSS entirely - the same reason /usr/bin/Xorg used to live here.

xip-rootfs: rootfs xip-image

# Re-stage and re-pack the XIP image WITHOUT re-running Buildroot.
#
# Buildroot is the whole cost of xip-rootfs: measured 2m33s against 1.6s to
# copy the result out and 42s to flash it, all to ship one changed binary. When
# only an overlay file has changed - which is every lvdesk iteration - sync the
# overlay into the target tree and repack. Use xip-rootfs when a *package*
# changed; this is for iterating on our own binaries.
# Install our X11 replacement libraries into the Buildroot OVERLAY, which is
# what makes them survive a target rebuild - and what makes the XIP closure
# small enough to pack at all. Staged from the stock libraries, xcalc's closure
# is 2.51 MB against a 1.41 MB partition, because the stock libX11 alone is
# 1.3 MB and drags in libxcb, libXau and libXdmcp behind it. Ours is 108 kB and
# needs none of them.
#
#   xlite    libX11                       docs/xlite.md
#   xtlite   libXt, libXaw7, libXmu       docs/xtlite.md
#   xstubs   libICE, libSM, libXext, libXpm
#
# Build them first with docker/build.sh on xlite/build.sh, xtlite/build.sh and
# xstubs/build.sh; this only installs what is in images/.
# libXrender and libXft joined this list later than the rest: xrlite and
# xftlite replace them, and leaving them out meant x11-stage silently shipped
# the STOCK libraries into XIP while the replacements existed only in the
# /root development tree - so the board ran one set and the images held another.
X11_REPLACEMENTS := libX11.so.6.4.0 libXt.so.6.0.0 libXaw7.so.7.0.0 \
	libXmu.so.6.2.0 libICE.so.6.3.0 libSM.so.6.0.1 libXext.so.6.4.0 \
	libXpm.so.4.11.0 libXrender.so.1.3.0 libXft.so.2.3.9 \
	libfontconfig.so.1.16.0 libXcursor.so.1.0.2

x11-stage:
	@echo "--- installing the X11 replacements into the overlay ---"
	@mkdir -p $(BUILDROOT_EXTERNAL)/board/esp32-s31/overlay/usr/lib
	@for f in $(X11_REPLACEMENTS); do \
		test -f images/$$f || { echo "ERROR: images/$$f is missing - build it first" >&2; exit 1; }; \
		cp -a images/$$f $(BUILDROOT_EXTERNAL)/board/esp32-s31/overlay/usr/lib/$$f; \
		printf "  %-24s %7d bytes\n" $$f $$(stat -f%z images/$$f 2>/dev/null || stat -c%s images/$$f); \
	done

xip-fast:
	@echo "--- syncing overlay into the Buildroot target ---"
	cp -a $(BUILDROOT_EXTERNAL)/board/esp32-s31/overlay/. $(BUILDROOT_OUT)/target/
	@$(MAKE) xip-image
	@$(MAKE) xip2-image

xip-image:
	@echo "--- userspace XIP image ---"
	@command -v mkcramfs >/dev/null || test -x $(BUILDROOT_OUT)/host/bin/mkcramfs || \
		{ echo "ERROR: mkcramfs not found; enable BR2_PACKAGE_HOST_CRAMFS" >&2; exit 1; }
	rm -rf $(XIP_STAGE)
	mkdir -p $(XIP_STAGE)
	@# Image 1 is staged first and image 2 excludes it, so the shared
	@# libraries land here once. The split between the two is therefore
	@# about which BINARIES go where, not which libraries: moving a binary
	@# to image 2 moves only the binary, because its libraries are already
	@# here. That is how the two partitions get balanced.
	XIP_SKIP="$(XIP_SKIP)" python3 $(CURDIR)/rootfs/mkxipstage.py $(CROSS_COMPILE)readelf \
		$(BUILDROOT_OUT)/target $(XIP_STAGE) $(XIP_ROOTS)
	@# Core-font clients need a fonts.dir index; Xft ones do not. The font
	@# files copy across on their own, so the server and st work and the
	@# omission is invisible until an Athena application starts and reports
	@# "Unable to load any usable ISO8859 font". Build an index describing
	@# exactly what was staged - the card's own fonts.dir lists 334 faces
	@# that are not here, and the flash copy replaces it via a bind mount.
	@FD=$(XIP_STAGE)/usr/share/fonts/X11/misc; \
	if [ -d "$$FD" ]; then \
		SRC=$(BUILDROOT_OUT)/target/usr/share/fonts/X11/misc/fonts.dir; \
		( cd "$$FD" && ls *.pcf.gz 2>/dev/null | wc -l; \
		  for f in "$$FD"/*.pcf.gz; do \
			grep "^$$(basename $$f) " "$$SRC" 2>/dev/null; \
		  done ) > "$$FD/fonts.dir"; \
		echo "staged fonts.dir: $$(head -1 $$FD/fonts.dir) faces"; \
		{ echo '-adobe-symbol-medium-r-normal--13-120-75-75-p-74-adobe-fontspecific 6x13'; \
		  echo '8x13bold "-misc-fixed-bold-r-normal--13-120-75-75-c-80-iso8859-1"'; \
		} >> "$$FD/fonts.alias"; \
	fi
	@# A rescue root, so a board with no usable microSD still boots.
	@#
	@# The image already carries /bin/busybox and its library closure, so
	@# making it bootable costs an /init and five directories. The kernel
	@# now mounts THIS as root and the script below hands over to the card
	@# when there is one - which is the only way to choose at runtime
	@# without an initramfs, and an initramfs does not fit: the linux
	@# partition has ~385 KB spare and a busybox one is several times that.
	@mkdir -p $(XIP_STAGE)/proc $(XIP_STAGE)/sys $(XIP_STAGE)/dev \
		 $(XIP_STAGE)/tmp $(XIP_STAGE)/mnt/sd $(XIP_STAGE)/etc \
		 $(XIP_STAGE)/lib
	@# chroot, not switch_root: busybox's switch_root refuses to run unless
	@# the current root is ramfs/tmpfs, because it is written for an
	@# initramfs that it can delete on the way out. This root is cramfs in
	@# flash and stays where it is, so the mounts are moved across and the
	@# card is entered with chroot - exec keeps PID 1.
	@#
	@# The ELF interpreter, which the closure walker does not include -
	@# it follows NEEDED entries, and the interpreter is not one. Without
	@# it every binary here fails to exec with ENOENT, which reads as "the
	@# file is missing" when the file is plainly there. That is what made
	@# the first rescue root panic with "Requested init /init failed (-2)".
	@ln -sf ../usr/lib/libc.so \
		$(XIP_STAGE)/lib/ld-musl-riscv32-sf.so.1
	@printf '%s\n' \
	  '#!/bin/busybox sh' \
	  '# Flash rescue root. Hands over to the microSD when it appears.' \
	  'B=/bin/busybox' \
	  '$$B mount -t proc proc /proc 2>/dev/null' \
	  '$$B mount -t sysfs sys /sys 2>/dev/null' \
	  '$$B mount -t devtmpfs dev /dev 2>/dev/null' \
	  'i=0' \
	  'while [ $$i -lt 20 ]; do' \
	  '        [ -b /dev/mmcblk0 ] && break' \
	  '        $$B sleep 1; i=$$(($$i + 1))' \
	  'done' \
	  'if [ -b /dev/mmcblk0 ] && $$B mount -t ext4 /dev/mmcblk0 /mnt/sd; then' \
	  '        $$B echo "root: microSD after $${i}s"' \
	  '        for d in proc sys dev; do' \
	  '                $$B mount --move /$$d /mnt/sd/$$d 2>/dev/null' \
	  '        done' \
	  '        exec $$B chroot /mnt/sd /init' \
	  'fi' \
	  '$$B echo "root: NO microSD after $${i}s - flash rescue root"' \
	  '$$B echo "     the card is absent or stuck busy; see patches/0012"' \
	  'exec $$B sh' \
	  > $(XIP_STAGE)/init
	@chmod 755 $(XIP_STAGE)/init
	@# -X TWICE, deliberately. One -X aligns data to 8 bytes and the kernel
	@# refuses the image with "data is not page aligned"; the second sets
	@# opt_xip_mmu and aligns to a page. A single -X mounts, runs, and
	@# silently never executes in place. The help text does not say this.
	$(BUILDROOT_OUT)/host/bin/mkcramfs -X -X $(XIP_STAGE) $(XIP_ROOTFS_IMG)
	@XIP_SIZE=$$(stat -c%s $(XIP_ROOTFS_IMG)); \
	if [ $$XIP_SIZE -gt $(ROOTFS_PARTITION_SIZE) ]; then \
		echo "ERROR: XIP image ($$XIP_SIZE bytes) exceeds the rootfs partition ($(ROOTFS_PARTITION_SIZE) bytes)"; \
		exit 1; \
	fi; \
	echo "XIP image $$XIP_SIZE bytes, $$(($(ROOTFS_PARTITION_SIZE) - $$XIP_SIZE)) bytes free in the rootfs partition"

flash-xip-rootfs:
	$(ESPFLASH) $(ROOTFS_OFFSET) $(call flashfile,$(XIP_ROOTFS_IMG))

# Historical/user-facing name for the root filesystem image.
initramfs: linux rootfs

# Second userspace XIP image, in the slot that used to be `persist`.
#
# persist held a JFFS2 upper layer for making a SquashFS flash root writable.
# That boot mode is unreachable now - the root is ext4 on the microSD and the
# flash rootfs partition holds the XIP cramfs - so the 1.4 MB was dead. It sits
# at 0x2A0000, between opensbi and linux, so it cannot be merged into rootfs;
# it becomes a second image and a second overlay lower layer instead.
#
# EXCLUDE_DIR keeps this from duplicating what the first image already holds.
# overlayfs merges both layers, so an object only needs to exist in one.
XIP2_STAGE := $(BUILD_DIR)/xipstage2
#
# foot was here and is gone: it is a Wayland-native terminal, so it has no
# client under X11.
#
# This image was FLTK's, which was a mistake: libfltk NEEDs libstdc++ and
# post-build.sh deletes libstdc++ from the target, so nothing here could load.
# It now carries xcalc and the Athena chain it drags in - libXaw7, libXt,
# libXmu, libXpm, libICE, libSM - which is the only off-the-shelf calculator
# that exists for X11.
#
# jwm also needs libXmu from this chain. Both flash images are lowerdirs of
# the same overlay, so image 1 sees these without carrying its own copy.
# xkbcomp is here rather than image 1 because it runs once, when the server
# starts, and never again - the least hot thing in the desktop.
# Image 2 holds everything that uses the Athena toolkit - jwm and xcalc - so
# libXaw7, libXt, libXmu, libICE and libSM are staged once, here. Splitting
# those two across images duplicates 800 kB of libraries and overflows both.
# xkbcomp is deliberately NOT here, and neither is libxkbfile - 288,820 bytes
# between them. Xorg forks xkbcomp once at startup to compile the keymap and
# never runs it again, so putting it in flash buys a one-second-faster server
# start and takes that space away from every binary that runs continuously.
# The second image existed to hold X clients (xcalc, jwm, xfiles). With those
# gone it staged EMPTY at 4,096 bytes while still reserving 1,441,792 bytes of
# flash - dead space on a board whose linux partition has been down to ~53 KB
# of slack.
#
# It now carries the three biggest things still running off the SD card. The
# selection is by "how continuously does it run", not by size:
#
#   ip          581,604  every ifup/ifdown, and the network path was measured
#   udevd       263,680  269 devices at coldplug, and a process launch is
#                        0.16 s here - see the boot audit in current-state.md
#
# With their closures - libkmod 79,160 and libblkid 329,436 - that is
# 1,253,880 of the 1,441,792 available. libc is NOT counted: image 1 holds it
# and overlayfs merges both layers.
#
# bluetoothd is the obvious omission and it does not fit. It is the largest
# RSS on the board and never exits, but it drags glib (1,218,632), pcre2
# (374,148) and dbus (275,812) behind it: 2,665,604 for the closure, against a
# 1,441,792 partition. Putting the binary in without its libraries buys almost
# nothing, because glib is the bulk of what it touches. It needs a bigger
# partition, not a cleverer selection - and partition geometry lives in three
# places that must agree.
#
# /sbin was added to S05xip's overlay list for ip and udevd; without it these
# bytes would sit in flash unmounted, which is exactly what had already
# happened to wpa_supplicant, iw and busybox for however many builds.
# sbin/ip was here and is not needed: nothing in the boot path calls it (the
# "ip link show" in S10udevd is a comment recording a measurement, and
# /etc/network/nfs_check is for an NFS root we do not use), and busybox has an
# ip applet anyway. It is 581,604 bytes - by far the largest single item in
# image 2 - held resident-free for a binary that never runs. It stays on the
# card, where it costs nothing until someone types it.
#
# usr/bin/xcalc pulls the whole X client chain in behind it, which is the point:
# in XIP that chain costs ZERO RSS instead of ~950 kB paged off the card.
# xclock and xfiles join xcalc here. A binary run from the ext4 card pays its
# whole text in RSS; from XIP it costs ZERO, because the pages are file-backed
# in flash and never copied. xfiles measured 188 kB of resident TEXT out of a
# 508 kB process - by far its largest single cost, and larger than everything
# the fontconfig and Xcursor replacements saved put together.
# libasound rides here so the desktop's volume path costs no RAM: 324 KB of
# resident text measured when it loaded from the SD card. The stock X
# transport chain (xkbfile/xcb/Xau/Xdmcp - xclock's DT_NEEDED drags it) goes
# the other way, to the SD lower layer: 280 KB of flash for calls that barely
# happen.
# udevd left the image when the factory partition grew for the dual-mode BT
# controller library: this kernel has no loadable modules, so libkmod was dead
# weight, and coldplug is already backgrounded and nice'd - udevd's SD-backed
# pages are clean and evictable once boot is over.
XIP2_ROOTS ?= usr/bin/xcalc usr/bin/xclock usr/bin/xfiles \
	usr/lib/libasound.so.2.0.0
XIP2_SKIP ?= libblkid.so.1.1.0 libxkbfile.so.1.0.2 libxcb.so.1.1.0 \
	libXau.so.6.0.0 libXdmcp.so.6.0.0

# Staged separately from image creation, because image 1 has to know what is
# in here before it stages itself - see the EXCLUDE_DIR note in xip-rootfs.
xip2-stage: xip-rootfs
	@echo "--- staging second userspace XIP image ---"
	rm -rf $(XIP2_STAGE)
	mkdir -p $(XIP2_STAGE)
	EXCLUDE_DIR=$(XIP_STAGE) XIP_SKIP="$(XIP2_SKIP)" python3 $(CURDIR)/rootfs/mkxipstage.py \
		$(CROSS_COMPILE)readelf $(BUILDROOT_OUT)/target $(XIP2_STAGE) \
		$(XIP2_ROOTS)

# The no-dependency form, for iterating on XIP2_ROOTS. xip2-rootfs drags in
# xip-rootfs and therefore a full Buildroot target-finalize, which is minutes
# and can fail for reasons that have nothing to do with the image being built.
# This assumes $(XIP_STAGE) is already populated - xip-image does that, and
# xip-fast runs both in the right order.
xip2-image:
	@echo "--- second userspace XIP image ---"
	rm -rf $(XIP2_STAGE)
	mkdir -p $(XIP2_STAGE)
	EXCLUDE_DIR=$(XIP_STAGE) XIP_SKIP="$(XIP2_SKIP)" python3 $(CURDIR)/rootfs/mkxipstage.py \
		$(CROSS_COMPILE)readelf $(BUILDROOT_OUT)/target $(XIP2_STAGE) \
		$(XIP2_ROOTS)
	$(BUILDROOT_OUT)/host/bin/mkcramfs -X -X $(XIP2_STAGE) $(XIP2_ROOTFS_IMG)
	@SZ=$$(stat -c%s $(XIP2_ROOTFS_IMG)); \
	if [ $$SZ -gt $(XIP2_PARTITION_SIZE) ]; then \
		echo "ERROR: xip2 image ($$SZ bytes) exceeds its partition ($(XIP2_PARTITION_SIZE) bytes)"; \
		exit 1; \
	fi; \
	echo "xip2 image $$SZ bytes, $$(($(XIP2_PARTITION_SIZE) - $$SZ)) bytes free"

xip2-rootfs: xip2-stage
	@echo "--- second userspace XIP image ---"
	$(BUILDROOT_OUT)/host/bin/mkcramfs -X -X $(XIP2_STAGE) $(XIP2_ROOTFS_IMG)
	@SZ=$$(stat -c%s $(XIP2_ROOTFS_IMG)); \
	if [ $$SZ -gt $(XIP2_PARTITION_SIZE) ]; then \
		echo "ERROR: xip2 image ($$SZ bytes) exceeds its partition ($(XIP2_PARTITION_SIZE) bytes)"; \
		exit 1; \
	fi; \
	echo "xip2 image $$SZ bytes, $$(($(XIP2_PARTITION_SIZE) - $$SZ)) bytes free"

flash-xip2-rootfs:
	$(ESPFLASH) $(XIP2_OFFSET) $(call flashfile,$(XIP2_ROOTFS_IMG))


bootloader:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	@echo "--- Build Bootloader ---"
	@echo "Using ESP-IDF from $(IDF_EXPORT)"
	bash -c "source $(IDF_EXPORT) && cd $(CURDIR)/bootloader && idf.py build"

# ---------------------------------------------------------------------------
# Restored in full. Commit 717bffe ("reclaim the dead persist partition")
# deleted this whole block along with the persist target it was actually meant
# to remove. Nine targets went with it - including `clean` and `fullclean`,
# which then silently did nothing, and the entire SD imager flow that
# docs/sd-imager.md documents. `make imager` printed "Nothing to be done" and
# was easy to read as a no-op rather than a missing rule.

buildroot-menuconfig: | $(BUILDROOT_OUT)
	$(BUILDROOT_MAKE) esp32s31_rootfs_defconfig
	$(BUILDROOT_MAKE) menuconfig

buildroot-clean:
	rm -rf $(BUILDROOT_OUT)

clean:
	rm -rf $(BUILD_DIR)

fullclean: clean
	@test ! -e $(TOOLCHAIN_DIR) || chmod -R u+w $(TOOLCHAIN_DIR)
	rm -rf $(TOOLCHAIN_DIR)

flash-opensbi:
	$(ESPFLASH) $(OPENSBI_OFFSET) $(call flashfile,$(FW_PAYLOAD))

flash-linux:
	$(ESPFLASH) $(LINUX_OFFSET) $(call flashfile,$(XIP_IMAGE))

# The flash `rootfs` partition holds the userspace XIP image, NOT the squashfs.
# Flashing the squashfs here would silently destroy the XIP image and take the
# desktop back to faulting its text off the SD card, so this is an alias for
# the thing that actually belongs there.
flash-rootfs: flash-xip-rootfs

# SD imager: a throwaway kernel carrying an initramfs, flashed over the normal
# kernel only for as long as it takes to write the microSD card, then flashed
# back with flash-linux. See docs/sd-imager.md.
IMAGER_OUT := $(BUILD_DIR)/linux-imager
IMAGER_IMAGE := $(BUILD_DIR)/xipImage-imager
IMAGER_STAGE := $(CURDIR)/imager/initramfs

imager: toolchain rootfs
	@echo "--- SD imager kernel ---"
	$(CROSS_COMPILE)gcc -Os -static \
		-march=$(S31_USER_ISA) -mabi=ilp32 \
		-o $(CURDIR)/imager/sdrecv $(CURDIR)/imager/sdrecv.c
	$(CROSS_COMPILE)strip $(CURDIR)/imager/sdrecv
	$(CURDIR)/imager/mkinitramfs.sh $(BUILDROOT_OUT)/target \
		$(IMAGER_STAGE) $(CURDIR)/imager/sdrecv
	$(MAKE) linux DEFCONFIG=esp32s31_imager_defconfig \
		LINUX_OUT=$(IMAGER_OUT) XIP_IMAGE=$(IMAGER_IMAGE) \
		FDT_DTB=$(BUILD_DIR)/imager.dtb LINUX_SIZE_FATAL=0

flash-imager:
	$(ESPFLASH) $(LINUX_OFFSET) $(call flashfile,$(IMAGER_IMAGE))

# Reset the board and hand the console back. esptool drives the strapping and
# reset lines properly; hand-rolled DTR/RTS toggling looks equivalent and is
# not - it leaves the reset non-deterministic, which shows up later as
# intermittent silence on the console and gets misread as a boot failure.
reset:
	@$(ESPTOOL) -p $(SERIAL_PORT) --after hard-reset chip-id >/dev/null 2>&1 || true
	@echo "reset $(SERIAL_PORT)"

flash-bootloader:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	@echo "--- Flash Bootloader ---"
	@echo "Using ESP-IDF from $(IDF_EXPORT)"
	bash -c "source $(IDF_EXPORT) && cd $(CURDIR)/bootloader && idf.py flash -p $(SERIAL_PORT) -b $(ESPTOOL_BAUD)"

flash-all: flash-opensbi flash-linux flash-rootfs flash-bootloader

erase:
	esptool -p /dev/ttyUSB0 -b 2000000 erase-flash
