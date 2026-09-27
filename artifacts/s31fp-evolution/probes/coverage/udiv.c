#define _GNU_SOURCE
#include <stdlib.h>
#include <math.h>
volatile unsigned long long src=4097740000000ULL, sink; volatile unsigned long long dv;
volatile double dsrc=12.37; volatile double dsink; volatile float fsrc=1.3f; volatile float fs1,fs2;
int main(int c,char**v){ int n=atoi(v[1]); int mode=atoi(v[2]); dv = mode==1?1000:1000000;
 for(int i=0;i<n;i++){ switch(mode){
  case 0: sink=src+i; break;
  case 1: case 2: sink=(src+i)/dv; break;
  case 3: dsink=floor(dsrc+i); break;
  case 4: dsink=sin(dsrc+i); break;
  case 5: sincosf(fsrc+i,(float*)&fs1,(float*)&fs2); break;
  case 6: dsink=sqrt(dsrc+i); break;
  case 7: dsink=dsrc+i; break;
  case 8: fs1=sinf(fsrc+i); break;
  case 9: dsink=atan2(dsrc+i,3.0); break;
  case 10: dsink=pow(dsrc+i,1.1); break;
  case 11: fs1=fsrc+i; break;
 }} return 0;}
