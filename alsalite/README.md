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

Worse than the size is **where it lives**. It is in `XIP_SKIP`, so it sits on
the SD card as ordinary page cache rather than in XIP flash. The recorded
reason (docs/current-state.md) is that the two XIP images total 8,205,052
bytes against 7,602,176 of partition, and that libasound is "NEEDED by lvdesk
but only for the volume mixer and occasional PCM writes". That reasoning
weighed lvdesk's volume slider and missed that **every audio client dlopens
the same library and calls into it on every period**. The result is that 943 kB
of the audio path is evictable page cache on the card, so a page can be
re-read from SD in the middle of playback - a plausible mechanism for
intermittent crackling, and one nothing in the current design prevents.

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
