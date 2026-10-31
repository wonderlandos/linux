/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LZFSE_ENC_H
#define LZFSE_ENC_H

#include <linux/types.h>

size_t apfs_lzfse_encode_scratch_size(void);
size_t apfs_lzfse_encode_buffer(uint8_t *dst_buffer, size_t dst_size, const uint8_t *src_buffer,
				size_t src_size, void *scratch_buffer);

#endif
