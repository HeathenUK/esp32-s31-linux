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
# The 7.1 port is the kernel this board runs, and the ONLY kernel tree here.
#
# linux-esp32-s31/ was the old 6.12 submodule. It was REMOVED on 2026-09-08:
# nothing built from it, but work was once written into it by mistake, compiled
# nowhere, and left the symbol absent from System.map with no clue why (see
# memory/s31-wrong-kernel-tree). It still held the stranded remains of that
# when it was deleted. Building it against the shared output volume would also
# silently relink 6.12 objects into a 7.1-named image.
#
# The guard below fails the build if it ever reappears, because a second
# kernel tree that is silently not built is a trap, not a resource.
LINUX_DIR := $(CURDIR)/linux-71-port
ifneq ($(wildcard $(CURDIR)/linux-esp32-s31/Makefile),)
$(error linux-esp32-s31/ has a kernel Makefile again. That is the OLD 6.12 tree, deleted deliberately - see linux-esp32-s31/README. The board runs 7.1.10 and make linux builds linux-71-port/. Remove it before building)
endif
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
# The loader app, read from the same CSV rather than hardcoded - the docs
# warn that partition geometry lives in several places that must agree.
LOADER_OFFSET := $(shell awk -F, '/factory/ {gsub(/ /, "", $$4); print $$4}' $(PARTITIONS_CSV))
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
# /proc/profile only exists with profile= on the command line, and the
# command line is CONFIG_CMDLINE (CMDLINE_FORCE), not the DTS. profile=6 is
# 64-byte buckets in a 256 KB buffer; profile=2 allocates ~4 MB and starves
# userspace, which reads as "hangs in udev" - see docs/current-state.md.
# Appended to whatever the defconfig carries, so it never diverges from it.
PROF_CMDLINE_TWEAK = --set-str CMDLINE "$$(sed -n 's/^CONFIG_CMDLINE=\"\(.*\)\"/\1/p' $(LINUX_OUT)/.config) profile=6"
else
PROF_CMDLINE_TWEAK :=
PROF_TWEAKS := --disable PROFILING --disable PERF_EVENTS
endif

