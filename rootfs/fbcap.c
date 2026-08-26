// SPDX-License-Identifier: GPL-2.0-only
/*
 * Record the panel to a file, to show motion rather than describe it.
 *
 * Two constraints shape this. The SoC has a hardware JPEG encoder
 * (SOC_JPEG_ENCODE_SUPPORTED) but Linux has no driver for it and it is not in
 * the device tree, so encoding here would be software and would compete with
 * the very responsiveness being filmed. And writing raw frames to the card
 * during capture is 2-5 MB/s of DMA and interrupts, for the same reason.
 *
 * So: capture into RAM, RLE-compressed, and write once at the end. The desktop
 * is mostly flat colour, so run-length coding is both cheap - one pass, no
 * search - and effective. Encoding to a real video format happens on the host,
 * where it costs nothing that matters.
 *
 * It also injects keystrokes while recording, so the clip shows characters
 * appearing rather than a still screen.
 *
 * Output: [magic][w][h][nframes] then per frame [u32 nbytes][pairs of u16
 * value,count]. Decoded by scripts/board/fbcap-decode.py.
 */

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DBG "/sys/kernel/debug/esp32s31_lcd/updates"
#define MAGIC 0x50414331	/* "PAC1" */

static unsigned long fb_base, pool_base;
static size_t fb_size = 768000;
static int W = 800, H = 480;

static uint64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int query(void)
{
	char buf[2048], *p;
	FILE *f = fopen(DBG, "r");
	int n;

	if (!f)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strstr(buf, "scanout=");
	if (!p)
		return -1;
	fb_base = strtoul(p + 8, NULL, 0);
	p = strstr(buf, "size=");
	if (p)
		fb_size = strtoul(p + 5, NULL, 10);
	return fb_base ? 0 : -1;
}

/* One pass, no search: value then run length, both 16-bit. */
static size_t rle(const volatile uint16_t *src, size_t npx, uint8_t *out, size_t cap)
{
	size_t i = 0, o = 0;

	while (i < npx) {
		uint16_t v = src[i];
		uint32_t run = 1;

		while (i + run < npx && src[i + run] == v && run < 65535)
			run++;
		if (o + 4 > cap)
			return 0;
		out[o++] = v & 0xff; out[o++] = v >> 8;
		out[o++] = run & 0xff; out[o++] = run >> 8;
		i += run;
	}
	return o;
}

static void emit(int fd, int t, int c, int v)
{
	struct input_event e = { .type = t, .code = c, .value = v };

	if (write(fd, &e, sizeof(e)) != sizeof(e)) { }
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 60;
	int interval = argc > 2 ? atoi(argv[2]) : 120;	/* ms between frames */
	const char *out = argc > 3 ? argv[3] : "/tmp/panel.pac";
	size_t budget = 3200000, used = 0;
	uint8_t *buf, *fbuf;
	uint32_t *sizes;
	volatile uint8_t *fb;
	int memfd, ui, i, kept = 0;
	struct uinput_setup us;
	FILE *f;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (query() < 0) { fprintf(stderr, "fbcap: need debugfs (DIAG=1)\n"); return 1; }
	pool_base = fb_base & ~0x3fffffUL;
	H = fb_size / (W * 2);
	if (H <= 0 || H > 480) { W = 640; H = fb_size / (W * 2); }
	printf("fbcap: %dx%d from 0x%08lx, %d frames every %d ms\n", W, H, fb_base, frames, interval);

	memfd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (memfd < 0) { perror("/dev/mem"); return 1; }
	fb = mmap(NULL, fb_size, PROT_READ, MAP_SHARED, memfd, fb_base);
	if (fb == MAP_FAILED) { perror("mmap"); return 1; }

	buf = malloc(budget);
	sizes = calloc(frames, sizeof(*sizes));
	fbuf = malloc(W * H * 4);
	if (!buf || !sizes || !fbuf) { fprintf(stderr, "fbcap: out of memory\n"); return 1; }

	/* a keyboard, so the clip shows typing rather than a still screen */
	ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (ui >= 0) {
		ioctl(ui, UI_SET_EVBIT, EV_KEY);
		ioctl(ui, UI_SET_EVBIT, EV_SYN);
		for (i = KEY_1; i <= KEY_0; i++) ioctl(ui, UI_SET_KEYBIT, i);
		ioctl(ui, UI_SET_KEYBIT, KEY_A);
		ioctl(ui, UI_SET_KEYBIT, KEY_ENTER);
		memset(&us, 0, sizeof(us));
		us.id.bustype = BUS_USB; us.id.vendor = 0x1d6b; us.id.product = 0x0106;
		strcpy(us.name, "fbcap");
		ioctl(ui, UI_DEV_SETUP, &us);
		ioctl(ui, UI_DEV_CREATE, &ui);
		sleep(3);
	}

	for (i = 0; i < frames; i++) {
		uint64_t due = now_ms() + interval;
		size_t n;

		/* type something every other frame so motion is visible */
		if (ui >= 0 && (i % 2) == 0) {
			int code = (i % 20 == 18) ? KEY_ENTER : KEY_A + (i % 8);

			emit(ui, EV_KEY, code, 1); emit(ui, EV_SYN, SYN_REPORT, 0);
			emit(ui, EV_KEY, code, 0); emit(ui, EV_SYN, SYN_REPORT, 0);
		}

		n = rle((const volatile uint16_t *)fb, (size_t)W * H,
			buf + used, budget - used);
		if (!n) { printf("fbcap: RAM budget reached at frame %d\n", i); break; }
		sizes[i] = n;
		used += n;
		kept++;
		while (now_ms() < due)
			usleep(2000);
	}

	f = fopen(out, "wb");
	if (!f) { perror(out); return 1; }
	{
		uint32_t hdr[4] = { MAGIC, (uint32_t)W, (uint32_t)H, (uint32_t)kept };

		fwrite(hdr, sizeof(hdr), 1, f);
	}
	used = 0;
	for (i = 0; i < kept; i++) {
		fwrite(&sizes[i], 4, 1, f);
		fwrite(buf + used, sizes[i], 1, f);
		used += sizes[i];
	}
	fclose(f);
	printf("fbcap: %d frames, %lu bytes to %s (%.1fx compression)\n",
	       kept, (unsigned long)used, out,
	       kept ? (double)kept * W * H * 2 / (double)used : 0.0);
	return 0;
}
