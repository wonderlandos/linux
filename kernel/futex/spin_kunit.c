// SPDX-License-Identifier: GPL-2.0-or-later
/* KUnit: WolKernel adaptive FUTEX_WAIT spin policy (futex_spin_budget/next). */
#include <kunit/test.h>
#include "futex.h"

static void budget_off_cases(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, futex_spin_budget(0, 8, true), 0ULL);
	KUNIT_EXPECT_EQ(t, futex_spin_budget(4000, 0, true), 0ULL);
	/* another runnable task on this CPU: never spin */
	KUNIT_EXPECT_EQ(t, futex_spin_budget(4000, 8, false), 0ULL);
}

static void budget_scales_with_score(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, futex_spin_budget(4000, 8, true), 4000ULL);
	KUNIT_EXPECT_EQ(t, futex_spin_budget(4000, 4, true), 2000ULL);
	KUNIT_EXPECT_EQ(t, futex_spin_budget(4000, 1, true), 500ULL);
	KUNIT_EXPECT_EQ(t, futex_spin_budget(4000, 99, true), 4000ULL);
}

static void score_moves(struct kunit *t)
{
	unsigned int s = FUTEX_SPIN_SCORE_MAX;
	int i;

	KUNIT_EXPECT_EQ(t, futex_spin_next(s, true), FUTEX_SPIN_SCORE_MAX);
	for (i = 0; i < 4; i++)
		s = futex_spin_next(s, false);
	KUNIT_EXPECT_EQ(t, s, 0U);	/* 8->4->2->1->0: four timeouts stop spinning */
	s = futex_spin_next(s, true);
	KUNIT_EXPECT_EQ(t, s, 1U);
}

static void waitv_inline_bound(struct kunit *t)
{
	bool saved = futex_waitv_inline_on;

	futex_waitv_inline_on = true;
	KUNIT_EXPECT_TRUE(t, futex_waitv_inline(1));
	KUNIT_EXPECT_TRUE(t, futex_waitv_inline(FUTEX_WAITV_INLINE));
	KUNIT_EXPECT_FALSE(t, futex_waitv_inline(FUTEX_WAITV_INLINE + 1));
	KUNIT_EXPECT_FALSE(t, futex_waitv_inline(FUTEX_WAITV_MAX));
	futex_waitv_inline_on = false;	/* off: always the heap vector */
	KUNIT_EXPECT_FALSE(t, futex_waitv_inline(1));
	futex_waitv_inline_on = saved;
}

static struct kunit_case futex_spin_cases[] = {
	KUNIT_CASE(budget_off_cases),
	KUNIT_CASE(budget_scales_with_score),
	KUNIT_CASE(score_moves),
	KUNIT_CASE(waitv_inline_bound),
	{}
};

static struct kunit_suite futex_spin_suite = {
	.name = "wolos_futex_spin",
	.test_cases = futex_spin_cases,
};
kunit_test_suite(futex_spin_suite);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
