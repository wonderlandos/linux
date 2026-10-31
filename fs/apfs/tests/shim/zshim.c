#include <zlib.h>
#include <string.h>
int zlib_inflate_blob(void *dst, unsigned dst_sz, const void *src, unsigned src_sz){
 z_stream z; memset(&z,0,sizeof z); if(inflateInit2(&z,-15)!=Z_OK) return -1;
 z.next_in=(void*)src; z.avail_in=src_sz; z.next_out=dst; z.avail_out=dst_sz;
 int r=inflate(&z,Z_FINISH); int n=dst_sz-z.avail_out; inflateEnd(&z);
 if(r!=Z_STREAM_END && r!=Z_BUF_ERROR && r!=Z_OK) return -1; return n; }
