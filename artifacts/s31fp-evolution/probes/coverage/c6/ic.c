#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
uint64_t s31_udivdi3(uint64_t,uint64_t);
extern uint64_t __udivdi3(uint64_t,uint64_t);
volatile uint64_t sink; volatile uint64_t D=1000;
int main(int c,char**v){ int m=atoi(v[1]); long n=atol(v[2]); uint64_t base=4097740000000ULL; /* 44-bit */
 for(long i=0;i<n;i++){ uint64_t a=base+i; sink = m==0? a : m==1? __udivdi3(a,D) : s31_udivdi3(a,D); }
 return 0;}
