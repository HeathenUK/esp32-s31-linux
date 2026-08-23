// SPDX-License-Identifier: GPL-2.0-only
/*
 * Where does the X server's time go, ioctl by ioctl?
 *
 * This board has no profiling infrastructure at all - no PERF_EVENTS, no
 * PROFILING, no ftrace, no PMU - so attributing X's cost has been guesswork,
 * and guesswork against a +/-40% noise floor produced at least one confident
 * wrong answer.
 *
 * X spends roughly half its time in the kernel and half in userspace during
 * pointer motion. Almost everything it asks the kernel for goes through
 * ioctl(), so interposing that one call splits the two halves exactly, and
 * says which request codes cost what - DRM cursor moves, dirty-fb, input
 * reads and the rest, separated.
 *
 * Build as a shared object and LD_PRELOAD it into the server. Send SIGUSR2 to
 * dump the table to /tmp/ioctlprof.txt and reset it, so a run can be bracketed
 * without restarting X.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SLOTS 64

struct slot {
	unsigned int req;
	unsigned long count;
	unsigned long long ns;
	unsigned long long worst;
};

static struct slot slots[SLOTS];
static unsigned long long other_ns, other_count;
static volatile sig_atomic_t dump_now;
/* musl declares ioctl(int, int, ...) - the request must match exactly. */
static int (*real_ioctl)(int, int, ...);

static unsigned long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void on_usr2(int sig)
{
	(void)sig;
	dump_now = 1;
}

__attribute__((constructor))
static void init(void)
{
	struct sigaction sa;

	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr2;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR2, &sa, NULL);
}

static void dump(void)
{
	char buf[256];
	int fd, i;

	dump_now = 0;
	fd = open("/tmp/ioctlprof.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;

	snprintf(buf, sizeof(buf),
		 "%-12s %8s %12s %10s %10s\n",
		 "request", "count", "total_us", "us/call", "worst_us");
	write(fd, buf, strlen(buf));

	for (i = 0; i < SLOTS; i++) {
		if (!slots[i].count)
			continue;
		snprintf(buf, sizeof(buf),
			 "0x%08x %8lu %12llu %10llu %10llu\n",
			 slots[i].req, slots[i].count,
			 slots[i].ns / 1000,
			 slots[i].ns / 1000 / slots[i].count,
			 slots[i].worst / 1000);
		write(fd, buf, strlen(buf));
	}
	if (other_count) {
		snprintf(buf, sizeof(buf), "overflow     %8llu %12llu\n",
			 other_count, other_ns / 1000);
		write(fd, buf, strlen(buf));
	}
	close(fd);

	memset(slots, 0, sizeof(slots));
	other_ns = other_count = 0;
}

int ioctl(int fd, int req, ...)
{
	unsigned long long t0, d;
	va_list ap;
	void *arg;
	int ret, i;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");

	t0 = now_ns();
	ret = real_ioctl(fd, req, arg);
	d = now_ns() - t0;

	for (i = 0; i < SLOTS; i++) {
		if (slots[i].count && slots[i].req != (unsigned int)req)
			continue;
		if (!slots[i].count)
			slots[i].req = (unsigned int)req;
		slots[i].count++;
		slots[i].ns += d;
		if (d > slots[i].worst)
			slots[i].worst = d;
		goto out;
	}
	other_count++;
	other_ns += d;
out:
	if (dump_now)
		dump();
	return ret;
}
