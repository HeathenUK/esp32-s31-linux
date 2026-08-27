// SPDX-License-Identifier: GPL-2.0-only
/*
 * Record the panel to an MJPEG file, using the hardware encoder.
 *
 * Userspace is not in the per-frame path at all: one ioctl starts the
 * recording, the kernel captures a frame whenever the display commits, and a
 * second ioctl stops it. Frames are then drained from the kernel's ring
 * afterwards, so the transfer costs nothing during the window being measured.
 *
 *   mjpegrec out.mjpeg <seconds> [max_fps] [quality] [ring_kb]
 *
 * The output is a concatenation of JPEGs, which is what MJPEG is. ffmpeg reads
 * it directly with -f mjpeg. A sidecar .txt records the capture timestamps, so
 * the real (variable) frame timing can be reconstructed - frames are produced
 * on damage, so they are deliberately not evenly spaced.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>

enum { REC_STATUS = 0, REC_START, REC_STOP };

struct rec_args {
	uint32_t op, ring_bytes, max_fps, quality;
	uint32_t frames, bytes, dropped, running;
};

struct frame_args {
	uint32_t index, size;
	uint64_t ptr, stamp_ns;
};

#define DRM_COMMAND_BASE 0x40
#define IOCTL_REC	_IOWR('d', DRM_COMMAND_BASE + 1, struct rec_args)
#define IOCTL_FRAME	_IOWR('d', DRM_COMMAND_BASE + 2, struct frame_args)

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "panel.mjpeg";
	int secs = argc > 2 ? atoi(argv[2]) : 5;
	int fps = argc > 3 ? atoi(argv[3]) : 20;
	int quality = argc > 4 ? atoi(argv[4]) : 80;
	int ring_kb = argc > 5 ? atoi(argv[5]) : 1024;
	struct rec_args ra = { 0 };
	struct frame_args fa;
	uint8_t *buf;
	FILE *f, *tf;
	char tname[256];
	int fd, i;

	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) { perror("open card0"); return 1; }

	ra.op = REC_START;
	ra.ring_bytes = (uint32_t)ring_kb * 1024;
	ra.max_fps = fps;
	ra.quality = quality;
	if (ioctl(fd, IOCTL_REC, &ra) < 0) { perror("REC_START"); return 1; }
	printf("recording %d s, cap %d fps, q%d, %d KB ring\n",
	       secs, fps, quality, ring_kb);

	sleep(secs);

	ra.op = REC_STOP;
	if (ioctl(fd, IOCTL_REC, &ra) < 0) { perror("REC_STOP"); return 1; }
	printf("captured %u frames, %u KB, %u dropped\n",
	       ra.frames, ra.bytes >> 10, ra.dropped);
	if (!ra.frames) { printf("nothing captured - was the screen static?\n"); return 0; }

	buf = malloc(256 * 1024);
	if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }
	f = fopen(out, "wb");
	if (!f) { perror("fopen"); return 1; }
	snprintf(tname, sizeof(tname), "%s.txt", out);
	tf = fopen(tname, "w");

	for (i = 0; i < (int)ra.frames; i++) {
		memset(&fa, 0, sizeof(fa));
		fa.index = i;
		fa.size = 256 * 1024;
		fa.ptr = (uint64_t)(uintptr_t)buf;
		if (ioctl(fd, IOCTL_FRAME, &fa) < 0) {
			fprintf(stderr, "frame %d: %s\n", i, strerror(errno));
			break;
		}
		fwrite(buf, 1, fa.size, f);
		if (tf)
			fprintf(tf, "%d %u %llu\n", i, fa.size,
				(unsigned long long)fa.stamp_ns);
	}
	fclose(f);
	if (tf) fclose(tf);
	printf("wrote %s\n", out);
	close(fd);
	return 0;
}
