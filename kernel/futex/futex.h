/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FUTEX_H
#define _FUTEX_H

#include <linux/futex.h>
#include <linux/rtmutex.h>
#include <linux/sched/wake_q.h>
#include <linux/compat.h>
#include <linux/uaccess.h>
#include <linux/cleanup.h>

#ifdef CONFIG_PREEMPT_RT
#include <linux/rcuwait.h>
#endif

#include <asm/futex.h>

/*
 * Futex flags used to encode options to functions and preserve them across
 * restarts.
 */
#define FLAGS_SIZE_8		0x0000
#define FLAGS_SIZE_16		0x0001
#define FLAGS_SIZE_32		0x0002
#define FLAGS_SIZE_64		0x0003

#define FLAGS_SIZE_MASK		0x0003

#ifdef CONFIG_MMU
# define FLAGS_SHARED		0x0010
#else
/*
 * NOMMU does not have per process address space. Let the compiler optimize
 * code away.
 */
# define FLAGS_SHARED		0x0000
#endif
#define FLAGS_CLOCKRT		0x0020
#define FLAGS_HAS_TIMEOUT	0x0040
#define FLAGS_NUMA		0x0080
#define FLAGS_STRICT		0x0100
#define FLAGS_MPOL		0x0200
#define FLAGS_ROBUST_UNLOCK	0x0400
#define FLAGS_ROBUST_LIST32	0x0800

/* FUTEX_ to FLAGS_ */
static inline unsigned int futex_to_flags(unsigned int op)
{
	unsigned int flags = FLAGS_SIZE_32;

	if (!(op & FUTEX_PRIVATE_FLAG))
		flags |= FLAGS_SHARED;

	if (op & FUTEX_CLOCK_REALTIME)
		flags |= FLAGS_CLOCKRT;

	if (op & FUTEX_ROBUST_UNLOCK)
		flags |= FLAGS_ROBUST_UNLOCK;

	if (op & FUTEX_ROBUST_LIST32)
		flags |= FLAGS_ROBUST_LIST32;

	return flags;
}

#define FUTEX2_VALID_MASK (FUTEX2_SIZE_MASK | FUTEX2_NUMA | FUTEX2_MPOL | FUTEX2_PRIVATE)

/* FUTEX2_ to FLAGS_ */
static inline unsigned int futex2_to_flags(unsigned int flags2)
{
	unsigned int flags = flags2 & FUTEX2_SIZE_MASK;

	if (!(flags2 & FUTEX2_PRIVATE))
		flags |= FLAGS_SHARED;

	if (flags2 & FUTEX2_NUMA)
		flags |= FLAGS_NUMA;

	if (flags2 & FUTEX2_MPOL)
		flags |= FLAGS_MPOL;

	return flags;
}

static inline unsigned int futex_size(unsigned int flags)
{
	return 1 << (flags & FLAGS_SIZE_MASK);
}

static inline bool futex_flags_valid(unsigned int flags)
{
	/* Only 64bit futexes for 64bit code */
	if (!IS_ENABLED(CONFIG_64BIT) || in_compat_syscall()) {
		if ((flags & FLAGS_SIZE_MASK) == FLAGS_SIZE_64)
			return false;
	}

	/* Only 32bit futexes are implemented -- for now */
	if ((flags & FLAGS_SIZE_MASK) != FLAGS_SIZE_32)
		return false;

	/*
	 * Must be able to represent both FUTEX_NO_NODE and every valid nodeid
	 * in a futex word.
	 */
	if (flags & FLAGS_NUMA) {
		int bits = 8 * futex_size(flags);
		u64 max = ~0ULL;

		max >>= 64 - bits;
		if (nr_node_ids >= max)
			return false;
	}

	return true;
}

static inline bool futex_validate_input(unsigned int flags, u64 val)
{
	int bits = 8 * futex_size(flags);

	if (bits < 64 && (val >> bits))
		return false;

	return true;
}

#ifdef CONFIG_FAIL_FUTEX
extern bool should_fail_futex(bool fshared);
#else
static inline bool should_fail_futex(bool fshared)
{
	return false;
}
#endif

