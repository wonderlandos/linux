// SPDX-License-Identifier: GPL-2.0-or-later
/* KUnit: WolKernel adaptive FUTEX_WAIT spin policy (futex_spin_budget/next). */
#include <kunit/test.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
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

static void prespin_budget_cases(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, futex_prespin_budget(0, 8, true), 0ULL);
	KUNIT_EXPECT_EQ(t, futex_prespin_budget(4000, 0, true), 0ULL);
	KUNIT_EXPECT_EQ(t, futex_prespin_budget(4000, 8, false), 0ULL);
	/* any score is the whole budget: a scaled one cannot reach a sleeping partner */
	KUNIT_EXPECT_EQ(t, futex_prespin_budget(4000, 1, true), 4000ULL);
	KUNIT_EXPECT_EQ(t, futex_prespin_budget(4000, 8, true), 4000ULL);
}

static void short_sleep_raises_score(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, futex_spin_slept(0, 3000, 4000), 1U);
	KUNIT_EXPECT_EQ(t, futex_spin_slept(0, 7999, 4000), 1U);
	/* only rearms: a wait just over the budget must not spin every time */
	KUNIT_EXPECT_EQ(t, futex_spin_slept(2, 100, 4000), 2U);
	/* a long sleep says nothing */
	KUNIT_EXPECT_EQ(t, futex_spin_slept(3, 8000, 4000), 3U);
	KUNIT_EXPECT_EQ(t, futex_spin_slept(0, 1000000, 4000), 0U);
	/* spin_ns=0: never */
	KUNIT_EXPECT_EQ(t, futex_spin_slept(0, 0, 0), 0U);
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

static void precheck_reads_user_value(struct kunit *t)
{
	unsigned long addr = kunit_vm_mmap(t, NULL, 0, PAGE_SIZE,
					   PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0);
	u32 __user *u = (u32 __user *)addr;
	u32 five = 5;

	KUNIT_ASSERT_NE(t, addr, 0UL);
	KUNIT_ASSERT_EQ(t, copy_to_user(u, &five, sizeof(five)), 0UL);
	KUNIT_EXPECT_FALSE(t, futex_value_differs(u, 5));	/* match: locked path decides */
	KUNIT_EXPECT_TRUE(t, futex_value_differs(u, 6));
	/* misaligned: keep -EINVAL; unmapped: keep -EFAULT handling */
	KUNIT_EXPECT_FALSE(t, futex_value_differs((u32 __user *)(addr + 1), 6));
	KUNIT_EXPECT_FALSE(t, futex_value_differs((u32 __user *)(addr - PAGE_SIZE), 6));
}

/* One page of user memory: word[] at the start, then the waitv array. */
struct scan_ctx {
	struct futex_waitv __user *uw;
	u32 __user *word;
	unsigned long addr;
};

