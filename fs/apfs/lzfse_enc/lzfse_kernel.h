/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Kernel glue for the LZFSE and LZVN encoders. The encoder sources are Apple's
 * (BSD-3-Clause, github.com/lzfse/lzfse) with libc includes replaced by this
 * header. The few symbols that the decoder copy in ../lzfse also defines are
 * renamed so both can be linked into one module.
 */
#ifndef LZFSE_KERNEL_H
#define LZFSE_KERNEL_H

#include <linux/types.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/bug.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif
#ifdef __KERNEL__
#define uintmax_t u64
#define INT32_MAX S32_MAX
#endif
#define restrict __restrict
#define assert(x) do { } while (0)
#define malloc(n) kmalloc((n), GFP_KERNEL)
#define free(p) kfree(p)

#define fse_init_encoder_table apfs_enc_fse_init_encoder_table
#define fse_init_decoder_table apfs_enc_fse_init_decoder_table
#define fse_init_value_decoder_table apfs_enc_fse_init_value_decoder_table
#define fse_normalize_freq apfs_enc_fse_normalize_freq

#endif
