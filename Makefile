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
LINUX_DIR := $(CURDIR)/linux-esp32-s31
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
	xip-rootfs flash-xip-rootfs xip2-stage \
	flash-rootfs xip2-rootfs flash-xip2-rootfs bootloader flash-bootloader erase \
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

FW_TEXT_START ?= 0x40220000
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
		--disable HZ_250 \
		--enable HZ_100 \
		--set-val HZ 100 \
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
		--enable DEBUG_FS \
		--set-val LOG_BUF_SHIFT 14 \
		--enable CMA \
		--enable DMA_CMA \
		--set-val CMA_SIZE_MBYTES 0 \
		--disable DYNAMIC_DEBUG \
		--disable USB_DWC2_DEBUG \
		--disable USB_DWC2_DEBUG_PERIODIC \
		--enable HID_SUPPORT \
		--enable DRM_FBDEV_EMULATION \
		--enable FRAMEBUFFER_CONSOLE \
		--disable DRM_DEBUG_MODESET_LOCK \
		--enable INPUT_MISC \
		--enable INPUT_UINPUT \
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
		--disable PROFILING \
		--disable PERF_EVENTS \
		--disable BPF_SYSCALL \
		--disable BPF_JIT \
		--disable PERF_EVENTS \
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
ROOTFS_PARTITION_SIZE ?= 6160384
XIP2_PARTITION_SIZE ?= 1441792
# The XIP kernel must start on a 4-MiB Sv32 megapage boundary, so the linux
# partition stays at 0x400000 and rootfs takes every byte the kernel does not
# need. Keep this in step with bootloader/partitions.csv.
LINUX_PARTITION_SIZE ?= 6422528

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
XIP_ROOTS ?= bin/busybox usr/bin/opkg usr/sbin/wpa_supplicant usr/sbin/iw

XIP_ROOTS_DESKTOP := usr/bin/Xorg \
	usr/lib/xorg/modules/drivers/modesetting_drv.so \
	usr/lib/xorg/modules/input/evdev_drv.so \
	usr/lib/xorg/modules/libshadow.so \
	usr/bin/st usr/bin/xsetroot \
	usr/lib/libXft.so \
	usr/share/fonts/X11/misc/6x13.pcf.gz \
	usr/share/fonts/X11/misc/6x13-ISO8859-1.pcf.gz \
	usr/share/fonts/X11/misc/6x13B.pcf.gz \
	usr/share/fonts/X11/misc/6x13B-ISO8859-1.pcf.gz \
	usr/share/fonts/X11/misc/6x13O-ISO8859-1.pcf.gz \
	usr/share/fonts/X11/misc/6x12-ISO8859-1.pcf.gz \
	usr/share/fonts/X11/misc/8x13.pcf.gz \
	usr/share/fonts/X11/misc/8x13-ISO8859-1.pcf.gz \
	usr/share/fonts/X11/misc/8x13B-ISO8859-1.pcf.gz \
	usr/share/fonts/X11/misc/10x20-ISO8859-1.pcf.gz \
	usr/share/fonts/X11/misc/cursor.pcf.gz \
	usr/share/fonts/X11/misc/fonts.alias \
	usr/share/fonts/dejavu/DejaVuSansMono.ttf

xip-rootfs: rootfs
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
	python3 $(CURDIR)/rootfs/mkxipstage.py $(CROSS_COMPILE)readelf \
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
XIP2_ROOTS ?= usr/bin/xcalc usr/bin/jwm usr/bin/xfiles

# Staged separately from image creation, because image 1 has to know what is
# in here before it stages itself - see the EXCLUDE_DIR note in xip-rootfs.
xip2-stage: xip-rootfs
	@echo "--- staging second userspace XIP image ---"
	rm -rf $(XIP2_STAGE)
	mkdir -p $(XIP2_STAGE)
	EXCLUDE_DIR=$(XIP_STAGE) python3 $(CURDIR)/rootfs/mkxipstage.py \
		$(CROSS_COMPILE)readelf $(BUILDROOT_OUT)/target $(XIP2_STAGE) \
		$(XIP2_ROOTS)

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
