#include <stdint.h>
/* Fast path: divisor < 2^32. Schoolbook 64/32 via two 32-bit divu.
   Falls back to a shift-subtract only when divisor >= 2^32. */
uint64_t s31_udivdi3(uint64_t n, uint64_t d){
  if(d>>32){ /* rare: full 64-bit divisor */
    if(n<d) return 0;
    int sh=__builtin_clzll(d)-__builtin_clzll(n);
    uint64_t q=0; d<<=sh;
    for(;sh>=0;sh--){ if(n>=d){n-=d; q|=1ULL<<sh;} d>>=1; }
    return q;
  }
  uint32_t dv=(uint32_t)d, nhi=n>>32, nlo=(uint32_t)n;
  if(!nhi) return nlo/dv;                 /* both halves fit */
  uint32_t qhi=nhi/dv, rhi=nhi%dv;
  /* now divide (rhi:nlo) / dv, rhi < dv < 2^32 -> fits in 64 */
  uint64_t rem=((uint64_t)rhi<<32)|nlo;
  uint32_t qlo=(uint32_t)(rem/dv);
  return ((uint64_t)qhi<<32)|qlo;
}
uint64_t s31_umoddi3(uint64_t n, uint64_t d){ return n - s31_udivdi3(n,d)*d; }
int64_t s31_divdi3(int64_t a,int64_t b){ int s=(a<0)^(b<0); uint64_t ua=a<0?-(uint64_t)a:a, ub=b<0?-(uint64_t)b:b; uint64_t q=s31_udivdi3(ua,ub); return s?-(int64_t)q:(int64_t)q; }
int64_t s31_moddi3(int64_t a,int64_t b){ uint64_t ua=a<0?-(uint64_t)a:a, ub=b<0?-(uint64_t)b:b; uint64_t r=s31_umoddi3(ua,ub); return a<0?-(int64_t)r:(int64_t)r; }
