// SPDX-License-Identifier: GPL-2.0-only
/*
 * Drive the hardware JPEG encoder at a fixed rate, with no per-frame forks.
 *
 * The shell version of this loop (`echo ... > jpeg; usleep 100000`) forks
 * busybox for every usleep, and on this board that costs more than the encode
 * does. Measuring the driver through it attributed the harness's cost to the
 * hardware. This opens the control file once and paces with clock_nanosleep.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CTL "/sys/kernel/debug/esp32s31_ppa/jpeg"
#define UPD "/sys/kernel/debug/esp32s31_lcd/updates"

static unsigned long scanout_addr(void)
{
	char buf[512], *p;
	unsigned long a = 0;
	int fd = open(UPD, O_RDONLY);
	ssize_t n;

	if (fd < 0) return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return 0;
	buf[n] = 0;
	p = strstr(buf, "scanout=0x");
	if (!p) return 0;
	a = strtoul(p + 10, NULL, 16);
	return a;
}

int main(int argc, char **argv)
{
	int fps = argc > 1 ? atoi(argv[1]) : 10;
	int secs = argc > 2 ? atoi(argv[2]) : 10;
	int quality = argc > 3 ? atoi(argv[3]) : 85;
	int w = argc > 4 ? atoi(argv[4]) : 800;
	int h = argc > 5 ? atoi(argv[5]) : 480;
	unsigned long addr = scanout_addr();
	struct timespec next;
	long period_ns;
	int fd, frames = 0, fails = 0;
	char cmd[96];
	int len;

	if (!addr) { fprintf(stderr, "jpegcap: no scanout address\n"); return 1; }
	fd = open(CTL, O_WRONLY);
	if (fd < 0) { perror("jpegcap: open"); return 1; }

	/* Geometry is fixed at the panel's; the driver rejects anything else. */
	len = snprintf(cmd, sizeof(cmd), "%lx %d %d %d", addr, w, h, quality);
	period_ns = fps > 0 ? 1000000000L / fps : 0;

	printf("jpegcap: %dx%d %d fps for %d s, q%d, scanout 0x%lx\n",
	       w, h, fps, secs, quality, addr);
	clock_gettime(CLOCK_MONOTONIC, &next);

	while (frames < fps * secs) {
		if (write(fd, cmd, len) != len)
			fails++;
		frames++;
		if (!period_ns)
			continue;
		next.tv_nsec += period_ns;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	close(fd);
	printf("jpegcap: %d frames, %d failed\n", frames, fails);
	return 0;
}
