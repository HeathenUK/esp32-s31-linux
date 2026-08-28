// SPDX-License-Identifier: GPL-2.0-only
/*
 * Record the panel to an MJPEG file, using the hardware encoder.
 *
 * Userspace is not in the per-frame path at all: one ioctl starts the
 * recording, the kernel captures a frame whenever the display commits, and a
 * second ioctl stops it. Frames are then drained from the kernel's ring
 * afterwards, so the transfer costs nothing during the window being measured.
 *
 *   mjpegrec  out.mjpeg <seconds> [max_fps] [quality] [ring_kb]
 *   mjpegrec -d out.mjpeg                  drain a recording already running
 *   mjpegrec -c out.mjpeg [fps] [q] [kb]   record continuously until SIGTERM
 *
 * The output is a concatenation of JPEGs, which is what MJPEG is. ffmpeg reads
 * it directly with -f mjpeg. A sidecar .txt records the capture timestamps, so
 * the real (variable) frame timing can be reconstructed - frames are produced
 * on damage, so they are deliberately not evenly spaced.
 *
 * **The timestamps are what make per-commit capture equivalent to capturing
 * every panel refresh.** The panel scans out 59 times a second whether or not
 * anything changed, and encoding an unchanged frame records no information:
 * measured, capturing every other refresh would cost ~21% of a core and
 * ~440 KB/s even on a completely static screen. Instead each frame carries the
 * moment it was captured, the gap to the next frame is how long that image was
 * on screen, and the reconstruction on the host turns that into a constant-rate
 * video by repeating frames - which costs the board nothing. The sidecar's last
 * line is `eof <stamp>`, because the final frame has no successor and its
 * duration would otherwise have to be guessed.
 *
 * Sidecar format, one line per frame:  <seq> <bytes> <stamp_ns>
 * seq is the kernel's sequence number, so a GAP means frames were dropped and
 * the video is genuinely missing time rather than quietly skipping.
 *
 * -c is what films a boot: the kernel arms the recorder at scanout, ~3 s before
 * userspace exists, so this attaches to a recording already in progress rather
 * than starting a new one and throwing that away. It drains to <out>.part and
 * renames to <out> on a clean stop, so the presence of the final name is the
 * signal that the file is complete - a rename within a directory is atomic, and
 * a file that is merely truncated still plays up to its last whole frame.
 */
#include <errno.h>
#include <signal.h>
#include <time.h>
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

static int fd = -1;
static volatile sig_atomic_t stop_now;

static void on_term(int sig) { (void)sig; stop_now = 1; }

/*
 * Pop one frame. Returns 1 if it got one, 0 if the kernel has none waiting,
 * -1 on a real error.
 */
