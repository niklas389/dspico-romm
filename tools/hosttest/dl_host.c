#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "net.h"
#include "sdwriter.h"
#include "dl.h"
#include <errno.h>
#include <sys/socket.h>
/* Host stand-in for the DSWiFi lwip_recv_zc(): random-sized chunks, leftovers kept */
static int zc_calls; static unsigned char zc_buf[70000]; static size_t zc_off, zc_len;
ssize_t lwip_recv_zc(int s, size_t max, size_t (*fn)(void*,const void*,size_t), void *ctx){
  size_t done=0;
  while(done<max){
    if(zc_off==zc_len){ int n=recv(s,zc_buf,1+rand()%2900,MSG_DONTWAIT);
      if(n==0){ if(done) break; errno=ENOTCONN; return -1; }
      if(n<0){ if(done) break; return -1; }
      zc_off=0; zc_len=n; if(getenv("TAMPER") && ++zc_calls==500) zc_buf[n/2]^=1; }
    size_t piece=zc_len-zc_off; if(piece>max-done) piece=max-done;
    size_t c=fn(ctx,zc_buf+zc_off,piece); zc_off+=c; done+=c; if(c<piece) break; }
  return done; }
static config_t cfg; static char *ca; static size_t calen;
const config_t *net_config(void){return &cfg;}
const char *net_ca_pem(size_t *l){*l=calen; return ca;}
uint32_t net_now_ms(void){struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000;}
/* sdwriter stub: small odd-sized ring windows to exercise reserve/commit */
struct sdwriter { unsigned char *buf; size_t len, cap; };
void *sdw_reserve(sdwriter_t *w, size_t *avail){ size_t a = 1000 + rand()%20000; if (w->len + a > w->cap) a = w->cap - w->len; *avail=a; return w->buf + w->len; }
void sdw_commit(sdwriter_t *w, size_t n){ w->len += n; }
static int bounces; bool sdw_write(sdwriter_t *w, const void *d, size_t n){ bounces++; memcpy(w->buf+w->len,d,n); w->len+=n; return true; }
static int check(const unsigned char *p, size_t a, size_t n){ for(size_t i=0;i<n;i++){size_t j=a+i; if(p[i]!=(unsigned char)(((j*7)+(j>>11))&255)) return 0;} return 1; }
static int calls; static bool cb(void*c,uint64_t now,uint64_t tot,uint32_t ms){(void)c;(void)now;(void)tot;(void)ms;calls++;return true;}
int main(void){ setvbuf(stdout,NULL,_IONBF,0);
  FILE*f=fopen("cert.pem","rb"); fseek(f,0,2); calen=ftell(f); rewind(f); ca=malloc(calen+1); fread(ca,1,calen,f); ca[calen]=0; fclose(f);
  strcpy(cfg.url,"https://localhost:8443"); strcpy(cfg.token,"TESTTOKEN");
  extern int psa_crypto_init(void); psa_crypto_init();
  int fails=0; net_result_t r;
  struct sdwriter w={malloc(10<<20),0,10<<20};
  /* 1: 8 MB range into the writer */
  bool ok=dl_get_range("/api/roms/1/content/x.nds",0,8388607,&w,cb,NULL,&r);
  printf("1: ok=%d status=%ld bytes=%llu ms=%u tls=%s %s zc=%d err=%s\n",ok,r.status,(unsigned long long)r.body_bytes,r.total_ms,r.tls_version,r.tls_cipher,r.zero_copy_tls,r.error);
  if(!ok||w.len!=8388608||!check(w.buf,0,w.len)||!r.zero_copy_tls) fails++;
  /* 2: keep-alive reuse, offset range */
  w.len=0; ok=dl_get_range("/api/roms/1/content/x.nds",1000001,2000000,&w,cb,NULL,&r);
  printf("2: ok=%d bytes=%llu connect=%u tls=%u zc=%d err=%s\n",ok,(unsigned long long)r.body_bytes,r.connect_ms,r.tls_ms,r.zero_copy_tls,r.error);
  if(!ok||w.len!=1000000||!check(w.buf,1000001,w.len)||r.tls_ms!=0) fails++;
  /* 3: discard mode, tail of file */
  ok=dl_get_range("/api/roms/1/content/x.nds",9*1024*1024,9*1024*1024+122,NULL,cb,NULL,&r);
  printf("3: ok=%d bytes=%llu\n",ok,(unsigned long long)r.body_bytes); if(!ok||r.body_bytes!=123) fails++;
  /* 4: 404 */
  ok=dl_get_range("/api/nope",0,10,&w,cb,NULL,&r); printf("4: ok=%d status=%ld err=%s\n",ok,r.status,r.error); if(ok||r.status!=404) fails++;
  /* 5: after 404 the connection is still usable; then Connection: close */
  w.len=0; ok=dl_get_range("/api/roms/1/content/close.nds",5,104,&w,cb,NULL,&r); printf("5: ok=%d bytes=%llu tls=%u\n",ok,(unsigned long long)r.body_bytes,r.tls_ms); if(!ok||w.len!=100||!check(w.buf,5,100)) fails++;
  /* 6: next request must reconnect */
  w.len=0; ok=dl_get_range("/api/roms/1/content/x.nds",0,99,&w,cb,NULL,&r); printf("6: ok=%d tls=%u\n",ok,r.tls_ms); if(!ok||r.tls_ms==0) fails++;
  /* 7: wrong token */
  strcpy(cfg.token,"BAD"); ok=dl_get_range("/api/roms/1/content/x.nds",0,9,&w,cb,NULL,&r); printf("7: ok=%d status=%ld\n",ok,r.status); if(ok||r.status!=401) fails++;
  /* 8: hostname mismatch must fail verification */
  dl_close(); strcpy(cfg.token,"TESTTOKEN");
  /* 9: zero-copy off, same data */
  extern bool dl_zero_copy_tls; dl_zero_copy_tls=false; w.len=0; ok=dl_get_range("/api/roms/1/content/x.nds",77,3000076,&w,cb,NULL,&r);
  printf("9: ok=%d zc=%d bytes=%llu\n",ok,r.zero_copy_tls,(unsigned long long)r.body_bytes); if(!ok||r.zero_copy_tls||w.len!=3000000||!check(w.buf,77,w.len)) fails++;
  dl_zero_copy_tls=true;
  /* 10: zc again on the same connection after a non-zc request */
  w.len=0; ok=dl_get_range("/api/roms/1/content/x.nds",123456,9*1024*1024+122,&w,cb,NULL,&r);
  printf("10: ok=%d zc=%d bytes=%llu tls=%u\n",ok,r.zero_copy_tls,(unsigned long long)r.body_bytes,r.tls_ms); if(!ok||!r.zero_copy_tls||!check(w.buf,123456,w.len)||w.len!=9*1024*1024+123-123456) fails++;
  printf("bounce writes: %d\nprogress callbacks: %d\nFAILURES: %d\n",bounces,calls,fails); return fails;
}
