/*
 * MIT-SHM 1.2 on the wire, tested as a client sees it.
 *
 * The sysroot's XShm.h predates the fd requests, so there is no C API here to
 * call - and inventing one would test our guess rather than the protocol. The
 * wire format IS the interface an off-the-shelf client uses, so this speaks it
 * directly: connection setup, QueryExtension, ShmQueryVersion, then
 * ShmCreateSegment (server allocates, hands back an fd) and ShmAttachFd
 * (client allocates a memfd, hands it over).
 *
 * Every step is followed by a GetInputFocus round trip. That is the standard
 * way to make an asynchronous X error land somewhere you can see it: errors
 * for earlier requests are delivered before the reply to a later one, so if
 * the reply comes back clean, everything before it was accepted.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

static int sk;
static uint32_t idbase, idmask, nextid;
static int fails;

static void p16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void p32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint16_t g16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t g32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int rdall(void *b, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(sk, (char *)b + got, n - got);
		if (r <= 0)
			return -1;
		got += r;
	}
	return 0;
}

/* Read one 32-byte reply/error. Returns 1 reply, 0 error, -1 dead.
 * If gotfd is non-NULL, collect an SCM_RIGHTS descriptor riding with it. */
static int rdreply(uint8_t *h, int *gotfd)
{
	struct msghdr m;
	struct iovec io;
	union { struct cmsghdr al; char b[CMSG_SPACE(sizeof(int))]; } u;
	struct cmsghdr *cm;
	ssize_t r;

	if (gotfd)
		*gotfd = -1;
	io.iov_base = h; io.iov_len = 32;
	memset(&m, 0, sizeof m); memset(&u, 0, sizeof u);
	m.msg_iov = &io; m.msg_iovlen = 1;
	m.msg_control = u.b; m.msg_controllen = sizeof u.b;
	r = recvmsg(sk, &m, 0);
	if (r <= 0)
		return -1;
	while (r < 32) {			/* short read, finish it */
		ssize_t k = read(sk, h + r, 32 - r);
		if (k <= 0)
			return -1;
		r += k;
	}
	for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
		if (cm->cmsg_level == SOL_SOCKET &&
		    cm->cmsg_type == SCM_RIGHTS && gotfd)
			memcpy(gotfd, CMSG_DATA(cm), sizeof(int));
	if (h[0] == 0) {
		fprintf(stderr, "  X error: code %u, major %u minor %u\n",
			h[1], h[10], g16(h + 8));
		return 0;
	}
	return 1;
}

/* GetInputFocus: a reply proves everything queued before it was accepted. */
static int sync_ok(const char *what)
{
	uint8_t r[4], h[32];

	r[0] = 43; r[1] = 0; p16(r + 2, 1);
	if (write(sk, r, 4) != 4)
		return 0;
	if (rdreply(h, NULL) != 1) {
		printf("FAIL %s\n", what);
		fails++;
		return 0;
	}
	printf("ok   %s\n", what);
	return 1;
}

