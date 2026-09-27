#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
double s31_floor(double),s31_ceil(double),s31_trunc(double),s31_sqrt(double);
typedef double (*d1)(double); volatile double sink; double X[64];
int main(int c,char**v){ FILE*f=fopen(v[1],"rb"); unsigned char*M=mmap(0,0xab000,7,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); fread(M,1,0xa8dac+0x520,f); fclose(f);
 int mode=atoi(v[2]); long n=atol(v[3]); for(int i=0;i<64;i++){X[i]=(i-32)*0.173+0.01;}
 d1 fn; unsigned off[]={0,0x2f9be,0x2be6c,0x3e50a,0x3b9f4}; d1 rp[]={0,s31_floor,s31_ceil,s31_trunc,s31_sqrt};
 int k=mode%10; int useimg=mode<10; volatile d1 vf = k==0?0: (useimg?(d1)(M+off[k]):rp[k]); fn=vf;
 for(long i=0;i<n;i++){ double x=X[i&63]; if(k==4) x=x<0?-x:x; sink = k? fn(x) : x; }
 return 0;}
