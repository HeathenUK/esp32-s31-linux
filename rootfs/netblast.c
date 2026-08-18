// SPDX-License-Identifier: GPL-2.0-only
/*
 * Push UDP frames at an interface as fast as the host will take them, and
 * report the byte rate.
 *
 * The point is to measure the host side of the ESP-Hosted SRAM transport - skb
 * to staging buffer to shared SRAM - without needing an access point. The frames
 * traverse the whole copy path whether or not hart0 has a radio association to
 * send them on, so this isolates host CPU cost per byte from radio throughput.
 *
 *   netblast <dest-ip> <payload-bytes> <seconds>
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *dest = argc > 1 ? argv[1] : "192.168.99.2";
	int size = argc > 2 ? atoi(argv[2]) : 1400;
	double secs = argc > 3 ? atof(argv[3]) : 10.0;
	static char buf[2048];
	struct sockaddr_in sa;
	unsigned long sent = 0, failed = 0;
	double t0, t1;
	int fd;

	if (size < 1 || size > (int)sizeof(buf))
		return fprintf(stderr, "bad size\n"), 1;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return perror("socket"), 1;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(9);
	if (inet_pton(AF_INET, dest, &sa.sin_addr) != 1)
		return fprintf(stderr, "bad address\n"), 1;

	t0 = now();
	do {
		if (sendto(fd, buf, size, 0, (struct sockaddr *)&sa,
			   sizeof(sa)) == size)
			sent++;
		else
			failed++;
		t1 = now();
	} while (t1 - t0 < secs);

	printf("BLAST sent=%lu failed=%lu bytes=%lu secs=%.2f rate=%.2f MB/s pps=%.0f\n",
	       sent, failed, sent * (unsigned long)size, t1 - t0,
	       sent * (double)size / (t1 - t0) / (1024 * 1024),
	       sent / (t1 - t0));
	return 0;
}
