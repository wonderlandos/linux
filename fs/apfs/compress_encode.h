/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _APFS_COMPRESS_ENCODE_H
#define _APFS_COMPRESS_ENCODE_H

#include <linux/types.h>

int apfs_compress_encode_block(u32 algo, const u8 *in, size_t n, u8 *out, size_t cap,
			       size_t *outlen);

#endif /* _APFS_COMPRESS_ENCODE_H */
