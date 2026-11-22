// SPDX-License-Identifier: MIT
// Host test: imgdec (streaming PNG, scaled JPEG, stb fallback) against stb_image.
//   cc -O2 -DIMGDEC_HOST -Isource -Isource/third_party/stb -Isource/third_party/tjpgd
//      -o /tmp/imgdec_host tools/hosttest/imgdec_host.c source/imgdec.c source/covers_img.c
//      source/third_party/tjpgd/tjpgd.c source/stb_image_impl.c -lz
//   /tmp/imgdec_host image.png image.jpg ...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "covers.h"
#include "imgdec.h"
#include "stb_image.h"
static unsigned char *rd(const char*p,size_t*n){FILE*f=fopen(p,"rb");fseek(f,0,2);*n=ftell(f);fseek(f,0,0);unsigned char*b=malloc(*n);fread(b,1,*n,f);fclose(f);return b;}
int main(int argc,char**argv){
 for(int a=1;a<argc;a++){ size_t n; unsigned char*b=rd(argv[a],&n);
  for(int fill=0;fill<2;fill++){
  static uint16_t o1[90*120],o2[90*120]; int w1,h1,w2,h2; imgdec_info_t inf;
  clock_t t=clock(); bool ok=imgdec_scaled(b,n,fill,fill?90:64,fill?120:64,o1,&w1,&h1,&inf); double ms=(clock()-t)*1000.0/CLOCKS_PER_SEC;
  int w,h,c; unsigned char*px=stbi_load_from_memory(b,n,&w,&h,&c,3);
  if(fill) {cover_fill(px,w,h,o2,90,120); w2=90;h2=120;} else cover_fit(px,w,h,o2,64,64,&w2,&h2);
  long diff=0,mx=0; if(ok&&w1==w2&&h1==h2) for(int i=0;i<w1*h1;i++) for(int k=0;k<3;k++){int d=abs(((o1[i]>>(5*k))&31)-((o2[i]>>(5*k))&31)); diff+=d; if(d>mx)mx=d;}
  printf("%-10s fill=%d ok=%d how=%-7s %dx%d -> %dx%d (ref %dx%d) meandiff=%.2f max=%ld  %.1f ms\n",strrchr(argv[a],'/')+1,fill,ok,inf.how?inf.how:"-",inf.w,inf.h,w1,h1,w2,h2,ok&&w1*h1?(double)diff/(w1*h1*3):-1,mx,ms);
  stbi_image_free(px);} free(b);}
}
