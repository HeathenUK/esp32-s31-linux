// SPDX-License-Identifier: GPL-2.0-only
/*
 * Input-to-pixels latency without perturbing what it measures.
 *
 * deskbench answers the same question by hashing the framebuffer, and that
 * costs 524 kB of PSRAM read per sample polled flat out: measured at 636
 * jiffies in 12 s, about 53% of this single 320 MHz core, against the
 * desktop's 13%. It reports stalls it caused, and hours were spent hunting
 * those stalls in udev, wifi, swap and page cache before the instrument was
 * suspected. Its medians are sound; its tails are its own.
 *
 * This asks the driver instead. Two fields in
 * /sys/kernel/debug/esp32s31_lcd/updates carry everything needed:
 *
 *   updates=N           plane updates so far
 *   last_update_ns=T    when the newest one happened, ktime_get_ns()
 *
 * So the polling rate no longer sets the accuracy. Poll `updates` slowly and
 * cheaply; once it moves, take the latency from the driver's own timestamp,
 * which records when the update actually happened rather than when this
 * process noticed. A 10 ms poll of a ~600 byte seq_file is well under 1% of
 * the core, and the answer is still exact to the driver's clock.
 *
 * ktime_get_ns() and clock_gettime(CLOCK_MONOTONIC) are the same clock, so
 * t_inject and last_update_ns are directly comparable.
 *
 * Requires DIAG=1 - a kernel built with DIAG=0 has no debugfs.
 */

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DBG "/sys/kernel/debug/esp32s31_lcd/updates"
#define MAXTRIAL 200

static int poll_us = 10000;	/* 10 ms; accuracy does not depend on this */

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

/* One read of the driver's counters. Returns 0 on success. */
static int read_state(unsigned long *updates, uint64_t *last_ns)
{
	char buf[2048], *p;
	int fd, n;

	fd = open(DBG, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;

	p = strstr(buf, "updates=");
	if (!p)
		return -1;
	*updates = strtoul(p + 8, NULL, 10);
	p = strstr(buf, "last_update_ns=");
	if (!p)
		return -1;
	*last_ns = strtoull(p + 15, NULL, 10);
	return 0;
}

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev = { .type = type, .code = code, .value = val };

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("uinput");
}

static void tap(int fd, int code)
{
	emit(fd, EV_KEY, code, 1);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	emit(fd, EV_KEY, code, 0);
	emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	int trials = argc > 1 ? atoi(argv[1]) : 25;
	int wait_s = argc > 2 ? atoi(argv[2]) : 4;
	int fd, i, k = 0, timeouts = 0;
	unsigned long u0, u1;
	uint64_t t0, tlast;
	double lat[MAXTRIAL];
	struct uinput_setup us;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (trials > MAXTRIAL)
		trials = MAXTRIAL;

	if (read_state(&u0, &tlast) < 0) {
		fprintf(stderr, "cannot read %s - is debugfs mounted, and is this a DIAG=1 kernel?\n", DBG);
		return 1;
	}

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) { perror("/dev/uinput"); return 1; }
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	for (i = KEY_1; i <= KEY_0; i++)
		ioctl(fd, UI_SET_KEYBIT, i);
	ioctl(fd, UI_SET_KEYBIT, KEY_A);
	ioctl(fd, UI_SET_KEYBIT, KEY_BACKSPACE);
	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor = 0x1d6b;
	us.id.product = 0x0105;
	strcpy(us.name, "latprobe");
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_DEV_CREATE, &fd);
	sleep(wait_s);

	for (i = 0; i < trials; i++) {
		uint64_t deadline;
		int settled = 0;

		/* wait for the panel to be still: no new update for 200 ms */
		deadline = now_ns() + 3000000000ull;
		read_state(&u0, &tlast);
		while (now_ns() < deadline) {
			usleep(poll_us);
			if (read_state(&u1, &tlast) < 0)
				continue;
			if (u1 != u0) { u0 = u1; continue; }
			if (now_ns() - tlast > 200000000ull) { settled = 1; break; }
		}
		if (!settled) { timeouts++; continue; }

		t0 = now_ns();
		tap(fd, KEY_A);

		deadline = t0 + 4000000000ull;
		while (now_ns() < deadline) {
			usleep(poll_us);
			if (read_state(&u1, &tlast) < 0)
				continue;
			if (u1 != u0)
				break;
		}
		if (u1 == u0) { timeouts++; tap(fd, KEY_BACKSPACE); usleep(250000); continue; }

		/*
		 * The driver's timestamp, not ours: when the update actually
		 * happened, so the 10 ms poll does not appear in the answer.
		 */
		if (tlast > t0)
			lat[k++] = (double)(tlast - t0) / 1e6;
		u0 = u1;
		tap(fd, KEY_BACKSPACE);
		usleep(250000);
	}

	if (!k) { printf("latprobe: no samples (%d timeouts)\n", timeouts); return 1; }
	qsort(lat, k, sizeof(*lat), cmp);
	printf("latprobe: n=%d timeouts=%d  median %.1f  p90 %.1f  max %.1f  min %.1f ms\n",
	       k, timeouts, lat[k / 2], lat[(k * 9) / 10 > k - 1 ? k - 1 : (k * 9) / 10],
	       lat[k - 1], lat[0]);
	return 0;
}