# EARLYCON=1 puts the S31 UART earlycon on the command line so a kernel that
# dies before console_init() says where. Self-cleaning: a plain build strips
# it again, because .config's CMDLINE persists across builds (the defconfig is
# not re-applied) and would otherwise carry it for ever.
# USB_HS=1 boots the dwc2 root port at high speed (dwc2.host_full_speed=0)
# instead of the forced full speed we ship. Same self-cleaning rule.
EARLYCON ?= 0
USB_HS ?= 0
# USB_BUFDMA=1 adds dwc2.desc_dma=0 (buffer DMA): the only mode in which
# full/low-speed devices behind a high-speed hub work, because descriptor
# DMA refuses split transactions (hcd_ddma.c). Costs the 8 kHz SOF.
# DEFAULT ON. This is the board's working configuration, and it must not
# depend on remembering to type it: CMDLINE_NOW strips these options out of the
# existing .config and re-adds them only from these flags, so ANY `make linux`
# without them silently reverts the port to descriptor DMA at high speed. That
# happened on 2026-09-13 across a day of audio rebuilds and was found when a
# mouse was plugged in and neither receiver enumerated at all.
USB_BUFDMA ?= 1
# USB_SOF=1 adds dwc2.sof_irq=1, unmasking the start-of-frame interrupt while
# KEEPING descriptor DMA.
#
# sof_irq was turned off because the 1 kHz flood was a leftover mask bit in
# DDMA mode and switching it off returned ~6% of the core. What that did not
# check is whether anything still needs SOF to advance the periodic schedule.
#
# RETRACTED, 2026-09-12: "zero interrupts in three seconds while idle" was
# cited here as proof the schedule had stalled. It is not. In descriptor DMA
# an interrupt-IN descriptor stays armed across NAKs and only raises IOC when
# data actually arrives, so zero interrupts from two idle HID devices is the
# correct and intended state - it is what the sof_irq=0 measurement recorded
# in the first place.
#
# The real gap is in the scheduler lists (hcd_queue.c). dwc2_schedule_periodic
# has an explicit DDMA case - "Don't rely on SOF and start in ready schedule" -
# but dwc2_hcd_qh_deactivate, which runs when a periodic transfer COMPLETES
# with QTDs still queued, does not. It falls back to comparing next_active_frame
# against hsotg->frame_number and parks the QH in periodic_sched_inactive,
# commented "we know SOF interrupt will handle future frames". With SOF masked
# no such interrupt exists, and dwc2_hcd_select_transactions only ever draws
# periodic work from periodic_sched_ready - so a QH that lands in inactive is
# stranded. hsotg->frame_number is also only refreshed off other traffic, which
# is why moving the mouse lets the next keyboard key through.
#
# Setting this to 1 restores upstream behaviour (upstream enables SOF on the
# first periodic QH unconditionally). Tried 2026-09-12: it did NOT fix input -
# keys then stuck down instead - so there is a second fault as well.
#
# The parameter is read at host initialisation, so a sysfs write does nothing
# and the controller does not survive an unbind/rebind. It has to come in on
# the command line, which is CMDLINE_FORCE here.
USB_SOF ?= 0
CMDLINE_NOW = $$(sed -n 's/^CONFIG_CMDLINE=\"\(.*\)\"/\1/p' $(LINUX_OUT)/.config | sed 's/ earlycon//g; s/ dwc2.host_full_speed=0//g; s/ dwc2.host_full_speed=1//g; s/ usbcore.autosuspend=-1//g; s/ dwc2.desc_dma=0//g; s/ dwc2.sof_irq=1//g; s/ snd_aloop.index=1//g; s/ profile=6//g')
# The ALSA loopback must not steal card 0 from the Korvo codec: it would
# silently redirect every app's default output into the loopback and leave
# the volume mixer attached to a card with no controls.
CMDLINE_ADD := snd_aloop.index=1
# PROF=1 must land here, not only in PROF_CMDLINE_TWEAK: the EARLYCON_TWEAK
# below re-sets CMDLINE from CMDLINE_NOW + CMDLINE_ADD after it, and on
# 2026-09-10 that silently dropped profile=6 - kernel #184 had PROFILING
# compiled in and no /proc/profile, because the param never reached it.
ifeq ($(PROF),1)
CMDLINE_ADD += profile=6
endif
ifeq ($(EARLYCON),1)
CMDLINE_ADD += earlycon
endif
ifeq ($(USB_HS),1)
CMDLINE_ADD += dwc2.host_full_speed=0
endif
ifeq ($(USB_SOF),1)
CMDLINE_ADD += dwc2.sof_irq=1
endif
ifeq ($(USB_BUFDMA),1)
CMDLINE_ADD += dwc2.desc_dma=0
endif
# USB_FS=1 PINS the root port to full speed (dwc2.host_full_speed=1) instead of
# leaving it adaptive. Adaptive only drops to full speed when a QH needs a
# split, and that refusal happens in the descriptor-DMA path - so with
# USB_BUFDMA=1 splits are supported, nothing ever refuses, and the port would
# stay at HIGH speed running the 8 kHz SOF (measured 39% of the core). Buffer
# DMA is therefore only affordable pinned to full speed, where the SOF is
# 1 kHz. Use the two together: make linux USB_BUFDMA=1 USB_FS=1.
USB_FS ?= 1
# USB_NOSUSPEND=1 adds usbcore.autosuspend=-1, which stops usbcore suspending
# the hubs.
#
# ROOT CAUSE of the long-standing "hotplug after boot does nothing". usbcore
# autosuspends a hub two seconds after it goes idle, and a SUSPENDED HUB NEVER
# REPORTS A NEWLY PLUGGED DEVICE - dwc2 does not surface remote wakeup from it
# in this mode. So devices present at boot worked (the initial scan found them)
# and anything plugged in later was invisible for ever. Proven 2026-09-13:
# with only the hub enumerated and "4 ports detected", writing "on" to
# power/control of usb1 and 1-1 made BOTH receivers appear within seconds.
#
# Default ON. These are HID receivers that have to be awake to be any use, the
# board is mains powered, and this is exactly the class of setting that must
# not depend on being remembered - see USB_BUFDMA above.
USB_NOSUSPEND ?= 1
ifeq ($(USB_FS),1)
CMDLINE_ADD += dwc2.host_full_speed=1
endif
ifeq ($(USB_NOSUSPEND),1)
CMDLINE_ADD += usbcore.autosuspend=-1
endif
EARLYCON_TWEAK = --set-str CMDLINE "$(CMDLINE_NOW)$(if $(CMDLINE_ADD), $(CMDLINE_ADD),)"

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

