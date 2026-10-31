// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Encoding of decmpfs blocks. The output is what the block decoders in
 * compress_codec.c read back, and what other readers of the format expect:
 * a zlib stream for type 3, an LZVN stream for type 7 and an LZFSE stream
 * ("bvx" blocks, see Apple's open-source lzfse) for type 11.
 */

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>
#include "apfs.h"
#include "compress_encode.h"
#include "lzfse_enc/lzfse_enc.h"
#include "lzfse_enc/lzvn_encode_base.h"

static int apfs_encode_zlib(const u8 *in, size_t n, u8 *out, size_t cap, size_t *outlen)
{
	z_stream strm = {0};
	int ret = -ENOSPC;

	strm.workspace = vmalloc(zlib_deflate_workspacesize(MAX_WBITS, MAX_MEM_LEVEL));
	if (!strm.workspace)
		return -ENOMEM;
	if (zlib_deflateInit2(&strm, 6, Z_DEFLATED, MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
		ret = -EIO;
		goto out;
	}
	strm.next_in = in;
	strm.avail_in = n;
	strm.next_out = out;
	strm.avail_out = cap;
	if (zlib_deflate(&strm, Z_FINISH) == Z_STREAM_END) {
		*outlen = strm.total_out;
		ret = 0;
	}
	zlib_deflateEnd(&strm);
out:
	vfree(strm.workspace);
	return ret;
}

static int apfs_encode_lzvn(const u8 *in, size_t n, u8 *out, size_t cap, size_t *outlen)
{
	void *scratch = kvmalloc(lzvn_encode_scratch_size(), GFP_KERNEL);
	size_t len;

	if (!scratch)
		return -ENOMEM;
	len = lzvn_encode_buffer(out, cap, in, n, scratch);
	kvfree(scratch);
	if (!len)
		return -ENOSPC;
	*outlen = len;
	return 0;
}

static int apfs_encode_lzfse(const u8 *in, size_t n, u8 *out, size_t cap, size_t *outlen)
{
	void *scratch = kvmalloc(apfs_lzfse_encode_scratch_size(), GFP_KERNEL);
	size_t len;

	if (!scratch)
		return -ENOMEM;
	len = apfs_lzfse_encode_buffer(out, cap, in, n, scratch);
	kvfree(scratch);
	if (!len)
		return -ENOSPC;
	*outlen = len;
	return 0;
}

/**
 * apfs_compress_encode_block - Compress one block for a decmpfs file
 * @algo:	decmpfs type, one of the inline types
 * @in:		data to compress
 * @n:		size of @in
 * @out:	buffer for the compressed block
 * @cap:	size of @out; the block is refused if it doesn't fit
 * @outlen:	on success, the size of the compressed block
 *
 * Returns 0 on success, -ENOSPC if the block doesn't fit in @cap, or another
 * negative error code.
 */
int apfs_compress_encode_block(u32 algo, const u8 *in, size_t n, u8 *out, size_t cap,
			       size_t *outlen)
{
	if (!n || !cap)
		return -EINVAL;

	switch (algo) {
	case APFS_COMPRESS_ZLIB_ATTR:
		return apfs_encode_zlib(in, n, out, cap, outlen);
	case APFS_COMPRESS_LZVN_ATTR:
		return apfs_encode_lzvn(in, n, out, cap, outlen);
	case APFS_COMPRESS_LZFSE_ATTR:
		return apfs_encode_lzfse(in, n, out, cap, outlen);
	default:
		return -EOPNOTSUPP;
	}
}
