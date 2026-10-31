// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Corellium LLC
 */

#include <linux/slab.h>
#include <linux/zlib.h>
#include <linux/mutex.h>

#include "apfs.h"
#include "compress_codec.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)

#include <linux/vmalloc.h>

static inline void *kvmalloc(size_t size, gfp_t flags)
{
	gfp_t kmalloc_flags = flags;
	void *ret;

	if ((flags & GFP_KERNEL) != GFP_KERNEL)
		return kmalloc(size, flags);

	if (size > PAGE_SIZE)
		kmalloc_flags |= __GFP_NOWARN | __GFP_NORETRY;

	ret = kmalloc(size, flags);
	if (ret || size < PAGE_SIZE)
		return ret;

	return vmalloc(size);
}

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0) */

struct apfs_compress_file_data {
	struct apfs_compress_hdr hdr;
	u8 *buf;
	ssize_t bufblk;
	size_t bufsize;
	struct mutex mtx;
	struct super_block *sb;
	struct apfs_compressed_data cdata;
};

static inline int apfs_compress_is_rsrc(u32 algo)
{
	return (algo & 1) == 0;
}

static inline bool apfs_compress_is_supported(u32 algo)
{
	switch (algo) {
	case APFS_COMPRESS_ZLIB_RSRC:
	case APFS_COMPRESS_ZLIB_ATTR:
	case APFS_COMPRESS_LZVN_RSRC:
	case APFS_COMPRESS_LZVN_ATTR:
	case APFS_COMPRESS_PLAIN_RSRC:
	case APFS_COMPRESS_PLAIN_ATTR:
	case APFS_COMPRESS_LZFSE_RSRC:
	case APFS_COMPRESS_LZFSE_ATTR:
	case APFS_COMPRESS_LZBITMAP_RSRC:
	case APFS_COMPRESS_LZBITMAP_ATTR:
		return true;
	default:
		/* Once will usually be enough, don't flood the console */
		pr_err_once("APFS: unsupported compression algorithm (%u)\n", algo);
		return false;
	}
}

static int apfs_compress_file_open(struct inode *inode, struct file *filp)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_compress_file_data *fd;
	ssize_t res;
	bool is_rsrc;

	/*
	 * The official implementation seems to transparently decompress files
	 * when you write to them. Doing that atomically inside the kernel is
	 * probably a chore, so for now I'll just leave it to the user to make
	 * an uncompressed copy themselves and replace the original. I might
	 * fix this in the future, but only if people complain (TODO).
	 */
	if (filp->f_mode & FMODE_WRITE) {
		apfs_warn(sb, "writes to compressed files are not supported");
		apfs_warn(sb, "you can work with a copy of the file instead");
		return -EOPNOTSUPP;
	}

	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;

	fd = kzalloc(sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return -ENOMEM;
	mutex_init(&fd->mtx);
	fd->sb = sb;

	down_read(&nxi->nx_big_sem);

	res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, &fd->hdr, sizeof(fd->hdr), 0);
	if (res != sizeof(fd->hdr)) {
		apfs_err(sb, "decmpfs header read failed");
		goto fail;
	}

	if (!apfs_compress_is_supported(le32_to_cpu(fd->hdr.algo))) {
		res = -EOPNOTSUPP;
		goto fail;
	}

	fd->buf = kvmalloc(APFS_COMPRESS_BLOCK, GFP_KERNEL);
	if (!fd->buf) {
		res = -ENOMEM;
		goto fail;
	}
	fd->bufblk = -1;

	is_rsrc = apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo));
	res = apfs_xattr_get_compressed_data(inode, is_rsrc ? APFS_XATTR_NAME_RSRC_FORK : APFS_XATTR_NAME_COMPRESSED, &fd->cdata);
	if (res) {
		apfs_err(sb, "failed to get compressed data");
		goto fail;
	}

	up_read(&nxi->nx_big_sem);

	filp->private_data = fd;
	return 0;

fail:
	apfs_release_compressed_data(&fd->cdata);
	if (fd->buf)
		kvfree(fd->buf);
	up_read(&nxi->nx_big_sem);
	kfree(fd);
	if (res > 0)
		res = -EINVAL;
	return res;
}

