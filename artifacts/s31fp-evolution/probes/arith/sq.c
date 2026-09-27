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
static uint64_t hwsqrt(uint64_t in){ uint64_t out;
  __asm__ volatile(".option push\n.option arch,+d\nfld ft0,0(%1)\nfsqrt.d ft0,ft0\nfsd ft0,0(%0)\n.option pop"::"r"(&out),"r"(&in):"ft0","memory"); return out; }
static uint64_t rs=0x9e3779b97f4a7c15ull; static uint64_t rnd(void){rs^=rs<<13;rs^=rs>>7;rs^=rs<<17;return rs;}
volatile fn Fs=sqrt; volatile double sink;
int main(int c,char**v){ int mode=atoi(v[1]); long n=atol(v[2]);
  if(mode>=10){ double X[64]; for(int i=0;i<64;i++){double t=(i-32)*0.173+0.01; X[i]=t<0?-t:t;}
    for(long i=0;i<n;i++){ double x=X[i&63]; sink = mode==11? Fs(x): x; } return 0; }
  long cnt=0,bad=0,badv=0,badf=0,nxcases=0,printed=0;
  for(long it=0;it<n;it++){
    uint64_t u; unsigned k=it%6; uint64_t sg=(k==5)?((uint64_t)(rnd()&1)<<63):0;
    unsigned e=rnd()%0x800; uint64_t m=rnd()&0xfffffffffffffull;
    if(k==1){ /* perfect squares and neighbours: s^2 for integer s */ uint64_t s=(rnd()>>38)|1; double d=(double)s*(double)s; u=bits(d)+ (int)(rnd()%3)-1; }
    else if(k==2){ m=0; u=sg|((uint64_t)e<<52); }
    else if(k==3){ e=0; u=m; } /* subnormal */
    else u=sg|((uint64_t)e<<52)|m;
    if(it<12){static const uint64_t sp[12]={0,1ull<<63,0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8000000000000ull,0x7ff0000000000001ull,0xbff0000000000000ull,1,0x8000000000000001ull,0x3ff0000000000000ull,0x4010000000000000ull,0x7fefffffffffffffull}; u=sp[it];}
    for(unsigned r=0;r<5;r++){
      setfrm(r); clrfl(); double a=Fs(mk(u)); unsigned fa=getfl();
      setfrm(r); clrfl(); uint64_t b=hwsqrt(u); unsigned fb=getfl(); setfrm(0);
      cnt++; if(fa&1) nxcases++;
      int nan_a = (bits(a)&0x7fffffffffffffffull)>0x7ff0000000000000ull, nan_b=(b&0x7fffffffffffffffull)>0x7ff0000000000000ull;
      int vb = !(nan_a&&nan_b) && bits(a)!=b; int fbad = fa!=fb;
      if(vb) badv++; if(fbad) badf++;
      if((vb||fbad)){ bad++; if(printed++<12) printf("MISMATCH x=%016llx frm=%u musl=%016llx/%x hw=%016llx/%x\n",(unsigned long long)u,r,(unsigned long long)bits(a),fa,(unsigned long long)b,fb);}
    }
  }
  printf("cases=%ld mismatches=%ld (value %ld, flags %ld) NX_cases=%ld\n",cnt,bad,badv,badf,nxcases); return 0; }
