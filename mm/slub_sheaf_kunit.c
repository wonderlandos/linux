// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit for the WolKernel sheaf scale: the base capacity table times
 * slub_sheaf_scale (clamped 1..4), and a new cache really gets it.
 */
#include <kunit/test.h>
#include <linux/slab.h>
#include "slab.h"

static void sheaf_base_table(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, slub_sheaf_base_capacity(64, 1), 60);
	KUNIT_EXPECT_EQ(test, slub_sheaf_base_capacity(256, 1), 26);
	KUNIT_EXPECT_EQ(test, slub_sheaf_base_capacity(1024, 1), 12);
	KUNIT_EXPECT_EQ(test, slub_sheaf_base_capacity(PAGE_SIZE, 1), 4);
	KUNIT_EXPECT_EQ(test, slub_sheaf_base_capacity(256, 2), 52);
	KUNIT_EXPECT_EQ(test, slub_sheaf_base_capacity(64, 0), 60);	/* clamped */
	KUNIT_EXPECT_EQ(test, slub_sheaf_base_capacity(64, 9), 240);	/* clamped */
}

static void sheaf_cache_uses_scale(struct kunit *test)
{
	struct kmem_cache *s = kmem_cache_create("wolos_sheaf_kunit", 128, 0, 0, NULL);
	unsigned int cap;
	bool debug;

	KUNIT_ASSERT_NOT_NULL(test, s);
	/* Default scale 2 (no boot parameter under KUnit), rounded up to a bucket. */
	debug = s->flags & SLAB_DEBUG_FLAGS;
	cap = s->sheaf_capacity;
	kmem_cache_destroy(s);
	if (debug)
		kunit_skip(test, "slub_debug active: no sheaves");
	KUNIT_EXPECT_GE(test, cap, 120U);
}

static struct kunit_case sheaf_cases[] = {
	KUNIT_CASE(sheaf_base_table),
	KUNIT_CASE(sheaf_cache_uses_scale),
	{}
};

static struct kunit_suite sheaf_suite = {
	.name = "wolos_slub_sheaf",
	.test_cases = sheaf_cases,
};
kunit_test_suite(sheaf_suite);
MODULE_DESCRIPTION("KUnit for the WolKernel SLUB sheaf scale");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
