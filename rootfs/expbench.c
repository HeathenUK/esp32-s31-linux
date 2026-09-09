/*
 * What does the 8-bit -> RGB565 expansion actually cost, and where?
 *
 * lvdesk measures 5.1 ms to expand a 320x200 window - 80 ns/pixel, about 25
 * cycles at 320 MHz - for a loop that is load-byte, index-a-512-byte-table,
 * store-halfword. That is 4-5x more than the instructions can accountfor, so
 * the cost is somewhere other than the arithmetic. This separates the
 * candidates instead of arguing about them:
 *
 *   ram    destination is ordinary heap        - the loop's intrinsic cost
 *   fb     destination is the KMS dumb buffer  - adds the real write path
 *
 * This binary lives on the SD card, so its text is demand-paged into RAM
 * rather than executed from XIP flash like lvdesk's. If `ram` here is much
 * faster than lvdesk's 80 ns/pixel for the same work, the difference is
 * lvdesk's instruction fetch, not the loop.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define W 320
#define H 200
#define PANEL_W 800
#define PANEL_H 480
#define REPS 200

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Exactly lvdesk's word-at-a-time inner loop, same shape, same peel. */
static void expand(uint16_t *dst, unsigned dst_pitch, const uint8_t *src,
		   unsigned src_stride, const uint16_t *pal, int wordwise)
{
	int y;

	for (y = 0; y < H; y++) {
		const uint8_t *sp = src + (size_t)y * src_stride;
		uint16_t *dp = (uint16_t *)((uint8_t *)dst + (size_t)y * dst_pitch);
		int k = 0, n = W;

		if (!wordwise) {
			for (k = 0; k < n; k++)
				dp[k] = pal[sp[k]];
			continue;
		}
		if ((((uintptr_t)dp) & 3u) && n > 0) {
			dp[0] = pal[sp[0]];
			k = 1;
		}
		for (; k + 1 < n; k += 2)
			*(uint32_t *)(dp + k) = (uint32_t)pal[sp[k]] |
						((uint32_t)pal[sp[k + 1]] << 16);
		for (; k < n; k++)
			dp[k] = pal[sp[k]];
	}
}

/*
 * IS THE EXPANSION LATENCY-BOUND OR BANDWIDTH-BOUND?
 *
 * docs/current-state.md calls it memory-bound at ~51 MB/s. But ShmPutImage's
 * copy+hash moves 128 kB at ~160 MB/s in the SAME frame, on the same core,
 * through the same PSRAM. Both cannot be bandwidth-limited.
 *
 * The suspicion is the dependent chain: lbu index -> sh1add into the LUT ->
 * lhu entry -> sh store. On an in-order core, iteration k+1's lbu cannot issue
 * until k's chain retires, so nothing is ever in flight to cover a miss.
 *
 * These arms separate the two. All of them read the same source bytes and
 * write the same destination bytes, so the TRAFFIC is identical; only the
 * dependency structure differs.
 *
 *   dep     the real loop - LUT address depends on the loaded byte
 *   nodep   same loads and stores, but the LUT index comes from the counter,
 *           so the second load does not wait on the first. If this is much
 *           faster, the chain is the cost and traffic is not.
 *   word    one lw fetches four indices, four independent LUT lookups
 *   word4   as word, plus 4x unrolled stores
 *
 * `sink` keeps the source loads live in the nodep arm so the compiler cannot
 * delete the stream we are trying to pay for.
 */
static volatile uint32_t sink;

static void arm_dep(uint16_t *dst, unsigned pitch, const uint8_t *src,
		    const uint16_t *pal)
{
	int y, k;

	for (y = 0; y < H; y++) {
		const uint8_t *sp = src + (size_t)y * W;
		uint16_t *dp = (uint16_t *)((uint8_t *)dst + (size_t)y * pitch);

		for (k = 0; k < W; k++)
			dp[k] = pal[sp[k]];
	}
}

static void arm_nodep(uint16_t *dst, unsigned pitch, const uint8_t *src,
		      const uint16_t *pal)
{
	uint32_t acc = 0;
	int y, k;

	for (y = 0; y < H; y++) {
		const uint8_t *sp = src + (size_t)y * W;
		uint16_t *dp = (uint16_t *)((uint8_t *)dst + (size_t)y * pitch);

		for (k = 0; k < W; k++) {
			acc += sp[k];		/* same load, not an address */
			dp[k] = pal[k & 0xff];	/* LUT load independent of it */
		}
	}
	sink = acc;
}

static void arm_word(uint16_t *dst, unsigned pitch, const uint8_t *src,
		     const uint16_t *pal)
{
	int y, k;

	for (y = 0; y < H; y++) {
		const uint8_t *sp = src + (size_t)y * W;
		uint16_t *dp = (uint16_t *)((uint8_t *)dst + (size_t)y * pitch);

		for (k = 0; k + 3 < W; k += 4) {
			uint32_t v = *(const uint32_t *)(sp + k);

			dp[k]     = pal[v & 0xff];
			dp[k + 1] = pal[(v >> 8) & 0xff];
			dp[k + 2] = pal[(v >> 16) & 0xff];
			dp[k + 3] = pal[(v >> 24) & 0xff];
		}
		for (; k < W; k++)
			dp[k] = pal[sp[k]];
	}
}

