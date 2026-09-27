#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
extern uint64_t M; extern int SH, ADD; extern void setup(uint64_t); extern uint64_t ref(uint64_t,uint64_t);
/* specialised K=1000: n/1000 = ((n>>3) * m') >> s' with 61-bit dividend -> no ADD fixup (standard pre-shift trick, 1000=8*125) */
static inline uint64_t mulh(uint64_t a, uint64_t b){
  uint32_t a0=a,a1=a>>32,b0=b,b1=b>>32;
  uint64_t p00=(uint64_t)a0*b0, p01=(uint64_t)a0*b1, p10=(uint64_t)a1*b0, p11=(uint64_t)a1*b1;
  uint64_t mid=(p00>>32)+(uint32_t)p01+(uint32_t)p10;
  return p11+(p01>>32)+(p10>>32)+(mid>>32);
}
#define MAG 0x20C49BA5E353F7CFull   /* ceil(2^66/125)?? verified below */
__attribute__((noinline)) uint64_t div1000(uint64_t n){ return mulh(n>>3, MAG) >> 4; }
static uint64_t s=88172645463325252ull; static uint64_t r64(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
uint64_t in[4096];
int main(int argc,char**argv){ long n=atol(argv[1]); int mode=atoi(argv[2]); unsigned long bad=0; uint64_t sink=0;
  for(int i=0;i<4096;i++) in[i]=r64();
  if(mode==9){ for(long i=0;i<n;i++){ uint64_t x=r64(); if(i&1) x>>=r64()%64; if(i%5==0) x=~0ull-(r64()%4096); if(i%5==1) x=(x/1000)*1000-(i&2?1:0);
      if(div1000(x)!=ref(x,1000)){ if(bad<3) printf("BAD %llu\n",(unsigned long long)x); bad++;} } printf("mismatch=%lu of %ld\n",bad,n); return 0; }
  if(mode==2) for(long i=0;i<n;i++) sink+=div1000(in[i&4095]); else for(long i=0;i<n;i++) sink+=in[i&4095];
  printf("%llu\n",(unsigned long long)sink); }
