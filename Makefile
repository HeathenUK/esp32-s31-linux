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
PERSIST_OFFSET := $(shell awk -F, '/persist/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))

FW_PAYLOAD := $(BUILD_DIR)/fw_payload.bin
XIP_IMAGE := $(BUILD_DIR)/xipImage
ROOTFS_IMG := $(BUILD_DIR)/rootfs.sqfs
PERSIST_IMG := $(BUILD_DIR)/persist.jffs2

# The S31-capable ESP-IDF lives at /opt/esp-idf in the build container, which is
# outside $HOME and so invisible to the search below. Check it first: searching
# $HOME on a developer machine tends to turn up several unrelated ESP-IDF
# checkouts, and the first one found is usually not the one that knows about
# esp32s31 - it fails late, at "toolchain-esp32s31.cmake not found".
IDF_EXPORT := $(shell test -f /opt/esp-idf/export.sh && echo /opt/esp-idf/export.sh || \
	find $(HOME) -maxdepth 5 -type f -name export.sh 2>/dev/null | grep esp-idf | head -n 1)

.PHONY: all download toolchain toolchain-source opensbi linux coremark rootfs initramfs s31-pie-cases \
	buildroot-menuconfig buildroot-clean clean fullclean flash-opensbi flash-linux \
	flash-rootfs persist flash-persist bootloader flash-bootloader erase \
	imager flash-imager

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

linux: toolchain | $(LINUX_OUT)
	@echo "--- Linux ---"
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" $(DEFCONFIG)
	$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
		--set-str BUILTIN_DTB_SOURCE "espressif/esp32s31_generic" \
		--enable RISCV_ISA_C \
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
		--disable DRM_FBDEV_EMULATION \
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
		--disable BLK_DEV_IO_TRACE
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" olddefconfig
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" \
		KCFLAGS="-march=$(S31_SAFE_ISA) $(S31_COMMON_FLAGS)" -j$(JOBS) $(LINUX_TARGET) dtbs
	cp -v $(LINUX_OUT)/arch/riscv/boot/$(LINUX_TARGET) $(XIP_IMAGE)
	cp -v $(LINUX_OUT)/arch/riscv/boot/dts/espressif/esp32s31_generic.dtb $(FDT_DTB)
	cp -v $(LINUX_OUT)/System.map $(BUILD_DIR)/System.map
	@XIP_SIZE=$$(stat -c%s $(XIP_IMAGE)); \
	if [ $$XIP_SIZE -gt $(LINUX_PARTITION_SIZE) ]; then \
		echo "ERROR: xipImage ($$XIP_SIZE bytes) exceeds the linux partition ($(LINUX_PARTITION_SIZE) bytes)"; \
		exit 1; \
	fi; \
	echo "xipImage $$XIP_SIZE bytes, $$(($(LINUX_PARTITION_SIZE) - $$XIP_SIZE)) bytes free in the linux partition"

coremark: rootfs
	@test -x "$(BUILDROOT_OUT)/target/usr/bin/coremark"
	@echo "Buildroot CoreMark: $(BUILDROOT_OUT)/target/usr/bin/coremark"

# Keep this decimal because POSIX test(1) and truncate(1) do not accept the
# partition table's 0x-prefixed value.
ROOTFS_PARTITION_SIZE ?= 4194304
PERSIST_PARTITION_SIZE ?= 1441792
# The XIP kernel must start on a 4-MiB Sv32 megapage boundary, so the linux
# partition stays at 0x400000 and rootfs takes every byte the kernel does not
# need. Keep this in step with bootloader/partitions.csv.
LINUX_PARTITION_SIZE ?= 8388608
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
	@ROOTFS_SIZE=$$(stat -c%s $(ROOTFS_IMG)); \
	if [ $$ROOTFS_SIZE -gt $(ROOTFS_PARTITION_SIZE) ]; then \
		echo "ERROR: Buildroot rootfs ($$ROOTFS_SIZE bytes) exceeds partition ($(ROOTFS_PARTITION_SIZE) bytes)"; \
		exit 1; \
	fi; \
	truncate -s $(ROOTFS_PARTITION_SIZE) $(ROOTFS_IMG)

# Historical/user-facing name for the root filesystem image.
initramfs: linux rootfs

# Generate an empty, NOR-compatible JFFS2 image for the persist partition.
# This is separate from normal firmware updates so user data is not erased.
persist: | $(BUILD_DIR)
	@command -v mkfs.jffs2 >/dev/null || { echo "ERROR: mkfs.jffs2 is required" >&2; exit 1; }
	@staging=$$(mktemp -d "$(BUILD_DIR)/persist.XXXXXX"); \
	trap 'rmdir "$$staging"' EXIT; \
	mkfs.jffs2 -q -e 0x2000 --pad=$(PERSIST_PARTITION_SIZE) \
		-d "$$staging" -o $(PERSIST_IMG)

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
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(OPENSBI_OFFSET) $(FW_PAYLOAD)

flash-linux:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(LINUX_OFFSET) $(XIP_IMAGE)

flash-rootfs:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(ROOTFS_OFFSET) $(ROOTFS_IMG)

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
		FDT_DTB=$(BUILD_DIR)/imager.dtb

flash-imager:
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(LINUX_OFFSET) $(IMAGER_IMAGE)

flash-persist: persist
	esptool -p /dev/ttyUSB0 -b 2000000 write-flash $(PERSIST_OFFSET) $(PERSIST_IMG)

bootloader:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	@echo "--- Build Bootloader ---"
	@echo "Using ESP-IDF from $(IDF_EXPORT)"
	bash -c "source $(IDF_EXPORT) && cd $(CURDIR)/bootloader && idf.py build"

flash-bootloader:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	@echo "--- Flash Bootloader ---"
	@echo "Using ESP-IDF from $(IDF_EXPORT)"
	bash -c "source $(IDF_EXPORT) && cd $(CURDIR)/bootloader && idf.py flash -p /dev/ttyUSB0 -b 2000000"

flash-all: flash-opensbi flash-linux flash-rootfs flash-bootloader

erase:
	esptool -p /dev/ttyUSB0 -b 2000000 erase-flash
