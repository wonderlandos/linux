// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * futex_waitv() contract that any fast path must keep: which error wins, which
 * index comes back, and that no wakeup is lost under wake, requeue, signal and
 * timeout races.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <linux/futex.h>

#include "kselftest_harness.h"

#ifndef SYS_futex_waitv
#define SYS_futex_waitv 449
#endif

#define F32	0x02		/* FUTEX2_SIZE_U32 */
#define FNUMA	0x04		/* FUTEX2_NUMA */
#define FPRIV	FUTEX_PRIVATE_FLAG

static int wv(struct futex_waitv *w, unsigned int n, unsigned int flags,
	      struct timespec *to, int clk)
{
	long r = syscall(SYS_futex_waitv, w, n, flags, to, clk);

	return r < 0 ? -errno : (int)r;
}

static long fwake(void *a, int n, int priv)
{
	return syscall(SYS_futex, a, FUTEX_WAKE | (priv ? FUTEX_PRIVATE_FLAG : 0), n, 0, 0, 0);
}

static void set(struct futex_waitv *w, void *a, unsigned long long v, unsigned int fl)
{
	w->uaddr = (uintptr_t)a;
	w->val = v;
	w->flags = fl;
	w->__reserved = 0;
}

static struct timespec in_ms(int ms)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_nsec += ms * 1000000L;
	t.tv_sec += t.tv_nsec / 1000000000L;
	t.tv_nsec %= 1000000000L;
	return t;
}

static uint32_t word[4] __attribute__((aligned(64)));
#define BAD_ADDR ((void *)8)

/*
 * The kernel makes every key before it compares any value, so an address that
 * cannot make a key beats a mismatch at any index. A private key needs no page:
 * an unmapped private word only fails when its turn to be read comes.
 */
struct order_case {
	const char *name;
	int first_mismatch;	/* entry 0 mismatches, entry 1 is the odd one; else reversed */
	enum { MISALIGNED, UNMAPPED, KERNEL_ADDR } odd;
	int priv;
	int expect;
};

static const struct order_case order_cases[] = {
	{ "mismatch then misaligned", 1, MISALIGNED, 1, -EINVAL },
	{ "misaligned then mismatch", 0, MISALIGNED, 1, -EINVAL },
	{ "mismatch then unmapped", 1, UNMAPPED, 1, -EAGAIN },
	{ "unmapped then mismatch", 0, UNMAPPED, 1, -EFAULT },
	{ "mismatch then unmapped shared", 1, UNMAPPED, 0, -EFAULT },
	{ "unmapped shared then mismatch", 0, UNMAPPED, 0, -EFAULT },
	{ "mismatch then kernel address", 1, KERNEL_ADDR, 1, -EFAULT },
	{ "kernel address then mismatch", 0, KERNEL_ADDR, 1, -EFAULT },
};

TEST(error_order_between_entries)
{
	unsigned int i;

	for (i = 0; i < sizeof(order_cases) / sizeof(order_cases[0]); i++) {
		const struct order_case *c = &order_cases[i];
		struct futex_waitv w[2];
		void *odd = c->odd == MISALIGNED ? (char *)&word[1] + 1 :
			    c->odd == UNMAPPED ? BAD_ADDR : (void *)0xffffffff81000000UL;
		struct futex_waitv *m = &w[c->first_mismatch ? 0 : 1];
		struct futex_waitv *o = &w[c->first_mismatch ? 1 : 0];

		word[0] = 7;
		set(m, &word[0], 8, F32 | FPRIV);
		set(o, odd, 0, F32 | (c->priv ? FPRIV : 0));
		EXPECT_EQ(c->expect, wv(w, 2, 0, NULL, 0)) TH_LOG("%s", c->name);
	}
}

/* Invalid entries win over a mismatch at any earlier index: parsing comes first. */
TEST(invalid_entry_beats_mismatch)
{
	struct futex_waitv w[3];
	int bad;

	word[0] = 7;
	for (bad = 0; bad < 5; bad++) {
		set(&w[0], &word[0], 8, F32 | FPRIV);
		set(&w[1], &word[1], 0, F32 | FPRIV);
		set(&w[2], &word[2], 0, F32 | FPRIV);
		switch (bad) {
		case 0: w[2].__reserved = 1; break;
		case 1: w[2].flags = 0x40 | F32; break;		/* unknown flag */
		case 2: w[2].flags = FPRIV; break;		/* 8-bit: not implemented */
		case 3: w[2].val = 1ULL << 32; break;		/* does not fit */
		case 4: w[2].flags = 0x03 | FPRIV; break;	/* 64-bit: not implemented */
		}
		EXPECT_EQ(-EINVAL, wv(w, 3, 0, NULL, 0)) TH_LOG("case %d", bad);
	}
}

