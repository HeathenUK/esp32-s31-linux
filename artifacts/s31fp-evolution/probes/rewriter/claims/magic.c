/* claim 10: 64-bit unsigned divide by constant K via 64x64->hi128 magic multiply, rv32 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
/* magic for K: m = ceil(2^(64+sh)/K), 65-bit case handled by add-shift trick (Granlund-Montgomery) */
static uint64_t M; static int SH; static int ADD;
__attribute__((noinline)) uint64_t umulh64(uint64_t a, uint64_t b){
  uint32_t a0=a,a1=a>>32,b0=b,b1=b>>32;
  uint64_t p00=(uint64_t)a0*b0, p01=(uint64_t)a0*b1, p10=(uint64_t)a1*b0, p11=(uint64_t)a1*b1;
  uint64_t mid=(p00>>32)+(uint32_t)p01+(uint32_t)p10;
  return p11+(p01>>32)+(p10>>32)+(mid>>32);
}
__attribute__((noinline)) uint64_t magicdiv(uint64_t n){
  uint64_t q=umulh64(n,M);
  if(ADD){ uint64_t t=((n-q)>>1)+q; return t>>(SH-1);} return q>>SH;
}
__attribute__((noinline)) uint64_t ref(uint64_t n, uint64_t k){ return n/k; }  /* libgcc __udivdi3 */
static void setup(uint64_t K){ /* Hacker's Delight unsigned magic, 64-bit, computed with 128-free long division */
  int l=0; while(l<64 && ((uint64_t)1<<l)<K) l++;       /* l = ceil(log2 K) */
  /* m' = floor(2^64*(2^l - K)/K) + 1 ; computed via bit-by-bit division of (2^l-K)<<64 by K */
  uint64_t num_hi = (l==64)?0:(((uint64_t)1<<l)-K), q=0, r=num_hi% K; (void)num_hi;
  /* long division of (num_hi:0) by K */
  uint64_t rem = num_hi; q=0;
  for(int i=63;i>=0;i--){ int top=rem>>63; rem<<=1; if(top||rem>=K){rem-=K; q|=(uint64_t)1<<i;} }
  (void)r; M=q+1; ADD=1; SH=l; if(l==0){SH=0;}
}
static uint64_t s=88172645463325252ull; static uint64_t r64(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
int main(int argc,char**argv){
  long n=atol(argv[1]); int mode=atoi(argv[2]); /* mode 0: verify, 1: time ref only, 2: magic only */
  static const uint64_t Ks[]={1000,1000000,10,3,7,60,1000000000ull,0xffffffffull,641,2147483647ull,86400,44100};
  unsigned long bad=0, sink=0;
  for(unsigned ki=0;ki<sizeof Ks/sizeof*Ks;ki++){
    uint64_t K=Ks[ki]; if(K==1) continue; setup(K);
    for(long i=0;i<n;i++){
      uint64_t x=r64(); if(i&1) x>>= (r64()%64); if(i%7==0) x = (x/K)*K - (r64()&1);
      if(mode==1){ sink+=ref(x,K); continue;} if(mode==2){ sink+=magicdiv(x); continue;}
      if(magicdiv(x)!=ref(x,K)){ if(bad<5) printf("BAD K=%llu x=%llu\n",(unsigned long long)K,(unsigned long long)x); bad++; }
    }
  }
  printf("mode=%d n/K=%ld mismatches=%lu sink=%lu\n",mode,n,bad,sink);
}
