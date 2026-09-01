/*
 * s31-thumb <in> <out.ppm> [maxdim]  -  also installed AS xfilesthumb.
 *
 * Hardware thumbnail: JPEG decode + PPA scale via one ioctl, preferably on
 * /dev/s31-jpeg (a misc device that opens in microseconds) with
 * /dev/dri/card0 as the fallback. Gates on JPEG magic bytes - no filename
 * filter - and bounds the feed at the first EOI so MJPEG frame one works.
 *
 * Freestanding ON PURPOSE: this runs once per file from xfiles' thumbnail
 * thread, and the measured phase split was ~86 ms of dynamic-link spawn,
 * 64-194 ms of DRM open and ~18 ms of buffered PPM writing against ~10 ms
 * of hardware. Raw syscalls + a static binary + one output write puts the
 * whole tool at the ~53 ms static-spawn floor plus the ioctl. musl-static
 * was 258 KB and did not fit XIP's slack; this is a few KB.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

static long sys(long n, long a, long b, long c, long d, long e)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;

	__asm__ volatile("ecall"
			 : "+r"(a0)
			 : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4)
			 : "memory");
	return a0;
}

#define AT_FDCWD	(-100)
#define O_RDONLY	0
#define O_WRONLY	1
#define O_RDWR		2
#define O_CREAT		0100
#define O_TRUNC		01000

static long xopen(const char *p, long fl, long mode)
{
	return sys(56, AT_FDCWD, (long)p, fl, mode, 0);
}
static long xclose(long fd) { return sys(57, fd, 0, 0, 0, 0); }
static long xread(long fd, void *b, long n) { return sys(63, fd, (long)b, n, 0, 0); }
static long xwrite(long fd, const void *b, long n) { return sys(64, fd, (long)b, n, 0, 0); }
static long xioctl(long fd, long req, void *a) { return sys(29, fd, req, (long)a, 0, 0); }
static void xexit(long c) { sys(94, c, 0, 0, 0, 0); for (;;) ; }

struct jpeg_thumb {
	u64 in_ptr;
	u32 in_len;
	u32 max_dim;
	u64 out_ptr;
	u32 out_max;
	u32 out_w, out_h;
	u32 src_w, src_h;
};
/* _IOWR('d', 0x40 + 0x04, 48-byte struct) */
#define JPEG_THUMB_IOCTL 0xC0306444u

#define IN_MAX (512 * 1024)
static u8 in[IN_MAX];
static u16 out[256 * 256];
static u8 ppm[256 * 256 * 3 + 32];

static long atoi_(const char *s)
{
	long v = 0;

	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return v;
}

static int itoa_(u8 *p, u32 v)
{
	u8 t[10];
	int n = 0, i;

	do {
		t[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	for (i = 0; i < n; i++)
		p[i] = t[n - 1 - i];
	return n;
}

void cmain(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	struct jpeg_thumb t;
	long fd, n, m, card;
	u32 i, px, hl;

	if (argc < 3)
		xexit(1);

	/*
	 * The A/B switch: `s31-thumbs off` creates this file and every
	 * thumbnail attempt exits before touching the SD or the codec.
	 * (xfiles still pays one ~80 ms spawn per file until relaunched
	 * without XDG_CACHE_HOME - the flag kills the WORK, not the exec.)
	 */
	if (sys(48, AT_FDCWD, (long)"/etc/s31-thumbs-off", 0, 0, 0) == 0)
		xexit(1);

	fd = xopen(argv[1], O_RDONLY, 0);
	if (fd < 0)
		xexit(1);
	n = xread(fd, in, 4);
	if (n < 4 || in[0] != 0xff || in[1] != 0xd8) {
		xclose(fd);
		xexit(1);
	}
	m = xread(fd, in + 4, IN_MAX - 4);
	xclose(fd);
	if (m > 0)
		n += m;

	/* Bound the feed at the first EOI: MJPEG files concatenate frames. */
	for (i = 2; i + 1 < (u32)n; i++) {
		if (in[i] == 0xff && in[i + 1] == 0xd9) {
			n = i + 2;
			break;
		}
	}

	card = xopen("/dev/s31-jpeg", O_RDWR, 0);
	if (card < 0)
		card = xopen("/dev/dri/card0", O_RDWR, 0);
	if (card < 0)
		xexit(1);
	t.in_ptr = (u32)(unsigned long)in;
	t.in_len = (u32)n;
	t.max_dim = argc > 3 ? (u32)atoi_(argv[3]) : 64;
	t.out_ptr = (u32)(unsigned long)out;
	t.out_max = sizeof(out);
	t.out_w = t.out_h = t.src_w = t.src_h = 0;
	if (xioctl(card, JPEG_THUMB_IOCTL, &t) != 0) {
		xclose(card);
		xexit(1);
	}
	xclose(card);

	/* Whole PPM in memory, one write. */
	hl = 0;
	ppm[hl++] = 'P'; ppm[hl++] = '6'; ppm[hl++] = '\n';
	hl += itoa_(ppm + hl, t.out_w);
	ppm[hl++] = ' ';
	hl += itoa_(ppm + hl, t.out_h);
	ppm[hl++] = '\n';
	ppm[hl++] = '2'; ppm[hl++] = '5'; ppm[hl++] = '5'; ppm[hl++] = '\n';
	px = t.out_w * t.out_h;
	for (i = 0; i < px; i++) {
		u16 v = out[i];

		ppm[hl + i * 3 + 0] = (v >> 11) << 3 | (v >> 13);
		ppm[hl + i * 3 + 1] = ((v >> 5) & 63) << 2 | ((v >> 9) & 3);
		ppm[hl + i * 3 + 2] = (v & 31) << 3 | ((v >> 2) & 7);
	}
	fd = xopen(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		xexit(1);
	if (xwrite(fd, ppm, hl + px * 3) != (long)(hl + px * 3)) {
		xclose(fd);
		xexit(1);
	}
	xclose(fd);
	xexit(0);
}

__asm__(".section .text\n"
	".global _start\n"
	"_start:\n"
	"	mv a0, sp\n"
	"	call cmain\n");