static int apfs_compress_file_read_block(struct apfs_compress_file_data *fd, loff_t block)
{
	struct super_block *sb = fd->sb;
	struct apfs_compressed_data *comp_data = &fd->cdata;
	u8 *cdata = NULL;
	u8 *tmp = fd->buf;
	u32 doffs = 0, coffs;
	size_t csize, bsize;
	int res = 0;

	if (apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo)) &&
	   le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZBITMAP_RSRC &&
	   le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZVN_RSRC &&
	   le32_to_cpu(fd->hdr.algo) != APFS_COMPRESS_LZFSE_RSRC) {
		struct apfs_compress_rsrc_hdr hdr = {0};
		struct apfs_compress_rsrc_data cd = {0};
		struct apfs_compress_rsrc_block blk = {0};
		u32 blk_off;

		res = apfs_compressed_data_read(comp_data, &hdr, sizeof(hdr), 0 /* offset */);
		if (res) {
			apfs_err(sb, "failed to read resource header");
			return res;
		}

		doffs = be32_to_cpu(hdr.data_offs);
		res = apfs_compressed_data_read(comp_data, &cd, sizeof(cd), doffs);
		if (res) {
			apfs_err(sb, "failed to read resource data header");
			return res;
		}
		if (block >= le32_to_cpu(cd.num))
			return 0;

		blk_off = doffs + sizeof(cd) + sizeof(blk) * block;
		res = apfs_compressed_data_read(comp_data, &blk, sizeof(blk), blk_off);
		if (res) {
			apfs_err(sb, "failed to read resource block metadata");
			return res;
		}

		bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
		if (bsize > APFS_COMPRESS_BLOCK)
			bsize = APFS_COMPRESS_BLOCK;

		csize = le32_to_cpu(blk.size);
		coffs = le32_to_cpu(blk.offs) + 4;
	} else if (apfs_compress_is_rsrc(le32_to_cpu(fd->hdr.algo))) {
		__le32 blks[2];
		u32 blk_off;

		blk_off = doffs + sizeof(__le32) * block;
		res = apfs_compressed_data_read(comp_data, blks, sizeof(blks), blk_off);
		if (res) {
			apfs_err(sb, "failed to read resource block metadata");
			return res;
		}

		bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
		if (bsize > APFS_COMPRESS_BLOCK)
			bsize = APFS_COMPRESS_BLOCK;

		coffs = le32_to_cpu(blks[0]);
		if (le32_to_cpu(blks[1]) <= coffs)
			return -EFSCORRUPTED;
		csize = le32_to_cpu(blks[1]) - coffs;
	} else {
		/*
		 * I think attr compression is only for single-block files, in
		 * fact none of these files ever seem to decompress to more than
		 * 2048 bytes.
		 */
		bsize = le64_to_cpu(fd->hdr.size);
		if (block != 0 || bsize > APFS_COMPRESS_BLOCK) {
			apfs_err(sb, "file too big for inline compression");
			return -EFSCORRUPTED;
		}

		/* The first few bytes are the decmpfs header */
		coffs = sizeof(struct apfs_compress_hdr);
		if (comp_data->size <= sizeof(struct apfs_compress_hdr))
			return -EFSCORRUPTED;
		csize = comp_data->size - sizeof(struct apfs_compress_hdr);
	}

	/* Incompressible blocks are stored whole, plus a marker byte */
	if (csize == 0 || csize > APFS_COMPRESS_BLOCK + 64) {
		apfs_err(sb, "bad compressed block size");
		return -EFSCORRUPTED;
	}
	cdata = kvmalloc(csize, GFP_KERNEL);
	if (!cdata)
		return -ENOMEM;
	res = apfs_compressed_data_read(comp_data, cdata, csize, (u64)doffs + coffs);
	if (res) {
		apfs_err(sb, "failed to read compressed block");
		goto fail;
	}

	res = apfs_decompress_block(le32_to_cpu(fd->hdr.algo), cdata, csize, tmp, APFS_COMPRESS_BLOCK, bsize, &bsize);
	if (res) {
		apfs_err(sb, "block decompression failed");
		goto fail;
	}
	fd->bufblk = block;
	fd->bufsize = bsize;
fail:
	kvfree(cdata);
	return res;
}