static inline bool futex_key_is_private(union futex_key *key)
{
	/*
	 * Relies on get_futex_key() to set either bit for shared
	 * futexes -- see comment with union futex_key.
	 */
	return !(key->both.offset & (FUT_OFF_INODE | FUT_OFF_MMSHARED));
}

/*
 * Hash buckets are shared by all the futex_keys that hash to the same
 * location.  Each key may have multiple futex_q structures, one for each task
 * waiting on a futex.
 */
struct futex_hash_bucket {
	atomic_t waiters;
	spinlock_t lock;
	struct plist_head chain;
	unsigned int spin_score;	/* WolKernel adaptive spin, see futex_spin_next() */
} ____cacheline_aligned_in_smp;

/*
 * WolKernel adaptive FUTEX_WAIT spin. Each bucket keeps a score in
 * [0, FUTEX_SPIN_SCORE_MAX]. The spin budget is spin_ns scaled by the score;
 * a spin that ends in a wakeup raises it, one that times out halves it, so
 * buckets with long waits stop wasting CPU and quick hand-offs keep spinning.
 * Pure functions so KUnit can check them (spin_kunit.c).
 */
static inline u64 futex_spin_budget(unsigned int spin_ns, unsigned int score,
				    bool cpu_alone)
{
	if (!spin_ns || !cpu_alone || !score)
		return 0;
	if (score > FUTEX_SPIN_SCORE_MAX)
		score = FUTEX_SPIN_SCORE_MAX;
	return (u64)spin_ns * score / FUTEX_SPIN_SCORE_MAX;
}

/*
 * WolKernel pre-queue spin: while a task's waits end within a few microseconds
 * it polls the user words before it queues, so a hand-off costs neither side a
 * key, a bucket lock or a wakeup, and the waker finds nobody to wake. Nothing
 * is queued, so a word that changes returns -EWOULDBLOCK, the answer the wait
 * would give had it started a moment later; when the budget runs out the wait
 * queues as usual and sees every later wake. No hash exists yet, so the score
 * lives in the task. Any score grants the whole budget: a budget scaled down
 * can never reach a partner that is itself asleep, and both sides stay stuck
 * in the slow regime. Misses halve the score. Pure policy, see spin_kunit.c.
 */
/*
 * WolKernel request/reply: a task that wakes one waiter and sleeps right after
 * hands its CPU over. The score counts that pattern; from FUTEX_SYNC_MIN on its
 * single wakes run the wakee here, without an IPI, once the waker blocks. Only
 * on a CPU something else wants: on a free one the pair belongs on two CPUs,
 * where they spin, and a hand-over would put them on one for good.
 */
#define FUTEX_SYNC_SCORE_MAX	8U
#define FUTEX_SYNC_MIN		2U
#define FUTEX_SYNC_GAP_NS	20000ULL

static inline unsigned int futex_sync_next(unsigned int score, u64 gap_ns)
{
	if (gap_ns >= FUTEX_SYNC_GAP_NS)
		return score / 2;
	return score < FUTEX_SYNC_SCORE_MAX ? score + 1 : score;
}

static inline bool futex_sync_wanted(int woken, unsigned int score, bool cpu_alone)
{
	return woken == 1 && score >= FUTEX_SYNC_MIN && !cpu_alone;
}

/* Only two things learn from a sleep: a stopped spin, and the gap since a wake. */
static inline bool futex_sleep_needs_clock(unsigned int spin_score, u64 last_wake)
{
	return !spin_score || last_wake;
}

/*
 * Priority Inheritance state:
 */
struct futex_pi_state {
	/*
	 * list of 'owned' pi_state instances - these have to be
	 * cleaned up in do_exit() if the task exits prematurely:
	 */
	struct list_head list;

	/*
	 * The PI object:
	 */
	struct rt_mutex_base pi_mutex;

	struct task_struct *owner;
	refcount_t refcount;

	union futex_key key;
} __randomize_layout;

struct futex_q;
typedef void (futex_wake_fn)(struct wake_q_head *wake_q, struct futex_q *q);