# Slab accounting OFF by default, because the thing that reports it is also the
# thing that costs memory. The shipping kernel sets CONFIG_SLUB_TINY, which is
# what a 15.4 MB machine wants - but SLUB_DEBUG depends on !SLUB_TINY, and
# SLUB_DEBUG is what creates /proc/slabinfo and /sys/kernel/slab. So there is
# no way to see the breakdown of Slab without changing the allocator that
# produced it.
#
# `make linux SLABDIAG=1` builds a kernel that can be asked. Use it for the
# BREAKDOWN only: turning SLUB_TINY off restores per-CPU slabs and moves the
# total, so the absolute number from that kernel is not the shipping number.
SLABDIAG ?= 0
ifeq ($(SLABDIAG),0)
SLAB_TWEAKS :=
else
SLAB_TWEAKS := --disable SLUB_TINY --enable SLUB_DEBUG
endif
# `make linux LOCKUP=1` builds a kernel that says something when it locks up.
#
# The shipping kernel has no soft-lockup detector, no hung-task detector, no
# SysRq, and - being single-hart TINY_RCU - no RCU stall detector either, so a
# hang prints nothing whatever its cause. This turns all three on and makes
# each of them panic, because a panic is the one thing guaranteed to reach the
# console. Not for shipping: the watchdog thread costs a wake-up every 4 s.
#
# LOCKUP=2 is the soft-lockup detector alone (a periodic wake, no SysRq) and
# LOCKUP=3 is SysRq alone (no periodic wake): the two halves, for telling a
# hang that a periodic wake-up prevents from one that a code layout hides.
LOCKUP ?= 0
ifeq ($(LOCKUP),0)
LOCKUP_TWEAKS :=
else ifeq ($(LOCKUP),2)
LOCKUP_TWEAKS := --enable SOFTLOCKUP_DETECTOR --enable BOOTPARAM_SOFTLOCKUP_PANIC
else ifeq ($(LOCKUP),3)
LOCKUP_TWEAKS := --enable MAGIC_SYSRQ --enable MAGIC_SYSRQ_SERIAL
else
LOCKUP_TWEAKS := --enable SOFTLOCKUP_DETECTOR --enable BOOTPARAM_SOFTLOCKUP_PANIC \
	--enable DETECT_HUNG_TASK --set-val DEFAULT_HUNG_TASK_TIMEOUT 20 \
	--enable BOOTPARAM_HUNG_TASK_PANIC \
	--enable MAGIC_SYSRQ --enable MAGIC_SYSRQ_SERIAL
endif
# The scheduler tick runs through idle (HZ_PERIODIC), on purpose - this is
# the fix for a silent, total hang, not a tuning choice.
#
# NO_HZ_IDLE (tickless idle) was turned on 2026-08-20 alongside HIGH_RES_TIMERS
# (commit d913bda) for the timer-resolution win. On this SoC's RISC-V timer,
# tickless idle loses a wake-up under interactive load: hart1 sleeps in WFI and
# never returns, which takes the hosted link to hart0 with it, so the board
# dies with nothing on the console and no ping - a heisenbug that vanishes
# under any instrument that keeps a periodic wake (the soft-lockup detector
# masked it all through debugging). Doom with mouse-look died within a minute
# on NO_HZ; HZ_PERIODIC survived 5/5 back-to-back motion hammers plus normal
# play. HIGH_RES_TIMERS is independent and stays on, so the 2026-08-20
# resolution win (4 ms -> ~1.5 ms sleeps) is kept in full. The cost is ~100
# idle wake-ups/s. `make linux TICK=nohz` restores tickless idle for anyone
# who fixes the timer driver and wants to A/B it.
TICK ?= periodic
ifeq ($(TICK),periodic)
TICK_TWEAKS := --disable NO_HZ_IDLE --disable NO_HZ_COMMON --disable NO_HZ --enable HZ_PERIODIC
else
TICK_TWEAKS :=
endif
LINUX_TARGET ?= xipImage

