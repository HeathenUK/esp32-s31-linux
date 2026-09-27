#include <unistd.h>
__attribute__((constructor)) static void c(void){ write(2,"CTOR libF\n",10); }
int fF(void){return 1;}
