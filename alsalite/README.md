# alsalite - our own libasound.so.2

The same argument as `xlite` for libX11, and the numbers are more lopsided.

## Why

`libasound.so.2.0.0` is **943,548 bytes**. Measured with
`nm -D --undefined-only` against what actually references it:

| caller | snd_ symbols referenced |
|---|---|
| libSDL-1.2 | 0 statically - it **dlopens** ALSA (`SDL_ALSA_DYNAMIC`) and resolves 30 functions by name |
| prboom | 0 |
| lvdesk | 22 (16 mixer, 6 PCM) |
| our s31route plugin | 33, including the ioplug machinery |

So the whole surface is about 50 functions, and roughly 30 of them are the
hw_params/sw_params setters, which are pure bookkeeping over one ioctl.

**RETRACTED, 2026-09-12: libasound is NOT on the SD card.** The Makefile's
`XIP_SKIP` names it, and the docs record the reasoning for leaving it on the
card, so I asserted that its pages were evictable page cache sitting in the
middle of the audio path and offered that as a mechanism for crackling. The
board says otherwise: `/mnt/xip2/usr/lib/libasound.so.2.0.0` exists, so it is
in XIP image 2, in flash, at zero RSS and not evictable. `XIP_SKIP` evidently
only governs image 1's staging. The eviction story was wrong and there is no
crackle mechanism there.

What survives is the size argument alone, and it is weaker on its own: 943 kB
of flash serving about 50 functions. Also on the record against it: the
`SYNC_PTR` ioctl in libasound's hw layer was measured at ~10% of the audio
path (docs/current-state.md).

And an on-CPU page profile of prboom WITH SOUND does not show libasound at
all - not one page above 0.3% of 1500 samples - nor s31route. The audio cost
is inside prboom's own mixer and in syscalls. So this shim is a memory and
tidiness argument, not a performance one, and it should not be sold as a
performance fix.

Already measured against it: the `SYNC_PTR` ioctl in libasound's hw layer was
~10% of the audio path (docs/current-state.md).

## What replacing it also deletes

`s31route` exists only because it is an ALSA **plugin**, and a plugin needs
libasound's config machinery to load it. Its actual job - follow
`/run/s31-sink`, mirror the app's ring, and upsample 22050 to the codec's
44100 by frame repeat - is a hundred lines that belong in the write path
itself. Its `S31ROUTE_DIRECT` mode already talks to the hw device directly,
which is what this does for everything.

## Surface to implement

PCM, from SDL 1.2's dlsym list plus lvdesk's six:

    open close nonblock prepare drain drop recover wait writei delay
    start state poll_descriptors poll_descriptors_revents set_params
    hw_params hw_params_any hw_params_copy hw_params_sizeof
    hw_params_set_{access,format,channels,rate,rate_near,
                   period_size_near,periods_near,buffer_size_near}
    hw_params_get_{buffer_size,channels,period_size,periods}
    hw_params_test_rate
    sw_params sw_params_current sw_params_sizeof
    sw_params_set_{avail_min,start_threshold}
    strerror

Mixer, from lvdesk (drives the codec through the kernel control interface):

    mixer_{open,close,attach,load,handle_events,first_elem,elem_next}
    mixer_selem_{register,id_sizeof,is_active,has_playback_volume,
                 get_playback_volume,get_playback_volume_range,
                 set_playback_volume_all,get_playback_d,set_playback_d}

## Status

NOT YET IMPLEMENTED. This directory holds the design and the measured case
for it. Do not delete libasound from the image until the shim passes a
by-ear check: this project's own rule is that audio instruments have called a
pure-noise stream "working" four times.
