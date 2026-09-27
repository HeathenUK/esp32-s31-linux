#include <unistd.h>
__attribute__((constructor)) static void c(void){ write(2,"CTOR libE\n",10); }
int fE(void){return 1;}
