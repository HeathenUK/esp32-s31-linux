#include <stdio.h>
int dep_inited;
__attribute__((constructor)) static void c(void){ dep_inited=1; fprintf(stderr,"dep ctor\n"); }
double dep_mul(double a,double b){return a*b;}