# An oversized kernel is fatal, because it silently runs past its partition into
# rootfs. The SD imager is the one deliberate exception: it is flashed over the
# normal kernel only for as long as it takes to write the card, and the restore
# step reflashes rootfs anyway. See docs/sd-imager.md.
# CMA's reservation must have its base AND size aligned to one pageblock
# (CMA_MIN_ALIGNMENT_BYTES in mm/cma.c); rmem_cma_setup() rejects the region
# outright otherwise and the board boots with no CMA at all. pageblock_order is
# PAGE_BLOCK_MAX_ORDER when there are no huge pages, which defaults to
# MAX_PAGE_ORDER = 10, i.e. 4 MiB - so the CMA pool could only ever be a
# multiple of 4 MiB.
#
# CONFIG_PAGE_BLOCK_MAX_ORDER lowers the pageblock WITHOUT lowering
# MAX_PAGE_ORDER, so the buddy allocator keeps its 4 MiB maximum allocation and
# only the migratetype granularity changes. Order 8 is 1 MiB, which makes a
# 5 MB CMA pool legal. mm/Kconfig's stated cost is THP success rate, and this
# kernel has neither CONFIG_HUGETLBFS nor THP.
PAGE_BLOCK_ORDER ?= 8

LINUX_SIZE_FATAL ?= 1

linux: toolchain | $(LINUX_OUT)
	@echo "--- Linux ---"
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" $(DEFCONFIG)
	$(LINUX_DIR)/scripts/config --file $(LINUX_OUT)/.config \
		--set-str BUILTIN_DTB_SOURCE "espressif/esp32s31_generic" \
		--set-str BUILTIN_DTB_NAME "espressif/esp32s31_generic" \
		--enable RISCV_ISA_C \
		--enable PROFILING \
		$(PROF_CMDLINE_TWEAK) \
		$(EARLYCON_TWEAK) \
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
		--enable UHID \
		--enable SND_ALOOP \
		--enable DEBUG_FS \
		--enable USB_MON \
		$(DIAG_TWEAKS) \
		$(SLAB_TWEAKS) \
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
		--enable DMABUF_HEAPS \
		--enable DMABUF_HEAPS_CMA \
		--disable FTRACE \
		--disable ENABLE_DEFAULT_TRACERS \
		--disable BLK_DEV_IO_TRACE \
		$(PROF_TWEAKS) \
		$(LOCKUP_TWEAKS) \
		$(TICK_TWEAKS) \
		--disable BPF_SYSCALL \
		--disable BPF_JIT \
		--disable PREEMPT_LAZY \
		--disable PREEMPT \
		--disable PREEMPT_VOLUNTARY \
		--enable PREEMPT_NONE \
		--enable DRM_FBDEV_EMULATION \
		--disable IPV6 \
		--enable SYSVIPC \
		--enable FUTEX \
		--set-val PAGE_BLOCK_MAX_ORDER $(PAGE_BLOCK_ORDER)
	$(MAKE) -C $(LINUX_DIR) O=$(LINUX_OUT) ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" olddefconfig
	@# Stock musl/SDL need blocking waits; ENOSYS turns contention into spinning.
	@grep -qx 'CONFIG_FUTEX=y' $(LINUX_OUT)/.config || { echo "ERROR: stock threaded userspace requires CONFIG_FUTEX=y"; exit 1; }
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
XIP2_PARTITION_SIZE ?= 1638400
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
# Every flash goes through the port lock. esptool knows nothing about the
# flock that scripts/board/ uses, and a flash landing inside a measurement is
# the worst collision available - it reboots the board AND rewrites its flash,
# so the run being timed silently becomes a run of something else. --wait
# queues rather than failing: better to start late than to corrupt a
# measurement or make the user re-run a build.
WITHLOCK = python3 $(CURDIR)/scripts/board/withlock.py --wait 900 --
ESPFLASH = $(WITHLOCK) $(ESPTOOL) -p $(SERIAL_PORT) -b $(ESPTOOL_BAUD) write-flash

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

