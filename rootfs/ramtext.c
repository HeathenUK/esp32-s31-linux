/*
 * Does it matter that our userspace hot loops execute from XIP flash?
 *
 * The kernel's answer is yes and costly: code in the XIP image runs ~6x
 * slower than the same code in RAM, which is the whole reason .text..fast
 * exists (docs/hot-text-plan.md). Userspace here is XIP too - lvdesk's text
 * maps straight out of flash at zero RSS, which is the feature - and nobody
 * has ever tested whether that costs speed. The only comparison on record is
 * flash against SD, which is a different question.
 *
 * There is a strong reason to expect the answer to be NO for a tight loop:
 * there is an instruction cache, and a forty-instruction pixel loop fits in
 * it, so after the first pass the backing store is irrelevant. The kernel's
 * hot paths are large and scattered and thrash it; a pixel loop does not.
 * That is a prediction, and this measures it instead of assuming it.
 *
 * METHOD. One binary, one loop, two addresses. The loop is a self-contained
 * leaf - every pointer arrives as an argument, so there are no PC-relative
 * references to globals and no calls - which means it can simply be copied to
 * an anonymous mapping and called there. Arms alternate, because this project
 * has been burnt repeatedly by two consecutive samples pointing the wrong
 * way.
 *
 * Build: see rootfs/build.sh conventions. Run: ramtext [reps] [px]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

/*
 * THE LOOP UNDER TEST: lvdesk's palette expansion, the paired-store form that
 * ships. Deliberately identical in shape to the real one - eight indices from
 * two 32-bit loads, four 32-bit stores - so that what is measured is the same
 * instruction mix, not a toy.
 *
 * noinline and no static locals: it must be copyable.
 */
__attribute__((noinline, aligned(64), section("text_exp")))
static void expand(uint16_t *dp, const uint8_t *sp, const uint16_t *pal, int n)
{
	int k = 0;
	uint32_t *d32 = (uint32_t *)dp;

	for (; k + 7 < n; k += 8) {
		uint32_t a4 = *(const uint32_t *)(sp + k);
		uint32_t b4 = *(const uint32_t *)(sp + k + 4);

		d32[k >> 1] = (uint32_t)pal[a4 & 0xff] |
			      ((uint32_t)pal[(a4 >> 8) & 0xff] << 16);
		d32[(k >> 1) + 1] = (uint32_t)pal[(a4 >> 16) & 0xff] |
				    ((uint32_t)pal[(a4 >> 24) & 0xff] << 16);
		d32[(k >> 1) + 2] = (uint32_t)pal[b4 & 0xff] |
				    ((uint32_t)pal[(b4 >> 8) & 0xff] << 16);
		d32[(k >> 1) + 3] = (uint32_t)pal[(b4 >> 16) & 0xff] |
				    ((uint32_t)pal[(b4 >> 24) & 0xff] << 16);
	}
	for (; k < n; k++)
		dp[k] = pal[sp[k]];
}

/*
 * THE SECOND ARM, and the one that can actually answer the question.
 *
 * A tight loop fits in the instruction cache, so it is the wrong probe: after
 * the first pass nothing is fetched from anywhere. The case where a flash
 * backing store should hurt is a LARGE, scattered working set that does not
 * fit - which is what the kernel's hot paths are, and what lvdesk's LVGL and
 * event plumbing look like. So: a deliberately big straight-line function,
 * every instruction executed once per call, far more code than any sane
 * cache holds.
 *
 * Still self-contained - all state in the argument and in locals - so it can
 * be copied and called at another address.
 */
