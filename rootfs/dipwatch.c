#define _GNU_SOURCE
/*
 * dipwatch - per-second dip attributor for a running game, in C.
 *
 * The shell version (scripts/board/dipwatch.sh) was starved by the workload
 * it watched: one "second" took 3-8 s while prboom and lvdesk owned the
 * core, and its own read loops were a measurable tax. This does the same
 * job with a handful of syscalls per sample and a real 1 Hz clock.
 *
 * Columns, one line per second, all deltas over the interval:
 *   up fps busy% game_cpu% lvdesk_cpu% game_majflt lvdesk_majflt
 *   sd_rd_kB swapout_kB memavail_kB hosted_irq
 *
 * fps comes from fpsonly.so's log (frames count per line); the last count
 * in the file is the frames so far. Build:
 *   ./docker/build.sh 'cd /src && sh rootfs/build-dipwatch.sh'
 * Use:
 *   setsid /root/dipwatch <fps-log> <seconds> <out.txt> </dev/null >/dev/null 2>&1 &
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long read_frames(const char *path)
{
	FILE *f = fopen(path, "r");
	long n = 0, last = 0;
	double t;

	if (!f)
		return 0;
	while (fscanf(f, "%ld %lf", &n, &t) == 2)
		last = n;
	fclose(f);
	return last;
}

static int find_pid(const char *comm)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	char p[64], buf[64];
	int pid = 0;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		FILE *f;

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
		f = fopen(p, "r");
		if (!f)
			continue;
		if (fgets(buf, sizeof buf, f)) {
			buf[strcspn(buf, "\n")] = 0;
			if (!strcmp(buf, comm))
				pid = atoi(e->d_name);
		}
		fclose(f);
		if (pid)
			break;
	}
	closedir(d);
	return pid;
}

/* utime+stime ticks and majflt of a pid; 0s if gone */
static void proc_stat(int pid, long *ticks, long *majflt)
{
	char p[64], buf[512], *s;
	FILE *f;
	long v[20] = {0};
	int i;

	*ticks = 0; *majflt = 0;
	if (!pid)
		return;
	snprintf(p, sizeof p, "/proc/%d/stat", pid);
	f = fopen(p, "r");
	if (!f)
		return;
	if (fgets(buf, sizeof buf, f)) {
		s = strrchr(buf, ')');
		if (s) {
			s += 2;			/* state */
			for (i = 0; i < 20 && s; i++) {
				char *n;
				v[i] = strtol(s, &n, 10);
				if (i == 0) { /* state char, not a number */ v[i] = 0; s = strchr(s, ' '); if (s) s++; continue; }
				s = (n && *n == ' ') ? n + 1 : NULL;
			}
			/* after ')': state(0) ppid(1) pgrp(2) session(3) tty(4) tpgid(5) flags(6) minflt(7) cminflt(8) majflt(9) cmajflt(10) utime(11) stime(12) */
			*majflt = v[9];
			*ticks = v[11] + v[12];
		}
	}
	fclose(f);
}

static void sys_stat(long *busy, long *total)
{
	FILE *f = fopen("/proc/stat", "r");
	long u = 0, n = 0, s = 0, i = 0, w = 0, q = 0, sq = 0;

	*busy = *total = 0;
	if (!f)
		return;
	if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld", &u, &n, &s, &i, &w, &q, &sq) >= 4) {
		*total = u + n + s + i + w + q + sq;
		*busy = *total - i;
	}
	fclose(f);
}

static long meminfo(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char k[64];
	long v = 0, out = 0;

	if (!f)
		return 0;
	while (fscanf(f, "%63s %ld kB", k, &v) == 2)
		if (!strcmp(k, key)) { out = v; break; }
	fclose(f);
	return out;
}

static long sd_sectors(void)
{
	FILE *f = fopen("/sys/block/mmcblk0/stat", "r");
	long a = 0, b = 0, c = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%ld %ld %ld", &a, &b, &c) < 3)
		c = 0;
	fclose(f);
	return c;
}

static long hosted_irqs(void)
{
	FILE *f = fopen("/proc/interrupts", "r");
	char line[512];
	long n = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (strstr(line, "hosted") || strstr(line, "wireless")) {
			char *c = strchr(line, ':');
			if (c) n = strtol(c + 1, NULL, 10);
			break;
		}
	fclose(f);
	return n;
}

int main(int argc, char **argv)
{
	const char *fl = argc > 1 ? argv[1] : "/root/doom/fps.txt";
	int secs = argc > 2 ? atoi(argv[2]) : 120;
	const char *outp = argc > 3 ? argv[3] : "/root/dip.txt";
	FILE *out = fopen(outp, "w");
	int game = find_pid("prboom"), lv = find_pid("lvdesk");
	long pf = read_frames(fl), pb, pt, pgt, pgm, plt, plm, psd, psf, phi;
	struct timespec next;
	int i;

	if (!out)
		return 1;
	if (!game) game = find_pid("chocolate-doom");
	fprintf(out, "# up fps busy game_cpu lvdesk_cpu game_majflt lvdesk_majflt sd_rd_kB swapout_kB memavail_kB hosted_irq  (game pid %d, lvdesk %d)\n", game, lv);
	sys_stat(&pb, &pt);
	proc_stat(game, &pgt, &pgm);
	proc_stat(lv, &plt, &plm);
	psd = sd_sectors(); psf = meminfo("SwapFree:"); phi = hosted_irqs();
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (i = 0; i < secs; i++) {
		long b, t, gt, gm, lt, lm, sd, sf, hi, f, up;
		FILE *uf;
		double upd = 0;

		next.tv_sec += 1;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		sys_stat(&b, &t);
		proc_stat(game, &gt, &gm);
		proc_stat(lv, &lt, &lm);
		sd = sd_sectors(); sf = meminfo("SwapFree:"); hi = hosted_irqs();
		f = read_frames(fl);
		uf = fopen("/proc/uptime", "r");
		if (uf) { if (fscanf(uf, "%lf", &upd) != 1) upd = 0; fclose(uf); }
		up = (long)upd;
		{
			long dt = t - pt;
			int busy = dt > 0 ? (int)((b - pb) * 100 / dt) : 0;
			int gcpu = dt > 0 ? (int)((gt - pgt) * 100 / dt) : 0;
			int lcpu = dt > 0 ? (int)((lt - plt) * 100 / dt) : 0;

			fprintf(out, "%ld %ld %d %d %d %ld %ld %ld %ld %ld %ld\n", up,
				f - pf, busy, gcpu, lcpu, gm - pgm, lm - plm,
				(sd - psd) / 2, psf - sf, meminfo("MemAvailable:"),
				hi - phi);
			fflush(out);
		}
		pb = b; pt = t; pgt = gt; pgm = gm; plt = lt; plm = lm;
		psd = sd; psf = sf; phi = hi; pf = f;
	}
	fclose(out);
	return 0;
}
