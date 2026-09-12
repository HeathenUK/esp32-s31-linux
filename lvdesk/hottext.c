/*
 * See hottext.h for why this exists and what it measured.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <signal.h>
#include "hottext.h"

/*
 * The section bounds, reached through DATA pointers rather than directly.
 *
 * Referencing __start_/__stop_ from code produces a PC-relative sequence the
 * linker may relax to a compressed LUI, and once the section sits far from
 * the code referencing it the link fails with "relocation truncated to fit:
 * R_RISCV_RVC_LUI against __stop_text_hot". A relocated data word has no such
 * range limit, and it costs one load.
 */
extern char __start_text_hot[], __stop_text_hot[];
static char *const hot_start = __start_text_hot;
static char *const hot_stop = __stop_text_hot;

extern char __start_text_mover[], __stop_text_mover[];
static char *const mover_start = __start_text_mover;
static char *const mover_stop = __stop_text_mover;

TEXTMOVER
long text_to_ram(void *start, void *stop)
{
	long ps = 4096;
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)(ps - 1);
	uintptr_t b = ((uintptr_t)stop + (uintptr_t)ps - 1) &
		      ~(uintptr_t)(ps - 1);
	size_t len;
	void *save;

	if (b <= a)
		return -1;
	len = (size_t)(b - a);
	/*
	 * REFUSE TO MOVE THE GROUND WE ARE STANDING ON. Between the mmap and
	 * the memcpy below, every page in the range reads as zero. If this
	 * function is in there, the return from mmap() executes zeros.
	 */
	if (a < (uintptr_t)mover_stop && b > (uintptr_t)mover_start) {
		uintptr_t ms = (uintptr_t)mover_start &
			       ~(uintptr_t)(ps - 1);
		uintptr_t me = ((uintptr_t)mover_stop + (uintptr_t)ps - 1) &
			       ~(uintptr_t)(ps - 1);

		/*
		 * The linker puts text_mover straight after text_hot, so they
		 * share a page and the rounded range swallows the mover. Clip
		 * rather than refuse: give up the pages the mover sits on and
		 * move the rest. Those pages stay in flash, which costs a
		 * couple of kB of hot code and is worth far more than moving
		 * nothing.
		 */
		if (ms > a && me >= b)
			b = ms;		/* mover at the top: drop the tail */
		else if (me < b && ms <= a)
			a = me;		/* mover at the bottom: drop the head */
		else {
			fprintf(stderr, "hottext: the mover (%p-%p) is in the "
				"middle of %p-%p; not moving anything\n",
				(void *)mover_start, (void *)mover_stop,
				(void *)a, (void *)b);
			return -1;
		}
		if (b <= a)
			return -1;
		len = (size_t)(b - a);
		fprintf(stderr, "hottext: clipped around the mover, moving "
			"%p-%p\n", (void *)a, (void *)b);
	}
	save = malloc(len);
	if (!save)
		return -1;
	memcpy(save, (const void *)a, len);
	/*
	 * BLOCK SIGNALS ACROSS THE WINDOW.
	 *
	 * Between the mmap and the memcpy below, every byte in the range
	 * reads as zero. This function and its callees are outside the range
	 * by construction, but a SIGNAL is not: a handler whose code happens
	 * to live in the range would execute zeros, and the failure would
	 * look like an unexplained hang rather than a fault in this function.
	 * lvdesk carries handlers for SIGCHLD and SIGUSR1 and the window is
	 * a couple of memcpys wide, so this is unlikely and cheap to remove
	 * entirely. An intermittent hang is not worth leaving a candidate
	 * mechanism for.
	 */
	{
		sigset_t all, prev;
		int failed = 0;

		sigfillset(&all);
		sigprocmask(SIG_SETMASK, &all, &prev);
		if (mmap((void *)a, len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
		    MAP_FAILED) {
			failed = 1;
		} else {
			memcpy((void *)a, save, len);
			if (mprotect((void *)a, len,
				     PROT_READ | PROT_EXEC) < 0)
				fprintf(stderr, "hottext: mprotect back to "
					"R+X failed; the range is left "
					"writable\n");
			/* Code written through the data side. */
			__asm__ volatile("fence.i" ::: "memory");
		}
		sigprocmask(SIG_SETMASK, &prev, NULL);
		if (failed) {
			free(save);
			return -1;
		}
	}
	/*
	 * VERIFY, then free. A move that copied the wrong bytes would show up
	 * as arbitrary misbehaviour somewhere else entirely - the worst kind
	 * of bug to chase, and one this project has paid for more than once.
	 * A memcmp of 12 kB, once, at startup, makes it impossible.
	 */
	if (memcmp((const void *)a, save, len)) {
		fprintf(stderr, "hottext: MOVE CORRUPTED %p-%p - the range "
			"does not match what was there. Do not trust this "
			"process.\n", (void *)a, (void *)(a + len));
		free(save);
		return -1;
	}
	free(save);
	return (long)len;
}

void hottext_init(void)
{
	size_t n = (size_t)(hot_stop - hot_start);
	long moved;

	if (getenv("LVDESK_NOHOTTEXT")) {
		fprintf(stderr, "hottext: disabled by LVDESK_NOHOTTEXT "
			"(%zu bytes would have moved)\n", n);
		return;
	}
	if (!n) {
		fprintf(stderr, "hottext: text_hot is EMPTY - nothing is "
			"marked HOTTEXT, or -fipa-icf folded it all away\n");
		return;
	}
	/*
	 * SWEEP MODE, for finding the hot cluster without a profiler.
	 *
	 * LVDESK_RAMTEXT=<hex offset>:<hex length> moves an arbitrary window
	 * of our own text instead of the marked section. The load address is
	 * 0x10000 here. Run the same workload across a series of windows and
	 * the one that moves the frame rate is the one that matters - which is
	 * cheaper than getting an on-CPU profiler working, and answers the
	 * same question.
	 */
	{
		const char *w = getenv("LVDESK_RAMTEXT");

		if (w) {
			unsigned long off = 0, len = 0;

			if (sscanf(w, "%lx:%lx", &off, &len) == 2 && len) {
				char *s = (char *)(uintptr_t)off;

				moved = text_to_ram(s, s + len);
				fprintf(stderr, "hottext: sweep %p+%#lx -> "
					"%ld bytes in RAM\n", (void *)s, len,
					moved);
				return;
			}
			fprintf(stderr, "hottext: LVDESK_RAMTEXT wants "
				"<hexoffset>:<hexlen>, got \"%s\"\n", w);
		}
	}
	moved = text_to_ram(hot_start, hot_stop);
	fprintf(stderr, "hottext: text_hot %zu bytes at %p -> %ld bytes now "
		"in RAM\n", n, (void *)hot_start, moved);
}
