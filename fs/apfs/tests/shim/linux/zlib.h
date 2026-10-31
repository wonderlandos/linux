#pragma once
#include <linux/types.h>
#include <zlib.h>
int zlib_inflate_blob(void *dst, unsigned dst_sz, const void *src, unsigned src_sz);
#define workspace opaque
#define zlib_deflate_workspacesize(a,b) 1
#define zlib_deflateInit2 deflateInit2
#define zlib_deflate deflate
#define zlib_deflateEnd deflateEnd
