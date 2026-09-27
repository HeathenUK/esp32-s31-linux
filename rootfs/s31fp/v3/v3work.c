/* v3work [iters] - a dynamically linked process doing PIE-eligible libc
 * string work (>= 64-byte memcpy/memcmp/memchr/strcmp through the PLT),
 * for the PIE-bounce count: started on CPU1 with A1 off it traps and is
 * moved; with A1 on it should not. Prints the CPU it ended on. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 1000;
	static char a[4096] __attribute__((aligned(16))), b[4096] __attribute__((aligned(16)));
	volatile long s = 0;
	int c0 = sched_getcpu();
	memset(a, 'x', sizeof(a) - 1);
	for (int i = 0; i < n; i++) {
		memcpy(b, a, 1024 + (i & 63));
		b[4095] = 0;
		s += memcmp(a, b, 512);
		s += (long)memchr(a, 'y', 2048);
		s += strcmp(a + 16, b + 16);
	}
	printf("WORK cpu start %d end %d sink %ld\n", c0, sched_getcpu(), (long)(s & 1));
	return 0;
}
