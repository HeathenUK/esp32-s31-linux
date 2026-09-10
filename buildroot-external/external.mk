include $(sort $(wildcard $(BR2_EXTERNAL_ESP32_S31_PATH)/package/*/*.mk))

# alsa-lib on musl/rv32: its uapi header picks the 64-bit-time PCM mmap
# offsets and struct layouts only under glibc's __USE_TIME_BITS64. musl has
# 64-bit time_t but never defines that macro, so alsa-lib asked the kernel
# for the OLD status/control mmap - which a 32-bit kernel refuses outright
# (sound/core/pcm_native.c: !IS_ENABLED(CONFIG_64BIT) -> -ENXIO) - and fell
# back to a SYNC_PTR ioctl on every position read: 345 ioctls/s under Doom,
# ~4% of the core. A build flag, not a patch; the header is written for it.
ALSA_LIB_CFLAGS += -D__USE_TIME_BITS64