TEST(array_fault_beats_mismatch)
{
	long pg = sysconf(_SC_PAGESIZE);
	char *m = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct futex_waitv *w;

	ASSERT_NE(MAP_FAILED, m);
	ASSERT_EQ(0, munmap(m + pg, pg));
	w = (void *)(m + pg - sizeof(*w));
	word[0] = 7;
	set(w, &word[0], 8, F32 | FPRIV);
	EXPECT_EQ(-EAGAIN, wv(w, 1, 0, NULL, 0));
	EXPECT_EQ(-EFAULT, wv(w, 2, 0, NULL, 0));
	munmap(m, pg);
}

TEST(syscall_args)
{
	struct futex_waitv w[1];
	struct timespec t = in_ms(1000);

	word[0] = 7;
	set(w, &word[0], 8, F32 | FPRIV);
	EXPECT_EQ(-EINVAL, wv(w, 1, 1, NULL, 0));
	EXPECT_EQ(-EINVAL, wv(w, 0, 0, NULL, 0));
	EXPECT_EQ(-EINVAL, wv(w, 129, 0, NULL, 0));
	EXPECT_EQ(-EINVAL, wv(NULL, 1, 0, NULL, 0));
	EXPECT_EQ(-EFAULT, wv((void *)8, 1, 0, NULL, 0));
	/* the clock only matters with a timeout */
	EXPECT_EQ(-EAGAIN, wv(w, 1, 0, NULL, 77));
	/* timeout errors beat a mismatch */
	EXPECT_EQ(-EINVAL, wv(w, 1, 0, &t, 77));
	EXPECT_EQ(-EFAULT, wv(w, 1, 0, (void *)8, CLOCK_MONOTONIC));
	t.tv_nsec = 2000000000L;
	EXPECT_EQ(-EINVAL, wv(w, 1, 0, &t, CLOCK_MONOTONIC));
	t = in_ms(1000);
	EXPECT_EQ(-EAGAIN, wv(w, 1, 0, &t, CLOCK_MONOTONIC));
	EXPECT_EQ(-EAGAIN, wv(w, 1, 0, &t, CLOCK_REALTIME));
}

TEST(timeout_when_all_match)
{
	struct futex_waitv w[3];
	struct timespec t;
	int i;

	for (i = 0; i < 3; i++) {
		word[i] = 3;
		set(&w[i], &word[i], 3, F32 | FPRIV);
	}
	t = in_ms(5);
	EXPECT_EQ(-ETIMEDOUT, wv(w, 3, 0, &t, CLOCK_MONOTONIC));
	/* already expired */
	clock_gettime(CLOCK_MONOTONIC, &t);
	EXPECT_EQ(-ETIMEDOUT, wv(w, 3, 0, &t, CLOCK_MONOTONIC));
}

/* A page not yet in this process's page tables: the scan may not call it a mismatch or a fault. */
TEST(untouched_pages)
{
	long pg = sysconf(_SC_PAGESIZE);
	struct futex_waitv w[1];
	struct timespec t;
	uint32_t v = 9;
	char name[] = "/tmp/fxwvXXXXXX";
	int fd = mkstemp(name);
	void *m;

	ASSERT_GE(fd, 0);
	unlink(name);
	ASSERT_EQ(0, ftruncate(fd, pg));
	ASSERT_EQ((ssize_t)sizeof(v), pwrite(fd, &v, sizeof(v), 0));
	m = mmap(NULL, pg, PROT_READ, MAP_SHARED, fd, 0);
	ASSERT_NE(MAP_FAILED, m);
	set(w, m, 8, F32);
	EXPECT_EQ(-EAGAIN, wv(w, 1, 0, NULL, 0));
	munmap(m, pg);
	m = mmap(NULL, pg, PROT_READ, MAP_SHARED, fd, 0);
	ASSERT_NE(MAP_FAILED, m);
	set(w, m, 9, F32);
	t = in_ms(2);
	EXPECT_EQ(-ETIMEDOUT, wv(w, 1, 0, &t, CLOCK_MONOTONIC));
	munmap(m, pg);
	m = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, m);
	set(w, m, 0, F32 | FPRIV);
	t = in_ms(2);
	EXPECT_EQ(-ETIMEDOUT, wv(w, 1, 0, &t, CLOCK_MONOTONIC));
	set(w, m, 1, F32 | FPRIV);
	EXPECT_EQ(-EAGAIN, wv(w, 1, 0, NULL, 0));
	munmap(m, pg);
	close(fd);
}

