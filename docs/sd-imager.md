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

At 115200 baud a 512 MB image compresses to roughly 9 MB and takes about
13 minutes on the wire, plus several minutes for the card write itself. Raising
the console baud rate for the imager kernel is the obvious way to cut that, and
is safe to do because both ends of this particular link are ours.

## Testing it without a board

`imager/sdrecv.c` builds natively, so the protocol can be exercised over a pty
against the real receiver binary - clean transfer, corrupted frames, injected
hart0 log lines, duplicated frames and swallowed acks. Worth re-running after any
change to the framing, because the failure it guards against costs 13 minutes to
observe on real hardware.
