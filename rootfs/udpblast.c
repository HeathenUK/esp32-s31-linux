/*
 * Max-rate UDP TX for network path measurement - ESP32-S31.
 *
 *   udpblast <ip> <port> <secs> [payload_bytes=1400]
 *
 * Written for the 2026-09-01 hosted-Wi-Fi throughput investigation. The two
 * numbers it produced that matter (see docs/current-state.md):
 *   - 1400 B datagrams: ~385 KB/s over Wi-Fi and ~237 KB/s over LOOPBACK -
 *     the radio was never the TX limit; each sendto costs ~3.6-5.8 ms of
 *     hart1 CPU regardless of destination.
 *   - 16384 B datagrams: ~1.45 MB/s end-to-end over Wi-Fi - the per-call
 *     fixed cost dominates, so fewer+bigger syscalls is the lever that works.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s ip port secs [payload=1400]\n",
			argv[0]);
		return 1;
	}
	int len = argc > 4 ? atoi(argv[4]) : 1400;
	if (len < 1 || len > 65000) {
		fprintf(stderr, "payload out of range\n");
		return 1;
	}
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET,
				 .sin_port = htons(atoi(argv[2])) };
	inet_pton(AF_INET, argv[1], &a.sin_addr);
	char *buf = malloc(len);
	memset(buf, 'x', len);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	long secs = atol(argv[3]), sent = 0, errs = 0;
	for (;;) {
		if (sendto(s, buf, len, 0, (struct sockaddr *)&a,
			   sizeof(a)) < 0)
			errs++;
		else
			sent++;
		if ((sent + errs) % 64 == 0) {
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (t1.tv_sec - t0.tv_sec >= secs)
				break;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("sent %ld pkts x %d B, %ld errs, %.1f s, %.0f KB/s\n", sent,
	       len, errs, el, sent * (double)len / 1024.0 / el);
	return 0;
}
