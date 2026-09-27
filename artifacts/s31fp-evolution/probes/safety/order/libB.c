#include <unistd.h>
__attribute__((constructor)) static void c(void){ write(2,"CTOR libB\n",10); }
int fB(void){return 1;}
