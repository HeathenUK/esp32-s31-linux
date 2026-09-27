/* directed: results straddling 2^-126 (tininess before vs after rounding) and overflow edge */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
static uint64_t s=12345;
static inline uint32_t rnd(void){ s^=s<<13; s^=s>>7; s^=s<<17; return (uint32_t)(s>>16); }
static inline float f(uint32_t u){ float x; memcpy(&x,&u,4); return x; }
static inline uint32_t u(float x){ uint32_t r; memcpy(&r,&x,4); return r; }
__attribute__((noinline)) static float chain(int op, float a, float b){
    volatile double da=a, db=b; double r = op==2? da*db : da/db; volatile float fr=(float)r; return fr; }
static inline float hw(int op,float a,float b){ float r; if(op==2) __asm__ volatile("fmul.s %0,%1,%2":"=f"(r):"f"(a),"f"(b)); else __asm__ volatile("fdiv.s %0,%1,%2":"=f"(r):"f"(a),"f"(b)); return r;}
int main(int argc,char**argv){
  long n=argc>1?atol(argv[1]):100000; long bad=0, ufnew[2]={0,0}, tinyafter=0, cases=0;
  for(int op=2;op<4;op++) for(int m=0;m<4;m++) for(long i=0;i<n;i++){
    uint32_t A=(rnd()&0x807fffff)|((100+rnd()%60)<<23), B;
    int edge = rnd()%2; /* 0: 2^-126 boundary, 1: FLT_MAX boundary */
    float target = edge? f(0x7f7fffff) : f(0x00800000);
    float bb = op==2 ? target/f(A&0x7fffffff) : f(A&0x7fffffff)/target;  /* in current (RNE) mode */
    B=u(bb)+ (int)(rnd()%7)-3; B|= (rnd()&0x80000000);
    if(((B>>23)&0xff)==0xff) continue;
    unsigned fl1,fl2; float r1,r2;
    __asm__ volatile("fsrm %0"::"r"(m)); __asm__ volatile("fsflags x0");
    r1=chain(op,f(A),f(B)); __asm__ volatile("frflags %0":"=r"(fl1));
    __asm__ volatile("fsrm %0"::"r"(m)); __asm__ volatile("fsflags x0");
    r2=hw(op,f(A),f(B)); __asm__ volatile("frflags %0":"=r"(fl2));
    __asm__ volatile("fsrm x0");
    cases++;
    if(fl2&2) ufnew[edge]++;
    if(!edge && (u(r2)&0x7fffffff)==0x00800000 && (fl2&1)) tinyafter++;   /* rounded up to MIN_NORMAL, inexact */
    if(u(r1)!=u(r2)||fl1!=fl2){ if(bad<5) printf("MISMATCH op=%d rm=%d a=%08x b=%08x chain=%08x/%02x hw=%08x/%02x\n",op,m,A,B,u(r1),fl1,u(r2),fl2); bad++; }
  }
  printf("cases=%ld mismatches=%ld UF-raised(near 2^-126)=%ld OF-edge UF=%ld results==+-MIN_NORMAL inexact=%ld\n",cases,bad,ufnew[0],ufnew[1],tinyafter);
  return 0;
}
