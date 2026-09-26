#include <stdio.h>
#include <stdint.h>
#include <math.h>
#define VM_RAMP 256
static void vm_ramp_table(uint16_t *t, int n, int shift, const uint16_t *ramp){int i,o;for(i=0;i<n;i++){int idx=(i*(VM_RAMP-1)+(n-1)/2)/(n-1);o=((int)ramp[idx]*(n-1)+32767)/65535;t[i]=(uint16_t)(o<<shift);}}
static void vm_gamma_table(uint16_t *t,int n,int shift,uint32_t v){float g=v/10000.0f,e;int i,o;e=1.0f/g;for(i=0;i<n;i++){o=(int)((float)(n-1)*powf((float)i/(n-1),e)+0.5f);if(o>n-1)o=n-1;t[i]=(uint16_t)(o<<shift);}}
int main(){uint16_t r[256],t[64],u[64];int i,bad=0;
for(i=0;i<256;i++)r[i]=i*257; vm_ramp_table(t,32,0,r); for(i=0;i<32;i++) if(t[i]!=i) bad++; vm_ramp_table(t,64,0,r); for(i=0;i<64;i++) if(t[i]!=i) bad++; printf("identity(i*257) mismatches %d\n",bad);
bad=0; for(i=0;i<256;i++)r[i]=i<<8; vm_ramp_table(t,32,0,r); for(i=0;i<32;i++) if(t[i]!=i) bad++; vm_ramp_table(t,64,0,r); for(i=0;i<64;i++) if(t[i]!=i) bad++; printf("identity(i<<8, TyrQuake gamma 1) mismatches %d\n",bad);
/* TyrQuake gamma 0.7: gammatable[i] = 255*pow((i+0.5)/255.5, 0.7)+0.5 */
for(i=0;i<256;i++){int v=(int)(255*pow((i+0.5)/255.5,0.7)+0.5); if(v>255)v=255; r[i]=v<<8;} vm_ramp_table(t,32,0,r); vm_gamma_table(u,32,0,14286); int md=0; for(i=0;i<32;i++){int d=t[i]-u[i]; if(d<0)d=-d; if(d>md)md=d;} printf("ramp(gamma .7) vs SetGamma(1.43) red max diff %d levels; e.g. in 8 -> %d vs %d\n",md,t[8],u[8]);}