static int apfs_compress_file_release(struct inode *inode, struct file *filp)
{
	struct apfs_compress_file_data *fd = filp->private_data;

	apfs_release_compressed_data(&fd->cdata);
	if (fd->buf)
		kvfree(fd->buf);
	kfree(fd);
	return 0;
}

static ssize_t apfs_compress_file_read_from_block(struct apfs_compress_file_data *fd, char *buf, size_t size, loff_t off)
{
	struct super_block *sb = fd->sb;
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_compressed_data cdata = fd->cdata;
	loff_t block;
	size_t bsize;
	ssize_t res;

	/*
	 * Request reads of all blocks before actually working with any of them.
	 * The compressed data is typically small enough that this is effective.
	 * It would be much better to make an inode for the xattr dstream and
	 * work with readahead as usual, but I'm not confident I can get that
	 * right (TODO).
	 */
	if (cdata.has_dstream && off == 0) {
		down_read(&nxi->nx_big_sem);
		apfs_nonsparse_dstream_preread(cdata.dstream);
		up_read(&nxi->nx_big_sem);
	}

	if (off >= le64_to_cpu(fd->hdr.size))
		return 0;
	if (size > le64_to_cpu(fd->hdr.size) - off)
		size = le64_to_cpu(fd->hdr.size) - off;

	block = off / APFS_COMPRESS_BLOCK;
	off -= block * APFS_COMPRESS_BLOCK;
	if (block != fd->bufblk) {
		down_read(&nxi->nx_big_sem);
		res = apfs_compress_file_read_block(fd, block);
		up_read(&nxi->nx_big_sem);
		if (res) {
			apfs_err(sb, "failed to read block into buffer");
			return res;
		}
	}
	bsize = fd->bufsize;

	if (bsize < off)
		return 0;
	bsize -= off;
	if (size > bsize)
		size = bsize;
	memcpy(buf, fd->buf + off, size);
	return size;
}

static ssize_t apfs_compress_file_read_page(struct file *filp, char *buf, loff_t off)
{
	struct apfs_compress_file_data *fd = filp->private_data;
	loff_t step;
	ssize_t block, res;
	size_t size = PAGE_SIZE;

	step = 0;
	while (step < size) {
		block = APFS_COMPRESS_BLOCK - ((off + step) & (APFS_COMPRESS_BLOCK - 1));
		if (block > size - step)
			block = size - step;
		mutex_lock(&fd->mtx);
		res = apfs_compress_file_read_from_block(fd, buf + step, block, off + step);
		mutex_unlock(&fd->mtx);
		if (res < block) {
			if (res < 0 && !step)
				return res;
			step += res > 0 ? res : 0;
			break;
		}
		step += block;
	}
	return step;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0) || RHEL_VERSION_GE(9, 3)
static int apfs_compress_read_folio(struct file *filp, struct folio *folio)
{
	struct page *page = &folio->page;
#else
static int apfs_compress_readpage(struct file *filp, struct page *page)
{
#endif
	char *addr = NULL;
	ssize_t ret;
	loff_t off;

	/* Mostly copied from ext4_read_inline_page() */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	off = page_folio(page)->index << PAGE_SHIFT;
#else
	off = page->index << PAGE_SHIFT;
#endif
	addr = kmap(page);
	ret = apfs_compress_file_read_page(filp, addr, off);
	flush_dcache_page(page);
	kunmap(page);
	if (ret >= 0) {
		zero_user_segment(page, ret, PAGE_SIZE);
		SetPageUptodate(page);
		ret = 0;
	}

	unlock_page(page);
	return ret;
}

const struct address_space_operations apfs_compress_aops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0) || RHEL_VERSION_GE(9, 3)
	.read_folio	= apfs_compress_read_folio,
#else
	.readpage	= apfs_compress_readpage,
#endif
};

/* TODO: these operations are all happening without proper locks */
const struct file_operations apfs_compress_file_operations = {
	.open		= apfs_compress_file_open,
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.release	= apfs_compress_file_release,
	.mmap		= apfs_file_mmap,
};

int apfs_compress_get_size(struct inode *inode, loff_t *size)
{
	struct apfs_compress_hdr hdr;
	int res = ____apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, &hdr, sizeof(hdr), 0);

	if (res < 0)
		return res;
	if (res != sizeof(hdr)) {
		apfs_err(inode->i_sb, "decmpfs header read failed");
		return 1;
	}

	if (!apfs_compress_is_supported(le32_to_cpu(hdr.algo)))
		return 1;

	*size = le64_to_cpu(hdr.size);
	return 0;
}

