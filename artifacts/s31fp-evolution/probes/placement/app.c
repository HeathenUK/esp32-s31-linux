#include <stdio.h>
#include <stdlib.h>
static volatile double sink;
int main(int c, char **v) {
	long n = c > 1 ? atol(v[1]) : 1000; int useDiv = c > 2 ? atoi(v[2]) : 1;
	unsigned x = 12345, h = 2166136261u; double acc = 0.5;
	for (long i = 0; i < n; i++) {
		x = x * 1103515245u + 12345u;
		double a = (double)(int)(x >> 8) * (1.0/1024), b = (double)(int)(x & 0xffff) + 1.0;
		double m = a * b, s = m + acc, d = useDiv ? s / b : s - b;
		float f = (float)d; double e = f;
		acc = (e > acc) ? e * 0.25 : acc - e * 0.125;
		int k = (int)(acc * 3.0);
		h = (h ^ (unsigned)k) * 16777619u; h = (h ^ (unsigned)(x & 7)) * 16777619u;
	}
	sink = acc; printf("%08x %d\n", h, (int)acc);
	return 0;
}