/* The same address, remapped to another object between two waits, is another futex. */
static uint32_t *remap_addr;
static atomic_int remap_phase;

static void *remap_waiter(void *p)
{
	struct futex_waitv w[1];
	struct timespec t;
	int *ret = p;

	set(w, remap_addr, 0, F32);
	t = in_ms(1);
	wv(w, 1, 0, &t, CLOCK_MONOTONIC);	/* key of the first object */
	atomic_store(&remap_phase, 1);
	while (atomic_load(&remap_phase) != 2)
		usleep(50);
	t = in_ms(3000);
	*ret = wv(w, 1, 0, &t, CLOCK_MONOTONIC);
	return NULL;
}

TEST(remap_between_waits)
{
	long pg = sysconf(_SC_PAGESIZE);
	int a = memfd_create("fxa", 0), b = memfd_create("fxb", 0), ret = 1, tries = 0;
	uint32_t *other;
	pthread_t th;

	ASSERT_EQ(0, ftruncate(a, pg));
	ASSERT_EQ(0, ftruncate(b, pg));
	remap_addr = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, a, 0);
	other = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, b, 0);
	ASSERT_NE(MAP_FAILED, remap_addr);
	ASSERT_NE(MAP_FAILED, other);
	ASSERT_EQ(0, pthread_create(&th, NULL, remap_waiter, &ret));
	while (atomic_load(&remap_phase) != 1)
		usleep(50);
	ASSERT_EQ(remap_addr, mmap(remap_addr, pg, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_FIXED, b, 0));
	atomic_store(&remap_phase, 2);
	/* no value change: only a queue on the right key can see this wake */
	while (fwake(other, 1, 0) != 1 && tries++ < 40000)
		usleep(100);
	pthread_join(th, NULL);
	EXPECT_EQ(0, ret);
}

/* --- the woken index --- */
#define MAXV 128
static _Atomic uint32_t many[MAXV * 16];
struct idx_arg { int n, ret, priv; atomic_int ready; };

static void *idx_waiter(void *p)
{
	struct idx_arg *a = p;
	struct futex_waitv w[MAXV];
	struct timespec t;
	int i;

	for (i = 0; i < a->n; i++)
		set(&w[i], &many[i * 16], 0, F32 | (a->priv ? FPRIV : 0));
	t = in_ms(5000);
	atomic_store(&a->ready, 1);
	a->ret = wv(w, a->n, 0, &t, CLOCK_MONOTONIC);
	return NULL;
}

TEST(woken_index_every_size)
{
	static const int sizes[] = { 1, 2, 4, 5, 8, 63, 64, 65, 127, 128 };
	unsigned int s;

	for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		int n = sizes[s], k;

		for (k = 0; k < n; k = k < 3 ? k + 1 : k * 2 + 1) {
			struct idx_arg a = { .n = n, .priv = k & 1 };
			pthread_t t;
			int tries = 0;

			memset(many, 0, sizeof(many));
			ASSERT_EQ(0, pthread_create(&t, NULL, idx_waiter, &a));
			while (!atomic_load(&a.ready))
				usleep(50);
			while (fwake(&many[k * 16], 1, a.priv) != 1 && tries++ < 20000)
				usleep(100);
			pthread_join(t, NULL);
			EXPECT_EQ(k, a.ret) TH_LOG("n=%d k=%d", n, k);
		}
	}
}

/* Two entries on one word: a single wake frees the waiter, the other entry must leave the queue. */
TEST(duplicate_addresses)
{
	static _Atomic uint32_t w0;
	struct futex_waitv w[4];
	struct timespec t;
	int i;

	for (i = 0; i < 4; i++)
		set(&w[i], &w0, 1, F32 | FPRIV);
	t = in_ms(1);
	atomic_store(&w0, 1);
	EXPECT_EQ(-ETIMEDOUT, wv(w, 4, 0, &t, CLOCK_MONOTONIC));
	EXPECT_EQ(0, fwake(&w0, 100, 1));	/* nothing left behind */
}

