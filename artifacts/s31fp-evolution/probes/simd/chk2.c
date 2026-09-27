#include <stdio.h>
#include <string.h>
int main(){ static unsigned char b[8192], r[8192]; for (int g=1; g<=1024; g*=2) { int bad=0; for (int j=0;j<1024;j+=37){
 for(int i=0;i<8192;i++) b[i]=r[i]=i*13; unsigned char *s=b+4096,*d=s-g; memmove(r+4096-g,r+4096,2000);
 for(int i=0;i<j;i++) d[i]=s[i]; for(int i=0;i<2000;i++) d[i]=s[i]; if(memcmp(b,r,8192)) bad++; } printf("g=%d bad=%d/28\n",g,bad);} }
