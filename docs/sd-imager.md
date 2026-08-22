# Writing the microSD card over the serial console

The Korvo-1's microSD slot is soldered to the board and the card is not meant to
be shuttled to a host reader every time the root filesystem changes. This is a
way to write the card in place, over the console cable that is already attached.

It is a *replacement* kernel, flashed only when a card needs writing and flashed
back afterwards. Nothing about the normal system changes, and no initramfs is
carried around permanently for a job that happens occasionally.

## Why streaming

The image is 512 MB and the board has 16 MB of RAM, of which about 14 MB is
usable. There is nowhere to stage the image - not in RAM, and not in the 16 MB
of flash that already holds the bootloader, OpenSBI, the kernel and the fallback
root filesystem. So it is never stored: the receiver, the decompressor and `dd`
run as one pipeline, and each block is written to the card as it arrives.

The card is never mounted. The imager owns `/dev/mmcblk0` exclusively and writes
from block zero, which is what lets it write a blank or half-written card.

## Why it is framed rather than a bare pipe

Two constraints shape the protocol:

- **hart0 shares the console.** It prints its own log lines at unpredictable
  moments, straight into the middle of the byte stream. So every frame carries a
  magic, and the receiver resynchronises by hunting for it; a frame that gets a
  log line spliced into it fails CRC and is resent.
- **The UART has no flow control, and SD cards stall.** An erase block can take
  hundreds of milliseconds, during which a free-running sender would overrun the
  link. Each frame is acknowledged, so the sender waits for the card.

A frame is `"S31I" | seq(le32) | len(le32) | crc32(le32) | payload`, and a
zero-length frame ends the stream.

The receiver re-acknowledges a frame it has already consumed rather than
rejecting it. This matters: without it, an ack lost on the wire deadlocks the
transfer, with the sender waiting for an ack of frame N while the receiver will
accept nothing but N+1.

## The imager kernel must fit the linux partition

`make imager` builds a *second* kernel and flashes it over the normal one at the
`linux` partition offset. Nothing checks the size at flash time - only `make
linux` does - so an oversized imager kernel flashes happily and runs off the end
of its partition:

    0x400000 + 6,570,053 = 0xA44045      rootfs starts at 0xA00000

That silently destroys the first 278 KB of the userspace XIP image, which then
fails to mount on the next boot for reasons that have nothing to do with the
card. If `make imager` reports the image exceeding the partition, **do not flash
it anyway**.

It got oversized for a structural reason worth knowing: the `linux` rule used to
apply its whole `scripts/config` list unconditionally, so selecting
`esp32s31_imager_defconfig` had DRM, Wi-Fi, HID, CMA, CRAMFS and PROFILING
switched straight back on over the top. The imager only writes an SD card and
needs none of it. The config is now split - `LINUX_BASE_CONFIG` is what any
kernel for this board needs, `LINUX_FEATURE_CONFIG` is the running system's
feature set, and the imager passes the base set alone.

## Use

    # 1. Build the imager kernel (embeds the initramfs) and flash it
    make imager
    make flash-imager

    # 2. Stream the image; the board is reset and announces IMAGER_READY
    imager/send_image.py build/buildroot/images/rootfs.ext2 --port /dev/cu.usbserial-130

    # 3. Put the normal kernel back
    make flash-linux

Step 2 prints progress and finishes by comparing checksums. The board reads back
exactly as many bytes as `dd` reported writing and hashes them; the host hashes
the file it sent. `VERIFIED` means the card holds the image byte for byte - it is
not inferred from the transfer completing.

Measured on a 512 MB image, which compresses to about 9 MB:

| console rate | wire rate | whole image |
|---|---|---|
| 115200 | 10.7 KiB/s | 19.7 min |
| 1 Mbps | 70 KiB/s early, 18 KiB/s average | 8.4 min |

The average is well below the peak because the tail of the image is all zeros:
those frames decompress to megabytes each, so the card becomes the limit rather
than the link, and no amount of baud rate helps there. Expect roughly 2x from
the rate change end to end, not the 8.7x the wire alone suggests.

Stop-and-wait costs more at the higher rate too - a 4 KB frame takes 41 ms at
1 Mbps against 355 ms at 115200, so the per-frame turnaround stops being noise.
A sliding window would recover some of that, at the cost of the flow control
that the ack currently provides for free against a stalling card.

## Testing it without a board

`imager/sdrecv.c` builds natively, so the protocol can be exercised over a pty
against the real receiver binary - clean transfer, corrupted frames, injected
hart0 log lines, duplicated frames and swallowed acks. Worth re-running after any
change to the framing, because the failure it guards against costs 13 minutes to
observe on real hardware.
