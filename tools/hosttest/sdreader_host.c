// SPDX-License-Identifier: MIT
// Host test for source/sdreader.c with a mock DMA engine (see unzip_host.c
// for the zlib sources; add source/sdreader.c and a mock nds.h that defines
// an empty cothread_yield()).
// sdreader + zip_extract_src with a mock engine: ops complete at once
// (and sometimes only on the next yield-less get, to keep it simple: at once).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdreader.h"
#include "unzip.h"
static FILE *IMG; static int ops, maxq;
bool sdraw_async_init(void){ return true; }
bool sdraw_op_submit(sdraw_op_t *op){
  if (((uintptr_t)op->buf & 31) || op->count==0 || op->lba==0) return false;
  memset(op->buf, 0xEE, op->count*512);
  fseek(IMG,(long)(op->lba-1)*512,SEEK_SET); fread(op->buf,1,op->count*512,IMG); // short read at EOF ok
  op->state = SDRAW_OP_DONE; ops++; return true; }
static unsigned char ring[100000]; static FILE *OUT;
static void *res(void *c,size_t *a){(void)c;*a=sizeof ring;return ring;}
static void com(void *c,size_t n){(void)c;fwrite(ring,1,n,OUT);}
static int g(void*c,const uint8_t**p,size_t*n){return sdr_get(c,p,n);}
static void cs(void*c,size_t n){sdr_consume(c,n);}
int main(int argc,char**argv){
  IMG=fopen(argv[1],"rb"); fseek(IMG,0,SEEK_END); long sz=ftell(IMG);
  zip_entry_t e; char err[128]; if(!zip_find_rom(IMG,argv[2],&e,err,sizeof err)){puts(err);return 1;}
  uint64_t off; zip_data_offset(IMG,&e,&off);
  // Split the file's sectors into extents of odd sizes; LBA = sector + 1 + gap
  // (gaps mapped back by the mock: keep identity by using no gaps but many extents)
  sdraw_extent_t ext[64]; int n=0; uint32_t secs=(sz+511)/512, s=0, sizes[]={3,1,700,129,128,5000};
  while(s<secs && n<63){ uint32_t c=sizes[n%6]; if(s+c>secs)c=secs-s; ext[n].lba=s+1; ext[n].count=c; s+=c; n++; }
  if(s<secs){ext[n].lba=s+1;ext[n].count=secs-s;n++;}
  sdreader_t *r=sdr_open(ext,n,off,e.comp_size);
  OUT=fopen(argv[3],"wb"); zip_sink_t sink={res,com,NULL}; zip_source_t src={g,cs,r}; zip_stats_t st;
  bool ok=zip_extract_src(&src,&e,&sink,NULL,NULL,&st); sdr_close(r); fclose(OUT);
  printf("%s: data off %llu, %d extents, %d read ops, in %llu out %llu %s\n",ok?"ok":"FAIL",(unsigned long long)off,n,ops,
    (unsigned long long)st.in_bytes,(unsigned long long)st.out_bytes,st.error); return !ok; }
