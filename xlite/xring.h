/*
 * XLITE-RING: the X byte stream in shared memory instead of an af_unix socket.
 *
 * WHY. Both ends of the socket are ours - xlite is the libX11 every client
 * links, xshim is the server - and a socket syscall on this board costs
 * 1.3-2.1 ms (docs/current-state.md, "socket syscalls cost ms"; the cost is
 * structural, .text..fast on the whole net spine measured zero). The pixels
 * already bypass it (MIT-SHM, XLITE-SHM); the requests and replies did not,
 * and a windowed SDL frame is a ShmPutImage + an XSync round trip: measured
 * 2026-09-19 at ~6 ms of xshim per frame and 11% of prboom's wall time
 * blocked in recvmsg waiting for the reply.
 *
 * WHAT. One memfd per connection holding two single-producer/single-consumer
 * byte rings (client->server requests, server->client replies and events),
 * plus one eventfd per direction as the doorbell. The rings carry exactly the
 * bytes the socket carried, so nothing above the transport changes. The
 * socket stays open: it carries the negotiation, SCM_RIGHTS descriptors
 * (with a one-byte dummy payload), and its HUP is how each side learns the
 * other died.
 *
 * NEGOTIATION. QueryExtension("XLITE-RING"), then request {major, minor 1}.
 * The 32-byte reply arrives on the SOCKET with three descriptors attached
 * (memfd, c2s eventfd, s2c eventfd); everything after it, both ways, is on
 * the rings. The Attach is a round trip, so nothing is in flight at the
 * switch. Anything that is not xlite never asks and keeps the socket.
 * XLITE_RING=0 (client) or XSHIM_RING=0 (server) turns it off.
 *
 * DOORBELLS. Eventfds are O_NONBLOCK and are polled, never read blocking.
 * A writer advances head, then - only if `sig` is clear - sets `sig` and
 * writes the eventfd. A reader that finds `sig` set DRAINS THE EVENTFD FIRST
 * and clears `sig` second, then drains the ring. The reader's order is
 * load-bearing: clear-then-drain lets a writer's bell be swallowed while
 * `sig` stays set, and its next message is then never announced. A stale count only costs
 * one spurious wake-up, and an empty-ring check costs no syscall at all -
 * which is what makes XPending() free. `full` is set by a writer that ran
 * out of room; the reader clears it after consuming and rings the writer's
 * INBOUND doorbell so it retries.
 *
 * TRUST. The header lives in memory the client can write. The server never
 * reads sizes or offsets from it - they are the constants below - and treats
 * head - tail > size as a dead client.
 */
#ifndef XRING_H
#define XRING_H

#include <stdint.h>
#include <string.h>

#define XRING_NAME		"XLITE-RING"
#define XRING_MAGIC		0x31474e52u	/* "RNG1" */
#define XRING_HDR_BYTES		4096u
#define XRING_C2S_SIZE		(64u << 10)	/* powers of two */
#define XRING_S2C_SIZE		(32u << 10)
#define XRING_C2S_OFF		XRING_HDR_BYTES
#define XRING_S2C_OFF		(XRING_HDR_BYTES + XRING_C2S_SIZE)
#define XRING_TOTAL		(XRING_HDR_BYTES + XRING_C2S_SIZE + XRING_S2C_SIZE)

struct xring_dir {
	uint32_t head;		/* bytes ever written; writer only */
	uint32_t tail;		/* bytes ever read; reader only */
	uint32_t sig;		/* writer rang the doorbell since the last drain */
	uint32_t full;		/* writer is waiting for room */
	uint32_t pad[12];	/* keep the two directions on separate lines */
};

struct xring_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t pad[14];
	struct xring_dir c2s;
	struct xring_dir s2c;
};

#define XR_LOAD(v)	__atomic_load_n(&(v), __ATOMIC_SEQ_CST)
#define XR_STORE(v, x)	__atomic_store_n(&(v), (x), __ATOMIC_SEQ_CST)

/* Bytes waiting for the reader; > size means the peer corrupted the header. */
static inline uint32_t xring_used(struct xring_dir *d)
{
	return XR_LOAD(d->head) - XR_LOAD(d->tail);
}

/* Copy in as much of [p, p+n) as fits; returns the count. Writer side. */
static inline uint32_t xring_write(struct xring_dir *d, uint8_t *data,
				   uint32_t size, const void *p, uint32_t n)
{
	uint32_t head = XR_LOAD(d->head);
	uint32_t used = head - XR_LOAD(d->tail);
	uint32_t room, pos, first;

	if (used > size)
		return 0;
	room = size - used;
	if (n > room)
		n = room;
	if (!n)
		return 0;
	pos = head & (size - 1);
	first = size - pos;
	if (first > n)
		first = n;
	memcpy(data + pos, p, first);
	if (n > first)
		memcpy(data, (const uint8_t *)p + first, n - first);
	XR_STORE(d->head, head + n);
	return n;
}

/* Copy out up to n bytes; returns the count. Reader side. */
static inline uint32_t xring_read(struct xring_dir *d, const uint8_t *data,
				  uint32_t size, void *p, uint32_t n)
{
	uint32_t tail = XR_LOAD(d->tail);
	uint32_t used = XR_LOAD(d->head) - tail;
	uint32_t pos, first;

	if (used > size)
		return 0;
	if (n > used)
		n = used;
	if (!n)
		return 0;
	pos = tail & (size - 1);
	first = size - pos;
	if (first > n)
		first = n;
	memcpy(p, data + pos, first);
	if (n > first)
		memcpy((uint8_t *)p + first, data, n - first);
	XR_STORE(d->tail, tail + n);
	return n;
}

#endif
