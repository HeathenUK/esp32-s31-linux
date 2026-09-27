/* Candidate integer replacements: floor/ceil/trunc/sqrt, musl-exact incl. NX. */
#include <stdint.h>
#include <string.h>
extern const uint16_t __rsqrt_tab[128];
static inline uint64_t bits(double x){uint64_t u; memcpy(&u,&x,8); return u;}
static inline double dbl(uint64_t u){double x; memcpy(&x,&u,8); return x;}
static inline void nx(void){ __asm__ volatile("csrsi fflags, 1"); }
static inline double rnd_int(double x, int up_if_neg /* 1=floor, 0=ceil */){
  uint64_t u=bits(x); uint32_t hi=u>>32; int e=(hi>>20)&0x7ff;
  if(e>=1075) return x;
  if(e<1023){ if((u<<1)==0) return x; nx();
    if(up_if_neg) return (hi>>31)? -1.0 : 0.0; else return (hi>>31)? -0.0 : 1.0; }
  uint64_t m=((uint64_t)1<<(1075-e))-1;
  if(!(u&m)) return x; nx();
  int neg=hi>>31;
  if(neg==up_if_neg) u+=m+1;   /* grow magnitude (carry into exponent is correct) */
  return dbl(u&~m);
}
double s31_floor(double x){ return rnd_int(x,1); }
double s31_ceil (double x){ return rnd_int(x,0); }
double s31_trunc(double x){
  uint64_t u=bits(x); int e=(int)(u>>52&0x7ff)-0x3ff+12;
  if(e>=64) return x; if(e<12) e=1;
  uint64_t m=-1ULL>>e; if(!(u&m)) return x; nx(); return dbl(u&~m);
}
static inline uint32_t mul32(uint32_t a, uint32_t b){ return (uint64_t)a*b>>32; }
static inline uint64_t mul64(uint64_t a, uint64_t b){
  uint64_t ahi=a>>32, alo=a&0xffffffff, bhi=b>>32, blo=b&0xffffffff;
  return ahi*bhi + (ahi*blo>>32) + (alo*bhi>>32); }
__attribute__((noinline)) static double sqrt_cold(double x, uint64_t ix){
  return (x-x)/(x-x); }  /* same expression as musl __math_invalid */
double s31_sqrt(double x){
  uint64_t ix=bits(x), top=ix>>52, m;
  if(__builtin_expect(top-1 >= 0x7ff-1,0)){
    if(ix*2==0) return x; if(ix==0x7ff0000000000000ULL) return x;
    if(ix>0x7ff0000000000000ULL) return sqrt_cold(x,ix);
    int p=63-__builtin_clzll(ix);        /* exact normalize, == musl x*0x1p52 */
    ix=((ix<<(52-p))&0x000fffffffffffffULL)|((uint64_t)(p+1)<<52);
    top=(uint64_t)(p+1)-52;
  }
  int even=top&1; m=(ix<<11)|0x8000000000000000ULL; if(even) m>>=1; top=(top+0x3ff)>>1;
  const uint64_t three=0xc0000000; uint64_t r,s,d,u,i;
  i=(ix>>46)%128; r=(uint32_t)__rsqrt_tab[i]<<16;
  s=mul32(m>>32,r); d=mul32(s,r); u=three-d; r=mul32(r,u)<<1; s=mul32(s,u)<<1;
  d=mul32(s,r); u=three-d; r=mul32(r,u)<<1; r=r<<32;
  s=mul64(m,r); d=mul64(s,r); u=(three<<32)-d; s=mul64(s,u); s=(s-2)>>9;
  uint64_t d0,d1,d2; d0=(m<<42)-s*s; d1=s-d0; d2=d1+s+1; s+=d1>>63;
  s&=0x000fffffffffffffULL; s|=top<<52;
  if(d2==0) return dbl(s);                    /* exact: no flags */
  nx();
  unsigned rm; __asm__ volatile("frrm %0":"=r"(rm));
  int below = !((d1^d2)>>63);  /* tiny positive <=> true value above y */
  if(rm==1||rm==2||rm==4){ if(!below) s-=1; } /* RTZ/RDN; libgcc soft-fp has no RMM case and truncates */
  else if(rm==3){ if(below) s+=1; }           /* RUP */
  return dbl(s);
}
