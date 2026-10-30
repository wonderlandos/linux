// SPDX-License-Identifier: GPL-2.0
/*
 * wolos_bend: work-stealing deque, arena, index pool and fork/join.
 * See include/linux/wolos_bend.h.
 */
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/minmax.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/wolos_bend.h>
#include <linux/workqueue.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "wolos_bend."

static bool enable = true;
module_param(enable, bool, 0644);
MODULE_PARM_DESC(enable, "Run fork/join pieces on idle CPUs (0: serial)");

bool bend_enabled(void)
{
	return READ_ONCE(enable);
}
EXPORT_SYMBOL_GPL(bend_enabled);

bool bend_set_enabled(bool on)
{
	return xchg(&enable, on);
}
EXPORT_SYMBOL_GPL(bend_set_enabled);

/* ---- deque ----------------------------------------------------------
 *
 * Chase-Lev (Le et al., PPoPP 2013 orderings) with one change for batched
 * steals: a thief claims [top, top + n) with one CAS, n <= BEND_STEAL_BATCH.
 * The owner pops from the bottom without a CAS only while at least
 * BEND_STEAL_BATCH entries separate it from top, so no claim can reach the
 * entry it takes; closer to top it competes with thieves through the same
 * CAS on top (it takes the oldest entry, which is fine for fork/join).
 */
void bend_deque_init(struct bend_deque *d)
{
	atomic_long_set(&d->top, 0);
	d->bottom = 0;
}
EXPORT_SYMBOL_GPL(bend_deque_init);

bool bend_deque_push(struct bend_deque *d, void *item)
{
	long b = d->bottom;
	long t = atomic_long_read_acquire(&d->top);

	if (b - t >= BEND_DEQUE_SIZE)
		return false;
	WRITE_ONCE(d->slot[b & (BEND_DEQUE_SIZE - 1)], item);
	smp_store_release(&d->bottom, b + 1);
	return true;
}
EXPORT_SYMBOL_GPL(bend_deque_push);

void *bend_deque_pop(struct bend_deque *d)
{
	long b = d->bottom - 1;
	long t;
	void *x;

	WRITE_ONCE(d->bottom, b);
	smp_mb();	/* bottom store before top load; pairs with steal */
	t = atomic_long_read(&d->top);
	if (t > b) {			/* empty */
		WRITE_ONCE(d->bottom, b + 1);
		return NULL;
	}
	if (b - t >= BEND_STEAL_BATCH)	/* out of reach of any claim */
		return READ_ONCE(d->slot[b & (BEND_DEQUE_SIZE - 1)]);

	/* Near top: put the entry back and take the oldest one by CAS. */
	WRITE_ONCE(d->bottom, b + 1);
	for (;;) {
		t = atomic_long_read_acquire(&d->top);
		if (t > b)
			return NULL;
		x = READ_ONCE(d->slot[t & (BEND_DEQUE_SIZE - 1)]);
		if (atomic_long_try_cmpxchg(&d->top, &t, t + 1))
			return x;
		cpu_relax();
	}
}
EXPORT_SYMBOL_GPL(bend_deque_pop);

int bend_deque_steal(struct bend_deque *d, void **out, int max)
{
	long t, b, n, i;

	max = min(max, BEND_STEAL_BATCH);
	if (max <= 0)
		return 0;
	t = atomic_long_read_acquire(&d->top);
	smp_mb();	/* top load before bottom load; pairs with pop */
	b = smp_load_acquire(&d->bottom);
	n = b - t;
	if (n <= 0)
		return 0;
	n = min_t(long, (n + 1) / 2, max);	/* half, rounded up */
	for (i = 0; i < n; i++)
		out[i] = READ_ONCE(d->slot[(t + i) & (BEND_DEQUE_SIZE - 1)]);
	if (!atomic_long_try_cmpxchg(&d->top, &t, t + n))
		return 0;	/* lost the race; slots may have been reused */
	return n;
}
EXPORT_SYMBOL_GPL(bend_deque_steal);

long bend_deque_size(struct bend_deque *d)
{
	long b = READ_ONCE(d->bottom);
	long t = atomic_long_read(&d->top);

	return b > t ? b - t : 0;
}
EXPORT_SYMBOL_GPL(bend_deque_size);

/* ---- arena ---------------------------------------------------------- */

struct bend_arena_chunk {
	struct bend_arena_chunk *next;
	unsigned long data[];
};

#define BEND_CHUNK_DATA	(PAGE_SIZE - sizeof(struct bend_arena_chunk))

void bend_arena_init(struct bend_arena *a, gfp_t gfp)
{
	a->head = NULL;
	a->used = BEND_CHUNK_DATA;	/* first alloc takes a chunk */
	a->gfp = gfp;
}
EXPORT_SYMBOL_GPL(bend_arena_init);

void *bend_arena_alloc(struct bend_arena *a, size_t size)
{
	struct bend_arena_chunk *c;
	void *p;

	size = ALIGN(size, sizeof(unsigned long));
	if (!size || size > BEND_CHUNK_DATA)
		return NULL;
	if (a->used + size > BEND_CHUNK_DATA) {
		c = kmalloc(PAGE_SIZE, a->gfp);
		if (!c)
			return NULL;
		c->next = a->head;
		a->head = c;
		a->used = 0;
	}
	p = (char *)a->head->data + a->used;
	a->used += size;
	return p;
}
EXPORT_SYMBOL_GPL(bend_arena_alloc);

