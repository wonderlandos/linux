#pragma once
#include <linux/types.h>
int zlib_inflate_blob(void *dst, unsigned dst_sz, const void *src, unsigned src_sz);
