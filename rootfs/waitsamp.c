/*
 * waitsamp - what is a thread blocked on? A histogram of its wait state.
 *
 *   waitsamp <tid> [secs=10] [period_ms=5]
 *
 * Every period it reads /proc/<tid>/stat (state), /proc/<tid>/wchan (the
 * kernel function it sleeps in) and /proc/<tid>/syscall (which syscall), and
 * at the end prints one line per distinct (state, wchan, syscall) with its
 * share of samples. Running samples are counted as "R". The user PC it
 * blocked at tells a syscall (PC just after an ecall in libc) from a page
 * fault (PC anywhere in the program). On riscv /proc/<tid>/syscall reports a7
 * even outside a syscall, so a fault shows a junk "sys" number; key on pc.
 *
 * Why a tool: a shell loop forks for every sleep and every cat, and that fork
 * cost lands on the CPUs being measured (docs: "measure the harness first").
 * This opens nothing but three proc files per sample and sleeps with
 * nanosleep. Run it pinned away from the thread it watches (oncpu).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXK 512

static struct { char key[96]; int n; } h[MAXK];
static int nk, other;

static int slurp(const char *path, char *buf, int len)
{
	int fd = open(path, O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	return n;
}

static void count(const char *key)
{
	int i;

	for (i = 0; i < nk; i++)
		if (!strcmp(h[i].key, key)) {
			h[i].n++;
			return;
		}
	if (nk == MAXK) {
		other++;
		return;
	}
	snprintf(h[nk].key, sizeof(h[nk].key), "%s", key);
	h[nk++].n = 1;
}

int main(int argc, char **argv)
{
	char p_stat[64], p_wchan[64], p_sys[64], buf[512], wchan[64], key[96];
	int tid, secs, period, total = 0, i;
	struct timespec ts;
	time_t end;

	if (argc < 2) {
		fprintf(stderr, "usage: waitsamp <tid> [secs] [period_ms]\n");
		return 2;
	}
	tid = atoi(argv[1]);
	secs = argc > 2 ? atoi(argv[2]) : 10;
	period = argc > 3 ? atoi(argv[3]) : 5;
	snprintf(p_stat, sizeof(p_stat), "/proc/%d/stat", tid);
	snprintf(p_wchan, sizeof(p_wchan), "/proc/%d/wchan", tid);
	snprintf(p_sys, sizeof(p_sys), "/proc/%d/syscall", tid);
	ts.tv_sec = period / 1000;
	ts.tv_nsec = (period % 1000) * 1000000L;
	end = time(NULL) + secs;

	while (time(NULL) < end) {
		char *rp, st, *sp;
		long nr = -2;
		unsigned long pc = 0;

		if (slurp(p_stat, buf, sizeof(buf)) <= 0)
			break;
		rp = strrchr(buf, ')');
		st = rp && rp[1] ? rp[2] : '?';
		if (st == 'R') {
			count("R running");
		} else {
			if (slurp(p_wchan, wchan, sizeof(wchan)) < 0)
				strcpy(wchan, "?");
			if (slurp(p_sys, buf, sizeof(buf)) > 0)
				nr = strtol(buf, &sp, 10);
			/* "nr a0..a5 sp pc": the last field is the user PC. (stat's
			 * kstkeip reads 0 for a live task.) */
			sp = strrchr(buf, ' ');
			pc = sp ? strtoul(sp + 1, NULL, 16) : 0;
			snprintf(key, sizeof(key), "%c %s sys=%ld pc=%08lx", st, wchan, nr, pc);
			count(key);
		}
		total++;
		nanosleep(&ts, NULL);
	}
	for (i = 0; i < nk; i++)
		printf("WS %5.1f%% %5d  %s\n", 100.0 * h[i].n / total, h[i].n, h[i].key);
	printf("WS total %d samples, %d unbinned\n", total, other);
	return 0;
}
