// SPDX-License-Identifier: GPL-2.0-only
/*
 * Framed receiver for streaming an image over the serial console.
 *
 * The console is shared with hart0, which prints its own log lines at any
 * moment, so the stream cannot be a bare byte pipe: every frame is checksummed
 * and the receiver resynchronises by hunting for the magic. A frame is
 *
 *     "S31I" | seq (le32) | len (le32) | crc32 (le32) | payload
 *
 * and each one is acknowledged on the ack fd so the sender waits rather than
 * overrunning a UART with no flow control - the card stalls for hundreds of
 * milliseconds on erase blocks, which is ample to lose data otherwise.
 *
 * Payload goes to stdout, so the caller pipes it: sdrecv | gunzip | dd ...
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAGIC      "S31I"
#define MAX_PAYLOAD (64 * 1024)

static uint32_t crc32_of(const unsigned char *p, unsigned int len)
{
	uint32_t crc = 0xffffffffu;
	unsigned int i, b;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320u & (-(int32_t)(crc & 1)));
	}
	return ~crc;
}

/* Blocking read of exactly n bytes; 0 on EOF. */
static int read_exact(int fd, void *buf, unsigned int n)
{
	unsigned char *p = buf;
	unsigned int got = 0;

	while (got < n) {
		ssize_t r = read(fd, p + got, n - got);

		if (r <= 0)
			return 0;
		got += (unsigned int)r;
	}
	return 1;
}

/* Downstream is a pipe into gunzip, which can accept a write partially. */
static int write_all(int fd, const void *buf, unsigned int n)
{
	const unsigned char *p = buf;
	unsigned int done = 0;

	while (done < n) {
		ssize_t w = write(fd, p + done, n - done);

		if (w <= 0)
			return 0;
		done += (unsigned int)w;
	}
	return 1;
}

int main(int argc, char **argv)
{
	int ack_fd = argc > 1 ? atoi(argv[1]) : 2;
	static unsigned char payload[MAX_PAYLOAD];
	unsigned char hdr[16];
	unsigned int matched = 0;
	uint32_t expect_seq = 0;
	char ack[32];

	for (;;) {
		unsigned char c;
		uint32_t seq, len, crc;

		/* Hunt for the frame magic, skipping anything hart0 injected. */
		if (!read_exact(0, &c, 1))
			break;
		if (c != (unsigned char)MAGIC[matched]) {
			matched = (c == (unsigned char)MAGIC[0]) ? 1 : 0;
			continue;
		}
		if (++matched < 4)
			continue;
		matched = 0;

		if (!read_exact(0, hdr, 12))
			break;
		seq = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
		len = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
		crc = hdr[8] | hdr[9] << 8 | hdr[10] << 16 | (uint32_t)hdr[11] << 24;

		if (len > MAX_PAYLOAD) {
			snprintf(ack, sizeof(ack), "NAK %lu\n",
				 (unsigned long)expect_seq);
			write(ack_fd, ack, strlen(ack));
			continue;
		}
		if (len && !read_exact(0, payload, len))
			break;

		if (crc32_of(payload, len) != crc) {
			snprintf(ack, sizeof(ack), "NAK %lu\n",
				 (unsigned long)expect_seq);
			write(ack_fd, ack, strlen(ack));
			continue;
		}

		/*
		 * A frame we already consumed. This is the normal outcome of an
		 * ack lost on the wire, or of the sender timing out while the SD
		 * card stalls on an erase block: re-acknowledge it and drop the
		 * payload, because writing it twice would duplicate data in the
		 * stream. Without this the transfer deadlocks - the sender waits
		 * for an ack of seq N while the receiver refuses everything but
		 * N+1.
		 */
		if (seq + 1 == expect_seq) {
			snprintf(ack, sizeof(ack), "ACK %lu\n",
				 (unsigned long)seq);
			write(ack_fd, ack, strlen(ack));
			continue;
		}
		if (seq != expect_seq) {
			snprintf(ack, sizeof(ack), "NAK %lu\n",
				 (unsigned long)expect_seq);
			write(ack_fd, ack, strlen(ack));
			continue;
		}

		if (len == 0) {			/* zero-length frame ends the stream */
			snprintf(ack, sizeof(ack), "ACK %lu\n",
				 (unsigned long)seq);
			write(ack_fd, ack, strlen(ack));
			break;
		}
		if (!write_all(1, payload, len))
			return 1;
		expect_seq++;
		snprintf(ack, sizeof(ack), "ACK %lu\n", (unsigned long)seq);
		write(ack_fd, ack, strlen(ack));
	}
	return 0;
}
