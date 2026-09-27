#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
/* 64/32 -> 32 with u1<v (Hacker's Delight divlu, clz-normalised) */
static inline uint32_t divlu(uint32_t u1,uint32_t u0,uint32_t v,uint32_t*r){
  const uint32_t b=65536; int s=__builtin_clz(v); v<<=s;
  uint32_t vn1=v>>16, vn0=v&0xffff;
  uint32_t un32= s? (u1<<s)|(u0>>(32-s)) : u1; uint32_t un10=u0<<s;
  uint32_t un1=un10>>16, un0=un10&0xffff;
  uint32_t q1=un32/vn1, rhat=un32-q1*vn1;
  while(q1>=b || q1*vn0 > b*rhat+un1){ q1--; rhat+=vn1; if(rhat>=b) break; }
  uint32_t un21=un32*b+un1-q1*v;
  uint32_t q0=un21/vn1; rhat=un21-q0*vn1;
  while(q0>=b || q0*vn0 > b*rhat+un0){ q0--; rhat+=vn1; if(rhat>=b) break; }
  if(r) *r=(un21*b+un0-q0*v)>>s;
  return q1*b+q0;
}
__attribute__((noinline)) uint64_t fudiv(uint64_t n,uint64_t d){
  uint32_t nh=n>>32, nl=(uint32_t)n, dh=d>>32, dl=(uint32_t)d;
  if(dh==0){
    if(nh==0) return nl/dl;
    uint32_t qh=nh/dl, rh=nh-qh*dl, r; uint32_t ql=divlu(rh,nl,dl,&r);
    return ((uint64_t)qh<<32)|ql; }
  extern uint64_t __udivdi3(uint64_t,uint64_t); return __udivdi3(n,d);
}
static uint64_t rs=0x2545F4914F6CDD1Dull; static uint64_t rnd(void){rs^=rs<<13;rs^=rs>>7;rs^=rs<<17;return rs;}
typedef uint64_t(*F)(uint64_t,uint64_t); extern uint64_t __udivdi3(uint64_t,uint64_t);
volatile F LG=__udivdi3, FP=fudiv; volatile uint64_t sink; volatile uint64_t src=4097740000000ULL, dv=1000;
int main(int c,char**v){ int m=atoi(v[1]); long n=atol(v[2]);
  if(m==0){ long bad=0; for(long i=0;i<n;i++){ uint64_t a=rnd(), d=rnd(); int k=i%5;
      if(k==1) d>>=32+(rnd()%32); else if(k==2){a>>=32;d>>=32+(rnd()%32);} else if(k==3){ d=1000+(rnd()%3)*999000; a>>=rnd()%40;} else if(k==4) d>>=rnd()%64;
      if(!d) d=1; if(fudiv(a,d)!=a/d) bad++; }
    printf("cases=%ld bad=%ld\n",n,bad); return 0; }
  F f = (m==1||m==3)?LG:FP;
  if(m==5){ for(long i=0;i<n;i++) sink=src+i; return 0;}
  if(m==6){ for(long i=0;i<n;i++) sink=(uint32_t)(src+i); return 0;}
  for(long i=0;i<n;i++){ uint64_t a = m<=2 ? src+i : (uint32_t)(src+i); sink=f(a,dv);} return 0; }
