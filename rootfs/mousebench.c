// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pointer-motion cost, with nobody at the mouse.
 *
 * Creates a uinput mouse, injects relative motion at a fixed rate, and reads
 * the LCD driver's commit-path counters either side of the run. The point is
 * to split one number - "the cursor renders at 2 fps" - into the four places
 * the time could actually be going:
 *
 *	events injected		what the compositor was asked to do
 *	updates			how many times it committed a frame
 *	upd_ns			time inside the driver's commit path
 *	flush_ns / ppa_ns	the two things that path spends time on
 *
 * If updates is far below the injected rate but upd_ns per update is small,
 * the driver is not the bottleneck and the compositor is not producing frames.
 * That distinction is the whole reason this exists.
 *
 * Usage: mousebench [events] [rate_hz]
 */

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LCD_DEBUGFS "/sys/kernel/debug/esp32s31_lcd/updates"

struct stats {
	unsigned long long upd_ns, upd_ns_max, flush_ns, flush_bytes;
	unsigned long long ppa_ns, gap_ns_max;
	unsigned int updates, flushes, ppa_ops;
};

static int read_stats(struct stats *s)
{
	char line[1024];
	FILE *f = fopen(LCD_DEBUGFS, "r");

	if (!f)
		return -1;
	memset(s, 0, sizeof(*s));
	while (fgets(line, sizeof(line), f)) {
		char *p;

		p = strstr(line, "updates=");
		if (p)
			sscanf(p, "updates=%u", &s->updates);
		p = strstr(line, "upd_ns=");
		if (p)
			sscanf(p, "upd_ns=%llu upd_ns_max=%llu flushes=%u "
				  "flush_ns=%llu flush_ns_max=%*u "
				  "flush_bytes=%llu ppa_ops=%u ppa_ns=%llu "
				  "ppa_ns_max=%*u gap_ns=%*u gap_ns_max=%llu",
			       &s->upd_ns, &s->upd_ns_max, &s->flushes,
			       &s->flush_ns, &s->flush_bytes, &s->ppa_ops,
			       &s->ppa_ns, &s->gap_ns_max);
	}
	fclose(f);
	return 0;
}

static void emit(int fd, unsigned short type, unsigned short code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("write");
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int events = argc > 1 ? atoi(argv[1]) : 500;
	int rate = argc > 2 ? atoi(argv[2]) : 125;
	struct uinput_setup us;
	struct stats a, b;
	double t0, t1;
	int fd, i, dx = 4;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/uinput");
		return 1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_RELBIT, REL_X);
	ioctl(fd, UI_SET_RELBIT, REL_Y);
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);

	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor = 0x1234;
	us.id.product = 0x5679;
	strcpy(us.name, "mousebench-virtual-mouse");
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_DEV_CREATE);

	/* Let the compositor notice the new device and settle. */
	sleep(3);

	if (read_stats(&a)) {
		fprintf(stderr, "cannot read %s\n", LCD_DEBUGFS);
		return 1;
	}
	t0 = now_s();

	for (i = 0; i < events; i++) {
		/* Bounce so the pointer stays on screen rather than pinning
		 * to an edge, where the compositor may stop damaging. */
		if (i % 50 == 0)
			dx = -dx;
		emit(fd, EV_REL, REL_X, dx);
		emit(fd, EV_REL, REL_Y, (i % 7) - 3);
		emit(fd, EV_SYN, SYN_REPORT, 0);
		usleep(1000000 / rate);
	}

	t1 = now_s();
	/* Give the last frame time to land. */
	usleep(300000);
	read_stats(&b);

	ioctl(fd, UI_DEV_DESTROY);
	close(fd);

	{
		unsigned int upd = b.updates - a.updates;
		double secs = t1 - t0;
		unsigned long long dupd = b.upd_ns - a.upd_ns;
		unsigned long long dfl = b.flush_ns - a.flush_ns;
		unsigned long long dppa = b.ppa_ns - a.ppa_ns;
		unsigned long long dby = b.flush_bytes - a.flush_bytes;

		printf("injected      %d events in %.2f s (%.1f/s)\n",
		       events, secs, events / secs);
		printf("updates       %u  (%.2f fps)\n", upd,
		       secs > 0 ? upd / secs : 0);
		if (!upd) {
			printf("NO COMMITS - the compositor never repainted\n");
			return 0;
		}
		printf("commit path   %.3f ms/update total, max %.3f ms\n",
		       dupd / 1e6 / upd, b.upd_ns_max / 1e6);
		printf("  cache flush %.3f ms/update  (%llu bytes/update)\n",
		       dfl / 1e6 / upd, dby / upd);
		printf("  ppa scale   %.3f ms/update\n", dppa / 1e6 / upd);
		printf("driver share  %.1f%% of wall clock\n",
		       100.0 * dupd / 1e9 / secs);
		printf("worst gap between updates: %.1f ms\n",
		       b.gap_ns_max / 1e6);
	}
	return 0;
}
