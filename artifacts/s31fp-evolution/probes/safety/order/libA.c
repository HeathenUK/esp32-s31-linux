#include <unistd.h>
__attribute__((constructor)) static void c(void){ write(2,"CTOR libA\n",10); }
int fA(void){return 1;}
