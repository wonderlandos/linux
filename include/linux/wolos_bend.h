/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wolos_bend: small parallel primitives (lib/wolos_bend/).
 *
 *  - bend_meet():  two parties meet at a slot with one compare-and-swap; the
 *                  second one gets the first one's value (link/subst).
 *  - bend_deque:   Chase-Lev work-stealing deque; thieves take up to
 *                  BEND_STEAL_BATCH entries with one CAS.
 *  - bend_arena:   bump allocator owned by one worker, freed in one call.
 *  - bend_pool:    fixed array of objects reused by index; handles carry a
 *                  generation so a stale handle is detected, not dereferenced.
 *  - bend_fork_join(): split [start, end) until @grain, run the pieces on the
 *                  caller and on idle CPUs (same LLC first), return when all
 *                  are done.  Serial when disabled (wolos_bend.enable=0).
 *
 * No threads of its own: helpers are work items on a per-CPU workqueue.
 */
#ifndef _LINUX_WOLOS_BEND_H
#define _LINUX_WOLOS_BEND_H

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/types.h>

/* ---- link/subst ---------------------------------------------------- */

/*
 * bend_meet - store @val in an empty @slot, or take the value already there.
 * Returns NULL when @val was stored (the caller arrived first), else the
 * partner's value; the slot is then empty again.  @val must not be NULL.
 * Exactly one of two concurrent callers gets the other's value.
 */
static inline void *bend_meet(atomic_long_t *slot, void *val)
{
	long old = atomic_long_cmpxchg(slot, 0, (long)val);

	if (!old)
		return NULL;
	/* Only the second party clears; a third party would be a user bug. */
	atomic_long_set_release(slot, 0);
	return (void *)old;
}

/* bend_subst - replace @expect by @val with one CAS; true when it happened. */
static inline bool bend_subst(atomic_long_t *slot, void *expect, void *val)
{
	long e = (long)expect;

	return atomic_long_try_cmpxchg(slot, &e, (long)val);
}

/* ---- work-stealing deque ------------------------------------------- */

#define BEND_DEQUE_SIZE		256	/* power of two */
#define BEND_STEAL_BATCH	4

struct bend_deque {
	atomic_long_t top ____cacheline_aligned_in_smp;	/* thieves */
	long bottom ____cacheline_aligned_in_smp;	/* owner */
	void *slot[BEND_DEQUE_SIZE];
};

void bend_deque_init(struct bend_deque *d);
/* Owner only.  False when full (the caller runs the item itself). */
bool bend_deque_push(struct bend_deque *d, void *item);
/* Owner only.  NULL when empty. */
void *bend_deque_pop(struct bend_deque *d);
/* Any CPU.  Takes up to min(@max, BEND_STEAL_BATCH) items into @out; count. */
int bend_deque_steal(struct bend_deque *d, void **out, int max);
long bend_deque_size(struct bend_deque *d);

/* ---- arena ---------------------------------------------------------- */

struct bend_arena_chunk;

struct bend_arena {
	struct bend_arena_chunk *head;
	size_t used;		/* bytes used in head */
	gfp_t gfp;
};

void bend_arena_init(struct bend_arena *a, gfp_t gfp);
/* Owner only.  8-byte aligned, at most PAGE_SIZE - header bytes; NULL on OOM. */
void *bend_arena_alloc(struct bend_arena *a, size_t size);
/* Frees every allocation at once. */
void bend_arena_free_all(struct bend_arena *a);

/* ---- index-reuse pool ---------------------------------------------- */

typedef u64 bend_handle_t;	/* generation << 32 | index + 1; 0 = none */

struct bend_pool {
	atomic64_t free;	/* free-list head: tag << 32 | index + 1 */
	u32 nr;
	size_t obj_size;
	u32 *next;		/* free-list links */
	atomic_t *gen;		/* generation per index, bumped on put */
	void *objs;
};

int bend_pool_init(struct bend_pool *p, u32 nr, size_t obj_size, gfp_t gfp);
void bend_pool_destroy(struct bend_pool *p);
/* Lock-free.  0 when exhausted. */
bend_handle_t bend_pool_get(struct bend_pool *p);
void bend_pool_put(struct bend_pool *p, bend_handle_t h);
/* The object, or NULL when @h is stale (put since) or invalid. */
void *bend_pool_deref(struct bend_pool *p, bend_handle_t h);

/* ---- fork/join ------------------------------------------------------ */

#define BEND_MAX_WORKERS	16

struct bend_job {
	void (*fn)(unsigned long start, unsigned long end, void *arg);
	void *arg;
	unsigned long start, end;
	unsigned long grain;	/* do not split below this many units */
	unsigned int max_workers;	/* caller included; 0 = all allowed */
};

/*
 * Runs fn over [start, end) in pieces of at least @grain, possibly
 * concurrently; returns when every piece is done.  Sleepable context only.
 * Helpers run only on idle CPUs.  Returns the number of workers used.
 */
int bend_fork_join(const struct bend_job *job);

bool bend_enabled(void);
/* Returns the previous state (tests, users with their own switch). */
bool bend_set_enabled(bool on);

/* ---- users ---------------------------------------------------------- */

struct page;
#ifdef CONFIG_WOLOS_BEND
/* Piece size for a parallel clear of @nr pages; 0 = clear serially. */
unsigned long bend_clear_pages_grain(unsigned long nr);
/* Clears @nr pages on idle CPUs and returns true, or returns false. */
bool bend_clear_user_highpages(struct page *page, unsigned long addr, unsigned int nr);
#else
static inline bool bend_clear_user_highpages(struct page *page, unsigned long addr,
					     unsigned int nr)
{
	return false;
}
#endif

#endif /* _LINUX_WOLOS_BEND_H */