/* --- races --- */
#define TOK_THREADS 8
#define TOK_WORDS 16
#define TOK_LAPS 60000
struct tok {
	_Atomic uint32_t w[TOK_WORDS * 16];
	_Atomic long left;
	int priv;
};

/* Tokens: a thread takes a unit from any word, gives one to the next word. No lost wake or it hangs. */
static void *tok_fn(void *p)
{
	struct tok *t = p;
	unsigned int seed = (unsigned int)(uintptr_t)&seed;

	while (atomic_load(&t->left) > 0) {
		struct futex_waitv w[TOK_WORDS];
		struct timespec to;
		int i, got = -1;

		for (i = 0; i < TOK_WORDS; i++) {
			uint32_t v = atomic_load(&t->w[i * 16]);

			if (v && atomic_compare_exchange_strong(&t->w[i * 16], &v, v - 1)) {
				got = i;
				break;
			}
			set(&w[i], &t->w[i * 16], v, F32 | (t->priv ? FPRIV : 0));
		}
		if (got < 0) {
			int r;

			to = in_ms(2000);
			r = wv(w, TOK_WORDS, 0, &to, CLOCK_MONOTONIC);
			if (r == -ETIMEDOUT) {
				fprintf(stderr, "lost wakeup\n");
				abort();
			}
			continue;
		}
		atomic_fetch_sub(&t->left, 1);
		got = rand_r(&seed) % TOK_WORDS;
		atomic_fetch_add(&t->w[got * 16], 1);
		fwake(&t->w[got * 16], 1, t->priv);
	}
	for (int i = 0; i < TOK_WORDS; i++) {
		atomic_fetch_add(&t->w[i * 16], TOK_THREADS);
		fwake(&t->w[i * 16], TOK_THREADS, t->priv);
	}
	return NULL;
}

