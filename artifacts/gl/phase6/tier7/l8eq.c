#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void oldf(const unsigned *q,int w,int k0,unsigned char*d,unsigned char*al,unsigned*g,unsigned*a,unsigned*ab){
 unsigned gacc=0,aand=~0u,abad=0;int x;
 for (x = 0; x < w; x++) { unsigned int v = q[x]; int k = k0 + x;
  gacc |= v ^ (v >> 8); aand &= v; abad |= (v + 0x01000000u) & 0xfe000000u; d[x] = (unsigned char)v;
  if (v >> 31) al[k >> 3] |= (unsigned char)(1u << (k & 7)); else al[k >> 3] &= (unsigned char)~(1u << (k & 7)); }
 *g=gacc;*a=aand;*ab=abad;}
static void newf(const unsigned *q,int w,int k0,unsigned char*d,unsigned char*al,unsigned*g,unsigned*a,unsigned*ab){
 unsigned gacc=0,aand=~0u,abad=0;int x;
        x = 0;
        for (; x < w && ((k0 + x) & 7); x++) {
          unsigned int v = q[x]; int k = k0 + x;
          gacc |= v ^ (v >> 8); aand &= v; abad |= v + 0x01000000u; d[x] = (unsigned char)v;
          if (v >> 31) al[k >> 3] |= (unsigned char)(1u << (k & 7)); else al[k >> 3] &= (unsigned char)~(1u << (k & 7)); }
        for (; x + 8 <= w; x += 8) { unsigned int b = 0; int i;
          for (i = 7; i >= 0; i--) { unsigned int v = q[x + i];
            gacc |= v ^ (v >> 8); aand &= v; abad |= v + 0x01000000u; d[x + i] = (unsigned char)v; b = b << 1 | v >> 31; }
          al[(k0 + x) >> 3] = (unsigned char)b; }
        for (; x < w; x++) { unsigned int v = q[x]; int k = k0 + x;
          gacc |= v ^ (v >> 8); aand &= v; abad |= v + 0x01000000u; d[x] = (unsigned char)v;
          if (v >> 31) al[k >> 3] |= (unsigned char)(1u << (k & 7)); else al[k >> 3] &= (unsigned char)~(1u << (k & 7)); }
        abad &= 0xfe000000u;
 *g=gacc;*a=aand;*ab=abad;}
int main(){ srand(1); unsigned q[300]; unsigned char d1[400],d2[400],a1[64],a2[64]; long bad=0;
 for(int it=0;it<2000000;it++){ int w=rand()%130, k0=rand()%100; int mode=rand()%4;
  for(int i=0;i<w;i++){unsigned l=rand()&255; unsigned al= mode==0? (rand()&1?255:0): mode==1? rand()&255 : 255; if(mode==3&&rand()%3==0) al=0; q[i]= (mode==1&&rand()%2? (unsigned)rand()<<8 : l*0x010101u) | al<<24;}
  for(int i=0;i<64;i++) a1[i]=a2[i]=rand(); memset(d1,7,400); memset(d2,7,400);
  unsigned g1,g2,x1,x2,b1,b2; oldf(q,w,k0,d1,a1,&g1,&x1,&b1); newf(q,w,k0,d2,a2,&g2,&x2,&b2);
  if(memcmp(d1,d2,400)||memcmp(a1,a2,64)||g1!=g2||x1!=x2||b1!=b2) bad++; }
 printf("mismatches %ld of 2000000\n",bad); return bad!=0; }
