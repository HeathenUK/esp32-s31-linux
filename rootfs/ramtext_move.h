/*
 * Move code out of XIP flash (or off the SD card) into anonymous RAM, in
 * place, at its own address. Header-only so the ALSA plugin and the preload
 * share one implementation.
 *
 * WHY. Userspace here executes straight out of the XIP cramfs, and
 * rootfs/ramtext.c measures that at 4.8x slower than RAM for code that
 * overflows the instruction cache. It measures exactly NOTHING for code that
 * fits - a 322-byte loop was 3623 us from flash and 3625 us from RAM - so
 * this is only worth applying to a large, scattered working set.
 *
 * The audio path is the case that matters. Our ALSA plugin is in XIP_ROOTS so
 * it runs from 80 MHz flash, and libasound is in XIP_SKIP so it lives on the
 * SD card as ORDINARY PAGE CACHE - evictable, and re-read from the card if it
 * is evicted, in the middle of playback. Re-backing with anonymous memory
 * both speeds the fetch and pins the pages, and the pinning may matter more.
 *
 * THE ONE RULE: the code doing the move must not be inside the range being
 * moved. Between the mmap and the copy-back every byte in the range reads as
 * zero, so a function moving its own page returns into zeros. Callers arrange
 * this either by being in a different mapping (the preload) or by putting the
 * mover in its own page-aligned section and skipping that page.
 */
#ifndef RAMTEXT_MOVE_H
#define RAMTEXT_MOVE_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <sys/mman.h>

#define RTM_PS 4096u

/* Put the mover in its own page so a caller can skip it. */
#define RTM_MOVER __attribute__((noinline, aligned(4096), \
				 section("text_mover")))

RTM_MOVER
static long rtm_move(uintptr_t a, uintptr_t b, const char *what, int verbose)
{
	size_t len;
	void *save;
	sigset_t all, prev;
	int failed = 0;

	a &= ~(uintptr_t)(RTM_PS - 1);
	b = (b + RTM_PS - 1) & ~(uintptr_t)(RTM_PS - 1);
	if (b <= a)
		return -1;
	len = (size_t)(b - a);
	save = malloc(len);
	if (!save)
		return -1;
	memcpy(save, (const void *)a, len);
	/*
	 * Signals blocked: a handler whose code lives in the range would
	 * execute zeros during the window, and that presents as an
	 * unexplained hang rather than a fault.
	 */
	sigfillset(&all);
	sigprocmask(SIG_SETMASK, &all, &prev);
	if (mmap((void *)a, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
	    MAP_FAILED) {
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
	/* A move that copied the wrong bytes must be loud here, not
	 * arbitrary misbehaviour somewhere else later. */
	if (memcmp((const void *)a, save, len)) {
		free(save);
		fprintf(stderr, "ramtext: MOVE CORRUPTED %s - do not trust "
			"this process\n", what);
		return -1;
	}
	free(save);
	if (verbose)
		fprintf(stderr, "ramtext: %s %p-%p (%zu bytes) in RAM%s\n",
			what, (void *)a, (void *)b, len,
			failed == 2 ? " (mprotect failed; left writable)" : "");
	return (long)len;
}

/*
 * Find the executable mapping whose path contains `needle`. Returns 1 and
 * fills lo/hi, or 0. `skip_addr`, when non-zero, must be an address inside
 * the caller's own mover: the page containing it is excluded by splitting the
 * range, which is what lets a library move its own text.
 */
static int rtm_find_text(const char *needle, uintptr_t *lo, uintptr_t *hi)
{
	char line[512];
	FILE *f = fopen("/proc/self/maps", "r");
	int got = 0;

	while (f && fgets(line, sizeof line, f)) {
		unsigned long a, b;
		char perm[8], path[400];

		path[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]",
			   &a, &b, perm, path) < 4)
			continue;
		if (perm[2] != 'x' || !path[0] || path[0] == '[')
			continue;
		if (!strstr(path, needle))
			continue;
		*lo = (uintptr_t)a;
		*hi = (uintptr_t)b;
		got = 1;
		break;
	}
	if (f)
		fclose(f);
	return got;
}

/*
 * Move a mapping's text, skipping the page that `here` falls in. Pass the
 * address of a function in the calling module's text_mover section.
 */
static long rtm_move_self(const char *needle, const void *here, int verbose)
{
	uintptr_t lo, hi, p, pe;
	long total = 0, r;

	if (!rtm_find_text(needle, &lo, &hi))
		return -1;
	p = (uintptr_t)here & ~(uintptr_t)(RTM_PS - 1);
	pe = p + RTM_PS;
	if (p > lo) {
		r = rtm_move(lo, p, needle, verbose);
		if (r > 0)
			total += r;
	}
	if (pe < hi) {
		r = rtm_move(pe, hi, needle, verbose);
		if (r > 0)
			total += r;
	}
	return total;
}

#endif
