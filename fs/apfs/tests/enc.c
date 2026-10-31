#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apfs.h"
#include "compress_codec.h"
#include "compress_encode.h"
/* Round trip: encode with the module's encoders, decode with the module's decoders, and write
 * each stream to disk (enc.out.<algo>.<n>) so reference tools can decode it as well. */
static unsigned char in[65536], out[70000], back[65536];
static unsigned long long s = 88172645463325252ull;
static unsigned rnd(void){ s^=s<<13; s^=s>>7; s^=s<<17; return s>>11; }
static void gen(int kind,size_t n){
  for(size_t i=0;i<n;i++) switch(kind){
    case 0: in[i]=(i/7)%13?'a'+(i%5):0; break;                 /* repetitive text-like */
    case 1: in[i]=rnd(); break;                                   /* incompressible */
    case 2: in[i]=0; break;                                       /* zeros */
    case 3: in[i]="the quick brown fox jumps over the lazy dog "[i%44]; break;
    default: in[i]=(i*i>>3)&0xff; }
}
int main(void){ int bad=0, ok=0, refused=0; static const unsigned algos[]={3,7,11};
 size_t sizes[]={1,2,15,64,100,500,1000,2000,4095,4096,4097,8000,20000,65536};
 for(int a=0;a<3;a++) for(int k=0;k<5;k++) for(unsigned z=0;z<sizeof sizes/sizeof *sizes;z++){
   size_t n=sizes[z], ol=0, bl=0; gen(k,n);
   int r=apfs_compress_encode_block(algos[a],in,n,out,3788,&ol);
   if(r){ if(r!=-ENOSPC){ printf("algo %u kind %d n %zu: encode error %d\n",algos[a],k,n,r); bad++; } else refused++; continue; }
   if(apfs_decompress_block(algos[a],out,ol,back,sizeof back,n,&bl)||bl!=n||memcmp(back,in,n)){
     printf("algo %u kind %d n %zu: ROUNDTRIP FAIL (comp %zu)\n",algos[a],k,n,ol); bad++; continue; }
   ok++; char p[64]; sprintf(p,"enc.out.%u.%d.%zu",algos[a],k,n); FILE*f=fopen(p,"wb"); fwrite(out,1,ol,f); fclose(f);
   sprintf(p,"enc.in.%u.%d.%zu",algos[a],k,n); f=fopen(p,"wb"); fwrite(in,1,n,f); fclose(f);
 }
 printf("encode: %d round trips ok, %d refused (do not fit or do not shrink), %d bad\n",ok,refused,bad); return bad?1:0; }
