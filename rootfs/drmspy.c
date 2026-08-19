// SPDX-License-Identifier: GPL-2.0-only
/*
 * LD_PRELOAD shim logging exactly what a client asks of drmModeAddFB2.
 *
 * Weston reports only "failed to create kms fb: Invalid argument", and three
 * plausible causes (modifiers, pixel format, pool size) were each tested and
 * ruled out. Rather than guess a fourth, record the arguments.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

static FILE *log_fp(void)
{
	static FILE *fp;

	if (!fp) {
		fp = fopen("/root/drmspy.log", "a");
		if (fp)
			setvbuf(fp, NULL, _IOLBF, 0);
	}
	return fp;
}

int drmModeAddFB2(int fd, uint32_t width, uint32_t height, uint32_t fourcc,
		  const uint32_t handles[4], const uint32_t pitches[4],
		  const uint32_t offsets[4], uint32_t *buf_id, uint32_t flags)
{
	static int (*real)(int, uint32_t, uint32_t, uint32_t, const uint32_t *,
			   const uint32_t *, const uint32_t *, uint32_t *,
			   uint32_t);
	FILE *fp = log_fp();
	int ret;

	if (!real)
		real = dlsym(RTLD_NEXT, "drmModeAddFB2");
	ret = real(fd, width, height, fourcc, handles, pitches, offsets,
		   buf_id, flags);
	if (fp)
		fprintf(fp,
			"AddFB2 %ux%u fourcc=%c%c%c%c handles=%u,%u pitches=%u,%u offsets=%u,%u flags=0x%x -> %d (%s)\n",
			width, height,
			fourcc & 0xff, (fourcc >> 8) & 0xff,
			(fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff,
			handles[0], handles[1], pitches[0], pitches[1],
			offsets[0], offsets[1], flags, ret,
			ret ? strerror(errno) : "ok");
	return ret;
}
