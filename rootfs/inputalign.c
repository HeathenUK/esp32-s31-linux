// SPDX-License-Identifier: GPL-2.0-only
/*
 * One timeline for the whole input path.
 *
 * "Keys go missing" is a claim about a chain - receiver, USB endpoint, HID,
 * evdev, desktop - and every previous attempt to find where has used a
 * separate tool per layer. Two logs with two clocks cannot answer "was there a
 * report for the key that never arrived?", which is the only question that
 * matters: a lost PRESS is silent at every instrument, because the next report
 * shows nothing held and that matches the host's state, so there is no event,
 * no error and no stuck key.
 *
 * So this opens every /dev/hidraw* and every /dev/input/event* at once, stamps
 * everything from one monotonic clock, and writes a single interleaved log.
 * At exit it reconstructs what evdev thinks was typed, so it can be compared
 * with what the human actually typed:
 *
 *     inputalign            # log to stdout until SIGINT/SIGTERM
 *     inputalign 30         # ...or for 30 seconds
 *
 * Deliberately device-agnostic. It does not know what an 8BitDo is, decodes no
 * vendor report layout, and needs no configuration - a composite receiver that
 * presents three interfaces is simply three hidraw nodes and three event
 * nodes, all watched the same way.
 *
 * Reading hidraw does NOT steal reports from usbhid: hidraw is a tap, so the
 * keyboard keeps working normally while this runs. Reading evdev likewise
 * gives each reader its own copy.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAXFD	32
#define MAXTEXT	4096

struct src {
	int fd;
	int is_hid;			/* hidraw, else evdev */
	char name[72];
	unsigned long reports;		/* hidraw reports / evdev key events */
	unsigned long rel;		/* evdev relative-motion events */
};

static struct src src[MAXFD];
static int nsrc;
static volatile sig_atomic_t stop;
static unsigned long stalls, worst_stall;

/* Reconstructed from evdev, so it can be diffed against what was typed. */
static char text[MAXTEXT];
static int ntext;
static int shift;

static struct timespec t_base;

static unsigned long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	if (!t_base.tv_sec)
		t_base = t;
	return (unsigned long)((t.tv_sec - t_base.tv_sec) * 1000 +
			       (t.tv_nsec - t_base.tv_nsec) / 1000000);
}

/* An evdev event's own kernel timestamp, on the same base. */
static unsigned long ev_ms(const struct input_event *ev)
{
	return (unsigned long)((ev->input_event_sec - t_base.tv_sec) * 1000 +
			       ev->input_event_usec / 1000);
}

static void on_stop(int sig) { (void)sig; stop = 1; }

/*
 * A US layout, only far enough to turn a typed sentence back into a string.
 * This is a comparison aid, not an input method: anything it cannot name is
 * printed as <code> so nothing is silently swallowed.
 */
static const char *keychar(int code, int sh)
{
	static char buf[16];
	static const char *row1 = "1234567890-=";
	static const char *row1s = "!@#$%^&*()_+";

	if (code >= KEY_1 && code <= KEY_EQUAL) {
		buf[0] = (sh ? row1s : row1)[code - KEY_1];
		buf[1] = 0;
		return buf;
	}
	switch (code) {
	case KEY_SPACE: return " ";
	case KEY_ENTER: return "\n";
	case KEY_TAB: return "\t";
	case KEY_BACKSPACE: return "<BS>";
	case KEY_COMMA: return sh ? "<" : ",";
	case KEY_DOT: return sh ? ">" : ".";
	case KEY_SLASH: return sh ? "?" : "/";
	case KEY_SEMICOLON: return sh ? ":" : ";";
	case KEY_APOSTROPHE: return sh ? "\"" : "'";
	case KEY_MINUS: return sh ? "_" : "-";
	}
	{
		static const char letters[] = {
			[KEY_A] = 'a', [KEY_B] = 'b', [KEY_C] = 'c',
			[KEY_D] = 'd', [KEY_E] = 'e', [KEY_F] = 'f',
			[KEY_G] = 'g', [KEY_H] = 'h', [KEY_I] = 'i',
			[KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l',
			[KEY_M] = 'm', [KEY_N] = 'n', [KEY_O] = 'o',
			[KEY_P] = 'p', [KEY_Q] = 'q', [KEY_R] = 'r',
			[KEY_S] = 's', [KEY_T] = 't', [KEY_U] = 'u',
			[KEY_V] = 'v', [KEY_W] = 'w', [KEY_X] = 'x',
			[KEY_Y] = 'y', [KEY_Z] = 'z',
		};

		if (code > 0 && code < (int)sizeof(letters) && letters[code]) {
			buf[0] = sh ? letters[code] - 32 : letters[code];
			buf[1] = 0;
			return buf;
		}
	}
	snprintf(buf, sizeof(buf), "<%d>", code);
	return buf;
}

static void add_src(const char *path, int is_hid)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd < 0 || nsrc >= MAXFD) {
		if (fd >= 0)
			close(fd);
		return;
	}
	src[nsrc].fd = fd;
	src[nsrc].is_hid = is_hid;
	src[nsrc].name[0] = 0;
	if (is_hid)
		ioctl(fd, _IOC(_IOC_READ, 'H', 0x04, sizeof(src[nsrc].name)),
		      src[nsrc].name);
	else {
		/*
		 * Timestamp evdev events with the KERNEL's clock, on the same
		 * base as ours.
		 *
		 * Using read-time instead made this tool lie in the most
		 * convincing way available: five key-downs appeared 80 ms
		 * BEFORE the HID reports they were derived from, which reads
		 * as the host inventing events. It was only the order in
		 * which this process happened to drain two file descriptors.
		 */
		int cid = CLOCK_MONOTONIC;

		ioctl(fd, EVIOCSCLOCKID, &cid);
		ioctl(fd, EVIOCGNAME(sizeof(src[nsrc].name)), src[nsrc].name);
	}
	if (!src[nsrc].name[0])
		snprintf(src[nsrc].name, sizeof(src[nsrc].name), "%s", path);
	printf("# %-8s %-14s %s\n", is_hid ? "hidraw" : "evdev", path,
	       src[nsrc].name);
	nsrc++;
}

