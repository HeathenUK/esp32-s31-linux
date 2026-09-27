#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
typedef double (*fn)(double);
static inline uint64_t bits(double d){uint64_t u;memcpy(&u,&d,8);return u;}
static inline double mk(uint64_t u){double d;memcpy(&d,&u,8);return d;}
static inline void setfrm(unsigned r){__asm__ volatile("fsrm %0"::"r"(r));}
static inline void clrfl(void){__asm__ volatile("csrw fflags, zero");}
static inline unsigned getfl(void){unsigned f;__asm__ volatile("frflags %0":"=r"(f));return f;}
__attribute__((noinline)) double ifloor(double x){
  union{double f;struct{uint32_t lo,hi;}w;}u={x};
  uint32_t hi=u.w.hi, lo=u.w.lo; int e=(hi>>20)&0x7ff;
  if(e>=0x3ff+52) return x;
  if(e<0x3ff){ if(((hi<<1)|lo)==0) return x; __asm__ volatile("csrsi fflags,0");
    u.w.hi = (int32_t)hi<0 ? 0xbff00000u : 0; u.w.lo=0; return u.f; }
  int s=e-0x3ff; /* 0..51 fractional bits = 52-s */
  uint32_t mlo, mhi;
  if(s<20){ mhi=0x000fffffu>>s; mlo=0xffffffffu; } else { mhi=0; mlo=0xffffffffu>>(s-20); }
  if(((hi&mhi)|(lo&mlo))==0) return x;
  __asm__ volatile("csrsi fflags,1");
  if((int32_t)hi<0){ uint32_t n=lo+mlo; hi+=mhi+(n<lo); lo=n; }
  u.w.hi=hi&~mhi; u.w.lo=lo&~mlo; return u.f;
}
__attribute__((noinline)) double iceil(double x){
  union{double f;struct{uint32_t lo,hi;}w;}u={x};
  uint32_t hi=u.w.hi, lo=u.w.lo; int e=(hi>>20)&0x7ff;
  if(e>=0x3ff+52) return x;
  if(e<0x3ff){ if(((hi<<1)|lo)==0) return x; __asm__ volatile("csrsi fflags,1");
    u.w.hi = (int32_t)hi<0 ? 0x80000000u : 0x3ff00000u; u.w.lo=0; return u.f; }
  int s=e-0x3ff; uint32_t mlo, mhi;
  if(s<20){ mhi=0x000fffffu>>s; mlo=0xffffffffu; } else { mhi=0; mlo=0xffffffffu>>(s-20); }
  if(((hi&mhi)|(lo&mlo))==0) return x;
  __asm__ volatile("csrsi fflags,1");
  if((int32_t)hi>=0){ uint32_t n=lo+mlo; hi+=mhi+(n<lo); lo=n; }
  u.w.hi=hi&~mhi; u.w.lo=lo&~mlo; return u.f;
}
static uint64_t rs=88172645463325252ull; static uint64_t rnd(void){rs^=rs<<13;rs^=rs>>7;rs^=rs<<17;return rs;}
volatile fn Ff=floor, Fc=ceil, If=ifloor, Ic=iceil;
volatile double sink;
int main(int c,char**v){
  int mode=atoi(v[1]); long n=c>2?atol(v[2]):0;
  if(mode>=10){ /* icount loops, lm.c inputs */
    double X[64]; for(int i=0;i<64;i++)X[i]=(i-32)*0.173+0.01;
    fn f = mode==10?0: mode==11?floor: mode==12?ceil: mode==13?ifloor: iceil;
    for(long i=0;i<n;i++){ double x=X[i&63]; sink = f? f(x): x; } return 0; }
  long cnt=0, bad[4]={0}, musl_wrong=0, snan_nv=0, printed=0;
  for(long it=0; it<n; it++){
    uint64_t u;
    unsigned k=it%8;
    uint64_t sg=(uint64_t)(rnd()&1)<<63; unsigned e=rnd()%0x800;
    uint64_t m=rnd()&0xfffffffffffffull;
    if(k==1) m=0; else if(k==2) m=1; else if(k==3) m=0xfffffffffffffull; else if(k==4) m&=~((1ull<<(rnd()%53))-1);
    else if(k==5){ e=0x3ff + (rnd()%54); } else if(k==6){ e=0x3fe + (rnd()%3); m &= 0xf0000000000ffull; }
    u=sg|((uint64_t)e<<52)|m;
    if(it<16){ static const uint64_t sp[16]={0,1ull<<63,0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8000000000000ull,0x7ff0000000000001ull,0xfff4000000000123ull,1,0x8000000000000001ull,0x4330000000000000ull,0x432fffffffffffffull,0xc32fffffffffffffull,0x3fe0000000000000ull,0xbfe0000000000000ull,0x3ff0000000000000ull,0xbff0000000000001ull}; u=sp[it]; }
    double x=mk(u);
    for(unsigned r=0;r<5;r++){
      for(int w=0;w<2;w++){
        fn ref = w? Fc: Ff; fn tst = w? Ic: If;
        setfrm(r); clrfl(); double a=ref(x); unsigned fa=getfl();
        setfrm(r); clrfl(); double b=tst(x); unsigned fb=getfl();
        setfrm(0);
        cnt++;
        int isnan_=((u>>52)&0x7ff)==0x7ff && (u&0xfffffffffffffull);
        if(isnan_ && !(u&0x8000000000000ull) && (fa&16)) snan_nv++;
        if(bits(a)!=bits(b)||fa!=fb){ bad[w]++; if(printed++<12) printf("MISMATCH %s x=%016llx frm=%u musl=%016llx/%x int=%016llx/%x\n", w?"ceil":"floor",(unsigned long long)u,r,(unsigned long long)bits(a),fa,(unsigned long long)bits(b),fb); }
        /* musl vs RNE-mode musl result: floor/ceil are mode-independent */
        if(r){ setfrm(0); double a0=ref(x); if(bits(a0)!=bits(a)) musl_wrong++; }
      }
    }
  }
  printf("cases(x*frm*fn)=%ld floor_mismatch=%ld ceil_mismatch=%ld musl_mode_dependent=%ld snan_raised_NV=%ld\n",cnt,bad[0],bad[1],musl_wrong,snan_nv);
  return 0;
}
