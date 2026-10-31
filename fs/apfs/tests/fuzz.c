/* userspace fuzz harness for fs/apfs/compress_codec.c, built against shims */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "apfs.h"
#include "compress_codec.h"
#define CAP 0x10000
static uint8_t seeds[64][70000]; static size_t slen[64]; static int ns; static uint32_t salgo[64];
static uint64_t s=88172645463325252ull; static uint32_t rnd(void){ s^=s<<13; s^=s>>7; s^=s<<17; return s>>11; }
static void try(uint32_t algo,const uint8_t*c,size_t n){
  uint8_t *in=malloc(n?n:1); memcpy(in,c,n);
  size_t want=rnd()%(CAP+1); if(!(rnd()&3)) want=CAP;
  uint8_t *out=malloc(CAP); size_t ol=0xdeadbeef;
  int r=apfs_decompress_block(algo,in,n,out,CAP,want,&ol);
  if(r==0 && ol>CAP){ fprintf(stderr,"outlen %zu > cap\n",ol); abort(); }
  free(in); free(out);
}
int main(int argc,char**argv){
  long iters=atol(argv[1]); s^=atol(argv[2]);
  for(int i=3;i+1<argc;i+=2){ uint32_t al=atoi(argv[i]); FILE*f=fopen(argv[i+1],"rb"); if(!f) continue; slen[ns]=fread(seeds[ns],1,70000,f); salgo[ns]=al; ns++; fclose(f);}
  uint32_t algos[]={3,4,7,8,9,10,11,12,13,14};
  for(long it=0;it<iters;it++){
    static uint8_t buf[70000]; size_t n; uint32_t al=algos[rnd()%10];
    if(ns&&(rnd()%8)){ int k=rnd()%ns; n=slen[k]; memcpy(buf,seeds[k],n); al=salgo[k];
      int m=1+rnd()%6; while(m--){ switch(rnd()%4){ case 0: if(n) buf[rnd()%n]^=1<<(rnd()%8); break; case 1: if(n) buf[rnd()%n]=rnd(); break; case 2: n=n?rnd()%(n+1):0; break; case 3: if(n>2){ size_t a=rnd()%n,b=rnd()%n; buf[a]=buf[b]; } break; } }
    } else { n=rnd()%300; for(size_t i=0;i<n;i++) buf[i]=rnd(); static const uint8_t mk[]={0x62,0x78,0x06,0x5a,0xff,0x0f,0x00}; if(n&&(rnd()&1)) buf[0]=mk[rnd()%7]; }
    try(al,buf,n);
  }
  printf("done %ld iters\n",iters); return 0; }