/**
 * struct futex_q - The hashed futex queue entry, one per waiting task
 * @list:		priority-sorted list of tasks waiting on this futex
 * @task:		the task waiting on the futex
 * @lock_ptr:		the hash bucket lock
 * @wake:		the wake handler for this queue
 * @wake_data:		data associated with the wake handler
 * @key:		the key the futex is hashed on
 * @pi_state:		optional priority inheritance state
 * @rt_waiter:		rt_waiter storage for use with requeue_pi
 * @requeue_pi_key:	the requeue_pi target futex key
 * @bitset:		bitset for the optional bitmasked wakeup
 * @requeue_state:	State field for futex_requeue_pi()
 * @drop_fph:		Waiter should drop the extra private hash reference when set
 * @requeue_wait:	RCU wait for futex_requeue_pi() (RT only)
 *
 * We use this hashed waitqueue, instead of a normal wait_queue_entry_t, so
 * we can wake only the relevant ones (hashed queues may be shared).
 *
 * A futex_q has a woken state, just like tasks have TASK_RUNNING.
 * It is considered woken when plist_node_empty(&q->list) || q->lock_ptr == 0.
 * The order of wakeup is always to make the first condition true, then
 * the second.
 *
 * PI futexes are typically woken before they are removed from the hash list via
 * the rt_mutex code. See futex_unqueue_pi().
 */
struct futex_q {
	struct plist_node list;

	struct task_struct *task;
	spinlock_t *lock_ptr;
	futex_wake_fn *wake;
	void *wake_data;
	union futex_key key;
	struct futex_pi_state *pi_state;
	struct rt_mutex_waiter *rt_waiter;
	union futex_key *requeue_pi_key;
	u32 bitset;
	atomic_t requeue_state;
	struct futex_private_hash *drop_fph;
#ifdef CONFIG_PREEMPT_RT
	struct rcuwait requeue_wait;
#endif
} __randomize_layout;

extern const struct futex_q futex_q_init;

enum futex_access {
	FUTEX_READ,
	FUTEX_WRITE
};

extern int get_futex_key(u32 __user *uaddr, unsigned int flags, union futex_key *key,
			 enum futex_access rw);
extern bool futex_key_cache_on;
extern int futex_key_get(u32 __user *uaddr, unsigned int flags, union futex_key *key,
			 enum futex_access rw);
extern void futex_q_lockptr_lock(struct futex_q *q) __acquires(q->lock_ptr);
extern struct hrtimer_sleeper *
futex_setup_timer(ktime_t *time, struct hrtimer_sleeper *timeout,
		  int flags, u64 range_ns);

/*
 * WolKernel: a shared futex key costs a GUP-fast and a folio lookup per
 * address, and Proton's fsync futexes are all shared. The key of an address
 * only changes with its VMA or with a read-only private page, which only
 * mprotect() makes writable, so it holds while the mm's mmap_lock sequence
 * stands still. Each task keeps its last keys, direct-mapped, emptied when
 * the sequence moves. Off: futex.key_cache=0.
 */
#define FUTEX_KEY_CACHE_SLOTS	16

struct futex_key_cache {
	unsigned int seq;
	unsigned int hits;	/* read by KUnit */
	struct {
		unsigned long uaddr;
		union futex_key key;
	} slot[FUTEX_KEY_CACHE_SLOTS];
};

struct futex_bucket_ref {
	struct futex_hash_bucket *hb;
	struct futex_private_hash *fph;
};

#ifdef CONFIG_FUTEX_PRIVATE_HASH
extern struct futex_private_hash *futex_private_hash(struct mm_struct *mm);
extern void futex_private_hash_put(struct futex_private_hash *fph);

#else /* !CONFIG_FUTEX_PRIVATE_HASH */
static inline struct futex_private_hash *futex_private_hash(struct mm_struct *mm) { return NULL; }
static inline void futex_private_hash_put(struct futex_private_hash *fph) { }
#endif

extern struct futex_bucket_ref futex_hash(union futex_key *key);

DEFINE_CLASS(hbr, struct futex_bucket_ref,
	     if (_T.fph) futex_private_hash_put(_T.fph),
	     futex_hash(key), union futex_key *key);

