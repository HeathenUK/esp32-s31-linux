#include <stdio.h>
#include <dlfcn.h>
double dep_mul(double,double);
int main(int c,char**v){ printf("main %g\n", dep_mul(c,2.5)); void*h=dlopen("./libdep2.so",RTLD_NOW); printf("dlopen %p\n",h); return 0;}
