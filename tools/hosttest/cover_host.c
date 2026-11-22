// SPDX-License-Identifier: MIT
// Host test: decodes a PNG/JPEG cover with stb_image, converts it like the DSi
// does (covers_img.c) and renders the game screens to a PPM file. Build:
//   cc -DGFX_HOST -Isource -Isource/third_party/stb -o /tmp/cover_host tools/hosttest/cover_host.c
//      source/covers_img.c source/gfx.c source/ui.c source/font_data.c stbi_impl.c -lm
// (stbi_impl.c: #define STB_IMAGE_IMPLEMENTATION + #include "stb_image.h")
#include <stdio.h>
#include <stdlib.h>
#include "covers.h"
#include "gfx.h"
#include "ui.h"
#define STBI_NO_HDR
#include "stb_image.h"
int main(int argc,char**argv){
  int w,h,c; unsigned char *rgb=stbi_load(argv[1],&w,&h,&c,3); if(!rgb){printf("decode fail\n");return 1;}
  static uint16_t top[COVER_W*COVER_H], tile[TILE_S*TILE_S];
  int tw,th,sw,sh; cover_fit(rgb,w,h,top,COVER_W,COVER_H,&tw,&th); cover_fit(rgb,w,h,tile,TILE_S,TILE_S,&sw,&sh);
  gfx_alloc();
  ui_game_t g={"Pokémon White Version","Nintendo DS","USA, Europe","RPG","1",NULL,2010,256u<<20,true,false,"in sync",UI_OK,top,tw,th,"19:42"};
  ui_wifi_level=3; ui_draw_game_top(gfx_top,&g);
  ui_tile_t t[3]={{"A","A",ui_name_color("A"),NULL,0,0,0},{"Pokémon White Version","PWV",0,tile,sw,sh,UI_TILE_ON_SD},{"Pokémon Black","PB",ui_name_color("PB"),tile,sw,sh,0}};
  bool letters[27]={0}; letters[16]=1;
  ui_strip_t s={"Nintendo DS","3 games",t,3,1,256,NULL,letters,16,"x",0,0};
  ui_draw_strip(gfx_bot,&s);
  FILE*f=fopen(argv[2],"wb"); fprintf(f,"P6\n256 390\n255\n");
  for(int k=0;k<2;k++){uint16_t*fb=k?gfx_bot:gfx_top; for(int i=0;i<256*192;i++){uint16_t q=fb[i];fputc((q&31)*255/31,f);fputc(((q>>5)&31)*255/31,f);fputc(((q>>10)&31)*255/31,f);} if(!k) for(int i=0;i<256*6;i++){fputc(0,f);fputc(0,f);fputc(0,f);}}
  fclose(f); printf("%dx%d ok\n",w,h); return 0;}