void bend_arena_free_all(struct bend_arena *a)
{
	struct bend_arena_chunk *c = a->head, *n;

	for (; c; c = n) {
		n = c->next;
		kfree(c);
	}
	a->head = NULL;
	a->used = BEND_CHUNK_DATA;
}
EXPORT_SYMBOL_GPL(bend_arena_free_all);

/* ---- index pool -----------------------------------------------------
 *
 * Free list is a Treiber stack of indexes; the head carries a tag bumped on
 * every pop so a concurrent pop/push/pop of the same index (ABA) fails the CAS.
 */
#define POOL_NONE	0u

int bend_pool_init(struct bend_pool *p, u32 nr, size_t obj_size, gfp_t gfp)
{
	u32 i;

	if (!nr || nr >= U32_MAX)
		return -EINVAL;
	p->nr = nr;
	p->obj_size = ALIGN(obj_size, sizeof(unsigned long));
	p->next = kvcalloc(nr, sizeof(*p->next), gfp);
	p->gen = kvcalloc(nr, sizeof(*p->gen), gfp);
	p->objs = kvcalloc(nr, p->obj_size, gfp);
	if (!p->next || !p->gen || !p->objs) {
		bend_pool_destroy(p);
		return -ENOMEM;
	}
	for (i = 0; i < nr; i++)
		p->next[i] = i + 2 <= nr ? i + 2 : POOL_NONE;	/* index + 1 */
	atomic64_set(&p->free, 1);
	return 0;
}
EXPORT_SYMBOL_GPL(bend_pool_init);

void bend_pool_destroy(struct bend_pool *p)
{
	kvfree(p->next);
	kvfree(p->gen);
	kvfree(p->objs);
	p->next = NULL;
	p->gen = NULL;
	p->objs = NULL;
}
EXPORT_SYMBOL_GPL(bend_pool_destroy);

bend_handle_t bend_pool_get(struct bend_pool *p)
{
	s64 head = atomic64_read_acquire(&p->free);
	u32 slot, next;
	s64 new;

	do {
		slot = (u32)head;
		if (slot == POOL_NONE)
			return 0;
		next = READ_ONCE(p->next[slot - 1]);
		new = (s64)((((u64)head >> 32) + 1) << 32 | next);
	} while (!atomic64_try_cmpxchg(&p->free, &head, new));
	return (u64)(u32)atomic_read(&p->gen[slot - 1]) << 32 | slot;
}
EXPORT_SYMBOL_GPL(bend_pool_get);

void bend_pool_put(struct bend_pool *p, bend_handle_t h)
{
	u32 slot = (u32)h;
	s64 head, new;

	if (!slot || slot > p->nr)
		return;
	/* Stale handles stop matching before the index can be handed out again. */
	if (atomic_cmpxchg(&p->gen[slot - 1], (u32)(h >> 32), (u32)(h >> 32) + 1) !=
	    (int)(u32)(h >> 32))
		return;		/* double put */
	head = atomic64_read(&p->free);
	do {
		WRITE_ONCE(p->next[slot - 1], (u32)head);
		new = (s64)(((u64)head >> 32) << 32 | slot);
	} while (!atomic64_try_cmpxchg_release(&p->free, &head, new));
}
EXPORT_SYMBOL_GPL(bend_pool_put);

void *bend_pool_deref(struct bend_pool *p, bend_handle_t h)
{
	u32 slot = (u32)h;

	if (!slot || slot > p->nr)
		return NULL;
	if ((u32)atomic_read(&p->gen[slot - 1]) != (u32)(h >> 32))
		return NULL;
	return (char *)p->objs + (size_t)(slot - 1) * p->obj_size;
}
EXPORT_SYMBOL_GPL(bend_pool_deref);

/* ---- fork/join ------------------------------------------------------ */

struct bend_range {
	unsigned long start, end;
};

struct bend_run;

struct bend_worker {
	struct bend_deque dq;
	struct bend_arena arena;
	struct work_struct work;
	struct bend_run *run;
	int cpu;
	bool queued;
};

struct bend_run {
	const struct bend_job *job;
	atomic_long_t remaining;	/* units not done yet */
	struct completion done;
	int nr;
	struct bend_worker *w[BEND_MAX_WORKERS];
};

static struct workqueue_struct *bend_wq;

static void bend_account(struct bend_run *r, unsigned long n)
{
	if (atomic_long_sub_return(n, &r->remaining) == 0)
		complete(&r->done);
}

/* Fork until grain: keep the left half, publish the right ones. */
static void bend_exec(struct bend_worker *w, unsigned long s, unsigned long e)
{
	const struct bend_job *job = w->run->job;

	while (e - s > job->grain && e - s >= 2) {
		unsigned long mid = s + (e - s) / 2;
		struct bend_range *rg = bend_arena_alloc(&w->arena, sizeof(*rg));

		if (!rg)
			break;	/* no memory: run the rest here */
		rg->start = mid;
		rg->end = e;
		if (!bend_deque_push(&w->dq, rg))
			break;	/* no room: run the rest here */
		e = mid;
	}
	job->fn(s, e, job->arg);
	bend_account(w->run, e - s);
	cond_resched();
}