int main(void)
{
	struct sockaddr_un a;
	uint8_t setup[12], hd[8], *rest, req[32], h[32];
	uint32_t seg1, seg2;
	int shmmajor, fd, mfd;
	void *p;
	unsigned len;

	memset(setup, 0, sizeof setup);
	setup[0] = 'l'; p16(setup + 2, 11); p16(setup + 4, 0);

	sk = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, "/tmp/.X11-unix/X0");
	if (connect(sk, (struct sockaddr *)&a, sizeof a) < 0) {
		perror("connect"); return 1;
	}
	if (write(sk, setup, 12) != 12 || rdall(hd, 8) < 0) {
		perror("setup"); return 1;
	}
	if (hd[0] != 1) {
		fprintf(stderr, "setup refused (%u)\n", hd[0]); return 1;
	}
	len = g16(hd + 6) * 4;
	rest = malloc(len);
	if (rdall(rest, len) < 0) { perror("setup body"); return 1; }
	idbase = g32(rest + 4);
	idmask = g32(rest + 8);
	nextid = idbase;
	printf("connected: id base 0x%x mask 0x%x\n", idbase, idmask);

	/* --- QueryExtension "MIT-SHM" --- */
	memset(req, 0, sizeof req);
	req[0] = 98; p16(req + 2, 5); p16(req + 4, 7);
	memcpy(req + 8, "MIT-SHM", 7);
	if (write(sk, req, 20) != 20) return 1;
	if (rdreply(h, NULL) != 1) { printf("FAIL QueryExtension\n"); return 1; }
	if (!h[8]) { printf("FAIL MIT-SHM not present\n"); return 1; }
	shmmajor = h[9];
	printf("ok   MIT-SHM present, major opcode %d\n", shmmajor);

	/* --- ShmQueryVersion: must say 1.2 to justify what follows --- */
	memset(req, 0, sizeof req);
	req[0] = shmmajor; req[1] = 0; p16(req + 2, 1);
	if (write(sk, req, 4) != 4) return 1;
	if (rdreply(h, NULL) != 1) { printf("FAIL ShmQueryVersion\n"); return 1; }
	printf("ok   version %u.%u, sharedPixmaps=%u, pixmapFormat=%u\n",
	       g16(h + 8), g16(h + 10), h[1], h[16]);
	if (g16(h + 8) != 1 || g16(h + 10) < 2) {
		printf("FAIL server does not claim 1.2\n");
		fails++;
	}

	/* --- ShmCreateSegment: the SERVER allocates and returns an fd --- */
	seg1 = nextid++;
	memset(req, 0, sizeof req);
	req[0] = shmmajor; req[1] = 7; p16(req + 2, 4);
	p32(req + 4, seg1); p32(req + 8, 64000); req[12] = 0;
	if (write(sk, req, 16) != 16) return 1;
	if (rdreply(h, &fd) != 1) {
		printf("FAIL ShmCreateSegment\n"); fails++;
	} else if (h[1] != 1 || fd < 0) {
		printf("FAIL ShmCreateSegment: nfd=%u fd=%d\n", h[1], fd);
		fails++;
	} else {
		p = mmap(NULL, 64000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			printf("FAIL mmap of returned fd: %s\n",
			       strerror(errno));
			fails++;
		} else {
			memset(p, 0xA5, 64000);	/* must be writable */
			printf("ok   ShmCreateSegment: fd %d, mapped and "
			       "written\n", fd);
			munmap(p, 64000);
		}
		close(fd);
	}
	sync_ok("no error after ShmCreateSegment");

	/* --- ShmAttachFd: the CLIENT allocates and passes an fd --- */
	mfd = memfd_create("shmfdtest", 0);
	if (mfd < 0 || ftruncate(mfd, 64000) < 0) {
		printf("FAIL memfd_create: %s\n", strerror(errno));
		return 1;
	}
	seg2 = nextid++;
	{
		struct msghdr m;
		struct iovec io;
		union { struct cmsghdr al; char b[CMSG_SPACE(sizeof(int))]; } u;
		struct cmsghdr *cm;

		memset(req, 0, sizeof req);
		req[0] = shmmajor; req[1] = 6; p16(req + 2, 3);
		p32(req + 4, seg2); req[8] = 0;
		io.iov_base = req; io.iov_len = 12;
		memset(&m, 0, sizeof m); memset(&u, 0, sizeof u);
		m.msg_iov = &io; m.msg_iovlen = 1;
		m.msg_control = u.b; m.msg_controllen = sizeof u.b;
		cm = CMSG_FIRSTHDR(&m);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &mfd, sizeof mfd);
		if (sendmsg(sk, &m, 0) < 0) {
			perror("sendmsg ShmAttachFd"); return 1;
		}
	}
	close(mfd);
	sync_ok("ShmAttachFd accepted");

	/* --- ShmDetach both, and prove the server survives it --- */
	memset(req, 0, sizeof req);
	req[0] = shmmajor; req[1] = 2; p16(req + 2, 2); p32(req + 4, seg1);
	if (write(sk, req, 8) != 8) return 1;
	p32(req + 4, seg2);
	if (write(sk, req, 8) != 8) return 1;
	sync_ok("ShmDetach of both segments");

	/* --- an op we do NOT implement must ERROR, not hang --- */
	memset(req, 0, sizeof req);
	req[0] = shmmajor; req[1] = 4; p16(req + 2, 1);	/* ShmGetImage */
	if (write(sk, req, 4) != 4) return 1;
	if (rdreply(h, NULL) == 0)
		printf("ok   unimplemented op errors instead of hanging\n");
	else {
		printf("FAIL unimplemented op did not error\n");
		fails++;
	}

	printf(fails ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", fails);
	return fails ? 1 : 0;
}
