#include <unistd.h>
__attribute__((constructor)) static void c(void){ write(2,"CTOR libD\n",10); }
int fD(void){return 1;}