DEFINE_CLASS(private_hash, struct futex_private_hash *,
	     if (_T) futex_private_hash_put(_T),
	     futex_private_hash(mm), struct mm_struct *mm);

/**
 * futex_match - Check whether two futex keys are equal
 * @key1:	Pointer to key1
 * @key2:	Pointer to key2
 *
 * Return 1 if two futex_keys are equal, 0 otherwise.
 */
static inline int futex_match(union futex_key *key1, union futex_key *key2)
{
	return (key1 && key2
		&& key1->both.word == key2->both.word
		&& key1->both.ptr == key2->both.ptr
		&& key1->both.offset == key2->both.offset);
}

extern int futex_wait_setup(u32 __user *uaddr, u32 val, unsigned int flags,
			    struct futex_q *q, union futex_key *key2,
			    struct task_struct *task);
extern void futex_do_wait(struct futex_q *q, struct hrtimer_sleeper *timeout);
extern bool __futex_wake_mark(struct futex_q *q);
extern void futex_wake_mark(struct wake_q_head *wake_q, struct futex_q *q);

extern int fault_in_user_writeable(u32 __user *uaddr);
extern struct futex_q *futex_top_waiter(struct futex_hash_bucket *hb, union futex_key *key);

static inline int futex_cmpxchg_value_locked(u32 *curval, u32 __user *uaddr, u32 uval, u32 newval)
{
	int ret;

	pagefault_disable();
	ret = futex_atomic_cmpxchg_inatomic(curval, uaddr, uval, newval);
	pagefault_enable();

	return ret;
}

/* Read from user memory with pagefaults disabled */
static inline int futex_get_value_locked(u32 *dest, u32 __user *from)
{
	guard(pagefault)();
	return get_user_inline(*dest, from);
}

extern void __futex_unqueue(struct futex_q *q);
extern void __futex_queue(struct futex_q *q, struct futex_hash_bucket *hb,
				struct task_struct *task);
extern int futex_unqueue(struct futex_q *q);

/**
 * futex_queue() - Enqueue the futex_q on the futex_hash_bucket
 * @q:	The futex_q to enqueue
 * @hb:	The destination hash bucket
 * @task: Task queueing this futex
 *
 * The hb->lock must be held by the caller, and is released here. A call to
 * futex_queue() is typically paired with exactly one call to futex_unqueue().  The
 * exceptions involve the PI related operations, which may use futex_unqueue_pi()
 * or nothing if the unqueue is done as part of the wake process and the unqueue
 * state is implicit in the state of woken task (see futex_wait_requeue_pi() for
 * an example).
 *
 * Note that @task may be NULL, for async usage of futexes.
 */
static inline void futex_queue(struct futex_q *q, struct futex_hash_bucket *hb,
			       struct task_struct *task)
	__releases(&hb->lock)
	__releases(q->lock_ptr)
{
	__futex_queue(q, hb, task);
	spin_unlock(&hb->lock);
	__release(q->lock_ptr);
}

extern void futex_unqueue_pi(struct futex_q *q);

extern void wait_for_owner_exiting(int ret, struct task_struct *exiting);

/*
 * Reflects a new waiter being added to the waitqueue.
 */
static inline void futex_hb_waiters_inc(struct futex_hash_bucket *hb)
{
#ifdef CONFIG_SMP
	atomic_inc(&hb->waiters);
	/*
	 * Full barrier (A), see the ordering comment above.
	 */
	smp_mb__after_atomic();
#endif
}

/*
 * Reflects a waiter being removed from the waitqueue by wakeup
 * paths.
 */
static inline void futex_hb_waiters_dec(struct futex_hash_bucket *hb)
{
#ifdef CONFIG_SMP
	atomic_dec(&hb->waiters);
#endif
}

static inline int futex_hb_waiters_pending(struct futex_hash_bucket *hb)
{
#ifdef CONFIG_SMP
	/*
	 * Full barrier (B), see the ordering comment above.
	 */
	smp_mb();
	return atomic_read(&hb->waiters);
#else
	return 1;
#endif
}

