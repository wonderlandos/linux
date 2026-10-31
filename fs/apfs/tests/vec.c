#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apfs.h"
#include "compress_codec.h"
static size_t rd(const char*p,unsigned char*b){FILE*f=fopen(p,"rb");if(!f)return 0;size_t n=fread(b,1,70000,f);fclose(f);return n;}
int main(void){ int bad=0; static unsigned char raw[70000],c[70000],o[0x10000]; char p[64];
 for(int k=0;k<6;k++){ if(k==2||k==3) continue; sprintf(p,"raw%d",k); size_t rn=rd(p,raw);
  struct{const char*pre;unsigned algo;}t[]={{"seed_z",3},{"seed_f",11},{"seed_n",7}};
  for(int j=0;j<3;j++){ sprintf(p,"%s%d",t[j].pre,k); size_t cn=rd(p,c); size_t ol=0;
   int r=apfs_decompress_block(t[j].algo,c,cn,o,0x10000,rn,&ol);
   int ok=!r&&ol==rn&&!memcmp(o,raw,rn); printf("%s algo=%u raw=%zu comp=%zu %s\n",p,t[j].algo,rn,cn,ok?"OK":"FAIL"); bad+=!ok; } }
 /* stored-uncompressed forms */
 { unsigned char s[11]={0xff,1,2,3,4,5,6,7,8,9,10}; size_t ol; unsigned char out[16];
   int r=apfs_decompress_block(11,s,11,out,16,10,&ol); printf("lzfse stored %s\n",(!r&&ol==10&&out[9]==10)?"OK":"FAIL"); bad+=!(!r&&ol==10);
   s[0]=0x06; r=apfs_decompress_block(7,s,11,out,16,10,&ol); printf("lzvn stored %s\n",(!r&&ol==10)?"OK":"FAIL"); bad+=!(!r&&ol==10);
   r=apfs_decompress_block(11,s,11,out,8,8,&ol); printf("oversize stored rejected: %s\n",r?"OK":"FAIL"); bad+=!r; r=apfs_decompress_block(9,s,0,out,8,8,&ol); printf("empty rejected: %s\n",r?"OK":"FAIL"); bad+=!r; }
 return bad?1:0; }
