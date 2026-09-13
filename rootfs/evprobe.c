/*
 * evprobe - what the pointer's event stream actually looks like, and how
 * stale it is by the time userspace sees it.
 *
 * WHY. The reported symptoms are (a) a diagonal move that lands on one axis
 * only, and (b) a pause before a move or click registers. Those have very
 * different causes and the same feel, so guessing between them is how weeks
 * go by. This separates them by measuring, at the evdev node, four things
 * nothing else here reports:
 *
 *  1. PACKET SHAPE. A mouse reports a movement as REL_X and/or REL_Y followed
 *     by SYN_REPORT. If diagonals arrive as single-axis packets, the device
 *     or the HID layer is splitting them and no amount of work above can
 *     recombine them faithfully. If they arrive paired, anything that loses
 *     an axis is losing it upstream in the desktop.
 *
 *  2. STALENESS. Every event carries a kernel timestamp. With EVIOCSCLOCKID
 *     set to CLOCK_MONOTONIC we can subtract it from now() at the moment we
 *     read it, which is exactly "how long did this sit in the buffer".
 *     That is the lag, measured rather than felt.
 *
 *  3. SYN_DROPPED. The kernel emits this when a client's buffer overflows and
 *     events were discarded for that client. It is the definitive answer to
 *     "did something get lost", and it is per-client, so a slow reader
 *     punishes only itself.
 *
 *  4. GAPS. Time between consecutive packets, bucketed. A device that stops
 *     reporting mid-gesture looks different from a reader that stops reading.
 *
 * It opens READ-ONLY and never grabs, so it cannot take events away from the
 * desktop: evdev gives every open file description its own copy.
 *
 * IT WATCHES EVERY POINTER AT ONCE, which is the point on this board. The
 * 8BitDo Retro Keyboard Receiver advertises REL_X and REL_Y and so is opened
 * as a mouse alongside the real one - two devices driving a single cursor.
 * A stray report from either yanks it, and from the outside that feels like
 * "something is fighting me". Attributing motion to a named device is the
 * whole diagnosis, so the summary is per device.
 *
 * It polls to a deadline rather than blocking in read(): the first version
 * blocked, so a silent device was indistinguishable from a hung probe, and
 * it printed nothing at all for a device that was simply quiet.
 *
 *   evprobe [seconds]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

#ifndef EVIOCSCLOCKID
#define EVIOCSCLOCKID _IOW('E', 0xa0, int)
#endif

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int has_rel_xy(int fd)
{
	unsigned long bits[(REL_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];

	memset(bits, 0, sizeof bits);
	if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof bits), bits) < 0)
		return 0;
#define BIT_SET(b, n) ((b)[(n) / (8 * sizeof(long))] & (1UL << ((n) % (8 * sizeof(long)))))
	return BIT_SET(bits, REL_X) && BIT_SET(bits, REL_Y);
}

struct dev {
	int fd;
	char name[128];
	char path[64];
	int n_x, n_y;
	unsigned both, x_only, y_only, none, dropped, buttons, packets;
	uint64_t last_syn, stale_sum, stale_max;
	unsigned stale_n;
	unsigned gap[6];
};

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 20;
	struct dev d[8];
	struct pollfd pfd[8];
	int nd = 0, i, k;
	uint64_t t_end;

	memset(d, 0, sizeof d);
	for (i = 0; i < 32 && nd < 8; i++) {
		char path[64];
		int fd, clk = CLOCK_MONOTONIC;

		snprintf(path, sizeof path, "/dev/input/event%d", i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (!has_rel_xy(fd)) { close(fd); continue; }
		d[nd].fd = fd;
		snprintf(d[nd].path, sizeof d[nd].path, "%s", path);
		ioctl(fd, EVIOCGNAME(sizeof d[nd].name), d[nd].name);
		/* Same clock the stamps will use; see the note above. */
		ioctl(fd, EVIOCSCLOCKID, &clk);
		pfd[nd].fd = fd;
		pfd[nd].events = POLLIN;
		printf("evprobe: watching %s \"%s\"\n", path, d[nd].name);
		nd++;
	}
	if (!nd) {
		fprintf(stderr, "evprobe: no device reports REL_X and REL_Y\n");
		return 1;
	}
	printf("evprobe: %d device(s), %d s - use the pointer now\n", nd, secs);
	fflush(stdout);
	t_end = now_ns() + (uint64_t)secs * 1000000000ull;

	while (now_ns() < t_end) {
		int64_t left = (int64_t)(t_end - now_ns()) / 1000000;
		int r;

		if (left <= 0)
			break;
		r = poll(pfd, nd, left > 500 ? 500 : (int)left);
		if (r < 0) {
			if (errno == EINTR) continue;
			break;
		}
		for (k = 0; k < nd; k++) {
			struct input_event ev[64];
			ssize_t n;
			uint64_t rd;

			if (!(pfd[k].revents & POLLIN))
				continue;
			n = read(d[k].fd, ev, sizeof ev);
			rd = now_ns();
			if (n <= 0)
				continue;
			for (i = 0; i < (int)(n / sizeof ev[0]); i++) {
				struct dev *p = &d[k];
				uint64_t ets =
					(uint64_t)ev[i].input_event_sec * 1000000000ull +
					(uint64_t)ev[i].input_event_usec * 1000ull;

				if (ev[i].type == EV_SYN &&
				    ev[i].code == SYN_DROPPED) {
					p->dropped++;
					continue;
				}
				if (ev[i].type == EV_SYN &&
				    ev[i].code == SYN_REPORT) {
					p->packets++;
					if (p->n_x && p->n_y) p->both++;
					else if (p->n_x) p->x_only++;
					else if (p->n_y) p->y_only++;
					else p->none++;
					p->n_x = p->n_y = 0;
					if (p->last_syn) {
						uint64_t g = (ets - p->last_syn) / 1000000ull;
						int b = g < 8 ? 0 : g < 16 ? 1 :
							g < 32 ? 2 : g < 64 ? 3 :
							g < 128 ? 4 : 5;
						p->gap[b]++;
					}
					p->last_syn = ets;
					if (rd > ets) {
						uint64_t st = rd - ets;

						p->stale_sum += st;
						p->stale_n++;
						if (st > p->stale_max)
							p->stale_max = st;
					}
					continue;
				}
				if (ev[i].type == EV_REL && ev[i].code == REL_X)
					p->n_x++;
				if (ev[i].type == EV_REL && ev[i].code == REL_Y)
					p->n_y++;
				if (ev[i].type == EV_KEY && ev[i].value != 2)
					p->buttons++;
			}
		}
	}

	for (k = 0; k < nd; k++) {
		struct dev *p = &d[k];

		printf("\n=== %s \"%s\"\n", p->path, p->name);
		if (!p->packets) {
			printf("    SILENT - not one motion packet\n");
			close(p->fd);
			continue;
		}
		printf("    packets %u (button transitions %u)\n",
		       p->packets, p->buttons);
		printf("    both axes %6u %5.1f%% | X only %6u %5.1f%% | "
		       "Y only %6u %5.1f%% | no motion %u\n",
		       p->both, 100.0 * p->both / p->packets,
		       p->x_only, 100.0 * p->x_only / p->packets,
		       p->y_only, 100.0 * p->y_only / p->packets, p->none);
		printf("    SYN_DROPPED %u  %s\n", p->dropped,
		       p->dropped ? "EVENTS LOST FOR THIS READER" : "");
		printf("    staleness mean %llu us worst %llu us\n",
		       (unsigned long long)(p->stale_n ?
			       p->stale_sum / p->stale_n / 1000 : 0),
		       (unsigned long long)(p->stale_max / 1000));
		printf("    gaps ms <8:%u <16:%u <32:%u <64:%u <128:%u >=128:%u\n",
		       p->gap[0], p->gap[1], p->gap[2], p->gap[3],
		       p->gap[4], p->gap[5]);
		close(p->fd);
	}
	return 0;
}
