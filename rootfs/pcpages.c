/*
 * WHICH 4 kB PAGES OF CODE IS A PROCESS ACTUALLY EXECUTING?
 *
 * There is no perf here. rootfs/pcsample.c samples another process's PC with
 * ptrace, which is the right mechanism, but it answers a different question in
 * two ways that matter for this one:
 *
 *   - It counts samples taken while the target is ASLEEP. On lvdesk that put
 *     62% of the profile in __syscall_cp_asm, which only says "it was in a
 *     syscall" and crowds out everything that was using CPU.
 *   - It buckets by 64 bytes, which is the right grain for finding a hot
 *     function and the wrong grain for deciding which PAGES to move out of
 *     XIP flash into RAM.
 *
 * So: sample only while the task is RUNNABLE, bucket by page, and report each
 * page as a file and a file-relative offset - which is what both the re-backer
 * and a symbol lookup need.
 *
 * WHY PAGES. Userspace on this board executes from the XIP cramfs, and
 * rootfs/ramtext.c measures that at 4.8x slower than RAM for code that
 * overflows the instruction cache. The re-backing mechanism works on page
 * ranges, so the question "what should move" is exactly "which pages are hot".
 * A guess at the function level already produced a null result: 19 kB of
 * hand-picked dispatch code moved to RAM changed prboom's timedemo by nothing
 * (27.7 against 27.1 fps, ranges overlapping).
 *
 *   pcpages <pid> [samples] [us between samples] [minpct]
 *
 * Attaching stops the target for each sample, so keep the count modest.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

struct rv_regs { unsigned long pc, ra, sp, gp, tp, t0, t1, t2, s0, s1,
	a0, a1, a2, a3, a4, a5, a6, a7, s2, s3, s4, s5, s6, s7, s8, s9,
	s10, s11, t3, t4, t5, t6; };

#define PS	4096u
#define MAXB	2048

struct pg { unsigned long page; unsigned n; };
static struct pg pages[MAXB];
static int npages;

static void tally(unsigned long pc)
{
	unsigned long p = pc & ~(unsigned long)(PS - 1);
	int i;

	for (i = 0; i < npages; i++)
		if (pages[i].page == p) { pages[i].n++; return; }
	if (npages < MAXB) { pages[npages].page = p; pages[npages].n = 1; npages++; }
}

/*
 * The mapping a page belongs to, and the FILE offset of that page.
 *
 * The offset must be relative to the FILE, not to the segment the page
 * happens to be in: /proc/pid/maps gives each mapping's file offset in field
 * four, so file_off = map_off + (page - map_start). pcsample.c takes the
 * lowest mapping of the same file instead, which is equivalent only when the
 * executable segment is the first one - and gets it wrong by a segment when
 * it is not. Reporting a wrong offset is worse than reporting none, because
 * it still resolves, just to an unrelated symbol.
 */
static void whereis(pid_t pid, unsigned long page, char *name, size_t n,
		    unsigned long *file_off, int *anon)
{
	char path[64], line[512];
	FILE *f;

	snprintf(path, sizeof path, "/proc/%d/maps", pid);
	snprintf(name, n, "?");
	*file_off = 0;
	*anon = 0;

	f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f)) {
		unsigned long lo, hi, off;
		char perm[8], p[400];

		p[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %399[^\n]",
			   &lo, &hi, perm, &off, p) < 4)
			continue;
		if (page < lo || page >= hi)
			continue;
		if (!p[0] || p[0] == '[') {
			snprintf(name, n, "%s", p[0] ? p : "[anon]");
			*anon = 1;
			*file_off = page - lo;
		} else {
			const char *b = strrchr(p, '/');

			snprintf(name, n, "%s", b ? b + 1 : p);
			*file_off = off + (page - lo);
		}
		break;
	}
	if (f) fclose(f);
}

/* 'R' means runnable-or-running. Read it BEFORE attaching: ptrace stops the
 * task, so asking afterwards always says stopped. */
static int is_running(pid_t pid)
{
	char path[64], buf[256], *p;
	int fd, n;

	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	/* comm can contain spaces and parens; the state is the char after the
	 * LAST ')' plus a space. */
	p = strrchr(buf, ')');
	if (!p || !p[1] || !p[2])
		return 0;
	return p[2] == 'R';
}

static int cmp(const void *a, const void *b)
{
	const struct pg *x = a, *y = b;

	return (int)y->n - (int)x->n;
}

int main(int argc, char **argv)
{
	pid_t pid = argc > 1 ? atoi(argv[1]) : 0;
	int want = argc > 2 ? atoi(argv[2]) : 2000;
	int gap = argc > 3 ? atoi(argv[3]) : 2000;
	double minpct = argc > 4 ? atof(argv[4]) : 0.5;
	int got = 0, skipped = 0, i, tries = 0;
	struct timespec ts;

	if (!pid) {
		fprintf(stderr, "usage: pcpages <pid> [samples] [us] "
			"[minpct]\n");
		return 1;
	}
	ts.tv_sec = gap / 1000000;
	ts.tv_nsec = (long)(gap % 1000000) * 1000;

	while (got < want && tries < want * 40) {
		struct rv_regs r;
		struct iovec io = { &r, sizeof r };
		int st;

		tries++;
		if (!is_running(pid)) {
			skipped++;
			nanosleep(&ts, NULL);
			continue;
		}
		if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0)
			break;
		if (waitpid(pid, &st, 0) < 0)
			break;
		if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &io) == 0) {
			tally(r.pc);
			got++;
		}
		ptrace(PTRACE_DETACH, pid, 0, 0);
		nanosleep(&ts, NULL);
	}

	printf("PCPAGES pid %d: %d on-CPU samples, %d skipped (asleep)\n",
	       pid, got, skipped);
	if (!got)
		return 0;
	qsort(pages, (size_t)npages, sizeof *pages, cmp);
	printf("  %%     samples  page        file                       "
	       "file offset\n");
	{
		double cum = 0;

		for (i = 0; i < npages; i++) {
			char nm[128];
			unsigned long fo;
			int anon;
			double pct = 100.0 * pages[i].n / got;

			if (pct < minpct)
				break;
			whereis(pid, pages[i].page, nm, sizeof nm, &fo, &anon);
			cum += pct;
			printf("%6.2f %8u  0x%08lx  %-26s %s0x%lx  (cum "
			       "%5.1f%%)\n", pct, pages[i].n, pages[i].page,
			       nm, anon ? "+" : "", fo, cum);
		}
	}
	return 0;
}
