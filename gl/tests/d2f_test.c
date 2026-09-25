/* s31_d2f_bits() against the compiler's (float)d (libgcc __truncdfsf2 on
   RV32): N (default 50M) random doubles plus ties and edge cases.
   Host: gcc -O2 -Igl/tinygl/source gl/tests/d2f_test.c; RV32: d2f_test.qemu */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "s31_float.h"
static uint64_t x=88172645463325252ull;
static uint64_t rnd(void){x^=x<<13;x^=x>>7;x^=x<<17;return x;}
int main(int argc,char**argv){
  long N=argc>1?atol(argv[1]):50000000;
  long bad=0,n=0;
  double specials[]={0.0,-0.0,1.0,-1.0,1e-40,1e40,3.4028235677973366e38,3.4028234663852886e38,1.1754943508222875e-38,1.1754942e-38,0.1,1.0/3};
  for(unsigned i=0;i<sizeof specials/sizeof *specials;i++){float a=(float)specials[i],b=s31_d2f_bits(specials[i]);if(memcmp(&a,&b,4)){bad++;printf("special %g\n",specials[i]);}n++;}
  for(long i=0;i<N;i++){
    uint64_t b=rnd(); double d;
    if(i&1){ /* bias exponents into float range */ b=(b&0x800fffffffffffffull)|((uint64_t)(880+(rnd()%290))<<52);}
    if((i%7)==0){ b&=~0x1fffffffull; b|= (rnd()&1)?0x10000000ull:0; } /* ties */
    memcpy(&d,&b,8);
    if(d!=d) continue;
    float f1=(float)d,f2=s31_d2f_bits(d);
    if(memcmp(&f1,&f2,4)){ if(bad<10) printf("mismatch %a %a %a\n",d,f1,f2); bad++;}
    n++;
  }
  printf("d2f: %ld tested, %ld mismatches\n",n,bad); return bad!=0;
}
