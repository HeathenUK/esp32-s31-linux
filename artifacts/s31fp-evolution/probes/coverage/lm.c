#define _GNU_SOURCE
#include <stdlib.h>
#include <math.h>
double X[64]; float F[64]; volatile double ds; volatile float f1,f2;
int main(int c,char**v){ int n=atoi(v[1]); int m=atoi(v[2]);
 for(int i=0;i<64;i++){X[i]=(i-32)*0.173+0.01; F[i]=(float)X[i];}
 for(int i=0;i<n;i++){ double x=X[i&63]; float f=F[i&63]; float a,b; switch(m){
 case 0: ds=x; break; case 1: ds=floor(x); break; case 2: ds=sqrt(x<0?-x:x); break;
 case 3: ds=sin(x); break; case 4: ds=atan2(x,1.7); break; case 5: ds=pow(x<0?-x:x,1.1); break;
 case 6: sincosf(f,&a,&b); f1=a; f2=b; break; case 7: f1=sinf(f); break; case 8: f1=f; break;
 case 9: {double s,co; sincos(x,&s,&co); ds=s+0;} break; case 10: ds=ceil(x); break;
 case 11: f1=sqrtf(f<0?-f:f); break; case 12: f1=floorf(f); break; case 13: ds=exp(x); break;
 case 14: f1=atan2f(f,1.7f); break; case 15: f1=powf(f<0?-f:f,1.1f); break;}}
 return 0;}
