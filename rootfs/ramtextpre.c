/*
 * LD_PRELOAD: move chosen shared libraries' code out of XIP flash into RAM.
 *
 * WHY A PRELOAD AND NOT A CONSTRUCTOR IN THE LIBRARY ITSELF. The move
 * replaces a range with an anonymous mapping, and between the mmap and the
 * copy-back every byte in it reads as zero. Code doing the move therefore
 * cannot live in the range it is moving. A constructor inside
 * libasound_module_pcm_s31route.so would be moving the page it is executing
 * from. A preload is a different mapping, so the problem does not arise.
 *
 * WHAT IT IS FOR. Userspace on this board executes straight out of the XIP
 * cramfs, and rootfs/ramtext.c measures that at 4.8x slower than RAM for code
 * that overflows the instruction cache. The audio path is the sharpest case:
 * our ALSA plugin is in XIP_ROOTS, so every period write runs the plugin's
 * code from 80 MHz flash, and jitter there is audible as crackling rather
 * than merely slow.
 *
 * libasound itself is the opposite problem and this does not fix it: it is in
 * XIP_SKIP, so it lives on the SD card as ordinary page cache, which means
 * its pages are EVICTABLE and can be re-read from the card in the middle of
 * playback. Re-backing them with anonymous memory incidentally pins them,
 * which is the point.
 *
 *   RAMTEXT_LIBS=libfoo.so,libbar.so          whole executable mapping
 *   RAMTEXT_LIBS=libc.so:59000+2000           one page range inside it
 *   RAMTEXT_VERBOSE=1                         say what moved
 *
 * Matching is by substring so a version suffix does not have to be spelled
 * out. Anything not found is reported, not silently skipped: this project has
 * lost days to optimisations that never actually installed.
 *
 * The page-range form exists because whole libraries are often too big to be
 * worth their RAM. libc's text is ~688 kB here, of which the interesting part
 * for a client is the few pages holding memcpy/memset/memmove - 6.2% of
 * prboom's on-CPU samples landed on one of them. Offsets are FILE offsets in
 * hex, exactly as rootfs/pcpages.c reports them, so a profile line can be
 * pasted in with no arithmetic.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <signal.h>

#define PS 4096u

static int verbose;

/* Exactly the mechanism from lvdesk/hottext.c; see that file for the reasoning
 * and for the two approaches that do not work. */
static long move_range(uintptr_t a, uintptr_t b, const char *what)
{
	size_t len = (size_t)(b - a);
	void *save;
	sigset_t all, prev;
	int failed = 0;

	if (b <= a)
		return -1;
	save = malloc(len);
	if (!save)
		return -1;
	memcpy(save, (const void *)a, len);
	sigfillset(&all);
	sigprocmask(SIG_SETMASK, &all, &prev);
	if (mmap((void *)a, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		failed = 1;
	} else {
		memcpy((void *)a, save, len);
		if (mprotect((void *)a, len, PROT_READ | PROT_EXEC) < 0)
			failed = 2;
		__asm__ volatile("fence.i" ::: "memory");
	}
	sigprocmask(SIG_SETMASK, &prev, NULL);
	if (failed == 1) {
		free(save);
		fprintf(stderr, "ramtext: mmap failed for %s\n", what);
		return -1;
	}
	if (memcmp((const void *)a, save, len)) {
		free(save);
		fprintf(stderr, "ramtext: MOVE CORRUPTED %s - do not trust "
			"this process\n", what);
		return -1;
	}
	free(save);
	if (verbose)
		fprintf(stderr, "ramtext: %s %p-%p (%zu bytes) now in RAM%s\n",
			what, (void *)a, (void *)b, len,
			failed == 2 ? " (still writable: mprotect failed)" : "");
	return (long)len;
}

__attribute__((constructor))
static void ramtext_init(void)
{
	const char *want = getenv("RAMTEXT_LIBS");
	char line[512], self[256];
	FILE *f;
	long total = 0;
	int n = 0;

	verbose = getenv("RAMTEXT_VERBOSE") != NULL;
	if (!want || !*want)
		return;

	/*
	 * Our own path, so we can never select ourselves. Belt and braces -
	 * a user who lists this library by name would otherwise unmap the
	 * code performing the unmap.
	 */
	self[0] = 0;
	f = fopen("/proc/self/maps", "r");
	while (f && fgets(line, sizeof line, f)) {
		uintptr_t lo, hi;
		char perm[8], path[400];

		path[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]",
			   (unsigned long *)&lo, (unsigned long *)&hi,
			   perm, path) < 4)
			continue;
		if ((uintptr_t)(void *)ramtext_init >= lo &&
		    (uintptr_t)(void *)ramtext_init < hi) {
			snprintf(self, sizeof self, "%s", path);
			break;
		}
	}
	if (f) fclose(f);

	f = fopen("/proc/self/maps", "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		uintptr_t lo, hi;
		char perm[8], path[400], *tok, *save_p, *list;

		path[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]",
			   (unsigned long *)&lo, (unsigned long *)&hi,
			   perm, path) < 4)
			continue;
		if (perm[2] != 'x' || !path[0] || path[0] == '[')
			continue;
		if (self[0] && !strcmp(path, self))
			continue;
		list = strdup(want);
		if (!list)
			continue;
		for (tok = strtok_r(list, ",", &save_p); tok;
		     tok = strtok_r(NULL, ",", &save_p)) {
			char nm[256];
			unsigned long roff = 0, rlen = 0;
			uintptr_t a, b;
			long r;
			const char *colon = strchr(tok, ':');

			if (!*tok)
				continue;
			if (colon) {
				size_t k = (size_t)(colon - tok);

				if (k >= sizeof nm)
					k = sizeof nm - 1;
				memcpy(nm, tok, k);
				nm[k] = 0;
				if (sscanf(colon + 1, "%lx+%lx",
					   &roff, &rlen) != 2 || !rlen) {
					fprintf(stderr, "ramtext: bad range "
						"\"%s\" - want "
						"name:hexoff+hexlen\n", tok);
					continue;
				}
			} else {
				snprintf(nm, sizeof nm, "%s", tok);
			}
			if (!strstr(path, nm))
				continue;
			if (rlen) {
				/*
				 * File offsets, translated through this
				 * mapping's own offset - which is why the
				 * mapping's fourth maps field is read above.
				 */
				unsigned long mo;

				if (sscanf(line, "%*lx-%*lx %*s %lx", &mo) != 1)
					mo = 0;
				if (roff < mo || roff - mo >= (hi - lo)) {
					fprintf(stderr, "ramtext: %s offset "
						"%#lx is outside this mapping "
						"(%#lx..%#lx)\n", nm, roff, mo,
						mo + (unsigned long)(hi - lo));
					break;
				}
				a = lo + (roff - mo);
				b = a + rlen;
				if (b > hi)
					b = hi;
				a &= ~(uintptr_t)(PS - 1);
				b = (b + PS - 1) & ~(uintptr_t)(PS - 1);
			} else {
				a = lo & ~(uintptr_t)(PS - 1);
				b = (hi + PS - 1) & ~(uintptr_t)(PS - 1);
			}
			r = move_range(a, b, path);
			if (r > 0) { total += r; n++; }
			break;
		}
		free(list);
	}
	fclose(f);
	if (verbose || !n)
		fprintf(stderr, "ramtext: %d mapping(s), %ld bytes moved to "
			"RAM (wanted \"%s\")\n", n, total, want);
}
