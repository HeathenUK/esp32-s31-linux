# s31fp wide opportunity survey: PARTIAL, UNVERIFIED (2026-09-28)

**Status.** The run hit the account's weekly usage limit. 44 of 802 agents
completed. The synthesis, the critic and **all three-lens verification** did
not run.

**What completed:**
- all eight censuses (profiles, syscalls, libc, libraries, compiler
  helpers, hardware fit, per-app frame paths, desktop/daemons);
- the coverage critic and six gap searches, together giving 197 raw
  candidates;
- 29 QEMU probes, listed below.

**How to read this.** Every result is a single agent's unreviewed claim.
Treat verdicts and numbers as leads to re-check, not findings. The full
records, with methods, commands and exactness evidence, are in
`partial-results.json`. Probe scratch builds were under `build/cloud/probes/`
(gitignored) and are not preserved here.

| Probe (single agent, unverified) | Verdict |
|---|---|
| alsa-lib route plugin mono->stereo (snd_pcm_route_co | refuted |
| alsa-lib 'linear' rate converter (pcm_rate_linear.c | refuted |
| SDL2 ALSA hotplug thread: snd_device_name_hint() eve | supported |
| ALSA SNDRV_PCM_IOCTL_SYNC_PTR on every avail/hwsync | refuted |
| musl float libm in soft double: expf/logf/powf/hypot | supported |
| musl float libm evaluated in soft double: sinf/cosf/ | supported |
| musl double libm residual: sincos/sin/cos/tan/atan2/ | supported |
| libgcc soft-quad helpers copied into apps (__addtf3/ | supported |
| printf floating conversion (musl fmt_fp in long doub | supported |
| libc memset (musl generic C, not in the xespv set, n | inconclusive |
| musl strlen (generic word loop without Zbb orc.b; n | supported |
| zlib crc32/crc32_z/adler32 | supported |
| musl pthread_mutex_lock/unlock/trylock owner path ( | supported |
| SDL 1.2 software blitters and surface conversion (S | refuted |
| malloc/free (musl mallocng) with internal __lock an | refuted |
| SDL audio format/rate converters (SDL1 SDL_ConvertA | refuted |
| kernel 64-by-32 division: generic __div64_32 bit lo | refuted |
| kernel read(): page-cache copy_to_user path (fallba | refuted |
| kernel string routines without Zbb (XIP disables RI | refuted |
| kernel memset/memcpy (clear_page, copy_page) in XIP | refuted |
| esp32s31_cache_range statistics: 3 generic atomic64 | supported |
| __clock_gettime inside musl (timed waits, timespec_ | supported |
| SDL 1.2 timer thread SDL_Delay(1) poll loop | refuted |
| SBI timer ecall (sbi_set_timer) and other M-mode tr | supported |
| XLITE ring doorbell: eventfd write + ppoll + eventf | refuted |
| mallocng mmap/munmap churn on malloc/free of 4 KB-1 | inconclusive |
| XSync round trip per SDL frame (GetInputFocus) | refuted |
| select(fd+1, {xlite efd}, 0,0, {0,0}) in SDL1 X11_P | supported |
| Per-process timer slack for SDL_Delay/usleep/poll s | refuted |
