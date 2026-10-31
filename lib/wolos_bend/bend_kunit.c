// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: wolos_bend (include/linux/wolos_bend.h).  The stress cases run
 * kthreads on every online CPU; under UML boot with ncpus=4 (CONFIG_SMP).
 */
#include <kunit/test.h>
#include <linux/bitmap.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/sizes.h>
#include <linux/kthread.h>
#include <linux/panic.h>
#include <linux/random.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/wolos_bend.h>

/* Run fn(i, ctx) on @n kthreads at once (spread over online CPUs). */
struct par {
	int (*fn)(int i, void *ctx);
	void *ctx;
	atomic_t ready, left, failed;
	struct completion go, done;
};

struct par_arg {
	struct par *p;
	int i;
};

static int par_thread(void *data)
{
	struct par_arg *a = data;
	struct par *p = a->p;

	atomic_inc(&p->ready);
	wait_for_completion(&p->go);
	if (p->fn(a->i, p->ctx))
		atomic_inc(&p->failed);
	if (atomic_dec_and_test(&p->left))
		complete(&p->done);
	return 0;
}

static int run_par(struct kunit *t, int n, int (*fn)(int, void *), void *ctx)
{
	struct par p = { .fn = fn, .ctx = ctx };
	struct par_arg *a = kunit_kcalloc(t, n, sizeof(*a), GFP_KERNEL);
	int i, cpu = cpumask_first(cpu_online_mask);

	KUNIT_ASSERT_NOT_NULL(t, a);
	atomic_set(&p.left, n);
	init_completion(&p.go);
	init_completion(&p.done);
	for (i = 0; i < n; i++) {
		struct task_struct *k;

		a[i].p = &p;
		a[i].i = i;
		k = kthread_create(par_thread, &a[i], "bend_kunit/%d", i);
		KUNIT_ASSERT_FALSE(t, IS_ERR(k));
		kthread_bind(k, cpu);
		cpu = cpumask_next(cpu, cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			cpu = cpumask_first(cpu_online_mask);
		wake_up_process(k);
	}
	while (atomic_read(&p.ready) < n)
		msleep(1);
	complete_all(&p.go);
	wait_for_completion(&p.done);
	return atomic_read(&p.failed);
}

static int nthreads(void)
{
	return max(2u, num_online_cpus());
}

/* ---- link/subst ---------------------------------------------------- */

static void meet_basic(struct kunit *t)
{
	atomic_long_t slot = ATOMIC_LONG_INIT(0);
	int a, b;

	KUNIT_EXPECT_NULL(t, bend_meet(&slot, &a));
	KUNIT_EXPECT_PTR_EQ(t, bend_meet(&slot, &b), (void *)&a);
	KUNIT_EXPECT_EQ(t, atomic_long_read(&slot), 0);
	/* subst: one CAS, only from the expected value */
	KUNIT_EXPECT_TRUE(t, bend_subst(&slot, NULL, &a));
	KUNIT_EXPECT_FALSE(t, bend_subst(&slot, &b, &a));
	KUNIT_EXPECT_TRUE(t, bend_subst(&slot, &a, &b));
	KUNIT_EXPECT_EQ(t, atomic_long_read(&slot), (long)&b);
}

#define MEET_SLOTS 20000

struct meet_ctx {
	atomic_long_t *slot;	/* MEET_SLOTS per pair */
	atomic_t *partner;	/* how many got the partner, per slot */
	atomic_t bad;
};

static int meet_thread(int i, void *c)
{
	struct meet_ctx *m = c;
	int pair = i / 2, j;
	void *me = (void *)(long)(i + 1), *want = (void *)(long)((i ^ 1) + 1);

	for (j = 0; j < MEET_SLOTS; j++) {
		int k = pair * MEET_SLOTS + j;
		void *got = bend_meet(&m->slot[k], me);

		if (got) {
			if (got != want)
				atomic_inc(&m->bad);
			atomic_inc(&m->partner[k]);
		}
		/* Wait for the slot to be consumed before moving on. */
		while (!got && atomic_long_read(&m->slot[k]))
			cond_resched();	/* one CPU: let the partner run */
	}
	return 0;
}

static void meet_stress(struct kunit *t)
{
	int n = nthreads() & ~1, pairs = n / 2, k;
	struct meet_ctx m = {};

	m.slot = kunit_kcalloc(t, pairs * MEET_SLOTS, sizeof(*m.slot), GFP_KERNEL);
	m.partner = kunit_kcalloc(t, pairs * MEET_SLOTS, sizeof(*m.partner), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(t, m.slot);
	KUNIT_ASSERT_NOT_NULL(t, m.partner);
	KUNIT_EXPECT_EQ(t, run_par(t, n, meet_thread, &m), 0);
	KUNIT_EXPECT_EQ(t, atomic_read(&m.bad), 0);
	for (k = 0; k < pairs * MEET_SLOTS; k++)
		if (atomic_read(&m.partner[k]) != 1) {
			KUNIT_FAIL(t, "slot %d met %d times", k, atomic_read(&m.partner[k]));
			break;
		}
}

/* ---- deque ---------------------------------------------------------- */

#define V(x) ((void *)(long)(x))

static void deque_basic(struct kunit *t)
{
	struct bend_deque *d = kunit_kzalloc(t, sizeof(*d), GFP_KERNEL);
	void *out[8];
	int i;

	KUNIT_ASSERT_NOT_NULL(t, d);
	bend_deque_init(d);
	KUNIT_EXPECT_NULL(t, bend_deque_pop(d));
	KUNIT_EXPECT_EQ(t, bend_deque_steal(d, out, 8), 0);
	for (i = 1; i <= 10; i++)
		KUNIT_EXPECT_TRUE(t, bend_deque_push(d, V(i)));
	/* owner is LIFO while far from top */
	KUNIT_EXPECT_PTR_EQ(t, bend_deque_pop(d), V(10));
	KUNIT_EXPECT_PTR_EQ(t, bend_deque_pop(d), V(9));
	/* thieves take the oldest half, capped at the batch */
	KUNIT_EXPECT_EQ(t, bend_deque_steal(d, out, 8), BEND_STEAL_BATCH);
	for (i = 0; i < BEND_STEAL_BATCH; i++)
		KUNIT_EXPECT_PTR_EQ(t, out[i], V(i + 1));
	KUNIT_EXPECT_EQ(t, bend_deque_size(d), 8 - BEND_STEAL_BATCH);
	KUNIT_EXPECT_EQ(t, bend_deque_steal(d, out, 1), 1);
	KUNIT_EXPECT_PTR_EQ(t, out[0], V(BEND_STEAL_BATCH + 1));
	/* 6,7,8 left: near top the owner takes the oldest */
	KUNIT_EXPECT_PTR_EQ(t, bend_deque_pop(d), V(6));
	KUNIT_EXPECT_PTR_EQ(t, bend_deque_pop(d), V(7));
	KUNIT_EXPECT_EQ(t, bend_deque_steal(d, out, 8), 1);	/* half of 1 rounds up */
	KUNIT_EXPECT_PTR_EQ(t, out[0], V(8));
	KUNIT_EXPECT_NULL(t, bend_deque_pop(d));
	/* full */
	for (i = 0; i < BEND_DEQUE_SIZE; i++)
		KUNIT_EXPECT_TRUE(t, bend_deque_push(d, V(i + 1)));
	KUNIT_EXPECT_FALSE(t, bend_deque_push(d, V(1)));
	KUNIT_EXPECT_EQ(t, bend_deque_size(d), BEND_DEQUE_SIZE);
}

#define DQ_ITEMS 200000

struct dq_ctx {
	struct bend_deque *d;
	unsigned long *seen;
	atomic_t taken, dup, owner_done;
};

static void dq_take(struct dq_ctx *c, void *x)
{
	if (test_and_set_bit((long)x - 1, c->seen))
		atomic_inc(&c->dup);
	atomic_inc(&c->taken);
}

static int dq_thread(int i, void *p)
{
	struct dq_ctx *c = p;
	void *out[BEND_STEAL_BATCH], *x;
	int n, k;

	if (i == 0) {		/* owner: bursts of pushes, some pops */
		long next = 1;

		while (next <= DQ_ITEMS) {
			int burst = 1 + get_random_u32_below(16);

			while (burst-- && next <= DQ_ITEMS) {
				if (!bend_deque_push(c->d, V(next)))
					break;
				next++;
			}
			for (k = get_random_u32_below(12); k; k--) {
				x = bend_deque_pop(c->d);
				if (!x)
					break;
				dq_take(c, x);
			}
		}
		while ((x = bend_deque_pop(c->d)))
			dq_take(c, x);
		atomic_set_release(&c->owner_done, 1);
		return 0;
	}
	for (;;) {		/* thieves */
		bool last = atomic_read_acquire(&c->owner_done);

		n = bend_deque_steal(c->d, out, 1 + (i % BEND_STEAL_BATCH));
		for (k = 0; k < n; k++)
			dq_take(c, out[k]);
		if (!n && last && !bend_deque_size(c->d))
			return 0;
		if (!n)
			cond_resched();
	}
}

static void deque_stress(struct kunit *t)
{
	struct dq_ctx c = {};

	c.d = kunit_kzalloc(t, sizeof(*c.d), GFP_KERNEL);
	c.seen = kunit_kcalloc(t, BITS_TO_LONGS(DQ_ITEMS), sizeof(long), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(t, c.d);
	KUNIT_ASSERT_NOT_NULL(t, c.seen);
	bend_deque_init(c.d);
	KUNIT_EXPECT_EQ(t, run_par(t, max(4, nthreads()), dq_thread, &c), 0);
	KUNIT_EXPECT_EQ(t, atomic_read(&c.dup), 0);
	KUNIT_EXPECT_EQ(t, atomic_read(&c.taken), DQ_ITEMS);
	KUNIT_EXPECT_EQ(t, (int)bitmap_weight(c.seen, DQ_ITEMS), DQ_ITEMS);
}

/* ---- arena ---------------------------------------------------------- */

static void arena_basic(struct kunit *t)
{
	struct bend_arena a;
	unsigned long *p[200];
	int i;

	bend_arena_init(&a, GFP_KERNEL);
	KUNIT_EXPECT_NULL(t, bend_arena_alloc(&a, 0));
	KUNIT_EXPECT_NULL(t, bend_arena_alloc(&a, PAGE_SIZE));
	for (i = 0; i < ARRAY_SIZE(p); i++) {
		p[i] = bend_arena_alloc(&a, 1 + i % 40);
		KUNIT_ASSERT_NOT_NULL(t, p[i]);
		KUNIT_EXPECT_EQ(t, (unsigned long)p[i] % sizeof(long), 0);
		*p[i] = i;
	}
	for (i = 0; i < ARRAY_SIZE(p); i++)	/* no overlap */
		KUNIT_EXPECT_EQ(t, *p[i], (unsigned long)i);
	bend_arena_free_all(&a);
	KUNIT_EXPECT_NULL(t, a.head);
	KUNIT_EXPECT_NOT_NULL(t, bend_arena_alloc(&a, 8));	/* reusable */
	bend_arena_free_all(&a);
}

/* ---- pool ----------------------------------------------------------- */

static void pool_basic(struct kunit *t)
{
	struct bend_pool p;
	bend_handle_t h[4], again;
	int i;

	KUNIT_ASSERT_EQ(t, bend_pool_init(&p, 4, 24, GFP_KERNEL), 0);
	for (i = 0; i < 4; i++) {
		h[i] = bend_pool_get(&p);
		KUNIT_EXPECT_NE(t, h[i], 0);
		KUNIT_EXPECT_NOT_NULL(t, bend_pool_deref(&p, h[i]));
	}
	KUNIT_EXPECT_EQ(t, bend_pool_get(&p), 0);	/* exhausted */
	bend_pool_put(&p, h[2]);
	KUNIT_EXPECT_NULL(t, bend_pool_deref(&p, h[2]));	/* stale */
	again = bend_pool_get(&p);
	KUNIT_EXPECT_EQ(t, (u32)again, (u32)h[2]);		/* index reused */
	KUNIT_EXPECT_NE(t, again, h[2]);			/* new generation */
	KUNIT_EXPECT_PTR_EQ(t, bend_pool_deref(&p, again),
			    (void *)((char *)p.objs + ((u32)h[2] - 1) * 24));
	bend_pool_put(&p, h[2]);	/* stale put: ignored */
	KUNIT_EXPECT_NOT_NULL(t, bend_pool_deref(&p, again));
	KUNIT_EXPECT_EQ(t, bend_pool_get(&p), 0);
	KUNIT_EXPECT_NULL(t, bend_pool_deref(&p, 0));
	KUNIT_EXPECT_NULL(t, bend_pool_deref(&p, 5));
	bend_pool_destroy(&p);
}

#define POOL_NR		64
#define POOL_ROUNDS	50000

struct pool_ctx {
	struct bend_pool p;
	atomic_t bad, got;
};

static int pool_thread(int i, void *c)
{
	struct pool_ctx *x = c;
	bend_handle_t mine[4];
	int r, k, n;

	for (r = 0; r < POOL_ROUNDS; r++) {
		n = 1 + (r % 4);
		for (k = 0; k < n; k++) {
			atomic_long_t *o;

			mine[k] = bend_pool_get(&x->p);
			if (!mine[k])
				break;
			atomic_inc(&x->got);
			o = bend_pool_deref(&x->p, mine[k]);
			/* exclusive: nobody else may hold this object */
			if (!o || atomic_long_cmpxchg(o, 0, i + 1) != 0)
				atomic_inc(&x->bad);
		}
		n = k;
		for (k = 0; k < n; k++) {
			atomic_long_t *o = bend_pool_deref(&x->p, mine[k]);

			if (!o || atomic_long_cmpxchg(o, i + 1, 0) != i + 1)
				atomic_inc(&x->bad);
			bend_pool_put(&x->p, mine[k]);
		}
	}
	return 0;
}

static void pool_stress(struct kunit *t)
{
	struct pool_ctx *x = kunit_kzalloc(t, sizeof(*x), GFP_KERNEL);
	bend_handle_t h;
	int n = 0;

	KUNIT_ASSERT_NOT_NULL(t, x);
	KUNIT_ASSERT_EQ(t, bend_pool_init(&x->p, POOL_NR, sizeof(long), GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(t, run_par(t, nthreads() * 2, pool_thread, x), 0);
	KUNIT_EXPECT_EQ(t, atomic_read(&x->bad), 0);
	KUNIT_EXPECT_GT(t, atomic_read(&x->got), 0);
	while ((h = bend_pool_get(&x->p)))	/* nothing leaked */
		n++;
	KUNIT_EXPECT_EQ(t, n, POOL_NR);
	bend_pool_destroy(&x->p);
}

/* ---- fork/join ------------------------------------------------------ */

struct fj_ctx {
	atomic_t *hits;
	atomic_t calls;
	cpumask_t cpus;
	spinlock_t lock;
};

static void fj_fn(unsigned long s, unsigned long e, void *arg)
{
	struct fj_ctx *c = arg;
	unsigned long i;

	for (i = s; i < e; i++)
		atomic_inc(&c->hits[i]);
	atomic_inc(&c->calls);
	spin_lock(&c->lock);
	cpumask_set_cpu(raw_smp_processor_id(), &c->cpus);
	spin_unlock(&c->lock);
	udelay(20);	/* long enough for helpers to steal */
}

static void fj_check(struct kunit *t, unsigned long len, unsigned long grain,
		     unsigned int max_workers, bool enable_it)
{
	struct fj_ctx *c = kunit_kzalloc(t, sizeof(*c), GFP_KERNEL);
	struct bend_job job = { .fn = fj_fn, .start = 0, .end = len,
				.grain = grain, .max_workers = max_workers };
	unsigned long i;
	bool was = bend_enabled();
	int used;

	KUNIT_ASSERT_NOT_NULL(t, c);
	c->hits = kunit_kcalloc(t, len, sizeof(*c->hits), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(t, c->hits);
	spin_lock_init(&c->lock);
	job.arg = c;
	bend_set_enabled(enable_it);
	used = bend_fork_join(&job);
	bend_set_enabled(was);
	for (i = 0; i < len; i++)
		if (atomic_read(&c->hits[i]) != 1) {
			KUNIT_FAIL(t, "len %lu grain %lu: unit %lu ran %d times", len, grain,
				   i, atomic_read(&c->hits[i]));
			break;
		}
	if (!enable_it || max_workers == 1)
		KUNIT_EXPECT_EQ(t, used, 1);
	KUNIT_EXPECT_LE(t, used, BEND_MAX_WORKERS);
	if (enable_it && max_workers != 1 && len / grain >= 2)	/* split */
		KUNIT_EXPECT_GE(t, atomic_read(&c->calls), 2);
}

static void fork_join_cover(struct kunit *t)
{
	fj_check(t, 1, 1, 0, true);
	fj_check(t, 7, 1, 0, true);
	fj_check(t, 1000, 3, 0, true);
	fj_check(t, 4096, 64, 0, true);
	fj_check(t, 100000, 1000, 0, true);
	fj_check(t, 4096, 64, 1, true);		/* one worker: caller only */
	fj_check(t, 4096, 64, 0, false);	/* switch off: serial */
}

static void fork_join_parallel(struct kunit *t)
{
	struct fj_ctx *c = kunit_kzalloc(t, sizeof(*c), GFP_KERNEL);
	struct bend_job job = { .fn = fj_fn, .start = 0, .end = 2048, .grain = 8 };
	int used, tries;

	if (num_online_cpus() < 2)
		kunit_skip(t, "needs SMP (UML: ncpus=4)");
	KUNIT_ASSERT_NOT_NULL(t, c);
	c->hits = kunit_kcalloc(t, 2048, sizeof(*c->hits), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(t, c->hits);
	spin_lock_init(&c->lock);
	job.arg = c;
	/* Idle CPUs exist on an otherwise quiet test VM; retry a little. */
	for (tries = 0; tries < 20; tries++) {
		memset(c->hits, 0, 2048 * sizeof(*c->hits));
		cpumask_clear(&c->cpus);
		used = bend_fork_join(&job);
		if (used > 1 && cpumask_weight(&c->cpus) > 1)
			break;
		msleep(10);
	}
	KUNIT_EXPECT_GT(t, used, 1);
	KUNIT_EXPECT_GT(t, cpumask_weight(&c->cpus), 1);
}

/* Reclaim may fork and join: no allocation that reclaims, a queue with a rescuer. */
static void fork_join_in_reclaim(struct kunit *t)
{
	struct fj_ctx *c = kunit_kzalloc(t, sizeof(*c), GFP_KERNEL);
	struct bend_job job = { .fn = fj_fn, .start = 0, .end = 2048, .grain = 8 };
	bool warned = test_taint(TAINT_WARN);
	unsigned int noreclaim;
	int i;

	KUNIT_ASSERT_NOT_NULL(t, c);
	c->hits = kunit_kcalloc(t, 2048, sizeof(*c->hits), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(t, c->hits);
	spin_lock_init(&c->lock);
	job.arg = c;
	/* Direct reclaim's lock map: lockdep reports any GFP_KERNEL under it. */
	fs_reclaim_acquire(GFP_KERNEL);
	bend_fork_join(&job);
	fs_reclaim_release(GFP_KERNEL);
	/* kswapd's flag: flushing a queue without a rescuer warns. */
	noreclaim = memalloc_noreclaim_save();
	bend_fork_join(&job);
	memalloc_noreclaim_restore(noreclaim);
	for (i = 0; i < 2048; i++)
		KUNIT_EXPECT_EQ(t, atomic_read(&c->hits[i]), 2);
	KUNIT_EXPECT_TRUE(t, debug_locks);
	KUNIT_EXPECT_EQ(t, test_taint(TAINT_WARN), warned);
}

/* Several callers at once: jobs never mix. */
static int fj_many(int i, void *p)
{
	unsigned long len = 3000 + i * 7, k;
	struct fj_ctx *c = kzalloc(sizeof(*c), GFP_KERNEL);
	struct bend_job job = { .fn = fj_fn, .start = 0, .end = len, .grain = 5 };
	int bad = 0, r;

	if (!c)
		return 1;
	c->hits = kcalloc(len, sizeof(*c->hits), GFP_KERNEL);
	if (!c->hits) {
		kfree(c);
		return 1;
	}
	spin_lock_init(&c->lock);
	job.arg = c;
	for (r = 0; r < 5; r++) {
		memset(c->hits, 0, len * sizeof(*c->hits));
		bend_fork_join(&job);
		for (k = 0; k < len; k++)
			bad |= atomic_read(&c->hits[k]) != 1;
	}
	kfree(c->hits);
	kfree(c);
	return bad;
}

static void fork_join_concurrent(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, run_par(t, nthreads(), fj_many, NULL), 0);
}

/* ---- users: huge folio clear ----------------------------------------- */

static void clear_policy(struct kunit *t)
{
	bool was = bend_set_enabled(true);
	unsigned long kb = PAGE_SIZE / SZ_1K;

	/* defaults: >= 4 MiB in 512 KiB pieces; 2 MiB THP stays serial */
	KUNIT_EXPECT_EQ(t, bend_clear_pages_grain(SZ_2M / PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(t, bend_clear_pages_grain(SZ_4M / PAGE_SIZE), 512 / kb);
	KUNIT_EXPECT_EQ(t, bend_clear_pages_grain(SZ_1G / PAGE_SIZE), 512 / kb);
	KUNIT_EXPECT_EQ(t, bend_clear_pages_grain(1), 0);
	/* fast path: one compare against bend_clear_min_pages */
	KUNIT_EXPECT_EQ(t, bend_clear_min_pages, SZ_4M / PAGE_SIZE);
	bend_set_enabled(false);
	KUNIT_EXPECT_EQ(t, bend_clear_pages_grain(SZ_1G / PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(t, bend_clear_min_pages, UINT_MAX);
	KUNIT_EXPECT_FALSE(t, bend_clear_user_highpages(NULL, 0, UINT_MAX - 1));
	bend_set_enabled(was);
	KUNIT_EXPECT_EQ(t, bend_clear_min_pages, SZ_4M / PAGE_SIZE);
}

static void clear_zeroes(struct kunit *t)
{
	const unsigned int order = min(10, MAX_PAGE_ORDER), nr = 1 << order;
	struct page *pg = alloc_pages(GFP_KERNEL | __GFP_NOWARN, order);
	bool was = bend_set_enabled(true), done;
	unsigned long *p, i;

	if (!pg)
		kunit_skip(t, "no order-%u block", order);
	p = page_address(pg);
	memset(p, 0xaa, (size_t)nr * PAGE_SIZE);
	done = bend_clear_user_highpages(pg, 0x7f0000000000UL, nr);
	if (nr * PAGE_SIZE >= SZ_4M)
		KUNIT_EXPECT_TRUE(t, done);
	if (done)
		for (i = 0; i < (size_t)nr * PAGE_SIZE / sizeof(long); i++)
			if (p[i]) {
				KUNIT_FAIL(t, "word %lu not cleared", i);
				break;
			}
	KUNIT_EXPECT_FALSE(t, bend_clear_user_highpages(pg, 0, 1));	/* small: serial */
	__free_pages(pg, order);
	bend_set_enabled(was);
}

static struct kunit_case bend_cases[] = {
	KUNIT_CASE(meet_basic),
	KUNIT_CASE_SLOW(meet_stress),
	KUNIT_CASE(deque_basic),
	KUNIT_CASE_SLOW(deque_stress),
	KUNIT_CASE(arena_basic),
	KUNIT_CASE(pool_basic),
	KUNIT_CASE_SLOW(pool_stress),
	KUNIT_CASE(fork_join_cover),
	KUNIT_CASE(fork_join_parallel),
	KUNIT_CASE(fork_join_in_reclaim),
	KUNIT_CASE_SLOW(fork_join_concurrent),
	KUNIT_CASE(clear_policy),
	KUNIT_CASE(clear_zeroes),
	{}
};

static struct kunit_suite bend_suite = {
	.name = "wolos_bend",
	.test_cases = bend_cases,
};
kunit_test_suite(bend_suite);

MODULE_DESCRIPTION("KUnit tests for wolos_bend");
MODULE_LICENSE("GPL");
