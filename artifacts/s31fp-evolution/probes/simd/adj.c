#include <stdio.h>
#include <stdint.h>
#define ADJUST_VOLUME(s, v) (s = (s*v)/128)
static inline int tdiv128(int x){ return (x + ((x >> 31) & 127)) >> 7; }
int main(void){ long bad=0,n=0; for (int v=-256; v<=256; v++) for (int s=-32768;s<32768;s++){ volatile int16_t r=s; int16_t rr=r; ADJUST_VOLUME(rr,v); int t=(int16_t)tdiv128(s*v); n++; if (rr!=t) bad++; } printf("adjust: all s16 x v in [-256,256]: n=%ld mismatches=%ld\n",n,bad); return 0;}
