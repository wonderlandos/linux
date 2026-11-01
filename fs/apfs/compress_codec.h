/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APFS_COMPRESS_CODEC_H
#define _APFS_COMPRESS_CODEC_H

#include <linux/types.h>

int apfs_decompress_block(u32 algo, const u8 *cdata, size_t csize, u8 *out, size_t cap,
			  size_t want, size_t *outlen);

#endif /* _APFS_COMPRESS_CODEC_H */
