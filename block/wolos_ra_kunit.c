// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit test of the default readahead of non-rotational disks: which queue
 * limits get wolos_mm.ssd_ra_kb, and that 0 restores the upstream default.
 */
#include <kunit/test.h>
#include <linux/backing-dev-defs.h>
#include <linux/blkdev.h>
#include <linux/pagemap.h>
#include <linux/sizes.h>
#include "blk.h"

static unsigned long ra_kb(blk_features_t features, unsigned int io_opt)
{
	struct queue_limits lim = {
		.features = features,
		.io_opt = io_opt,
		.max_sectors = SZ_1M >> SECTOR_SHIFT,
		.logical_block_size = 512,
	};
	struct backing_dev_info bdi = { };

	blk_apply_bdi_limits(&bdi, &lim);
	return bdi.ra_pages << (PAGE_SHIFT - 10);
}

static void ssd_ra_default(struct kunit *test)
{
	unsigned int saved = blk_ssd_ra_kb;
	unsigned long base = VM_READAHEAD_PAGES << (PAGE_SHIFT - 10);

	blk_ssd_ra_kb = 2048;
	KUNIT_EXPECT_EQ(test, ra_kb(0, 0), 2048UL);
	/* A device that asks for more keeps it. */
	KUNIT_EXPECT_EQ(test, ra_kb(0, SZ_4M), 2UL * SZ_4M / SZ_1K);
	/* RAM-backed: nothing to read ahead. */
	KUNIT_EXPECT_EQ(test, ra_kb(BLK_FEAT_SYNCHRONOUS, 0), base);
	/* Rotational disks keep the upstream rule. */
	KUNIT_EXPECT_EQ(test, ra_kb(BLK_FEAT_ROTATIONAL, 0), 2UL * SZ_1K);

	blk_ssd_ra_kb = 0;
	KUNIT_EXPECT_EQ(test, ra_kb(0, 0), base);
	blk_ssd_ra_kb = saved;
}

static struct kunit_case wolos_ra_cases[] = {
	KUNIT_CASE(ssd_ra_default),
	{}
};

static struct kunit_suite wolos_ra_suite = {
	.name = "wolos_ra",
	.test_cases = wolos_ra_cases,
};
kunit_test_suite(wolos_ra_suite);

MODULE_DESCRIPTION("KUnit test of the SSD readahead default");
MODULE_LICENSE("GPL");