/* One steal sweep, same-LLC workers first.  Runs what it got; true if any. */
static bool bend_steal_sweep(struct bend_worker *w)
{
	struct bend_run *r = w->run;
	void *got[BEND_STEAL_BATCH];
	int pass, i, k, n;

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < r->nr; i++) {
			struct bend_worker *v = r->w[i];

			if (v == w || !READ_ONCE(v->queued))
				continue;
			if (cpus_share_cache(w->cpu, v->cpu) != (pass == 0))
				continue;
			n = bend_deque_steal(&v->dq, got, BEND_STEAL_BATCH);
			if (!n)
				continue;
			/* Keep the extras stealable from here, run the first. */
			for (k = n - 1; k > 0; k--) {
				if (!bend_deque_push(&w->dq, got[k])) {
					struct bend_range *rg = got[k];

					bend_exec(w, rg->start, rg->end);
				}
			}
			bend_exec(w, ((struct bend_range *)got[0])->start,
				  ((struct bend_range *)got[0])->end);
			return true;
		}
	}
	return false;
}

static void bend_drain(struct bend_worker *w)
{
	struct bend_range *rg;

	for (;;) {
		while ((rg = bend_deque_pop(&w->dq)))
			bend_exec(w, rg->start, rg->end);
		if (atomic_long_read(&w->run->remaining) <= 0 || !bend_steal_sweep(w))
			return;
	}
}

static void bend_helper(struct work_struct *work)
{
	struct bend_worker *w = container_of(work, struct bend_worker, work);

	w->cpu = raw_smp_processor_id();
	bend_drain(w);
}

/* Idle CPUs for helpers, same LLC as @this first. */
static int bend_pick_cpus(int this, int want, int *cpus)
{
	int pass, cpu, n = 0;

	for (pass = 0; pass < 2 && n < want; pass++) {
		for_each_online_cpu(cpu) {
			if (n >= want)
				break;
			if (cpu == this || !idle_cpu(cpu))
				continue;
			if (cpus_share_cache(this, cpu) != (pass == 0))
				continue;
			cpus[n++] = cpu;
		}
	}
	return n;
}

int bend_fork_join(const struct bend_job *job)
{
	unsigned long units = job->end - job->start;
	unsigned long grain = max(job->grain, 1UL);
	int cpus[BEND_MAX_WORKERS];
	struct bend_run *r;
	int want, i, n, this;

	might_sleep();
	if (job->end <= job->start)
		return 0;
	want = min_t(unsigned long, BEND_MAX_WORKERS, units / grain);
	if (job->max_workers)
		want = min_t(int, want, job->max_workers);
	if (!bend_enabled() || !bend_wq || want < 2)
		goto serial;
	r = kzalloc(sizeof(*r), GFP_KERNEL | __GFP_NOWARN);
	if (!r)
		goto serial;

	this = raw_smp_processor_id();
	n = bend_pick_cpus(this, want - 1, cpus);
	r->job = job;
	atomic_long_set(&r->remaining, units);
	init_completion(&r->done);
	for (i = 0; i <= n; i++) {
		struct bend_worker *w = kmalloc(sizeof(*w), GFP_KERNEL | __GFP_NOWARN);

		if (!w)
			break;
		bend_deque_init(&w->dq);
		bend_arena_init(&w->arena, GFP_KERNEL | __GFP_NOWARN);
		INIT_WORK(&w->work, bend_helper);
		w->run = r;
		w->cpu = i ? cpus[i - 1] : this;
		w->queued = false;
		r->w[r->nr++] = w;
	}
	if (!r->nr) {
		kfree(r);
		goto serial;
	}
	/* Caller is worker 0; helpers start stealing from it. */
	WRITE_ONCE(r->w[0]->queued, true);
	for (i = 1; i < r->nr; i++) {
		WRITE_ONCE(r->w[i]->queued, true);
		queue_work_on(r->w[i]->cpu, bend_wq, &r->w[i]->work);
	}
	bend_exec(r->w[0], job->start, job->end);
	bend_drain(r->w[0]);
	wait_for_completion(&r->done);
	/* Every unit is done; helpers may still be returning. */
	for (i = 1; i < r->nr; i++)
		flush_work(&r->w[i]->work);
	n = r->nr;
	for (i = 0; i < r->nr; i++) {
		bend_arena_free_all(&r->w[i]->arena);
		kfree(r->w[i]);
	}
	kfree(r);
	return n;

serial:
	job->fn(job->start, job->end, job->arg);
	return 1;
}
EXPORT_SYMBOL_GPL(bend_fork_join);

static int __init bend_init(void)
{
	bend_wq = alloc_workqueue("wolos_bend", WQ_PERCPU, 0);
	return bend_wq ? 0 : -ENOMEM;
}
core_initcall(bend_init);
