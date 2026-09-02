/*
 * How long does a window take to resize and refill?
 *
 * "Maximise xfiles and watch it re-lay-out" is the interaction that most
 * obviously feels slow, and it cannot be timed from a shell: a busybox
 * poll loop costs ~29 ms per iteration in fork+exec alone, which is the
 * same order as the thing being measured. This sends the ctl command
 * itself and then samples the two processes' CPU counters at 20 ms until
 * both go quiet, so the number is "when did the desktop stop working",
 * not "when did my polling notice".
 *
 * usage: resizebench <lvdesk-pid> <client-pid> <ctl-command>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
/* the ctl endpoint is a FIFO, despite what lvdesk's comment says */

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static long cpu_ticks(int pid)
{
	char p[64], buf[512];
	int fd, n, i;
	long u = 0, s = 0;
	char *q;

	snprintf(p, sizeof(p), "/proc/%d/stat", pid);
	fd = open(p, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	/* fields after the ")" so a comm with spaces cannot shift them */
	q = strrchr(buf, ')');
	if (!q)
		return -1;
	q += 2;
	for (i = 3; i <= 15 && q; i++) {
		if (i == 14) sscanf(q, "%ld", &u);
		if (i == 15) sscanf(q, "%ld", &s);
		q = strchr(q, ' ');
		if (q) q++;
	}
	return u + s;
}

int main(int argc, char **argv)
{
	int lv = argc > 1 ? atoi(argv[1]) : 0;
	int cl = argc > 2 ? atoi(argv[2]) : 0;
	const char *cmd = argc > 3 ? argv[3] : "max 0";
	double t0, quiet_since = 0;
	long lv0, cl0, lvp, clp;
	int fd, idle_ms = 400, capped = 0;
	char line[128];

	fd = open("/tmp/lvdesk.ctl", O_WRONLY);
	if (fd < 0) {
		perror("open /tmp/lvdesk.ctl");
		return 1;
	}
	snprintf(line, sizeof(line), "%s\n", cmd);

	lvp = lv0 = cpu_ticks(lv);
	clp = cl0 = cpu_ticks(cl);
	t0 = now_ms();
	if (write(fd, line, strlen(line)) < 0) {
		perror("write");
		return 1;
	}
	for (;;) {
		double t;
		long l, c;

		usleep(20000);
		t = now_ms();
		l = cpu_ticks(lv);
		c = cpu_ticks(cl);
		if (l != lvp || c != clp) {
			lvp = l; clp = c;
			quiet_since = 0;
		} else if (!quiet_since) {
			quiet_since = t;
		} else if (t - quiet_since > idle_ms) {
			printf("settled in %.0f ms  (lvdesk %ld ticks, "
			       "client %ld ticks)\n",
			       quiet_since - t0, lvp - lv0, clp - cl0);
			break;
		}
		if (t - t0 > 20000) { capped = 1; break; }
	}
	if (capped)
		printf("NOT settled within 20 s (lvdesk %ld, client %ld)\n",
		       lvp - lv0, clp - cl0);
	return 0;
}
