// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watch a keyboard at two levels at once, and say where a keystroke was lost.
 *
 *	kbdtrace /dev/hidrawN /dev/input/eventN [seconds]
 *
 * Everything about this board's keyboard fault has so far been inferred. The
 * kernel autorepeats a key nobody is holding, which means it never processed
 * the release - but "never processed" has two very different causes and
 * nothing has ever distinguished them:
 *
 *   the release never arrived on the wire   -> the URB path, below HID
 *   it arrived and evdev did not follow     -> above HID, in the input layer
 *
 * hidraw gives the raw boot report; evdev gives what the input layer made of
 * it. Reading both with one clock answers it directly.
 *
 * A boot keyboard report is eight bytes: modifiers, a reserved byte, and six
 * keycodes, zero-padded. It carries ABSOLUTE state - the keys currently held -
 * so a release is a keycode DISAPPEARING between reports, not an event. That
 * is why one lost report usually cannot stick a key: the next report corrects
 * it. The exception matters, though - if the lost report is the LAST one, no
 * correction ever comes. A keyboard with SET_IDLE(0) sends nothing while its
 * state is unchanged, so dropping the release of the final key leaves it held
 * indefinitely. A key stuck for hundreds of repeats therefore means either no
 * report arrived for that interval, or the one that did was the last.
 *
 * So the line to watch for is STUCK: hidraw says nothing is held while evdev
 * still believes something is. That is the fault, caught in the act, and it
 * says the loss is above HID. If instead hidraw itself never shows the key
 * being released, the loss is below - in the URB path - and usbmon is the next
 * instrument.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HID_REPORT_MAX	16
#define NKEYS		256

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *hidpath = argc > 1 ? argv[1] : "/dev/hidraw0";
	const char *evpath = argc > 2 ? argv[2] : "/dev/input/event0";
	double secs = argc > 3 ? atof(argv[3]) : 60.0;
	/* What each layer believes is held down. */
	unsigned char hid_down[NKEYS] = { 0 };
	unsigned char ev_down[NKEYS] = { 0 };
	unsigned hid_reports = 0, ev_events = 0, stuck = 0, repeats = 0;
	unsigned char hid_mod = 0;
	struct pollfd fds[2];
	double t0;
	int hf, ef;

	hf = open(hidpath, O_RDONLY | O_NONBLOCK);
	if (hf < 0) { perror(hidpath); return 1; }
	ef = open(evpath, O_RDONLY | O_NONBLOCK);
	if (ef < 0) { perror(evpath); return 1; }

	fds[0].fd = hf; fds[0].events = POLLIN;
	fds[1].fd = ef; fds[1].events = POLLIN;

	printf("# t(s) source detail\n");
	printf("# watching %s and %s for %.0fs\n", hidpath, evpath, secs);
	fflush(stdout);
	t0 = now_s();

	while (now_s() - t0 < secs) {
		double t;
		int i;

		if (poll(fds, 2, 200) <= 0)
			goto check;

		if (fds[0].revents & POLLIN) {
			unsigned char r[HID_REPORT_MAX];
			int n = read(hf, r, sizeof(r));

			if (n >= 8) {
				unsigned char seen[NKEYS] = { 0 };

				t = now_s() - t0;
				hid_reports++;
				/*
				 * Print the length and every byte, not a
				 * guessed layout. A device that prefixes a
				 * report ID shifts every field along by one,
				 * and the only way that ever shows up is a
				 * byte 0 that never changes: a capture here
				 * had mod=0c on all 125 reports and not one
				 * 00, which is a report ID being read as
				 * modifiers.
				 */
				printf("%8.3f HID  mod=%02x len=%d raw", t,
				       r[0], n);
				for (i = 0; i < n && i < HID_REPORT_MAX; i++)
					printf(" %02x", r[i]);
				printf("\n");
				for (i = 2; i < 8; i++)
					if (r[i])
						seen[r[i]] = 1;
				/*
				 * Absolute state: anything not in this report
				 * is, by definition, released.
				 */
				memcpy(hid_down, seen, sizeof(hid_down));
				/*
				 * Modifiers live in the modifier BYTE, never
				 * in the keycode slots, so a held Alt leaves
				 * every slot empty. Missing that made the
				 * tool shout STUCK through a capture whose
				 * only fault was a user holding Alt to
				 * Alt-Tab - a false verdict on the one run
				 * that was supposed to settle the question.
				 */
				hid_mod = r[0];
			}
		}

		if (fds[1].revents & POLLIN) {
			struct input_event ev;

			while (read(ef, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type != EV_KEY)
					continue;
				t = now_s() - t0;
				ev_events++;
				if (ev.value == 2)
					repeats++;
				printf("%8.3f EVDEV code=%u value=%d%s\n", t,
				       ev.code, ev.value,
				       ev.value == 2 ? "  (autorepeat)" : "");
				if (ev.code < NKEYS)
					ev_down[ev.code] = (ev.value != 0);
			}
		}

check:
		/*
		 * The whole point. evdev holding a key that the hardware is
		 * no longer reporting is the stuck key, caught live - and it
		 * locates the fault ABOVE hid, because the wire is clean.
		 */
		{
			int any_hid = 0, any_ev = 0;

			for (i = 0; i < NKEYS; i++) {
				any_hid |= hid_down[i];
				any_ev |= ev_down[i];
			}
			any_hid |= hid_mod != 0;
			if (any_ev && !any_hid) {
				if (!stuck++)
					printf("%8.3f STUCK evdev holds a key "
					       "the hardware is not reporting - "
					       "loss is ABOVE hid\n",
					       now_s() - t0);
			}
		}
		fflush(stdout);
	}

	printf("\n# hid reports %u, evdev events %u, autorepeats %u, stuck %u\n",
	       hid_reports, ev_events, repeats, stuck);
	/*
	 * Only draw a conclusion when there is something to conclude from. The
	 * first version printed the "loss is BELOW hid" verdict unconditionally
	 * and did so on a run that captured nothing at all, which is precisely
	 * the sort of instrument that has wasted time on this board already.
	 */
	if (!hid_reports && !ev_events)
		printf("# NOTHING CAPTURED - wrong nodes, or nobody typed. No verdict.\n");
	else if (repeats && !stuck)
		printf("# autorepeats with no STUCK: the release never reached\n"
		       "# hidraw either, so the loss is BELOW hid - use usbmon\n");
	else if (stuck)
		printf("# STUCK seen: the wire was clean and the input layer\n"
		       "# lost the release - the fault is ABOVE hid\n");
	else
		printf("# clean run: no autorepeat storms, no stuck keys\n");
	close(hf);
	close(ef);
	return 0;
}