static void scan(const char *dir, const char *prefix, int is_hid)
{
	struct dirent *e;
	DIR *d = opendir(dir);
	char path[300];

	if (!d)
		return;
	while ((e = readdir(d)))
		if (!strncmp(e->d_name, prefix, strlen(prefix))) {
			snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
			add_src(path, is_hid);
		}
	closedir(d);
}

int main(int argc, char **argv)
{
	struct pollfd pfd[MAXFD];
	unsigned long deadline = argc > 1 ? (unsigned long)atoi(argv[1]) * 1000
					  : 0;
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);

	now_ms();			/* start the clock before anything */
	scan("/dev", "hidraw", 1);
	scan("/dev/input", "event", 0);
	if (!nsrc) {
		printf("no input sources - run as root\n");
		return 1;
	}
	printf("# %d sources; one clock, ms since start. "
	       "Type the agreed sentence now.\n", nsrc);

	while (!stop) {
		int n;

		for (i = 0; i < nsrc; i++) {
			pfd[i].fd = src[i].fd;
			pfd[i].events = POLLIN;
			pfd[i].revents = 0;
		}
		{
			/*
			 * Our own scheduling, recorded.
			 *
			 * A gap between mouse reports means either the system
			 * stalled or the hand stopped moving, and those are
			 * indistinguishable from the reports alone - which is
			 * how "the desktop is laggy" has stayed unfalsifiable.
			 * A 10 ms poll that comes back late is the system,
			 * and nothing the user does can fake it.
			 */
			static unsigned long prev;
			unsigned long t = now_ms();

			if (prev && t - prev > 25) {
				printf("%8lu STALL %-28s %lu ms with no "
				       "wakeup\n", t, "(this process)",
				       t - prev);
				stalls++;
				if (t - prev > worst_stall)
					worst_stall = t - prev;
			}
			prev = t;
		}
		n = poll(pfd, nsrc, 10);
		if (deadline && now_ms() > deadline)
			break;
		if (n <= 0)
			continue;
		for (i = 0; i < nsrc; i++) {
			unsigned char buf[64];
			ssize_t r;

			if (!(pfd[i].revents & POLLIN))
				continue;
			if (src[i].is_hid) {
				while ((r = read(src[i].fd, buf,
						 sizeof(buf))) > 0) {
					int b;

					src[i].reports++;
					printf("%8lu HID   %-28s len=%2d ",
					       now_ms(), src[i].name, (int)r);
					for (b = 0; b < r; b++)
						printf("%02x", buf[b]);
					printf("\n");
				}
				continue;
			}
			while ((r = read(src[i].fd, buf, sizeof(struct input_event)))
			       == sizeof(struct input_event)) {
				struct input_event ev;

				memcpy(&ev, buf, sizeof(ev));
				if (ev.type == EV_SYN && ev.code == SYN_DROPPED)
					printf("%8lu DROP  %-28s "
					       "*** EVDEV OVERFLOW ***\n",
					       now_ms(), src[i].name);
				if (ev.type == EV_REL) {
					src[i].rel++;
					printf("%8lu REL   %-28s %s=%d\n",
					       ev_ms(&ev), src[i].name,
					       ev.code == REL_X ? "dx" :
					       ev.code == REL_Y ? "dy" : "r",
					       (int)ev.value);
					continue;
				}
				if (ev.type != EV_KEY)
					continue;
				src[i].reports++;
				if (ev.code == KEY_LEFTSHIFT ||
				    ev.code == KEY_RIGHTSHIFT)
					shift = ev.value != 0;
				printf("%8lu EVDEV %-28s key=%-4d %s %s\n",
				       ev_ms(&ev), src[i].name, ev.code,
				       ev.value == 1 ? "down" :
				       ev.value == 0 ? "up  " : "rept",
				       keychar(ev.code, shift));
				if (ev.value == 1) {
					const char *s = keychar(ev.code, shift);

					while (*s && ntext < MAXTEXT - 1)
						text[ntext++] = *s++;
				}
			}
		}
	}

	printf("\n# ---- summary ----\n");
	for (i = 0; i < nsrc; i++)
		printf("# %-6s %-34s %6lu %-11s %6lu motion\n",
		       src[i].is_hid ? "hidraw" : "evdev", src[i].name,
		       src[i].reports, src[i].is_hid ? "reports" : "key events",
		       src[i].rel);
	printf("# scheduling: %lu stalls over 25 ms, worst %lu ms\n",
	       stalls, worst_stall);
	text[ntext] = 0;
	printf("# evdev reconstructed as typed:\n%s\n", text);
	printf("# ---- compare that with what was actually typed. A character\n"
	       "# missing here AND with no HID line at that moment was never\n"
	       "# in a USB report; missing here WITH a HID line is a host bug.\n");
	return 0;
}
