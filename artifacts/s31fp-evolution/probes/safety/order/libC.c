#include <unistd.h>
__attribute__((constructor)) static void c(void){ write(2,"CTOR libC\n",10); }
int fC(void){return 1;}