extern void futex_q_lock(struct futex_q *q, struct futex_hash_bucket *hb)
	__acquires(&hb->lock)
	__acquires(q->lock_ptr);

extern void futex_q_unlock(struct futex_hash_bucket *hb)
	__releases(&hb->lock);

extern int futex_lock_pi_atomic(u32 __user *uaddr, struct futex_hash_bucket *hb,
				union futex_key *key,
				struct futex_pi_state **ps,
				struct task_struct *task,
				struct task_struct **exiting,
				int set_waiters);

extern int refill_pi_state_cache(void);
extern void get_pi_state(struct futex_pi_state *pi_state);
extern void put_pi_state(struct futex_pi_state *pi_state);
extern int fixup_pi_owner(u32 __user *uaddr, struct futex_q *q, int locked);

/*
 * Express the locking dependencies for lockdep:
 */
static inline void
double_lock_hb(struct futex_hash_bucket *hb1, struct futex_hash_bucket *hb2)
	__acquires(&hb1->lock)
	__acquires(&hb2->lock)
	__no_context_analysis
{
	if (hb1 > hb2)
		swap(hb1, hb2);

	spin_lock(&hb1->lock);
	if (hb1 != hb2)
		spin_lock_nested(&hb2->lock, SINGLE_DEPTH_NESTING);
}

static inline void
double_unlock_hb(struct futex_hash_bucket *hb1, struct futex_hash_bucket *hb2)
	__releases(&hb1->lock)
	__releases(&hb2->lock)
	__no_context_analysis
{
	spin_unlock(&hb1->lock);
	if (hb1 != hb2)
		spin_unlock(&hb2->lock);
}

/* syscalls */

extern int futex_wait_requeue_pi(u32 __user *uaddr, unsigned int flags, u32
				 val, ktime_t *abs_time, u32 bitset, u32 __user
				 *uaddr2);

extern int futex_requeue(u32 __user *uaddr1, unsigned int flags1,
			 u32 __user *uaddr2, unsigned int flags2,
			 int nr_wake, int nr_requeue,
			 u32 *cmpval, int requeue_pi);

extern int __futex_wait(u32 __user *uaddr, unsigned int flags, u32 val,
			struct hrtimer_sleeper *to, u32 bitset);

extern int futex_wait(u32 __user *uaddr, unsigned int flags, u32 val,
		      ktime_t *abs_time, u32 bitset);

/**
 * struct futex_vector - Auxiliary struct for futex_waitv()
 * @w: Userspace provided data
 * @q: Kernel side data
 *
 * Struct used to build an array with all data need for futex_waitv()
 */
struct futex_vector {
	struct futex_waitv w;
	struct futex_q q;
};

/*
 * WolKernel: futex_waitv() on up to FUTEX_WAITV_INLINE futexes (Wine/Proton
 * waits on one to three objects almost always) uses an on-stack vector
 * instead of a kzalloc/kfree per call. Off: futex.waitv_inline=0.
 */
#define FUTEX_WAITV_INLINE	4
extern bool futex_waitv_inline_on;

static inline bool futex_waitv_inline(unsigned int nr)
{
	return READ_ONCE(futex_waitv_inline_on) && nr <= FUTEX_WAITV_INLINE;
}

/*
 * WolKernel: past the stack vector, up to FUTEX_WAITV_TASK futexes use a vector
 * the task keeps, instead of a zeroed allocation per call. A task sleeps in one
 * futex_waitv() at a time. NULL means the caller allocates as before.
 */
#define FUTEX_WAITV_TASK	64
extern struct futex_vector *futex_task_vector(unsigned int nr);

extern bool futex_precheck_on;

/*
 * Lockless pre-read: true only when *uaddr is aligned, readable without
 * faulting and differs from @val. A fault or a match says nothing; the
 * locked path decides.
 */
static inline bool futex_value_differs(u32 __user *uaddr, u32 val)
{
	u32 cur;

	/* Misaligned addresses keep their -EINVAL from get_futex_key(). */
	if (!IS_ALIGNED((unsigned long)uaddr, sizeof(u32)))
		return false;
	return !futex_get_value_locked(&cur, uaddr) && cur != val;
}