static void arm_word8(uint16_t *dst, unsigned pitch, const uint8_t *src,
		      const uint16_t *pal)
{
	int y, k;

	for (y = 0; y < H; y++) {
		const uint8_t *sp = src + (size_t)y * W;
		uint16_t *dp = (uint16_t *)((uint8_t *)dst + (size_t)y * pitch);

		for (k = 0; k + 7 < W; k += 8) {
			uint32_t a = *(const uint32_t *)(sp + k);
			uint32_t b = *(const uint32_t *)(sp + k + 4);

			dp[k]     = pal[a & 0xff];
			dp[k + 1] = pal[(a >> 8) & 0xff];
			dp[k + 2] = pal[(a >> 16) & 0xff];
			dp[k + 3] = pal[(a >> 24) & 0xff];
			dp[k + 4] = pal[b & 0xff];
			dp[k + 5] = pal[(b >> 8) & 0xff];
			dp[k + 6] = pal[(b >> 16) & 0xff];
			dp[k + 7] = pal[(b >> 24) & 0xff];
		}
		for (; k < W; k++)
			dp[k] = pal[sp[k]];
	}
}

static void run_arms(uint16_t *dst, unsigned pitch, const uint8_t *src,
		     const uint16_t *pal)
{
	struct { const char *n; void (*f)(uint16_t *, unsigned,
					  const uint8_t *, const uint16_t *); }
	arms[] = {
		{ "dep   (today's loop)", arm_dep },
		{ "nodep (chain broken)", arm_nodep },
		{ "word  (4/lw)        ", arm_word },
		{ "word8 (8/2lw)       ", arm_word8 },
	};
	int a, r;

	printf("\n--- dependency arms: identical traffic, different chains ---\n");
	/* Alternate the arms so drift cannot favour whichever ran first. */
	for (r = 0; r < 3; r++) {
		for (a = 0; a < 4; a++) {
			uint64_t t0, t1;
			int i;

			arms[a].f(dst, pitch, src, pal);	/* warm */
			t0 = now_ns();
			for (i = 0; i < REPS; i++)
				arms[a].f(dst, pitch, src, pal);
			t1 = now_ns();
			printf("  rep%d %s  %6llu us/frame  %5llu ns/px\n",
			       r, arms[a].n,
			       (unsigned long long)((t1 - t0) / REPS / 1000),
			       (unsigned long long)((t1 - t0) / REPS / (W * H)));
			fflush(stdout);
		}
	}
}

static void run(const char *name, uint16_t *dst, unsigned pitch,
		const uint8_t *src, const uint16_t *pal, int wordwise)
{
	uint64_t t0, t1;
	int r;

	expand(dst, pitch, src, W, pal, wordwise);	/* warm */
	t0 = now_ns();
	for (r = 0; r < REPS; r++)
		expand(dst, pitch, src, W, pal, wordwise);
	t1 = now_ns();
	printf("%-22s %s  %6llu us/frame  %5llu ns/px\n", name,
	       wordwise ? "word" : "half",
	       (unsigned long long)((t1 - t0) / REPS / 1000),
	       (unsigned long long)((t1 - t0) / REPS / (W * H)));
	fflush(stdout);
}

int main(void)
{
	uint8_t *src = malloc(W * H);
	uint16_t *pal = malloc(256 * 2);
	uint16_t *ram = malloc(PANEL_W * PANEL_H * 2);
	int i, fd;

	for (i = 0; i < W * H; i++)
		src[i] = i * 7;			/* not a flat run */
	for (i = 0; i < 256; i++)
		pal[i] = (uint16_t)(i * 257);

	run("heap dst", ram, PANEL_W * 2, src, pal, 1);
	run("heap dst", ram, PANEL_W * 2, src, pal, 0);
	run_arms(ram, PANEL_W * 2, src, pal);

	/* The real destination: a KMS dumb buffer, mapped exactly as lvdesk does. */
	fd = open("/dev/dri/card0", O_RDWR);
	if (fd >= 0) {
		struct drm_mode_create_dumb creq;
		struct drm_mode_map_dumb mreq;
		void *map;

		memset(&creq, 0, sizeof creq);
		creq.width = PANEL_W; creq.height = PANEL_H; creq.bpp = 16;
		if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) == 0) {
			memset(&mreq, 0, sizeof mreq);
			mreq.handle = creq.handle;
			if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == 0) {
				map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE,
					   MAP_SHARED, fd, mreq.offset);
				if (map != MAP_FAILED) {
					run("dumb buffer dst", map, creq.pitch,
					    src, pal, 1);
					run("dumb buffer dst", map, creq.pitch,
					    src, pal, 0);
					munmap(map, creq.size);
				} else {
					printf("dumb buffer: mmap failed\n");
				}
			}
		} else {
			printf("dumb buffer: CREATE_DUMB failed\n");
		}
		close(fd);
	} else {
		printf("dumb buffer: no /dev/dri/card0\n");
	}
	return 0;
}
