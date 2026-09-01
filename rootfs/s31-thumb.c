/*
 * s31-thumb <in> <out.ppm> [maxdim]  -  also installed AS xfilesthumb.
 *
 * Hardware thumbnail: the S31 JPEG codec decodes and the PPA scales, via
 * the DRM_IOCTL_ESP32S31_JPEG_THUMB one-call ioctl; this tool only reads
 * the file, bounds the feed at the first EOI (so the first frame of a
 * concatenated MJPEG works), and writes the result as a binary PPM for
 * xfiles' thumbnailer. Exits nonzero on anything unsupported - progressive,
 * grayscale, oversized - and xfiles keeps its generic icon.
 *
 * There is no filename filter, deliberately: the four magic bytes are read
 * first and anything that is not a JPEG exits before the bulk read. That
 * replaces the shell wrapper this used to hide behind - process spawn is
 * ~ms-expensive here and the wrapper's only job was a case statement.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct jpeg_thumb {
	uint64_t in_ptr;
	uint32_t in_len;
	uint32_t max_dim;
	uint64_t out_ptr;
	uint32_t out_max;
	uint32_t out_w, out_h;
	uint32_t src_w, src_h;
};
/* DRM_IOCTL_BASE 'd', DRM_COMMAND_BASE 0x40, ESP32S31_JPEG_THUMB 0x04 */
#define JPEG_THUMB_IOCTL _IOWR('d', 0x44, struct jpeg_thumb)

#define IN_MAX (512 * 1024)

int main(int argc, char **argv)
{
	static uint8_t in[IN_MAX];
	static uint16_t out[256 * 256];
	struct jpeg_thumb t = { 0 };
	uint32_t maxdim = argc > 3 ? (uint32_t)atoi(argv[3]) : 64;
	ssize_t n;
	size_t i, px;
	int fd, card;
	FILE *o;

	if (argc < 3) {
		fprintf(stderr, "usage: %s in.jpg out.ppm [maxdim]\n",
			argv[0]);
		return 1;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0)
		return 1;
	n = read(fd, in, 4);
	if (n < 4 || in[0] != 0xff || in[1] != 0xd8) {
		close(fd);
		return 1;
	}
	{
		ssize_t m = read(fd, in + 4, sizeof(in) - 4);

		if (m > 0)
			n += m;
	}
	close(fd);

	/* Bound the feed at the first EOI: MJPEG files concatenate frames. */
	for (i = 2; i + 1 < (size_t)n; i++) {
		if (in[i] == 0xff && in[i + 1] == 0xd9) {
			n = i + 2;
			break;
		}
	}

	card = open("/dev/dri/card0", O_RDWR);
	if (card < 0)
		return 1;
	t.in_ptr = (uintptr_t)in;
	t.in_len = (uint32_t)n;
	t.max_dim = maxdim;
	t.out_ptr = (uintptr_t)out;
	t.out_max = sizeof(out);
	if (ioctl(card, JPEG_THUMB_IOCTL, &t) != 0) {
		close(card);
		return 1;
	}
	close(card);

	o = fopen(argv[2], "w");
	if (!o)
		return 1;
	fprintf(o, "P6\n%u %u\n255\n", t.out_w, t.out_h);
	px = (size_t)t.out_w * t.out_h;
	for (i = 0; i < px; i++) {
		uint16_t v = out[i];
		uint8_t rgb[3];

		rgb[0] = (v >> 11) << 3 | (v >> 13);
		rgb[1] = ((v >> 5) & 63) << 2 | ((v >> 9) & 3);
		rgb[2] = (v & 31) << 3 | ((v >> 2) & 7);
		fwrite(rgb, 1, 3, o);
	}
	if (fclose(o) != 0)
		return 1;
	return 0;
}
