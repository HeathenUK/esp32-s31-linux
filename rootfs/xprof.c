// SPDX-License-Identifier: GPL-2.0-only
/*
 * A sampling profiler for one process, in about a hundred lines.
 *
 * This board has no perf tool and buildroot cannot easily build one - the
 * kernel is built outside buildroot, and a perf without libelf could not
 * resolve symbols anyway. But CONFIG_PERF_EVENTS is available, and
 * perf_event_open() with a software cpu-clock event is all that sampling
 * needs: it interrupts the target at a fixed rate and hands back the
 * instruction pointer each time.
 *
 * Written because X was burning ~11 ms of CPU per pointer motion event with
 * no clients open, roughly half of it in userspace, and nothing on this board
 * could say where. Everything else had been excluded by measurement.
 *
 * Output is a histogram of instruction pointers, resolved offline against the
 * binary on the build host - the target is stripped and has no symbols.
 *
 *   xprof <pid> <seconds> [hz]
 */

#define _GNU_SOURCE
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define NBUCKET 4096

struct bucket {
	uint64_t ip;
	uint32_t count;
};

static struct bucket buckets[NBUCKET];
static unsigned long total, dropped;

static void record(uint64_t ip)
{
	unsigned int i, h = (unsigned int)((ip >> 4) % NBUCKET);

	total++;
	for (i = 0; i < NBUCKET; i++) {
		struct bucket *b = &buckets[(h + i) % NBUCKET];

		if (b->count && b->ip != ip)
			continue;
		b->ip = ip;
		b->count++;
		return;
	}
	dropped++;
}

static int cmp(const void *a, const void *b)
{
	return (int)((const struct bucket *)b)->count -
	       (int)((const struct bucket *)a)->count;
}

int main(int argc, char **argv)
{
	int pid = argc > 1 ? atoi(argv[1]) : 0;
	int secs = argc > 2 ? atoi(argv[2]) : 5;
	int hz = argc > 3 ? atoi(argv[3]) : 1000;
	struct perf_event_attr attr;
	struct perf_event_mmap_page *meta;
	unsigned int pages = 33;	/* 1 metadata + 32 data */
	size_t len = pages * 4096;
	uint64_t tail;
	void *base;
	char *data;
	int fd, i;

	if (!pid) {
		fprintf(stderr, "usage: xprof <pid> [seconds] [hz]\n");
		return 1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_SOFTWARE;
	attr.config = PERF_COUNT_SW_CPU_CLOCK;
	attr.sample_type = PERF_SAMPLE_IP;
	attr.sample_freq = hz;
	attr.freq = 1;
	attr.disabled = 1;
	attr.exclude_kernel = 0;	/* want both halves */
	attr.exclude_hv = 1;

	fd = syscall(__NR_perf_event_open, &attr, pid, -1, -1, 0);
	if (fd < 0) {
		perror("perf_event_open");
		fprintf(stderr, "  (needs CONFIG_PERF_EVENTS and "
				"perf_event_paranoid <= 1)\n");
		return 1;
	}

	base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	meta = base;
	data = (char *)base + 4096;

	ioctl(fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

	tail = meta->data_tail;
	for (i = 0; i < secs * 10; i++) {
		uint64_t head;

		usleep(100000);
		head = meta->data_head;
		__sync_synchronize();

		while (tail < head) {
			size_t off = tail % (32 * 4096);
			struct perf_event_header *hdr =
				(struct perf_event_header *)(data + off);

			if (hdr->size == 0)
				break;
			if (hdr->type == PERF_RECORD_SAMPLE) {
				uint64_t ip;

				memcpy(&ip, data + off + sizeof(*hdr),
				       sizeof(ip));
				record(ip);
			}
			tail += hdr->size;
		}
		meta->data_tail = tail;
		__sync_synchronize();
	}
	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

	qsort(buckets, NBUCKET, sizeof(buckets[0]), cmp);
	printf("XPROF samples=%lu dropped=%lu\n", total, dropped);
	for (i = 0; i < 40 && buckets[i].count; i++)
		printf("XPROF %012llx %6u %5.1f%%\n",
		       (unsigned long long)buckets[i].ip, buckets[i].count,
		       100.0 * buckets[i].count / (total ? total : 1));
	return 0;
}