static void token_race(struct __test_metadata *_metadata, int priv)
{
	int fd = memfd_create("fxwv", 0);
	struct tok *t;
	pthread_t th[TOK_THREADS];
	int i;

	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, ftruncate(fd, sizeof(*t)));
	t = mmap(NULL, sizeof(*t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ASSERT_NE(MAP_FAILED, t);
	t->priv = priv;
	t->left = TOK_LAPS;
	atomic_store(&t->w[0], 3);
	for (i = 0; i < TOK_THREADS; i++)
		ASSERT_EQ(0, pthread_create(&th[i], NULL, tok_fn, t));
	for (i = 0; i < TOK_THREADS; i++)
		pthread_join(th[i], NULL);
	EXPECT_LE(atomic_load(&t->left), 0);
	munmap(t, sizeof(*t));
	close(fd);
}

TEST(token_race_private) { token_race(_metadata, 1); }
TEST(token_race_shared) { token_race(_metadata, 0); }

/* Waiters on {A, B}: requeue A to C, wake C; the index reported is still A's entry. */
static _Atomic uint32_t ra, rb, rc;
static atomic_int rq_stop, rq_inflight;

static void *rq_waiter(void *p)
{
	long *wrong = p;

	while (!atomic_load(&rq_stop)) {
		struct futex_waitv w[2];
		struct timespec t = in_ms(2000);
		int r;

		set(&w[0], &rb, 0, F32 | FPRIV);
		set(&w[1], &ra, 0, F32 | FPRIV);
		atomic_fetch_add(&rq_inflight, 1);
		r = wv(w, 2, 0, &t, CLOCK_MONOTONIC);
		atomic_fetch_sub(&rq_inflight, 1);
		if (r == -ETIMEDOUT) {
			fprintf(stderr, "lost wakeup across requeue\n");
			abort();
		}
		if (r != 1 && r != -EAGAIN)
			(*wrong)++;
	}
	return NULL;
}

TEST(requeue_race)
{
	pthread_t th[8];
	long wrong[8] = {};
	double end;
	struct timespec n;
	int i;

	atomic_store(&rq_stop, 0);
	for (i = 0; i < 8; i++)
		ASSERT_EQ(0, pthread_create(&th[i], NULL, rq_waiter, &wrong[i]));
	clock_gettime(CLOCK_MONOTONIC, &n);
	end = n.tv_sec + n.tv_nsec / 1e9 + 2;
	for (;;) {
		long moved = syscall(SYS_futex, &ra, FUTEX_CMP_REQUEUE_PRIVATE, 1, 4, &rc,
				     atomic_load(&ra));

		(void)moved;
		fwake(&rc, 8, 1);
		fwake(&ra, 2, 1);
		clock_gettime(CLOCK_MONOTONIC, &n);
		if (n.tv_sec + n.tv_nsec / 1e9 > end)
			break;
	}
	atomic_store(&rq_stop, 1);
	atomic_fetch_add(&ra, 1);
	while (atomic_load(&rq_inflight)) {
		atomic_fetch_add(&ra, 1);
		fwake(&ra, 8, 1);
		fwake(&rc, 8, 1);
		usleep(100);
	}
	for (i = 0; i < 8; i++) {
		pthread_join(th[i], NULL);
		EXPECT_EQ(0, wrong[i]);
	}
	/* nobody left queued */
	EXPECT_EQ(0, fwake(&ra, 100, 1));
	EXPECT_EQ(0, fwake(&rb, 100, 1));
	EXPECT_EQ(0, fwake(&rc, 100, 1));
}

/* Signals and very short timeouts against wakes: no stray entry, EINTR/ETIMEDOUT/index only. */
static void on_usr1(int s) { (void)s; }
static _Atomic uint32_t sa[8 * 16];
static atomic_int sg_stop;

static void *sg_waiter(void *p)
{
	long *bad = p;

	while (!atomic_load(&sg_stop)) {
		struct futex_waitv w[8];
		struct timespec t = in_ms(1);
		int i, r;

		for (i = 0; i < 8; i++)
			set(&w[i], &sa[i * 16], 0, F32 | FPRIV);
		r = wv(w, 8, 0, &t, CLOCK_MONOTONIC);
		if (!(r >= 0 && r < 8) && r != -ETIMEDOUT && r != -EINTR &&
		    r != -ERESTART && r != -EAGAIN)
			(*bad)++;
	}
	return NULL;
}

TEST(signal_timeout_wake_race)
{
	pthread_t th[4];
	long bad[4] = {};
	struct sigaction act = { .sa_handler = on_usr1 };
	int i, loop;

	sigaction(SIGUSR1, &act, NULL);
	for (i = 0; i < 4; i++)
		ASSERT_EQ(0, pthread_create(&th[i], NULL, sg_waiter, &bad[i]));
	for (loop = 0; loop < 200000; loop++) {
		fwake(&sa[(loop & 7) * 16], 2, 1);
		if (!(loop & 63))
			pthread_kill(th[(loop >> 6) & 3], SIGUSR1);
	}
	atomic_store(&sg_stop, 1);
	for (i = 0; i < 4; i++) {
		pthread_join(th[i], NULL);
		EXPECT_EQ(0, bad[i]);
	}
	for (i = 0; i < 8; i++)
		EXPECT_EQ(0, fwake(&sa[i * 16], 100, 1));
}

/* Thread exit with a wake chasing it, and many sleepers woken at once. */
#define HERD 64
static _Atomic uint32_t herd;
static atomic_int herd_done;

static void *herd_fn(void *p)
{
	struct futex_waitv w[2];
	uint32_t other = 0;
	struct timespec t = in_ms(5000);
	int r;

	(void)p;
	set(&w[0], &other, 0, F32 | FPRIV);
	set(&w[1], &herd, 0, F32 | FPRIV);
	r = wv(w, 2, 0, &t, CLOCK_MONOTONIC);
	if (r == 1 || r == -EAGAIN)
		atomic_fetch_add(&herd_done, 1);
	return NULL;
}

TEST(wake_all_herd)
{
	int round;

	for (round = 0; round < 40; round++) {
		pthread_t th[HERD];
		int i, woken = 0;
		long r;

		atomic_store(&herd, 0);
		atomic_store(&herd_done, 0);
		for (i = 0; i < HERD; i++)
			ASSERT_EQ(0, pthread_create(&th[i], NULL, herd_fn, NULL));
		usleep(round & 1 ? 2000 : 0);
		atomic_store(&herd, 1);
		while (atomic_load(&herd_done) + woken < HERD) {
			r = fwake(&herd, HERD, 1);
			if (r < 0)
				break;
			woken += 0;
			usleep(10);
		}
		for (i = 0; i < HERD; i++)
			pthread_join(th[i], NULL);
		EXPECT_EQ(HERD, atomic_load(&herd_done));
	}
}

TEST_HARNESS_MAIN