/*
 * WolKernel: futex_waitv()'s non-blocking answer in one pass over the user's
 * array and timeout, inside one user access window: no key, no hash, no lock,
 * no copy. Only when every entry is plain (u32, private or shared, aligned, no
 * reserved bits) and the timeout is valid does the scan have a verdict, and
 * any word that differs means the kernel can only say -EWOULDBLOCK. Everything
 * else, a fault included, is FUTEX_SCAN_FULL and the full path decides, so no
 * error can change: the scan reads every word because an unreadable one at any
 * index may have to win over the mismatch. A word that differs is a valid
 * linearization; a word that matches is read again under the bucket lock
 * before any sleep. May fault in user pages: only for syscall context.
 */
enum { FUTEX_SCAN_FULL, FUTEX_SCAN_MATCH, FUTEX_SCAN_DIFFERS };

#ifndef masked_user_access_begin
#define mask_user_address(p) (access_ok(p, sizeof(*(p))) ? (p) : NULL)
#endif

static __always_inline int futex_waitv_scan(const struct futex_waitv __user *uw,
					    unsigned int nr,
					    const struct __kernel_timespec __user *to)
{
	u32 odd = 0, diff = 0;
	unsigned int i;

	scoped_user_read_access_size(uw, nr * sizeof(*uw), full) {
		if (to) {
			const struct __kernel_timespec __user *t = mask_user_address(to);
			s64 sec, nsec;

			unsafe_get_user(sec, &t->tv_sec, full);
			unsafe_get_user(nsec, &t->tv_nsec, full);
			if (in_compat_syscall())
				nsec = (u32)nsec;
			odd = (sec < 0) | ((u64)nsec >= NSEC_PER_SEC);
		}
		for (i = 0; i < nr; i++) {
			u64 val, uaddr;
			u32 fl, rsv, cur;
			u32 __user *word;

			unsafe_get_user(val, &uw[i].val, full);
			unsafe_get_user(uaddr, &uw[i].uaddr, full);
			unsafe_get_user(fl, &uw[i].flags, full);
			unsafe_get_user(rsv, &uw[i].__reserved, full);
			odd |= ((fl & ~FUTEX2_PRIVATE) ^ FUTEX2_SIZE_U32) | rsv |
			       (u32)(val >> 32) | ((u32)uaddr & 3);
			word = mask_user_address(u64_to_user_ptr(uaddr));
			unsafe_get_user(cur, word, full);
			diff |= cur ^ (u32)val;
		}
	}
	if (odd)
		return FUTEX_SCAN_FULL;
	return diff ? FUTEX_SCAN_DIFFERS : FUTEX_SCAN_MATCH;
full:
	return FUTEX_SCAN_FULL;
}

extern bool futex_waitv_prespin(const struct futex_waitv __user *uw, unsigned int nr,
				const struct __kernel_timespec __user *to);

extern int futex_parse_waitv(struct futex_vector *futexv,
			     struct futex_waitv __user *uwaitv,
			     unsigned int nr_futexes, futex_wake_fn *wake,
			     void *wake_data);

extern int futex_wait_multiple_setup(struct futex_vector *vs, int count,
				     int *woken);

extern int futex_unqueue_multiple(struct futex_vector *v, int count);

extern int futex_wait_multiple(struct futex_vector *vs, unsigned int count,
			       struct hrtimer_sleeper *to);

extern int futex_wake(u32 __user *uaddr, unsigned int flags, void __user *pop,
		      int nr_wake, u32 bitset);

extern int futex_wake_op(u32 __user *uaddr1, unsigned int flags,
			 u32 __user *uaddr2, int nr_wake, int nr_wake2, int op);

extern int futex_unlock_pi(u32 __user *uaddr, unsigned int flags, void __user *pop);

extern int futex_lock_pi(u32 __user *uaddr, unsigned int flags, ktime_t *time, int trylock);

bool futex_robust_list_clear_pending(void __user *pop, unsigned int flags);

#endif /* _FUTEX_H */
