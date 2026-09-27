/* claim 7: (float)((double)a OP (double)b) via libgcc soft-fp vs single fOP.s, bits + fflags */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static uint64_t s=0x9e3779b97f4a7c15ull;
static inline uint32_t rnd(void){ s^=s<<13; s^=s>>7; s^=s<<17; return (uint32_t)(s>>16) ^ (uint32_t)s; }
static inline float f(uint32_t u){ float x; memcpy(&x,&u,4); return x; }
static inline uint32_t u(float x){ uint32_t r; memcpy(&r,&x,4); return r; }
__attribute__((noinline)) static float chain(int op, float a, float b){
    volatile double da=a, db=b; double r;
    switch(op){case 0: r=da+db; break; case 1: r=da-db; break; case 2: r=da*db; break; default: r=da/db;}
    volatile float fr=(float)r; return fr;
}
static inline float hw(int op, float a, float b){
    float r;
    switch(op){case 0: __asm__ volatile("fadd.s %0,%1,%2":"=f"(r):"f"(a),"f"(b)); break;
    case 1: __asm__ volatile("fsub.s %0,%1,%2":"=f"(r):"f"(a),"f"(b)); break;
    case 2: __asm__ volatile("fmul.s %0,%1,%2":"=f"(r):"f"(a),"f"(b)); break;
    default: __asm__ volatile("fdiv.s %0,%1,%2":"=f"(r):"f"(a),"f"(b));}
    return r;
}
static uint32_t gen(int cls, uint32_t other, int op){
    uint32_t x=rnd();
    switch(cls){
    case 0: return x;                                   /* any bits */
    case 1: return (x&0x807fffff)|((other&0x7f800000)+((rnd()%9-4)<<23)); /* close exponent to other */
    case 2: return (x&0x807fffff)|((rnd()%8)<<23);      /* tiny / subnormal range */
    case 3: return (x&0x807fffff)|((0x7f-(rnd()%64)+ (rnd()&1?64:-64)+ (rnd()%3))<<23); /* products/quotients near 2^-126 or overflow */
    case 4: { static const uint32_t sp[]={0,0x80000000,0x7f800000,0xff800000,0x7fc00000,0x7fa00001,0xffa00000,0x7f7fffff,0x00800000,0x00000001,0x807fffff,0x3f800000,0x3f7fffff,0x3f800001};
              return (rnd()&3)? sp[rnd()%14] : x; }
    default: /* tiny result targeting: exponent sum near -126..-150 for mul, diff for div */
        { int ea=(other>>23)&0xff; int want; if(op==2) want=127-126-(int)(rnd()%26)+127-ea; else if(op==3) want=ea+126+(int)(rnd()%26)-127; else want=ea;
          if(want<0)want=0; if(want>254)want=254; if(op==3){} return (x&0x807fffff)|((uint32_t)want<<23);}
    }
}
int main(int argc,char**argv){
    long n=argc>1?atol(argv[1]):100000; if(argc>2) s^=strtoull(argv[2],0,0);
    static const char *on[]={"add","sub","mul","div"}; static const char *rm[]={"RNE","RTZ","RDN","RUP","RMM"};
    long bad_total=0;
    for(int op=0;op<4;op++) for(int m=0;m<5;m++){
        long bad=0, badflag=0, badval=0, uf=0, of=0, nx=0, nv=0;
        for(long i=0;i<n;i++){
            uint32_t A=gen(i%6, 0, op); uint32_t B=gen((i/6)%6, A, op);
            if(rnd()&1){uint32_t t=A;A=B;B=t;}
            uint32_t pre=rnd()&0x1f;               /* random preset flags */
            unsigned fl1,fl2; float r1,r2;
            __asm__ volatile("fsrm %0"::"r"(m)); __asm__ volatile("fsflags %0"::"r"(pre));
            r1=chain(op,f(A),f(B)); __asm__ volatile("frflags %0":"=r"(fl1));
            __asm__ volatile("fsrm %0"::"r"(m)); __asm__ volatile("fsflags %0"::"r"(pre));
            r2=hw(op,f(A),f(B)); __asm__ volatile("frflags %0":"=r"(fl2));
            if(fl2&2)uf++; if(fl2&4)of++; if(fl2&1)nx++; if(fl2&16)nv++;
            if(u(r1)!=u(r2)||fl1!=fl2){ bad++; if(u(r1)!=u(r2))badval++; if(fl1!=fl2)badflag++;
                if(bad<=3) printf("  MISMATCH %s %s a=%08x b=%08x pre=%02x chain=%08x/%02x hw=%08x/%02x\n",on[op],rm[m],A,B,pre,u(r1),fl1,u(r2),fl2);}
        }
        printf("%s %s n=%ld mismatches=%ld (value %ld, flags %ld)  hw-flag counts UF=%ld OF=%ld NX=%ld NV=%ld\n",on[op],rm[m],n,bad,badval,badflag,uf,of,nx,nv);
        if(m<4) bad_total+=bad;
    }
    printf("total mismatches excluding RMM: %ld\n",bad_total);
    return 0;
}