static int pop(uint8_t *buf, size_t cap, struct frame_args *fa)
{
	memset(fa, 0, sizeof(*fa));
	fa->size = cap;
	fa->ptr = (uint64_t)(uintptr_t)buf;
	if (ioctl(fd, IOCTL_FRAME, fa) == 0)
		return 1;
	if (errno == ENOENT)
		return 0;
	fprintf(stderr, "mjpegrec: frame: %s\n", strerror(errno));
	return -1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Drain whatever is waiting into the open files. Returns frames written. */
static int drain(uint8_t *buf, size_t cap, FILE *f, FILE *tf,
		 uint32_t *last_seq, int *gaps)
{
	struct frame_args fa;
	int n = 0, r;

	while ((r = pop(buf, cap, &fa)) == 1) {
		if (*last_seq != (uint32_t)-1 && fa.index != *last_seq + 1)
			(*gaps)++;
		*last_seq = fa.index;
		fwrite(buf, 1, fa.size, f);
		if (tf)
			fprintf(tf, "%u %u %llu\n", fa.index, fa.size,
				(unsigned long long)fa.stamp_ns);
		n++;
	}
	return r < 0 ? -1 : n;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "panel.mjpeg";
	int secs = argc > 2 ? atoi(argv[2]) : 5;
	int fps = argc > 3 ? atoi(argv[3]) : 20;
	int quality = argc > 4 ? atoi(argv[4]) : 80;
	int ring_kb = argc > 5 ? atoi(argv[5]) : 1024;
	int cont = 0, attach = 0;
	struct rec_args ra = { 0 };
	uint8_t *buf;
	FILE *f, *tf;
	char tname[400], pname[300], dname[400];
	uint32_t last_seq = (uint32_t)-1;
	int gaps = 0, total = 0;

    if (argc > 1 && !strcmp(argv[1], "-d")) {
		attach = 1;
		out = argc > 2 ? argv[2] : "panel.mjpeg";
	} else if (argc > 1 && !strcmp(argv[1], "-c")) {
		cont = 1;
		out = argc > 2 ? argv[2] : "panel.mjpeg";
		fps = argc > 3 ? atoi(argv[3]) : 30;
		quality = argc > 4 ? atoi(argv[4]) : 70;
		ring_kb = argc > 5 ? atoi(argv[5]) : 1024;
	}

	/*
	 * The render node, not the card node. The first process to open the
	 * PRIMARY node becomes DRM master, so a drainer started from an init
	 * script took master before the desktop could and lvdesk died with
	 * "SET_MASTER: Resource busy" - after which nothing committed and the
	 * recorder dutifully filmed a blank screen. The recorder needs no
	 * modesetting; its ioctls are DRM_RENDER_ALLOW.
	 *
	 * Falling back to card0 keeps this working on a kernel built before
	 * the driver advertised DRIVER_RENDER, and dropping master immediately
	 * makes that fallback safe rather than merely lucky.
	 */
	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) {
		fd = open("/dev/dri/card0", O_RDWR);
		if (fd < 0) { perror("open card0"); return 1; }
		if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) < 0 && errno != EINVAL)
			fprintf(stderr, "mjpegrec: drop master: %s\n",
				strerror(errno));
	}

	buf = malloc(256 * 1024);
	if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }

	if (argc > 1 && !strcmp(argv[1], "-s")) {
		/*
		 * Status only. The init script needs to know whether the
		 * kernel armed a recording at scanout before it starts a
		 * drainer, or every boot would quietly start filming itself.
		 * Exit status is the answer; stdout is for humans.
		 */
		ra.op = REC_STATUS;
		if (ioctl(fd, IOCTL_REC, &ra) < 0) { perror("REC_STATUS"); return 2; }
		printf("running=%u waiting=%u bytes=%u dropped=%u\n",
		       ra.running, ra.frames, ra.bytes, ra.dropped);
		return ra.running ? 0 : 1;
	}

	if (attach) {
		ra.op = REC_STOP;
		if (ioctl(fd, IOCTL_REC, &ra) < 0) { perror("REC_STOP"); return 1; }
		printf("held %u frames, %u KB, %u dropped\n",
		       ra.frames, ra.bytes >> 10, ra.dropped);
		goto write_out;
	}

	if (cont) {
		/*
		 * Attach if the kernel already armed a recording at boot; only
		 * start one if it did not. Starting unconditionally would
		 * discard the early-boot frames, which are the whole reason
		 * the kernel arms it that early.
		 */
		ra.op = REC_STATUS;
		if (ioctl(fd, IOCTL_REC, &ra) < 0) { perror("REC_STATUS"); return 1; }
		if (!ra.running) {
			memset(&ra, 0, sizeof(ra));
			ra.op = REC_START;
			ra.ring_bytes = (uint32_t)ring_kb * 1024;
			ra.max_fps = fps;
			ra.quality = quality;
			if (ioctl(fd, IOCTL_REC, &ra) < 0) { perror("REC_START"); return 1; }
			printf("started: cap %d fps, q%d, %d KB ring\n",
			       fps, quality, ring_kb);
		} else {
			attach = 1;
			printf("attached to the recording armed at boot\n");
		}

		snprintf(pname, sizeof(pname), "%s.part", out);
		snprintf(tname, sizeof(tname), "%s.txt", pname);
		f = fopen(pname, "wb");
		tf = fopen(tname, "w");
		if (!f || !tf) { perror("fopen"); return 1; }

		signal(SIGTERM, on_term);
		signal(SIGINT, on_term);

		/*
		 * Hand over from the boot recording to the session one.
		 *
		 * The kernel arms a big, slow, low-quality buffer at scanout:
		 * big because nothing drains it for the first few seconds and a
		 * console frame is ~85 KB, slow because a booting console does
		 * not need many frames a second. Neither setting suits the
		 * desktop, and leaving megabytes of buffer allocated for the
		 * whole session on a 15 MB machine is worse still.
		 *
		 * So once the backlog is on disk, stop and restart with the
		 * session settings. That frees the boot buffer, switches to the
		 * requested rate and quality, and costs whatever commits in the
		 * microseconds between the two ioctls - at most one frame, and
		 * it shows up as a sequence gap rather than vanishing quietly.
		 */
		if (attach) {
			int n;

			while ((n = drain(buf, 256 * 1024, f, tf,
					  &last_seq, &gaps)) > 0)
				total += n;
			ra.op = REC_STOP;
			ioctl(fd, IOCTL_REC, &ra);
			printf("boot buffer drained: %d frames, %u dropped before"
			       " the drainer attached\n", total, ra.dropped);

			memset(&ra, 0, sizeof(ra));
			ra.op = REC_START;
			ra.ring_bytes = (uint32_t)ring_kb * 1024;
			ra.max_fps = fps;
			ra.quality = quality;
			if (ioctl(fd, IOCTL_REC, &ra) < 0) {
				perror("REC_START (session)");
				return 1;
			}
			printf("session recording: cap %d fps, q%d, %d KB\n",
			       fps, quality, ring_kb);
		}

		/*
		 * Poll rather than block: there is no wait interface, and the
		 * buffer holds seconds of video, so 100 ms is far tighter than
		 * it needs to be and costs one ioctl that returns ENOENT.
		 */
		while (!stop_now) {
			int n = drain(buf, 256 * 1024, f, tf, &last_seq, &gaps);

			if (n < 0) break;
			total += n;
			usleep(100000);
		}

		ra.op = REC_STOP;
		ioctl(fd, IOCTL_REC, &ra);
		total += drain(buf, 256 * 1024, f, tf, &last_seq, &gaps);

		/* The last frame has no successor, so say when it stopped. */
		fprintf(tf, "eof %llu\n", (unsigned long long)now_ns());

		/*
		 * fsync BOTH before the rename. ext4 defers allocation, and
		 * this project has already been bitten by a file of the right
		 * name and mode that turned out to be zero length after a
		 * reset.
		 */
		fflush(f); fsync(fileno(f)); fclose(f);
		fflush(tf); fsync(fileno(tf)); fclose(tf);

		{
			char fin[300], ftxt[400];

			snprintf(fin, sizeof(fin), "%s", out);
			snprintf(ftxt, sizeof(ftxt), "%s.txt", out);
			rename(pname, fin);
			rename(tname, ftxt);
		}
		snprintf(dname, sizeof(dname), "%s.done", out);
		tf = fopen(dname, "w");
		if (tf) {
			fprintf(tf, "frames %d\ngaps %d\ndropped %u\n",
				total, gaps, ra.dropped);
			fflush(tf); fsync(fileno(tf)); fclose(tf);
		}
		sync();
		printf("wrote %s: %d frames, %d gaps, %u dropped\n",
		       out, total, gaps, ra.dropped);
		close(fd);
		return 0;
	}

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

write_out:
	if (!ra.frames) { printf("nothing captured - was the screen static?\n"); return 0; }

	f = fopen(out, "wb");
	if (!f) { perror("fopen"); return 1; }
	snprintf(tname, sizeof(tname), "%s.txt", out);
	tf = fopen(tname, "w");

	total = drain(buf, 256 * 1024, f, tf, &last_seq, &gaps);
	if (tf) {
		fprintf(tf, "eof %llu\n", (unsigned long long)now_ns());
		fflush(tf); fsync(fileno(tf)); fclose(tf);
	}
	fflush(f); fsync(fileno(f)); fclose(f);
	sync();
	printf("wrote %s: %d frames, %d gaps\n", out, total, gaps);
	close(fd);
	return 0;
}