static void scan_setup(struct kunit *t, struct scan_ctx *c)
{
	u32 five = 5;

	c->addr = kunit_vm_mmap(t, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE(t, c->addr, 0UL);
	c->word = (u32 __user *)c->addr;
	c->uw = (struct futex_waitv __user *)(c->addr + 256);
	KUNIT_ASSERT_EQ(t, copy_to_user(c->word, &five, sizeof(five)), 0UL);
}

enum { NORMAL, BAD_FLAGS, BAD_NUMA, BAD_RSV, BAD_VAL, BAD_ALIGN, BAD_UNMAPPED, BAD_KERNEL };

static const struct {
	const char *name;
	u64 v0;		/* expected value of entry 0 (word holds 5) */
	u32 fl0;
	int odd;	/* what is wrong with entry 1 */
	int want;
} scan_cases[] = {
	{ "all match", 5, FUTEX2_SIZE_U32, NORMAL, FUTEX_SCAN_MATCH },
	{ "differs", 6, FUTEX2_SIZE_U32, NORMAL, FUTEX_SCAN_DIFFERS },
	{ "differs private", 6, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, NORMAL, FUTEX_SCAN_DIFFERS },
	{ "unknown flag", 6, FUTEX2_SIZE_U32, BAD_FLAGS, FUTEX_SCAN_FULL },
	{ "numa entry", 6, FUTEX2_SIZE_U32, BAD_NUMA, FUTEX_SCAN_FULL },
	{ "reserved set", 6, FUTEX2_SIZE_U32, BAD_RSV, FUTEX_SCAN_FULL },
	{ "value over 32 bits", 6, FUTEX2_SIZE_U32, BAD_VAL, FUTEX_SCAN_FULL },
	{ "misaligned word", 6, FUTEX2_SIZE_U32, BAD_ALIGN, FUTEX_SCAN_FULL },
	{ "unmapped word", 6, FUTEX2_SIZE_U32, BAD_UNMAPPED, FUTEX_SCAN_FULL },
	{ "kernel word", 6, FUTEX2_SIZE_U32, BAD_KERNEL, FUTEX_SCAN_FULL },
};

static void waitv_scan_table(struct kunit *t)
{
	struct scan_ctx c;
	unsigned int i;

	scan_setup(t, &c);
	for (i = 0; i < ARRAY_SIZE(scan_cases); i++) {
		struct futex_waitv w[2] = {
			{ .val = scan_cases[i].v0, .uaddr = c.addr, .flags = scan_cases[i].fl0 },
			{ .val = 5, .uaddr = c.addr, .flags = FUTEX2_SIZE_U32 },
		};

		switch (scan_cases[i].odd) {
		case BAD_FLAGS:		w[1].flags |= 0x40; break;
		case BAD_NUMA:		w[1].flags |= FUTEX2_NUMA; break;
		case BAD_RSV:		w[1].__reserved = 1; break;
		case BAD_VAL:		w[1].val |= 1ULL << 32; break;
		case BAD_ALIGN:		w[1].uaddr += 1; break;
		case BAD_UNMAPPED:	w[1].uaddr = c.addr - PAGE_SIZE; break;
		case BAD_KERNEL:	w[1].uaddr = (unsigned long)&c; break;
		}
		KUNIT_ASSERT_EQ(t, copy_to_user(c.uw, w, sizeof(w)), 0UL);
		KUNIT_EXPECT_EQ_MSG(t, futex_waitv_scan(c.uw, 2, NULL), scan_cases[i].want,
				    "%s", scan_cases[i].name);
	}
}

static void waitv_scan_array_fault(struct kunit *t)
{
	struct scan_ctx c;
	struct futex_waitv w = { .val = 6, .flags = FUTEX2_SIZE_U32 };
	struct futex_waitv __user *last;

	scan_setup(t, &c);
	w.uaddr = c.addr;
	last = (struct futex_waitv __user *)(c.addr + PAGE_SIZE - sizeof(w));
	KUNIT_ASSERT_EQ(t, copy_to_user(last, &w, sizeof(w)), 0UL);
	KUNIT_EXPECT_EQ(t, futex_waitv_scan(last, 1, NULL), FUTEX_SCAN_DIFFERS);
	/* entry 1 runs into the next page: whatever is there, no verdict */
	vm_munmap(c.addr + PAGE_SIZE, PAGE_SIZE);
	KUNIT_EXPECT_EQ(t, futex_waitv_scan(last, 2, NULL), FUTEX_SCAN_FULL);
}

static void waitv_scan_every_index(struct kunit *t)
{
	static struct futex_waitv w[FUTEX_WAITV_MAX];
	struct scan_ctx c;
	unsigned long big;
	unsigned int i;

	scan_setup(t, &c);
	big = kunit_vm_mmap(t, NULL, 0, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			    MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE(t, big, 0UL);
	for (i = 0; i < FUTEX_WAITV_MAX; i++) {
		w[i].val = 5;
		w[i].uaddr = c.addr;
		w[i].flags = FUTEX2_SIZE_U32;
		w[i].__reserved = 0;
	}
	for (i = 0; i < FUTEX_WAITV_MAX; i++) {
		w[i].val = 4;
		KUNIT_ASSERT_EQ(t, copy_to_user((void __user *)big, w, sizeof(w)), 0UL);
		KUNIT_EXPECT_EQ(t, futex_waitv_scan((void __user *)big, FUTEX_WAITV_MAX, NULL),
				FUTEX_SCAN_DIFFERS);
		w[i].val = 5;
	}
	KUNIT_ASSERT_EQ(t, copy_to_user((void __user *)big, w, sizeof(w)), 0UL);
	KUNIT_EXPECT_EQ(t, futex_waitv_scan((void __user *)big, FUTEX_WAITV_MAX, NULL),
			FUTEX_SCAN_MATCH);
}

static void waitv_scan_timeout(struct kunit *t)
{
	static const struct {
		s64 sec, nsec;
		int want;
	} cases[] = {
		{ 1, 0, FUTEX_SCAN_DIFFERS },
		{ 0, 999999999, FUTEX_SCAN_DIFFERS },
		{ -1, 0, FUTEX_SCAN_FULL },
		{ 0, NSEC_PER_SEC, FUTEX_SCAN_FULL },
		{ 0, -1, FUTEX_SCAN_FULL },
	};
	struct futex_waitv w = { .val = 6, .flags = FUTEX2_SIZE_U32 };
	struct __kernel_timespec __user *ut;
	struct scan_ctx c;
	unsigned int i;

	scan_setup(t, &c);
	w.uaddr = c.addr;
	ut = (struct __kernel_timespec __user *)(c.addr + 128);
	KUNIT_ASSERT_EQ(t, copy_to_user(c.uw, &w, sizeof(w)), 0UL);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct __kernel_timespec ts = { cases[i].sec, cases[i].nsec };

		KUNIT_ASSERT_EQ(t, copy_to_user(ut, &ts, sizeof(ts)), 0UL);
		KUNIT_EXPECT_EQ(t, futex_waitv_scan(c.uw, 1, ut), cases[i].want);
	}
	/* an unreadable timeout has no verdict either */
	KUNIT_EXPECT_EQ(t, futex_waitv_scan(c.uw, 1, (void __user *)(c.addr - PAGE_SIZE)),
			FUTEX_SCAN_FULL);
}

#define SHARED32	(FLAGS_SHARED | FLAGS_SIZE_32)

static u32 __user *shared_word(struct kunit *t)
{
	unsigned long a = kunit_vm_mmap(t, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					MAP_ANONYMOUS | MAP_SHARED, 0);
	u32 zero = 0;

	KUNIT_ASSERT_NE(t, a, 0UL);
	KUNIT_ASSERT_EQ(t, copy_to_user((void __user *)a, &zero, sizeof(zero)), 0UL);
	return (u32 __user *)a;
}

static void key_cache_hits_until_mmap_moves(struct kunit *t)
{
	union futex_key k1 = FUTEX_KEY_INIT, k2 = FUTEX_KEY_INIT;
	u32 __user *u = shared_word(t);
	struct futex_key_cache *c;
	unsigned int hits;

	KUNIT_ASSERT_EQ(t, futex_key_get(u, SHARED32, &k1, FUTEX_READ), 0);
	c = current->futex.key_cache;
	KUNIT_ASSERT_NOT_NULL(t, c);
	hits = c->hits;
	KUNIT_ASSERT_EQ(t, futex_key_get(u, SHARED32, &k2, FUTEX_READ), 0);
	KUNIT_EXPECT_EQ(t, c->hits, hits + 1);
	KUNIT_EXPECT_TRUE(t, futex_match(&k1, &k2));
	/* same key as the uncached path gives */
	KUNIT_ASSERT_EQ(t, get_futex_key(u, SHARED32, &k2, FUTEX_READ), 0);
	KUNIT_EXPECT_TRUE(t, futex_match(&k1, &k2));

	/* any mmap_lock writer empties it */
	KUNIT_ASSERT_NE(t, (unsigned long)shared_word(t), 0UL);
	hits = c->hits;
	KUNIT_ASSERT_EQ(t, futex_key_get(u, SHARED32, &k2, FUTEX_READ), 0);
	KUNIT_EXPECT_EQ(t, c->hits, hits);
}

static void key_cache_only_plain_reads(struct kunit *t)
{
	union futex_key k = FUTEX_KEY_INIT;
	u32 __user *u = shared_word(t);
	struct futex_key_cache *c;
	unsigned int hits;

	KUNIT_ASSERT_EQ(t, futex_key_get(u, SHARED32, &k, FUTEX_READ), 0);
	KUNIT_ASSERT_EQ(t, futex_key_get(u, SHARED32, &k, FUTEX_READ), 0);
	c = current->futex.key_cache;
	KUNIT_ASSERT_NOT_NULL(t, c);
	hits = c->hits;
	/* writers, private and misaligned words go down the plain path */
	KUNIT_EXPECT_EQ(t, futex_key_get(u, SHARED32, &k, FUTEX_WRITE), 0);
	KUNIT_EXPECT_EQ(t, futex_key_get(u, FLAGS_SIZE_32, &k, FUTEX_READ), 0);
	KUNIT_EXPECT_EQ(t, futex_key_get((void __user *)u + 1, SHARED32, &k, FUTEX_READ),
			-EINVAL);
	KUNIT_EXPECT_EQ(t, c->hits, hits);
}

static void key_cache_off(struct kunit *t)
{
	union futex_key k = FUTEX_KEY_INIT;
	u32 __user *u = shared_word(t);
	bool saved = futex_key_cache_on;
	unsigned int hits;

	KUNIT_ASSERT_EQ(t, futex_key_get(u, SHARED32, &k, FUTEX_READ), 0);
	KUNIT_ASSERT_NOT_NULL(t, current->futex.key_cache);
	hits = current->futex.key_cache->hits;
	futex_key_cache_on = false;
	KUNIT_EXPECT_EQ(t, futex_key_get(u, SHARED32, &k, FUTEX_READ), 0);
	futex_key_cache_on = saved;
	KUNIT_EXPECT_EQ(t, current->futex.key_cache->hits, hits);
}

static struct kunit_case futex_spin_cases[] = {
	KUNIT_CASE(budget_off_cases),
	KUNIT_CASE(budget_scales_with_score),
	KUNIT_CASE(score_moves),
	KUNIT_CASE(prespin_budget_cases),
	KUNIT_CASE(short_sleep_raises_score),
	KUNIT_CASE(waitv_inline_bound),
	KUNIT_CASE(precheck_reads_user_value),
	KUNIT_CASE(waitv_scan_table),
	KUNIT_CASE(waitv_scan_array_fault),
	KUNIT_CASE(waitv_scan_every_index),
	KUNIT_CASE(waitv_scan_timeout),
	KUNIT_CASE(key_cache_hits_until_mmap_moves),
	KUNIT_CASE(key_cache_only_plain_reads),
	KUNIT_CASE(key_cache_off),
	{}
};

static struct kunit_suite futex_spin_suite = {
	.name = "wolos_futex_spin",
	.test_cases = futex_spin_cases,
};
kunit_test_suite(futex_spin_suite);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
