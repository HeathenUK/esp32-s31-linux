// Sample another process's program counter, without perf.
//
// There is no perf, no ftrace and no gprof on this board, and the profiler
// trick used for Qt (a SIGPROF handler inside the program) needs the program's
// cooperation. bluetoothd is off-the-shelf and cannot be modified, so attach
// with ptrace instead and read the PC directly.
//
//   pcsample <pid> [samples] [us between samples]
//
// Prints a histogram of PCs, and - because the interesting binaries here are
// stripped and PIE - the offset within the mapping the PC landed in, which is
// what can actually be resolved against a symbol table on the host:
//
//   riscv32-esp-linux-musl-nm -n /path/to/binary | awk ...
//
// Attaching stops the target briefly for each sample, so keep the count modest
// on a board where a spinning daemon is already eating the CPU.
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

/* riscv user_regs_struct starts with pc, then the x registers. */
struct rv_regs { unsigned long pc, ra, sp, gp, tp, t0, t1, t2, s0, s1,
	a0, a1, a2, a3, a4, a5, a6, a7, s2, s3, s4, s5, s6, s7, s8, s9,
	s10, s11, t3, t4, t5, t6; };

struct bucket { unsigned long pc; unsigned n; };
static struct bucket buckets[4096];
static int nbuckets;

static void tally(unsigned long pc)
{
	pc &= ~63UL;				/* 64-byte buckets */
	for (int i = 0; i < nbuckets; i++)
		if (buckets[i].pc == pc) { buckets[i].n++; return; }
	if (nbuckets < 4096) { buckets[nbuckets].pc = pc; buckets[nbuckets].n = 1; nbuckets++; }
}

/* Which mapping does this address fall in, and what is that FILE's load base?
 *
 * The base must be the lowest mapping of the same file, NOT the mapping the PC
 * happens to land in. An ELF has several PT_LOAD segments and the kernel maps
 * each separately, so a PC in the executable segment of a multi-segment binary
 * is offset from that segment, not from the file. Reporting the segment-local
 * offset makes every symbol lookup wrong - and wrong in a way that still
 * resolves, because it lands on some earlier unrelated function. That happened:
 * lvdesk offsets came out ~0x10000 low and "resolved" to nothing, while a libc
 * offset resolved confidently to pthread_spin_unlock+98 when it was really
 * __syscall_cp_asm. Two passes: find the containing mapping's file, then the
 * lowest mapping of that same file. */
static void whereis(pid_t pid, unsigned long pc, char *out, size_t n, unsigned long *base)
{
	char path[64], line[512], want[400];
	unsigned long lo, hi, lowest = 0;
	int found = 0;
	FILE *f;

	snprintf(path, sizeof path, "/proc/%d/maps", pid);
	snprintf(out, n, "?");
	want[0] = 0;
	*base = 0;

	f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f)) {
		char perm[8], p[400];

		p[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]", &lo, &hi, perm, p) < 3)
			continue;
		if (pc >= lo && pc < hi) {
			const char *b = strrchr(p, '/');

			snprintf(out, n, "%s", b ? b + 1 : (p[0] ? p : "[anon]"));
			snprintf(want, sizeof want, "%s", p);
			*base = lo;			/* anonymous: no better answer */
			found = 1;
			break;
		}
	}
	if (f) fclose(f);
	if (!found || !want[0])
		return;

	f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f)) {
		char perm[8], p[400];

		p[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]", &lo, &hi, perm, p) < 3)
			continue;
		if (!strcmp(p, want) && (!lowest || lo < lowest))
			lowest = lo;
	}
	if (f) fclose(f);
	if (lowest)
		*base = lowest;
}

int main(int argc, char **argv)
{
	pid_t pid = argc > 1 ? atoi(argv[1]) : 0;
	int want = argc > 2 ? atoi(argv[2]) : 400;
	int gap = argc > 3 ? atoi(argv[3]) : 5000;
	int got = 0;

	if (!pid) { fprintf(stderr, "usage: pcsample <pid> [samples] [us]\n"); return 1; }

	for (int i = 0; i < want; i++) {
		struct rv_regs r;
		struct iovec io = { .iov_base = &r, .iov_len = sizeof r };
		int st;

		if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) {
			perror("attach");
			return 1;
		}
		if (waitpid(pid, &st, 0) < 0) break;
		if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &io) == 0) {
			tally(r.pc);
			got++;
		}
		ptrace(PTRACE_DETACH, pid, 0, 0);
		usleep(gap);
	}

	printf("PCSAMPLE %d samples of pid %d\n", got, pid);
	for (int pass = 0; pass < 12; pass++) {
		int best = -1;
		for (int i = 0; i < nbuckets; i++)
			if (buckets[i].n && (best < 0 || buckets[i].n > buckets[best].n))
				best = i;
		if (best < 0 || !buckets[best].n) break;
		char who[128];
		unsigned long base;
		whereis(pid, buckets[best].pc, who, sizeof who, &base);
		printf("  %5.1f%%  %-30s +0x%lx\n",
		       100.0 * buckets[best].n / (got ? got : 1), who,
		       buckets[best].pc - base);
		buckets[best].n = 0;
	}
	return 0;
}
