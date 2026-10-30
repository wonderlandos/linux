// SPDX-License-Identifier: GPL-2.0
/* KUnit: WolKernel policy decisions (include/linux/wolos_policy.h). */
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/wolos_policy.h>

static void tsc_trust(struct kunit *t)
{
	struct wolos_tsc_facts desktop_amd = { true, true, false, false, 1 };
	struct wolos_tsc_facts intel = { true, true, true, false, 1 };
	struct wolos_tsc_facts vm = { true, true, false, true, 1 };
	struct wolos_tsc_facts dual = { true, true, false, false, 2 };
	struct wolos_tsc_facts old = { true, false, false, false, 1 };

	/* one package, invariant TSC, bare metal: trusted even without TSC_ADJUST */
	KUNIT_EXPECT_TRUE(t, wolos_tsc_trusted(&desktop_amd));
	KUNIT_EXPECT_TRUE(t, wolos_tsc_trusted(&intel));	/* stock rule still holds */
	KUNIT_EXPECT_FALSE(t, wolos_tsc_trusted(&vm));
	KUNIT_EXPECT_FALSE(t, wolos_tsc_trusted(&dual));
	KUNIT_EXPECT_FALSE(t, wolos_tsc_trusted(&old));
}

static void kbd_bypass(struct kunit *t)
{
	KUNIT_EXPECT_TRUE(t, wolos_kbd_bypass(true, true));	/* compositor session */
	KUNIT_EXPECT_FALSE(t, wolos_kbd_bypass(false, true));
	KUNIT_EXPECT_FALSE(t, wolos_kbd_bypass(true, false));
	KUNIT_EXPECT_FALSE(t, wolos_kbd_bypass(false, false));	/* text VT */
}

static void oom_bias(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, wolos_oom_bias(800, 8000, true, false), 200L);
	KUNIT_EXPECT_EQ(t, wolos_oom_bias(100, 8000, false, true), 1100L);
	KUNIT_EXPECT_EQ(t, wolos_oom_bias(500, 8000, false, false), 500L);
	/* foreground wins over SCHED_IDLE */
	KUNIT_EXPECT_EQ(t, wolos_oom_bias(800, 8000, true, true), 200L);
	/* background (100+1000) now ranks above a bigger foreground app (800/4) */
	KUNIT_EXPECT_GT(t, wolos_oom_bias(100, 8000, false, true),
			wolos_oom_bias(800, 8000, true, false));
}

static void input_boost(struct kunit *t)
{
	unsigned long gap = 50;

	KUNIT_EXPECT_TRUE(t, wolos_boost_should_fire(1000, 950, gap));
	KUNIT_EXPECT_FALSE(t, wolos_boost_should_fire(1000, 951, gap));	/* rate limit */
	/* across the jiffies wrap */
	KUNIT_EXPECT_TRUE(t, wolos_boost_should_fire(10, ULONG_MAX - 40, gap));
	KUNIT_EXPECT_FALSE(t, wolos_boost_should_fire(5, ULONG_MAX - 40, gap));

	KUNIT_EXPECT_EQ(t, wolos_boost_floor(100, 200, 1024, 60), 614UL);
	KUNIT_EXPECT_EQ(t, wolos_boost_floor(200, 200, 1024, 60), 0UL);	/* window closed */
	KUNIT_EXPECT_EQ(t, wolos_boost_floor(300, 200, 1024, 60), 0UL);
	KUNIT_EXPECT_EQ(t, wolos_boost_floor(100, 200, 1024, 250), 1024UL);	/* clamped */
	KUNIT_EXPECT_EQ(t, wolos_boost_floor(ULONG_MAX - 5, 10, 1000, 50), 500UL); /* wrap */
}

static struct kunit_case wolos_policy_cases[] = {
	KUNIT_CASE(input_boost),
	KUNIT_CASE(tsc_trust),
	KUNIT_CASE(kbd_bypass),
	KUNIT_CASE(oom_bias),
	{}
};

static struct kunit_suite wolos_policy_suite = {
	.name = "wolos_policy",
	.test_cases = wolos_policy_cases,
};
kunit_test_suite(wolos_policy_suite);
MODULE_LICENSE("GPL");
