// SPDX-License-Identifier: GPL-2.0
/* KUnit: WolKernel legacy-path decisions (include/linux/wolos_compat.h). */
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/wolos_compat.h>

static void audit_lazy(struct kunit *t)
{
	/* audit on, no syscall rules: no per-syscall work */
	KUNIT_EXPECT_FALSE(t, wolos_audit_needs_work(true, false, 0));
	KUNIT_EXPECT_TRUE(t, wolos_audit_needs_work(true, false, 1));
	/* a task the filters exclude never needs it */
	KUNIT_EXPECT_FALSE(t, wolos_audit_needs_work(true, true, 5));
	KUNIT_EXPECT_FALSE(t, wolos_audit_needs_work(false, true, 0));
	/* switch off: stock behaviour, work even without rules */
	KUNIT_EXPECT_TRUE(t, wolos_audit_needs_work(false, false, 0));
}

static void int80_64(struct kunit *t)
{
	KUNIT_EXPECT_TRUE(t, wolos_int80_allowed(false, false));	/* 32-bit game */
	KUNIT_EXPECT_TRUE(t, wolos_int80_allowed(false, true));
	KUNIT_EXPECT_FALSE(t, wolos_int80_allowed(true, false));	/* 64-bit task */
	KUNIT_EXPECT_TRUE(t, wolos_int80_allowed(true, true));		/* abi.int80_64=1 */
}

static void retbleed_stuff(struct kunit *t)
{
	KUNIT_EXPECT_TRUE(t, wolos_retbleed_prefer_stuff(true, true, false));
	KUNIT_EXPECT_FALSE(t, wolos_retbleed_prefer_stuff(true, true, true));	/* eIBRS */
	KUNIT_EXPECT_FALSE(t, wolos_retbleed_prefer_stuff(true, false, false));	/* no CDT */
	KUNIT_EXPECT_FALSE(t, wolos_retbleed_prefer_stuff(false, true, false));
}

static void defaults(struct kunit *t)
{
	KUNIT_EXPECT_GT(t, WOLOS_MAX_MAP_COUNT, DEFAULT_MAX_MAP_COUNT);
	KUNIT_EXPECT_LE(t, (long)WOLOS_MAX_MAP_COUNT, (long)INT_MAX);
	/* proc_watchdog_thresh accepts 0..60 */
	KUNIT_EXPECT_GT(t, WOLOS_WATCHDOG_THRESH, 10);
	KUNIT_EXPECT_LE(t, WOLOS_WATCHDOG_THRESH, 60);
}

static struct kunit_case wolos_compat_cases[] = {
	KUNIT_CASE(audit_lazy),
	KUNIT_CASE(int80_64),
	KUNIT_CASE(retbleed_stuff),
	KUNIT_CASE(defaults),
	{}
};

static struct kunit_suite wolos_compat_suite = {
	.name = "wolos_compat",
	.test_cases = wolos_compat_cases,
};
kunit_test_suite(wolos_compat_suite);
MODULE_DESCRIPTION("KUnit tests for WolKernel legacy-path decisions");
MODULE_LICENSE("GPL");