# flashfile falls back to images/ when the build directory is not on the host -
# and it usually is not, because $(BUILD_DIR) is a Docker volume. The fallback
# is SILENT, so a build that succeeded inside the container and was never
# copied out gets flashed as whatever images/ still holds. esptool then writes
# it, verifies the hash, and reports complete success while putting month-old
# content on the board. That cost two full flashes and forty minutes on
# 2026-09-05, with the board dutifully running the old libraries afterwards.
#
# Say which file is actually going to the board, and when it was built, before
# every flash. Use `make sync-images` to copy fresh artefacts out of the volume.
sayflash = @f="$(call flashfile,$(1))"; \
	echo "--- flashing $$f"; \
	echo "    built $$(date -r "$$f" '+%Y-%m-%d %H:%M' 2>/dev/null || echo UNKNOWN)"

# Copy build artefacts out of the Docker volume into images/, which is where
# the flash targets look when the build directory is not visible on the host.
sync-images:
	@echo "--- copying build artefacts out of the container volume ---"
	@# EVERY flashable artefact, the kernel included. It was omitted here for
	@# a long time, so `make linux` left build/xipImage in the volume and the
	@# flash silently wrote the stale images/xipImage - a whole day of "the
	@# fix does nothing" on 2026-09-09 was a diagnostic kernel that never
	@# reached the board. flash-linux now refuses to flash a kernel older
	@# than the build (see check-kernel-fresh), but the fix is to copy it.
	./docker/build.sh 'for f in rootfs-xip.cramfs rootfs-xip2.cramfs rootfs.squashfs xipImage System.map fw_payload.bin; do if [ -f /src/build/$$f ]; then cp -v /src/build/$$f /src/images/$$f; fi; done'
	@# The loader builds inside bootloader/build, not build/.
	./docker/build.sh 'test -f /src/bootloader/build/hello_world.bin && cp -v /src/bootloader/build/hello_world.bin /src/images/hello_world.bin || true'
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
# bluetoothd LEFT XIP on 2026-09-11, deliberately this time: it is the
# control plane only (pairing, SDP, AVDTP signalling, AVRCP buttons) and is
# out of the data path once a device is connected - HID goes controller to
# kernel hidp to evdev, A2DP from s31-bt straight into the L2CAP socket. Its
# text and glib's were 2.4 MB of this image; that space is SDL2 now. The
# cost is a few 4 KB faults off the card when a headset button or a reconnect
# wakes it under a game. The trap from the LAST eviction (an Aug-23 copy on
# the card without the a2dp plugin) is closed by checksum: the card's copy
# matched the target tree (b7d1d1d8) before this was shipped.
# libpng16 and libz join for prboom's screenshots and xcalc for the menu -
# 290 KB of text off the card on every use, measured as churn in smaps.
XIP_ROOTS ?= bin/busybox usr/sbin/wpa_supplicant usr/sbin/iw usr/bin/lvdesk \
	usr/lib/alsa-lib/libasound_module_pcm_s31route.so \
	usr/bin/s31-coex usr/bin/s31swapon usr/lib/libSDL-1.2.so.0.11.4 \
	usr/lib/libSDL2-2.0.so.0.3200.10 usr/lib/libSDL2_mixer-2.0.so.0.600.3 \
	usr/lib/libXrandr.so.2.2.0 \
	usr/lib/libpng16.so.16.58.0 usr/lib/libz.so.1.3.2 usr/bin/xcalc \
	usr/lib/libdbus-1.so.3.32.4 \
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
# libxkbfile was BUILT by xftlite and never listed here, so x11-stage never
# installed it and the board ran the stock 120,488-byte library instead of our
# 5,212-byte stub. That one omission kept libxcb (128,696), libXau (9,476) and
# libXdmcp (17,672) alive as well - 276 KB of library, resident from the SD
# layer rather than XIP, because xclock calls exactly ONE libxkbfile function:
# XkbStdBell, which rings a bell on hardware that has no bell. Verified before
# shipping the stub that XkbStdBell is xclock's only Xkb import and the only
# symbol the stub exports.
X11_REPLACEMENTS := libX11.so.6.4.0 libXt.so.6.0.0 libXaw7.so.7.0.0 \
	libXmu.so.6.2.0 libICE.so.6.3.0 libSM.so.6.0.1 libXext.so.6.4.0 \
	libXpm.so.4.11.0 libXrender.so.1.3.0 libXft.so.2.3.9 \
	libfontconfig.so.1.16.0 libXcursor.so.1.0.2 libxkbfile.so.1.0.2 \
	libfreetype.so.6.20.6 libXrandr.so.2.2.0