#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/xattr.h>
#include "compress_encode.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)

/**
 * apfs_compress_read_data - Read the whole contents of a small file
 * @inode:	the file
 * @data:	buffer with room for i_size bytes
 */
static int apfs_compress_read_data(struct inode *inode, u8 *data)
{
	size_t n = i_size_read(inode);
	size_t off = 0;

	while (off < n) {
		struct folio *folio = read_mapping_folio(inode->i_mapping, off >> PAGE_SHIFT, NULL);
		size_t fo, len;

		if (IS_ERR(folio))
			return PTR_ERR(folio);
		fo = offset_in_folio(folio, off);
		len = min_t(size_t, n - off, folio_size(folio) - fo);
		memcpy_from_folio(data + off, folio, fo, len);
		folio_put(folio);
		off += len;
	}
	return 0;
}

/**
 * apfs_compress_inode - Turn a regular file into an inline compressed file
 * @inode:	the file, with its inode lock held
 *
 * Needs the "compress" mount option. The data is compressed into the decmpfs
 * xattr and the data stream is freed, the way Apple's tools store small
 * compressed files; nothing happens if the result would not be smaller. Only
 * files up to one compression block are handled (see the README for what is
 * left out). Returns 0 on success, or a negative error code in case of
 * failure.
 */
int apfs_compress_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_compress_hdr *hdr;
	size_t n = i_size_read(inode), clen = 0;
	u8 *data, *blob;
	int err;

	if (!sbi->s_compress_algo)
		return -EOPNOTSUPP;
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;
	if (!n || n > APFS_COMPRESS_BLOCK || !ai->i_has_dstream)
		return -EOPNOTSUPP;
	if (ai->i_dstream.ds_shared || apfs_vol_is_encrypted(sb) ||
	    (ai->i_int_flags & APFS_INODE_HAS_RSRC_FORK))
		return -EOPNOTSUPP;

	err = filemap_write_and_wait(inode->i_mapping);
	if (err)
		return err;

	data = kvmalloc(n, GFP_KERNEL);
	blob = kvmalloc(APFS_XATTR_MAX_EMBEDDED_SIZE, GFP_KERNEL);
	if (!data || !blob) {
		err = -ENOMEM;
		goto out;
	}
	err = apfs_compress_read_data(inode, data);
	if (err)
		goto out;

	hdr = (struct apfs_compress_hdr *)blob;
	err = apfs_compress_encode_block(sbi->s_compress_algo, data, n, blob + sizeof(*hdr),
					 APFS_XATTR_MAX_EMBEDDED_SIZE - sizeof(*hdr), &clen);
	if (!err && sizeof(*hdr) + clen >= n)
		err = -ENOSPC;
	if (err)
		goto out;
	hdr->signature = cpu_to_le32(APFS_COMPRESS_MAGIC);
	hdr->algo = cpu_to_le32(sbi->s_compress_algo);
	hdr->size = cpu_to_le64(n);

	err = apfs_transaction_start(sb, APFS_TRANS_REG);
	if (err)
		goto out;
	apfs_inode_join_transaction(sb, inode);

	err = apfs_xattr_set(inode, APFS_XATTR_NAME_COMPRESSED, blob, sizeof(*hdr) + clen, XATTR_CREATE);
	if (!err)
		err = apfs_inode_drop_dstream(inode);
	if (err) {
		apfs_transaction_abort(sb);
		goto out;
	}

	ai->i_bsd_flags |= APFS_INOBSD_COMPRESSED;
	ai->i_int_flags |= APFS_INODE_HAS_UNCOMPRESSED_SIZE;
	inode->i_blocks = (n + 511) >> 9;
	inode->i_fop = &apfs_compress_file_operations;
	inode->i_mapping->a_ops = &apfs_compress_aops;

	err = apfs_transaction_commit(sb);
	if (err)
		apfs_transaction_abort(sb);
out:
	kvfree(blob);
	kvfree(data);
	return err;
}

#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) */

int apfs_compress_inode(struct inode *inode)
{
	return -EOPNOTSUPP;
}

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) */
