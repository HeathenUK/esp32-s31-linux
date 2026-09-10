// SPDX-License-Identifier: GPL-2.0-only
/*
 * How many syscalls does a frame cost? LD_PRELOAD this into the client and
 * send SIGUSR2: it writes per-call, per-fd counts to /tmp/syscount.txt and
 * resets, so two signals bracket a window. Plain %lu everywhere - a 64-bit
 * vararg dump printed garbage on this rv32 target (ioctlprof).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <time.h>

enum { C_READ, C_WRITE, C_READV, C_WRITEV, C_RECVMSG, C_SENDMSG, C_POLL, C_IOCTL, C_N };
static const char *names[C_N] = { "read", "write", "readv", "writev", "recvmsg", "sendmsg", "poll", "ioctl" };
#define NFD 64
static unsigned long cnt[C_N][NFD];
static unsigned long total[C_N];
static unsigned long ns_lo[C_N][NFD];		/* time inside the call, us */
#define NREQ 16
static unsigned long req_code[NREQ], req_cnt[NREQ], req_us[NREQ];

static unsigned long now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000000ul + ts.tv_nsec / 1000;
}
static void tally_time(int c, int fd, unsigned long us)
{
	if (fd >= 0 && fd < NFD)
		ns_lo[c][fd] += us;
}
static void tally_req(unsigned long req, unsigned long us)
{
	int i;
	for (i = 0; i < NREQ; i++) {
		if (req_cnt[i] && req_code[i] != req)
			continue;
		req_code[i] = req; req_cnt[i]++; req_us[i] += us;
		return;
	}
}
static volatile sig_atomic_t dump_now;

static void tally(int c, int fd)
{
	total[c]++;
	if (fd >= 0 && fd < NFD)
		cnt[c][fd]++;
}

static void dump(void)
{
	FILE *f = fopen("/tmp/syscount.txt", "w");
	int c, fd;

	if (!f)
		return;
	for (c = 0; c < C_N; c++)
		fprintf(f, "%-8s total %lu\n", names[c], total[c]);
	for (fd = 0; fd < NFD; fd++) {
		unsigned long s = 0;
		for (c = 0; c < C_N; c++)
			s += cnt[c][fd];
		if (!s)
			continue;
		fprintf(f, "fd %2d:", fd);
		for (c = 0; c < C_N; c++)
			if (cnt[c][fd])
				fprintf(f, " %s=%lu/%luus", names[c], cnt[c][fd], ns_lo[c][fd]);
		fprintf(f, "\n");
	}
	for (c = 0; c < NREQ; c++)
		if (req_cnt[c])
			fprintf(f, "ioctl 0x%lx: %lu calls %lu us\n", req_code[c], req_cnt[c], req_us[c]);
	memset(ns_lo, 0, sizeof(ns_lo));
	memset(req_cnt, 0, sizeof(req_cnt));
	memset(req_us, 0, sizeof(req_us));
	fclose(f);
	memset(cnt, 0, sizeof(cnt));
	memset(total, 0, sizeof(total));
}

static void on_usr2(int sig) { (void)sig; dump_now = 1; }
static void maybe_dump(void) { if (dump_now) { dump_now = 0; dump(); } }

__attribute__((constructor)) static void init(void)
{
	struct sigaction sa = { .sa_handler = on_usr2 };
	sigaction(SIGUSR2, &sa, NULL);
}

#define REAL(name) static typeof(name) *real_##name; if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name)

ssize_t read(int fd, void *b, size_t n) { unsigned long t = now_us(); ssize_t r; REAL(read); tally(C_READ, fd); maybe_dump(); r = real_read(fd, b, n); tally_time(C_READ, fd, now_us() - t); return r; }
ssize_t write(int fd, const void *b, size_t n) { unsigned long t = now_us(); ssize_t r; REAL(write); tally(C_WRITE, fd); maybe_dump(); r = real_write(fd, b, n); tally_time(C_WRITE, fd, now_us() - t); return r; }
ssize_t readv(int fd, const struct iovec *v, int n) { REAL(readv); tally(C_READV, fd); maybe_dump(); return real_readv(fd, v, n); }
ssize_t writev(int fd, const struct iovec *v, int n) { REAL(writev); tally(C_WRITEV, fd); maybe_dump(); return real_writev(fd, v, n); }
ssize_t recvmsg(int fd, struct msghdr *m, int fl) { unsigned long t = now_us(); ssize_t r; REAL(recvmsg); tally(C_RECVMSG, fd); maybe_dump(); r = real_recvmsg(fd, m, fl); tally_time(C_RECVMSG, fd, now_us() - t); return r; }
ssize_t sendmsg(int fd, const struct msghdr *m, int fl) { REAL(sendmsg); tally(C_SENDMSG, fd); maybe_dump(); return real_sendmsg(fd, m, fl); }
int poll(struct pollfd *p, nfds_t n, int t) { unsigned long t0 = now_us(); int r; REAL(poll); tally(C_POLL, n == 1 ? p[0].fd : -1); maybe_dump(); r = real_poll(p, n, t); tally_time(C_POLL, n == 1 ? p[0].fd : -1, now_us() - t0); return r; }
int ioctl(int fd, int req, ...) { va_list ap; void *a; unsigned long t = now_us(), d; int r; REAL(ioctl); va_start(ap, req); a = va_arg(ap, void *); va_end(ap); tally(C_IOCTL, fd); maybe_dump(); r = real_ioctl(fd, req, a); d = now_us() - t; tally_time(C_IOCTL, fd, d); tally_req((unsigned long)req, d); return r; }
