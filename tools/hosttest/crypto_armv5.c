#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/chachapoly.h"
void mbedtls_platform_zeroize(void *b, size_t n){ volatile unsigned char *p=b; while(n--)*p++=0; }
int mbedtls_ct_memcmp(const void *a,const void *b,size_t n){return memcmp(a,b,n);}
void mbedtls_xor(unsigned char *r,const unsigned char *a,const unsigned char *b,size_t n){for(size_t i=0;i<n;i++)r[i]=a[i]^b[i];}
void ref_chacha20_init(mbedtls_chacha20_context*); int ref_chacha20_setkey(mbedtls_chacha20_context*,const unsigned char*);
int ref_chacha20_starts(mbedtls_chacha20_context*,const unsigned char*,uint32_t); int ref_chacha20_update(mbedtls_chacha20_context*,size_t,const unsigned char*,unsigned char*);
void ref_poly1305_init(mbedtls_poly1305_context*); int ref_poly1305_starts(mbedtls_poly1305_context*,const unsigned char*);
int ref_poly1305_update(mbedtls_poly1305_context*,const unsigned char*,size_t); int ref_poly1305_finish(mbedtls_poly1305_context*,unsigned char*);
static void ref(const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad, size_t al,
                const unsigned char *in, size_t len, unsigned char *out, unsigned char tag[16]){
  mbedtls_chacha20_context c; mbedtls_poly1305_context p; unsigned char ok[64]={0};
  ref_chacha20_init(&c); ref_chacha20_setkey(&c,key); ref_chacha20_starts(&c,nonce,0);
  ref_chacha20_update(&c,64,ok,ok); ref_poly1305_init(&p); ref_poly1305_starts(&p,ok);
  unsigned char z[16]={0}; ref_poly1305_update(&p,aad,al); if(al%16) ref_poly1305_update(&p,z,16-al%16);
  ref_chacha20_update(&c,len,in,out); ref_poly1305_update(&p,out,len); if(len%16) ref_poly1305_update(&p,z,16-len%16);
  unsigned char lens[16]; for(int i=0;i<8;i++){lens[i]=(unsigned char)((unsigned long long)al>>(8*i)); lens[8+i]=(unsigned char)((unsigned long long)len>>(8*i));}
  ref_poly1305_update(&p,lens,16); ref_poly1305_finish(&p,tag);}
int main(void){ srand(11); int fails=0; static unsigned char in[20000+8],a[20000+8],b[20000+8],aad[32],key[32],n[12],t1[16],t2[16];
 for(int t=0;t<3000;t++){ size_t len=rand()%17000, al=rand()%14; int oi=rand()%4;
  if(t<50) len=t; for(size_t i=0;i<len;i++)in[oi+i]=rand();
  for(int i=0;i<32;i++)key[i]=rand(); for(int i=0;i<12;i++)n[i]=rand(); for(size_t i=0;i<al;i++)aad[i]=rand();
  ref(key,n,aad,al,in+oi,len,a,t1);
  mbedtls_chachapoly_context c; mbedtls_chachapoly_init(&c); mbedtls_chachapoly_setkey(&c,key);
  memcpy(b+oi,in+oi,len);
  mbedtls_chachapoly_encrypt_and_tag(&c,len,n,aad,al,b+oi,b+oi,t2);
  if(memcmp(a,b+oi,len)||memcmp(t1,t2,16)) fails++;
  if(mbedtls_chachapoly_auth_decrypt(&c,len,n,aad,al,t2,b+oi,b+oi)!=0||memcmp(b+oi,in+oi,len)) fails++;
  /* streaming poly with odd chunks and unaligned input */
  mbedtls_poly1305_context p1,p2; unsigned char m1[16],m2[16];
  ref_poly1305_init(&p1); ref_poly1305_starts(&p1,key); ref_poly1305_update(&p1,in+oi,len); ref_poly1305_finish(&p1,m1);
  mbedtls_poly1305_init(&p2); mbedtls_poly1305_starts(&p2,key); size_t d=0; while(d<len){size_t k=rand()%100+1; if(k>len-d)k=len-d; mbedtls_poly1305_update(&p2,in+oi+d,k); d+=k;} mbedtls_poly1305_finish(&p2,m2);
  if(memcmp(m1,m2,16)) fails++;
  /* chacha streaming, unaligned out */
  mbedtls_chacha20_context x1,x2; uint32_t ctr=rand(); if(t%7==0) ctr=0xFFFFFFFFu-(rand()%3);
  ref_chacha20_init(&x1); ref_chacha20_setkey(&x1,key); ref_chacha20_starts(&x1,n,ctr); ref_chacha20_update(&x1,len,in+oi,a);
  mbedtls_chacha20_init(&x2); mbedtls_chacha20_setkey(&x2,key); mbedtls_chacha20_starts(&x2,n,ctr);
  int oo=rand()%4; d=0; while(d<len){size_t k=rand()%700+1; if(k>len-d)k=len-d; mbedtls_chacha20_update(&x2,k,in+oi+d,b+oo+d); d+=k;}
  if(memcmp(a,b+oo,len)) fails++;
 }
 printf("armv5 asm vs reference: %d failures\n",fails); return fails!=0; }