x11-stage:
	@echo "--- installing the X11 replacements into the overlay ---"
	@mkdir -p $(BUILDROOT_EXTERNAL)/board/esp32-s31/overlay/usr/lib
	@for f in $(X11_REPLACEMENTS); do \
		test -f images/$$f || { echo "ERROR: images/$$f is missing - build it first" >&2; exit 1; }; \
		cp -a images/$$f $(BUILDROOT_EXTERNAL)/board/esp32-s31/overlay/usr/lib/$$f; \
		printf "  %-24s %7d bytes\n" $$f $$(stat -f%z images/$$f 2>/dev/null || stat -c%s images/$$f); \
	done

# ---------------------------------------------------------------------------
# /etc DOES NOT COME FROM FLASH.
#
# The XIP images cover usr/bin, usr/lib, usr/libexec and lib. Everything else,
# /etc included, lives on the ext4 card, and the card is only rewritten by a
# full re-image. So editing an init script in the overlay, committing it and
# flashing changes NOTHING on a running board - the file is correct in the
# repo and in the next image, and stale on the card in front of you.
#
# That cost a measurement: the fix that stops ntpd stepping the clock under a
# running benchmark was committed, flashed twice, and never reached the board,
# so the next run absorbed the step again and reported 0.1 fps.
#
# This pushes the overlay's /etc to the card and verifies it. ETC=<path>
# restricts it to one file, relative to the overlay root.
#
#   make deploy-etc                      everything under overlay/etc
#   make deploy-etc ETC=etc/init.d/S30clock    just that one
# ---------------------------------------------------------------------------
OVERLAY := $(BUILDROOT_EXTERNAL)/board/esp32-s31/overlay
ETC ?=