#define STEP8(x) do { \
	(x) += 0x9e3779b9u; (x) ^= (x) >> 3; \
	(x) += 0x85ebca6bu; (x) ^= (x) << 5; \
	(x) += 0xc2b2ae35u; (x) ^= (x) >> 7; \
	(x) += 0x27d4eb2fu; (x) ^= (x) << 11; \
	(x) += 0x165667b1u; (x) ^= (x) >> 13; \
	(x) += 0xd3a2646cu; (x) ^= (x) << 17; \
	(x) += 0xfd7046c5u; (x) ^= (x) >> 19; \
	(x) += 0xb55a4f09u; (x) ^= (x) << 23; \
} while (0)
#define STEP64(x)  do { STEP8(x); STEP8(x); STEP8(x); STEP8(x); \
			STEP8(x); STEP8(x); STEP8(x); STEP8(x); } while (0)
#define STEP512(x) do { STEP64(x); STEP64(x); STEP64(x); STEP64(x); \
			STEP64(x); STEP64(x); STEP64(x); STEP64(x); } while (0)

__attribute__((noinline, aligned(64), section("text_big")))
static uint32_t bigcode(uint32_t x)
{
	STEP512(x); STEP512(x); STEP512(x); STEP512(x);
	STEP512(x); STEP512(x); STEP512(x); STEP512(x);
	return x;
}

typedef void (*expand_fn)(uint16_t *, const uint8_t *, const uint16_t *, int);
typedef uint32_t (*big_fn)(uint32_t);

/*
 * EXACT SECTION BOUNDS, not a guessed span.
 *
 * The first version copied a fixed 4 kB from the function's address, which is
 * fine for a small leaf and fatal for a big one: copying 512 kB from a 63 kB
 * binary reads far past the text segment and the arm died silently. ld
 * generates __start_/__stop_ for any section whose name is a valid C
 * identifier, so ask it instead of guessing.
 */
extern char __start_text_exp[], __stop_text_exp[];
extern char __start_text_big[], __stop_text_big[];

/*
 * ARM THREE: the mechanism we would actually ship.
 *
 * Copying a function to a fresh mapping only works for a self-contained leaf.
 * Real code references globals and calls its neighbours, both PC-relative, so
 * a copy at a different address breaks. What is needed is the kernel's
 * .text..fast trick in userspace: code that LIVES at its link address but is
 * backed by RAM rather than flash.
 *
 * FIRST ATTEMPT, and why it is not here: declare the section writable as well
 * as executable, so the loader gives it an RWE segment, then fault it in by
 * writing a byte per page. Two things kill it. GCC emits the section as "ax"
 * and the assembler REFUSES the conflict ("changed section attributes for
 * text_hot") rather than keeping the first flags. And even with the flags, a
 * private file-backed mapping only copies on WRITE, and executing is not
 * writing, so it would have kept fetching from flash anyway.
 *
 * WHAT WORKS: re-back the address range itself.
 *
 *   1. Save the bytes of every page the range touches.
 *   2. mmap MAP_FIXED anonymous RW over exactly those pages.
 *   3. Write the saved bytes back.
 *   4. mprotect to R+X, then fence.i.
 *
 * The code ends up at the SAME address, so every auipc, jal and GOT
 * reference is still correct, and it is now anonymous RAM. Saving whole pages
 * rather than just the section means anything sharing the first and last page
 * survives byte for byte, so the range does not have to be page aligned.
 *
 * No special section flags, no linker script, no relocation processing. The
 * section exists only to delimit the range.
 */
__attribute__((noinline, noipa, aligned(64), section("text_hot")))
static uint32_t bigcode_hot(uint32_t x)
{
	STEP512(x); STEP512(x); STEP512(x); STEP512(x);
	STEP512(x); STEP512(x); STEP512(x); STEP512(x);
	STEP8(x);
	return x;
}

extern char __start_text_hot[], __stop_text_hot[];

/*
 * Move an address range from wherever it is backed to anonymous RAM, in
 * place. Returns bytes moved, or -1 and leaves everything untouched.
 */
