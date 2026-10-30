// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit for the WolKernel ntsync inline wait buffer (included from ntsync.c):
 * waits with up to NTSYNC_Q_INLINE entries use the caller's buffer, larger
 * ones and the off switch use the heap, and put frees only heap queues.
 */
#include <kunit/test.h>

static void ntsync_q_inline_or_heap(struct kunit *test)
{
	struct ntsync_q_buf buf;
	bool saved = ntsync_inline_wait;
	struct ntsync_q *q;
	__u32 n;

	ntsync_inline_wait = true;
	for (n = 1; n <= NTSYNC_Q_INLINE; n++) {
		q = ntsync_q_get(&buf, n);
		KUNIT_EXPECT_PTR_EQ(test, q, (struct ntsync_q *)buf.raw);
		/* every entry fits in the buffer */
		KUNIT_EXPECT_LE(test, (u8 *)&q->entries[n] - buf.raw, (long)sizeof(buf.raw));
		ntsync_q_put(&buf, q);
	}
	q = ntsync_q_get(&buf, NTSYNC_Q_INLINE + 1);
	KUNIT_ASSERT_NOT_NULL(test, q);
	KUNIT_EXPECT_PTR_NE(test, q, (struct ntsync_q *)buf.raw);
	ntsync_q_put(&buf, q);

	ntsync_inline_wait = false;	/* off: the stock heap queue */
	q = ntsync_q_get(&buf, 1);
	KUNIT_ASSERT_NOT_NULL(test, q);
	KUNIT_EXPECT_PTR_NE(test, q, (struct ntsync_q *)buf.raw);
	ntsync_q_put(&buf, q);
	ntsync_inline_wait = saved;
}

static struct kunit_case ntsync_q_cases[] = {
	KUNIT_CASE(ntsync_q_inline_or_heap),
	{}
};

static struct kunit_suite ntsync_q_suite = {
	.name = "wolos_ntsync_q",
	.test_cases = ntsync_q_cases,
};
kunit_test_suite(ntsync_q_suite);