deploy-etc:
	@set -e; \
	if [ -n "$(ETC)" ]; then \
		LIST="$(ETC)"; \
	else \
		LIST=$$(cd $(OVERLAY) && find etc -type f | sort); \
	fi; \
	for f in $$LIST; do \
		test -f "$(OVERLAY)/$$f" || { echo "ERROR: $(OVERLAY)/$$f is missing" >&2; exit 1; }; \
		echo "--- $$f"; \
		python3 scripts/board/deploy.py "$(OVERLAY)/$$f" "/$$f" || exit 1; \
	done; \
	echo "deploy-etc: done. deploy.py verifies each file by md5 on both ends."

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
	  '# Check the card before mounting it read-write, but ONLY when its' \
	  '# superblock is flagged with errors: this board is hard-reset all' \
	  '# day, a plain unclean journal is replayed by the mount, and a full' \
	  '# e2fsck of the 111 GB card takes over a minute. e2fsck refuses a' \
	  '# mounted device (even read-only, off the root), so the tools are' \
	  '# copied from a read-only, noload mount into RAM, the card is' \
	  '# unmounted, and the check runs from there. -p fixes only what is' \
	  '# safe unattended; the rc is printed for the console log.' \
	  'if [ -b /dev/mmcblk0 ] && $$B mount -t ext4 -o ro,noload /dev/mmcblk0 /mnt/sd; then' \
	  '        $$B mount -t tmpfs tmpfs /tmp' \
	  '        for f in sbin/e2fsck sbin/tune2fs lib/libblkid.so.1 lib/libuuid.so.1 \' \
	  '                 usr/lib/libext2fs.so.2 usr/lib/libcom_err.so.2 usr/lib/libe2p.so.2; do' \
	  '                $$B cp /mnt/sd/$$f /tmp/ 2>/dev/null' \
	  '        done' \
	  '        $$B umount /mnt/sd' \
	  '        if LD_LIBRARY_PATH=/tmp /tmp/tune2fs -l /dev/mmcblk0 2>/dev/null | $$B grep -q "state:.*errors"; then' \
	  '                $$B echo "root: card flagged with errors, running e2fsck -p"' \
	  '                # e2fsck of a 111 GB, 31M-inode card wants ~12 MB of' \
	  '                # bitmaps and was OOM-killed at 7 MB here. The live' \
	  '                # system survives the same check only because the SD' \
	  '                # swap is on. Pre-pivot the card is unmounted, so give' \
	  '                # it compressed swap in RAM instead: the bitmaps are' \
	  '                # almost all zeros and cost next to nothing in zram.' \
	  '                if [ -e /sys/block/zram0/disksize ]; then' \
	  '                        $$B echo 48M > /sys/block/zram0/disksize' \
	  '                        $$B mkswap /dev/zram0 >/dev/null 2>&1 && $$B swapon /dev/zram0' \
	  '                fi' \
	  '                LD_LIBRARY_PATH=/tmp /tmp/e2fsck -p /dev/mmcblk0' \
	  '                $$B echo "root: e2fsck rc=$$?"' \
	  '                $$B swapoff /dev/zram0 2>/dev/null' \
	  '                $$B echo 1 > /sys/block/zram0/reset 2>/dev/null' \
	  '        fi' \
	  '        $$B umount /tmp' \
	  'fi' \
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
	$(call sayflash,$(XIP_ROOTFS_IMG))
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
# The freetype stub is here for st, the off-the-shelf terminal we use as an
# input CONTROL. st links libfreetype.so.6 but calls not one FT_ function
# (`nm -D --undefined-only st | grep ^FT_` is empty), so this is 4,764 bytes
# against the real library's 660 kB. st itself stays out of XIP - adding it
# pushed the image 8,192 bytes past its partition, and it runs perfectly well
# from where it already lives.
# xcalc left flash for prboom's libSDL_mixer and libSDL_net (2026-09-10):
# the game stays on the SD by choice, its libraries do not - libSDL itself
# is an XIP root. xcalc still runs, from the SD layer of the overlay.
XIP2_ROOTS ?= usr/lib/libSDL_mixer-1.2.so.0.12.1 usr/lib/libSDL_net-1.2.so.0.8.1 \
	usr/bin/xclock usr/bin/xfiles \
	usr/lib/libasound.so.2.0.0 usr/bin/s31-bt \
	usr/lib/libfreetype.so.6.20.6
# libxkbfile and the xcb chain used to be pushed to the SD layer here, because
# the STOCK libxkbfile is 120,488 bytes and drags libxcb (128,696), libXau
# (9,476) and libXdmcp (17,672) behind it - 276 KB of flash for xclock's one
# call to XkbStdBell. That trade is gone: xftlite builds a 5,212-byte stub
# exporting exactly XkbStdBell (verified to be xclock's only Xkb import), it is
# in X11_REPLACEMENTS now, and with it in place libxcb, libXau and libXdmcp
# leave the closure altogether rather than being paid for from the card.
XIP2_SKIP ?= libblkid.so.1.1.0

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
	$(call sayflash,$(XIP2_ROOTFS_IMG))
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

# Rebuild ONE Buildroot package in place: make br-rebuild-alsa-lib. The
# narrowest target that reaches a library change; a full rootfs rebuild is
# ruinous here. Follow with xip-fast and sync-images.
# Both finish with target-finalize: a per-package rebuild leaves the
# package's files in target/ UNSTRIPPED (stripping is a finalize step), the
# XIP stager copies what it finds, and on 2026-09-10 an unstripped
# libasound (1.24 MB against 943 KB) pushed the xip2 image 45 KB past its
# partition - the build refused it all afternoon, hidden behind an output
# filter, and the board ran the old image.
br-rebuild-%: | $(BUILDROOT_OUT)
	$(BUILDROOT_MAKE) $*-rebuild
	$(BUILDROOT_MAKE) target-finalize

# CFLAGS and configure options are baked in at configure time, so a flag
# change needs this one, not -rebuild (which silently builds the old flags).
br-reconfigure-%: | $(BUILDROOT_OUT)
	$(BUILDROOT_MAKE) $*-reconfigure
	$(BUILDROOT_MAKE) target-finalize

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
	$(call sayflash,$(FW_PAYLOAD))
	$(ESPFLASH) $(OPENSBI_OFFSET) $(call flashfile,$(FW_PAYLOAD))