static long text_to_ram(void *start, void *stop)
{
	long ps = 4096;
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)(ps - 1);
	uintptr_t b = ((uintptr_t)stop + ps - 1) & ~(uintptr_t)(ps - 1);
	size_t len = (size_t)(b - a);
	void *save;

	if (b <= a)
		return -1;
	save = malloc(len);
	if (!save)
		return -1;
	memcpy(save, (const void *)a, len);
	/*
	 * MAP_FIXED over live code. The pages vanish and reappear as zeros
	 * between these two calls, so NOTHING in this range may execute in
	 * between - which is why the copy back is the very next statement and
	 * why this function itself must not live in the range.
	 */
	if (mmap((void *)a, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		free(save);
		return -1;
	}
	memcpy((void *)a, save, len);
	free(save);
	if (mprotect((void *)a, len, PROT_READ | PROT_EXEC) < 0)
		return -1;
	__asm__ volatile("fence.i" ::: "memory");
	return (long)len;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	/*
	 * MONOTONIC, not the wall clock. ntpd steps this board's wall clock
	 * once per boot and SDL's use of gettimeofday is what turned one
	 * prboom timedemo into "0.1 fps".
	 */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	int reps = argc > 1 ? atoi(argv[1]) : 200;
	int px = argc > 2 ? atoi(argv[2]) : 76800;	/* 320x240 */
	size_t span;
	uint8_t *src;
	uint16_t *dst, *pal;
	void *ram;
	expand_fn f_flash = expand, f_ram;
	int i, arm;
	uint64_t t;

	src = malloc((size_t)px);
	dst = malloc((size_t)px * 2);
	pal = malloc(256 * 2);
	if (!src || !dst || !pal)
		return 1;
	for (i = 0; i < px; i++)
		src[i] = (uint8_t)(i * 7);
	for (i = 0; i < 256; i++)
		pal[i] = (uint16_t)(i * 257);

	/*
	 * The RAM arm. MAP_ANONYMOUS is plain RAM; the flash arm is whatever
	 * the loader gave us, which under the XIP overlay is the flash itself.
	 */
	span = (size_t)(__stop_text_exp - __start_text_exp);
	ram = mmap(NULL, span, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ram == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memcpy(ram, __start_text_exp, span);
	f_ram = (expand_fn)(uintptr_t)((char *)ram +
		((char *)(uintptr_t)f_flash - __start_text_exp));
	if (mprotect(ram, span, PROT_READ | PROT_EXEC) < 0) {
		perror("mprotect");
		return 1;
	}
	/*
	 * mprotect does not invalidate the instruction cache, and we have
	 * just written code through the data side. Without this the first
	 * call can fetch stale bytes.
	 */
	__asm__ volatile("fence.i" ::: "memory");

	/* Correctness before speed: a fast wrong answer is not interesting. */
	memset(dst, 0, (size_t)px * 2);
	f_flash(dst, src, pal, px);
	{
		uint16_t *ref = malloc((size_t)px * 2);

		if (!ref)
			return 1;
		memcpy(ref, dst, (size_t)px * 2);
		memset(dst, 0, (size_t)px * 2);
		f_ram(dst, src, pal, px);
		if (memcmp(ref, dst, (size_t)px * 2)) {
			fprintf(stderr, "ramtext: RAM copy computed a "
				"DIFFERENT result - the function is not "
				"self-contained, the comparison is void\n");
			return 1;
		}
		free(ref);
	}

	printf("ramtext: %d px, %d reps per arm, loop at flash %p ram %p\n",
	       px, reps, (void *)f_flash, (void *)f_ram);
	/* Warm both, then alternate three times each. */
	f_flash(dst, src, pal, px);
	f_ram(dst, src, pal, px);
	for (arm = 0; arm < 6; arm++) {
		expand_fn f = (arm & 1) ? f_ram : f_flash;

		t = now_ns();
		for (i = 0; i < reps; i++)
			f(dst, src, pal, px);
		t = now_ns() - t;
		printf("  %-5s %6llu us/rep  %5llu ns/1000px\n",
		       (arm & 1) ? "RAM" : "FLASH",
		       (unsigned long long)(t / (uint64_t)reps / 1000),
		       (unsigned long long)(t / (uint64_t)reps /
					    (uint64_t)(px / 1000)));
	}

	/* ---- the large-footprint arm ---- */
	{
		size_t bspan = (size_t)(__stop_text_big - __start_text_big);
		void *bram = mmap(NULL, bspan, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		big_fn b_flash = bigcode, b_ram;
		uint32_t chk_f, chk_r;
		int breps = reps / 4 > 0 ? reps / 4 : 1;

		if (bram == MAP_FAILED) {
			perror("mmap big");
			return 0;
		}
		memcpy(bram, __start_text_big, bspan);
		if (mprotect(bram, bspan, PROT_READ | PROT_EXEC) < 0) {
			perror("mprotect big");
			return 0;
		}
		__asm__ volatile("fence.i" ::: "memory");
		b_ram = (big_fn)(uintptr_t)((char *)bram +
			((char *)(uintptr_t)b_flash - __start_text_big));
		printf("ramtext: text_exp %zu bytes, text_big %zu bytes\n",
		       span, bspan);

		chk_f = b_flash(1u);
		chk_r = b_ram(1u);
		if (chk_f != chk_r) {
			fprintf(stderr, "ramtext: bigcode differs (%08x vs "
				"%08x) - not self-contained, arm void\n",
				chk_f, chk_r);
			return 0;
		}
		printf("ramtext: bigcode at flash %p ram %p, %d reps\n",
		       (void *)b_flash, (void *)b_ram, breps);
		for (arm = 0; arm < 6; arm++) {
			big_fn f = (arm & 1) ? b_ram : b_flash;
			uint32_t acc = 1u;

			t = now_ns();
			for (i = 0; i < breps; i++)
				acc = f(acc);
			t = now_ns() - t;
			printf("  %-5s %6llu us/rep  (acc %08x)\n",
			       (arm & 1) ? "RAM" : "FLASH",
			       (unsigned long long)(t / (uint64_t)breps / 1000),
			       acc);
		}
	}

	/* ---- arm three: writable-executable section, faulted to RAM ---- */
	{
		big_fn h = bigcode_hot;
		uint32_t chk;
		int breps = reps / 4 > 0 ? reps / 4 : 1;
		long pages;

		printf("ramtext: text_hot %u bytes at %p\n",
		       (unsigned)(__stop_text_hot - __start_text_hot),
		       (void *)__start_text_hot);
		/* Cold first: still flash-backed at this point. */
		chk = h(1u);
		if (&__stop_text_hot[0] == &__start_text_hot[0]) {
			fprintf(stderr, "ramtext: text_hot is EMPTY - the "
				"function was folded or the section dropped; "
				"arm void\n");
			return 1;
		}
		for (arm = 0; arm < 2; arm++) {
			uint32_t acc = 1u;

			t = now_ns();
			for (i = 0; i < breps; i++)
				acc = h(acc);
			t = now_ns() - t;
			printf("  HOTSEC-flash %6llu us/rep\n",
			       (unsigned long long)(t / (uint64_t)breps /
						    1000));
			(void)acc;
		}
		pages = text_to_ram(__start_text_hot, __stop_text_hot);
		printf("ramtext: re-backed %ld bytes with RAM\n", pages);
		if (pages < 0) {
			fprintf(stderr, "ramtext: re-backing failed\n");
			return 1;
		}
		if (h(1u) != chk) {
			fprintf(stderr, "ramtext: HOTSEC computed a different "
				"result after the fault - the page did not "
				"come across intact\n");
			return 1;
		}
		for (arm = 0; arm < 3; arm++) {
			uint32_t acc = 1u;

			t = now_ns();
			for (i = 0; i < breps; i++)
				acc = h(acc);
			t = now_ns() - t;
			printf("  HOTSEC-ram   %6llu us/rep\n",
			       (unsigned long long)(t / (uint64_t)breps /
						    1000));
			(void)acc;
		}
	}
	return 0;
}
