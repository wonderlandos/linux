// SPDX-License-Identifier: GPL-2.0-only
/*
 * Block decoding for decmpfs compressed files. The input is untrusted disk
 * data: every length is checked before it is used, and a block never decodes
 * to more than the buffer it is given.
 */

#include <linux/slab.h>
#include <linux/zlib.h>
#include "apfs.h"
#include "libzbitmap.h"
#include "lzfse/lzfse.h"
#include "lzfse/lzvn_decode_base.h"
#include "compress_codec.h"

/* Stored without compression: a marker byte and then the raw bytes */
static int apfs_decompress_copy(u8 *out, size_t cap, const u8 *cdata, size_t csize, size_t *outlen)
{
	if (csize - 1 > cap)
		return -EFSCORRUPTED;
	memcpy(out, cdata + 1, csize - 1);
	*outlen = csize - 1;
	return 0;
}

/**
 * apfs_decompress_block - Decode one block of a decmpfs file
 * @algo:	decmpfs compression type
 * @cdata:	compressed block, including the marker byte
 * @csize:	size of @cdata
 * @out:	buffer for the decoded block
 * @cap:	size of @out
 * @want:	size the block is expected to decode to
 * @outlen:	on success, the decoded size (never more than @cap)
 */
int apfs_decompress_block(u32 algo, const u8 *cdata, size_t csize, u8 *out, size_t cap,
			  size_t want, size_t *outlen)
{
	int res;

	if (csize < 1 || want > cap)
		return -EFSCORRUPTED;

	switch (algo) {
	case APFS_COMPRESS_ZLIB_RSRC:
	case APFS_COMPRESS_ZLIB_ATTR:
		if (cdata[0] == 0x78 && csize >= 2) {
			res = zlib_inflate_blob(out, want, cdata + 2, csize - 2);
			if (res <= 0)
				return -EFSCORRUPTED;
			*outlen = res;
			return 0;
		}
		if ((cdata[0] & 0x0F) == 0x0F)
			return apfs_decompress_copy(out, cap, cdata, csize, outlen);
		return -EFSCORRUPTED;
	case APFS_COMPRESS_LZVN_RSRC:
	case APFS_COMPRESS_LZVN_ATTR:
		if (cdata[0] == 0x06) {
			return apfs_decompress_copy(out, cap, cdata, csize, outlen);
		} else {
			lzvn_decoder_state dstate = {0};

			dstate.src = cdata;
			dstate.src_end = dstate.src + csize;
			dstate.dst = dstate.dst_begin = out;
			dstate.dst_end = dstate.dst + want;
			lzvn_decode(&dstate);
			*outlen = dstate.dst - out;
			return 0;
		}
	case APFS_COMPRESS_LZBITMAP_RSRC:
	case APFS_COMPRESS_LZBITMAP_ATTR:
		if (cdata[0] == 0x5a) {
			*outlen = want;
			res = zbm_decompress(out, want, cdata, csize, outlen);
			if (res < 0 || *outlen > want)
				return -EFSCORRUPTED;
			return 0;
		}
		if ((cdata[0] & 0x0F) == 0x0F)
			return apfs_decompress_copy(out, cap, cdata, csize, outlen);
		return -EFSCORRUPTED;
	case APFS_COMPRESS_LZFSE_RSRC:
	case APFS_COMPRESS_LZFSE_ATTR:
		if (cdata[0] == 0x62 && csize >= 2) {
			size_t n = lzfse_decode_buffer(out, want, cdata, csize, NULL);

			if (n == 0 || n > want)
				return -EFSCORRUPTED;
			*outlen = n;
			return 0;
		}
		/* 0xff marks a block stored as is */
		return apfs_decompress_copy(out, cap, cdata, csize, outlen);
	case APFS_COMPRESS_PLAIN_RSRC:
	case APFS_COMPRESS_PLAIN_ATTR:
		return apfs_decompress_copy(out, cap, cdata, csize, outlen);
	default:
		return -EINVAL;
	}
}