flash-linux: check-kernel-fresh
	$(call sayflash,$(XIP_IMAGE))
	$(ESPFLASH) $(LINUX_OFFSET) $(call flashfile,$(XIP_IMAGE))

# Refuse to flash a stale kernel. If build/xipImage (in the container volume)
# is newer than images/xipImage (what actually gets flashed), someone ran
# `make linux` and forgot `make sync-images`, and the flash would put an old
# kernel on the board while reporting success. Compare mtimes and stop.
# A saved variant deliberately copied over images/xipImage is newer than the
# build, so this never false-alarms on that workflow.
check-kernel-fresh:
	@bt=$$(./docker/build.sh 'stat -c %Y /src/build/xipImage 2>/dev/null' 2>/dev/null | tr -dc 0-9); 	it=$$(stat -f %m "$(CURDIR)/images/xipImage" 2>/dev/null || stat -c %Y "$(CURDIR)/images/xipImage" 2>/dev/null); 	if [ -n "$$bt" ] && [ -n "$$it" ] && [ "$$bt" -gt "$$it" ]; then 		echo "ERROR: build/xipImage is newer than images/xipImage."; 		echo "       You built a kernel but did not copy it out - run 'make sync-images'."; 		echo "       (build $$(date -r /dev/stdin '+%H:%M' 2>/dev/null; echo) newer; refusing to flash a stale kernel.)"; 		exit 1; 	fi

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
	@$(WITHLOCK) $(ESPTOOL) -p $(SERIAL_PORT) --after hard-reset chip-id >/dev/null 2>&1 || true
	@echo "reset $(SERIAL_PORT)"

# FLASH THE LOADER THE SAME WAY AS EVERYTHING ELSE.
#
# flash-bootloader below shells out to idf.py, which needs a working host
# ESP-IDF - and this host's is a different tree whose toolchains are not
# installed, so it fails with "tool riscv32-esp-elf has no installed
# versions" before it ever reaches the serial port. Every other flash target
# uses $(ESPFLASH), which is IDF's esptool from ~/.espressif, and that works.
#
# The loader app is hello_world.bin at the factory offset, NOT bootloader.bin.
# It carries shared/s31_memory_layout.h, so any change to the memory map needs
# this flashed alongside OpenSBI and the kernel or the three disagree - and
# for the audio SRAM pool specifically, a stale loader lets hart0's heap
# allocate inside Linux's DMA ring, which is silent corruption rather than a
# boot failure.
flash-loader: sync-images
	$(call sayflash,$(CURDIR)/images/hello_world.bin)
	$(ESPFLASH) $(LOADER_OFFSET) $(CURDIR)/images/hello_world.bin

flash-bootloader:
	@if [ -z "$(IDF_EXPORT)" ]; then echo "ERROR: ESP-IDF export.sh not found under $(HOME)"; exit 1; fi
	@echo "--- Flash Bootloader ---"
	@echo "Using ESP-IDF from $(IDF_EXPORT)"
	bash -c "source $(IDF_EXPORT) && cd $(CURDIR)/bootloader && idf.py flash -p $(SERIAL_PORT) -b $(ESPTOOL_BAUD)"

flash-all: flash-opensbi flash-linux flash-rootfs flash-bootloader

erase:
	esptool -p /dev/ttyUSB0 -b 2000000 erase-flash

# --- regression gates (scripts/board/gate.py, acceptance.sh) -------------
# `make gate` is the fast one (~2.5 min: on-board config contract, desktop
# smoke, two sdlbench canaries against scripts/board/gate-baseline.json).
# `make gate-quick` skips the canaries (~80 s). `make acceptance` resets the
# board and adds the fbcon<->lvdesk handover test and the real Doom timedemos
# (~8 min). Run `make gate` after EVERY flash; both the FUTEX loss and the USB
# flag loss of September 2026 would have been caught by it in under a minute.
.PHONY: gate gate-quick gate-baseline acceptance
gate:
	python3 scripts/board/gate.py
gate-quick:
	python3 scripts/board/gate.py --quick
gate-baseline:
	python3 scripts/board/gate.py --update-baseline
acceptance:
	bash scripts/board/acceptance.sh
