include $(sort $(wildcard $(BR2_EXTERNAL_ESP32_S31_PATH)/package/*/*.mk))

# alsa-lib on musl/rv32 falls back to a SYNC_PTR ioctl for every PCM position
# read (345/s under Doom, ~4% of the core) because the kernel refuses the
# status/control mmap: sound/core/pcm_native.c allows it only on x86, PPC and
# Alpha ("coherent mmap"). Not fixable from userspace - defining
# __USE_TIME_BITS64 here made alsa-lib ask for the 64-bit-time offsets and
# the kernel refused those identically (ENXIO, measured 2026-09-10). Left
# stock. The reducible part is the COUNT, which s31route's period
# negotiation controls.
