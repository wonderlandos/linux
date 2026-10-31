// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/moduleparam.h>
#include <kunit/visibility.h>
#include <linux/sched/clock.h>
#include <linux/random.h>
#include <linux/sched/stat.h>
#include <linux/plist.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/freezer.h>

#include "futex.h"

/*
 * READ this before attempting to hack on futexes!
 *
 * Basic futex operation and ordering guarantees
 * =============================================
 *
 * The waiter reads the futex value in user space and calls
 * futex_wait(). This function computes the hash bucket and acquires
 * the hash bucket lock. After that it reads the futex user space value
 * again and verifies that the data has not changed. If it has not changed
 * it enqueues itself into the hash bucket, releases the hash bucket lock
 * and schedules.
 *
 * The waker side modifies the user space value of the futex and calls
 * futex_wake(). This function computes the hash bucket and acquires the
 * hash bucket lock. Then it looks for waiters on that futex in the hash
 * bucket and wakes them.
 *
 * In futex wake up scenarios where no tasks are blocked on a futex, taking
 * the hb spinlock can be avoided and simply return. In order for this
 * optimization to work, ordering guarantees must exist so that the waiter
 * being added to the list is acknowledged when the list is concurrently being
 * checked by the waker, avoiding scenarios like the following:
 *
 * CPU 0                               CPU 1
 * val = *futex;
 * sys_futex(WAIT, futex, val);
 *   futex_wait(futex, val);
 *   uval = *futex;
 *                                     *futex = newval;
 *                                     sys_futex(WAKE, futex);
 *                                       futex_wake(futex);
 *                                       if (queue_empty())
 *                                         return;
 *   if (uval == val)
 *      lock(hash_bucket(futex));
 *      queue();
 *     unlock(hash_bucket(futex));
 *     schedule();
 *
 * This would cause the waiter on CPU 0 to wait forever because it
 * missed the transition of the user space value from val to newval
 * and the waker did not find the waiter in the hash bucket queue.
 *
 * The correct serialization ensures that a waiter either observes
 * the changed user space value before blocking or is woken by a
 * concurrent waker:
 *
 * CPU 0                                 CPU 1
 * val = *futex;
 * sys_futex(WAIT, futex, val);
 *   futex_wait(futex, val);
 *
 *   waiters++; (a)
 *   smp_mb(); (A) <-- paired with -.
 *                                  |
 *   lock(hash_bucket(futex));      |
 *                                  |
 *   uval = *futex;                 |
 *                                  |        *futex = newval;
 *                                  |        sys_futex(WAKE, futex);
 *                                  |          futex_wake(futex);
 *                                  |
 *                                  `--------> smp_mb(); (B)
 *   if (uval == val)
 *     queue();
 *     unlock(hash_bucket(futex));
 *     schedule();                         if (waiters)
 *                                           lock(hash_bucket(futex));
 *   else                                    wake_waiters(futex);
 *     waiters--; (b)                        unlock(hash_bucket(futex));
 *
 * Where (A) orders the waiters increment and the futex value read through
 * atomic operations (see futex_hb_waiters_inc) and where (B) orders the write
 * to futex and the waiters read (see futex_hb_waiters_pending()).
 *
 * This yields the following case (where X:=waiters, Y:=futex):
 *
 *	X = Y = 0
 *
 *	w[X]=1		w[Y]=1
 *	MB		MB
 *	r[Y]=y		r[X]=x
 *
 * Which guarantees that x==0 && y==0 is impossible; which translates back into
 * the guarantee that we cannot both miss the futex variable change and the
 * enqueue.
 *
 * Note that a new waiter is accounted for in (a) even when it is possible that
 * the wait call can return error, in which case we backtrack from it in (b).
 * Refer to the comment in futex_q_lock().
 *
 * Similarly, in order to account for waiters being requeued on another
 * address we always increment the waiters for the destination bucket before
 * acquiring the lock. It then decrements them again  after releasing it -
 * the code that actually moves the futex(es) between hash buckets (requeue_futex)
 * will do the additional required waiter count housekeeping. This is done for
 * double_lock_hb() and double_unlock_hb(), respectively.
 */

bool __futex_wake_mark(struct futex_q *q)
{
	if (WARN(q->pi_state || q->rt_waiter, "refusing to wake PI futex\n"))
		return false;

	__futex_unqueue(q);
	/*
	 * The waiting task can free the futex_q as soon as q->lock_ptr = NULL
	 * is written, without taking any locks. This is possible in the event
	 * of a spurious wakeup, for example. A memory barrier is required here
	 * to prevent the following store to lock_ptr from getting ahead of the
	 * plist_del in __futex_unqueue().
	 */
	smp_store_release(&q->lock_ptr, NULL);

	return true;
}

/*
 * The hash bucket lock must be held when this is called.
 * Afterwards, the futex_q must not be accessed. Callers
 * must ensure to later call wake_up_q() for the actual
 * wakeups to occur.
 */
void futex_wake_mark(struct wake_q_head *wake_q, struct futex_q *q)
{
	struct task_struct *p = q->task;

	get_task_struct(p);

	if (!__futex_wake_mark(q)) {
		put_task_struct(p);
		return;
	}

	/*
	 * Queue the task for later wakeup for after we've released
	 * the hb->lock.
	 */
	wake_q_add_safe(wake_q, p);
}

/*
 * If requested, clear the robust list pending op and unlock the futex
 */
static bool futex_robust_unlock(u32 __user *uaddr, unsigned int flags, void __user *pop)
{
	if (!(flags & FLAGS_ROBUST_UNLOCK))
		return true;

	/* First unlock the futex, which requires release semantics. */
	scoped_user_write_access(uaddr, efault)
		unsafe_atomic_store_release_user(0, uaddr, efault);

	/*
	 * Clear the pending list op now. If that fails, then the task is in
	 * deeper trouble as the robust list head is usually part of the TLS.
	 * The chance of survival is close to zero.
	 */
	return futex_robust_list_clear_pending(pop, flags);

efault:
	return false;
}

static void futex_wake_up_q(struct wake_q_head *wake_q, int woken);

/*
 * Wake up waiters matching bitset queued on this futex (uaddr).
 */
int futex_wake(u32 __user *uaddr, unsigned int flags, void __user *pop, int nr_wake, u32 bitset)
{
	union futex_key key = FUTEX_KEY_INIT;
	struct futex_q *this, *next;
	DEFINE_WAKE_Q(wake_q);
	int ret;

	if (!bitset)
		return -EINVAL;

	ret = futex_key_get(uaddr, flags, &key, FUTEX_READ);
	if (unlikely(ret != 0))
		return ret;

	if (!futex_robust_unlock(uaddr, flags, pop))
		return -EFAULT;

	if ((flags & FLAGS_STRICT) && !nr_wake)
		return 0;

	CLASS(hbr, hbr)(&key);
	auto hb = hbr.hb;

	/* Make sure we really have tasks to wakeup */
	if (!futex_hb_waiters_pending(hb))
		return ret;

	spin_lock(&hb->lock);

	plist_for_each_entry_safe(this, next, &hb->chain, list) {
		if (futex_match (&this->key, &key)) {
			if (this->pi_state || this->rt_waiter) {
				ret = -EINVAL;
				break;
			}

			/* Check if one of the bits is set in both bitsets */
			if (!(this->bitset & bitset))
				continue;

			this->wake(&wake_q, this);
			if (++ret >= nr_wake)
				break;
		}
	}

	spin_unlock(&hb->lock);
	futex_wake_up_q(&wake_q, ret);
	return ret;
}

static int futex_atomic_op_inuser(unsigned int encoded_op, u32 __user *uaddr)
{
	unsigned int op =	  (encoded_op & 0x70000000) >> 28;
	unsigned int cmp =	  (encoded_op & 0x0f000000) >> 24;
	int oparg = sign_extend32((encoded_op & 0x00fff000) >> 12, 11);
	int cmparg = sign_extend32(encoded_op & 0x00000fff, 11);
	int oldval, ret;

	if (encoded_op & (FUTEX_OP_OPARG_SHIFT << 28)) {
		if (oparg < 0 || oparg > 31) {
			/*
			 * kill this print and return -EINVAL when userspace
			 * is sane again
			 */
			pr_info_ratelimited("futex_wake_op: %s tries to shift op by %d; fix this program\n",
					    current->comm, oparg);
			oparg &= 31;
		}
		oparg = 1 << oparg;
	}

	pagefault_disable();
	ret = arch_futex_atomic_op_inuser(op, oparg, &oldval, uaddr);
	pagefault_enable();
	if (ret)
		return ret;

	switch (cmp) {
	case FUTEX_OP_CMP_EQ:
		return oldval == cmparg;
	case FUTEX_OP_CMP_NE:
		return oldval != cmparg;
	case FUTEX_OP_CMP_LT:
		return oldval < cmparg;
	case FUTEX_OP_CMP_GE:
		return oldval >= cmparg;
	case FUTEX_OP_CMP_LE:
		return oldval <= cmparg;
	case FUTEX_OP_CMP_GT:
		return oldval > cmparg;
	default:
		return -ENOSYS;
	}
}

/*
 * Wake up all waiters hashed on the physical page that is mapped
 * to this virtual address:
 */
int futex_wake_op(u32 __user *uaddr1, unsigned int flags, u32 __user *uaddr2,
		  int nr_wake, int nr_wake2, int op)
{
	union futex_key key1 = FUTEX_KEY_INIT, key2 = FUTEX_KEY_INIT;
	struct futex_q *this, *next;
	int ret, op_ret;
	DEFINE_WAKE_Q(wake_q);

retry:
	ret = get_futex_key(uaddr1, flags, &key1, FUTEX_READ);
	if (unlikely(ret != 0))
		return ret;
	ret = get_futex_key(uaddr2, flags, &key2, FUTEX_WRITE);
	if (unlikely(ret != 0))
		return ret;

retry_private:
	if (1) {
		CLASS(hbr, hbr1)(&key1);
		CLASS(hbr, hbr2)(&key2);
		auto hb1 = hbr1.hb;
		auto hb2 = hbr2.hb;

		double_lock_hb(hb1, hb2);
		op_ret = futex_atomic_op_inuser(op, uaddr2);
		if (unlikely(op_ret < 0)) {
			double_unlock_hb(hb1, hb2);

			if (!IS_ENABLED(CONFIG_MMU) ||
			    unlikely(op_ret != -EFAULT && op_ret != -EAGAIN)) {
				/*
				 * we don't get EFAULT from MMU faults if we don't have
				 * an MMU, but we might get them from range checking
				 */
				ret = op_ret;
				return ret;
			}

			if (op_ret == -EFAULT) {
				ret = fault_in_user_writeable(uaddr2);
				if (ret)
					return ret;
			}

			cond_resched();
			if (!(flags & FLAGS_SHARED))
				goto retry_private;
			goto retry;
		}

		plist_for_each_entry_safe(this, next, &hb1->chain, list) {
			if (futex_match(&this->key, &key1)) {
				if (this->pi_state || this->rt_waiter) {
					ret = -EINVAL;
					goto out_unlock;
				}
				this->wake(&wake_q, this);
				if (++ret >= nr_wake)
					break;
			}
		}

		if (op_ret > 0) {
			op_ret = 0;
			plist_for_each_entry_safe(this, next, &hb2->chain, list) {
				if (futex_match(&this->key, &key2)) {
					if (this->pi_state || this->rt_waiter) {
						ret = -EINVAL;
						goto out_unlock;
					}
					this->wake(&wake_q, this);
					if (++op_ret >= nr_wake2)
						break;
				}
			}
			ret += op_ret;
		}

out_unlock:
		double_unlock_hb(hb1, hb2);
	}
	wake_up_q(&wake_q);
	return ret;
}

static long futex_wait_restart(struct restart_block *restart);

/**
 * futex_do_wait() - wait for wakeup, timeout, or signal
 * @q:		the futex_q to queue up on
 * @timeout:	the prepared hrtimer_sleeper, or null for no timeout
 */
/*
 * WolKernel: bounded spin before sleeping in FUTEX_WAIT. Most lock hand-offs
 * between game/engine threads complete within a few microseconds, which is
 * cheaper than a sleep plus wakeup IPI. 0 disables. futex.spin_ns=N on the
 * command line or /sys/module/futex/parameters/spin_ns at runtime.
 */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "futex."
static unsigned int futex_spin_ns __read_mostly = 4000;
module_param_named(spin_ns, futex_spin_ns, uint, 0644);

bool futex_waitv_inline_on __read_mostly = true;
module_param_named(waitv_inline, futex_waitv_inline_on, bool, 0644);
EXPORT_SYMBOL_IF_KUNIT(futex_waitv_inline_on);

/*
 * WolKernel: a futex whose value already differs from the expected one
 * returns -EWOULDBLOCK before any key lookup, hashing or bucket lock. That
 * is a valid linearization (the value differed at that instant); a value that
 * matches is still re-read under the bucket lock before sleeping, so no
 * wakeup can be lost. Wine/Proton fsync waits on already-signaled objects all
 * the time. Off: futex.precheck=0, for FUTEX_WAIT and for futex_waitv_scan().
 */
bool futex_precheck_on __read_mostly = true;
module_param_named(precheck, futex_precheck_on, bool, 0644);
EXPORT_SYMBOL_IF_KUNIT(futex_precheck_on);

bool futex_key_cache_on __read_mostly = true;
module_param_named(key_cache, futex_key_cache_on, bool, 0644);
EXPORT_SYMBOL_IF_KUNIT(futex_key_cache_on);

static bool futex_sync_on __read_mostly = true;
module_param_named(sync_wake, futex_sync_on, bool, 0644);

static void futex_wake_up_q(struct wake_q_head *wake_q, int woken)
{
	bool sync = READ_ONCE(futex_sync_on), alone = single_task_running();

	/* The gap is only asked about on a CPU a hand-over could help. */
	current->futex.last_wake = sync && !alone && woken > 0 ? local_clock() : 0;
	if (sync && futex_sync_wanted(woken, current->futex.sync_score, alone))
		wake_up_q_sync(wake_q);
	else
		wake_up_q(wake_q);
}

/* Off: futex.spin_polls=1, a clock read every pass and at every sleep. */
static unsigned int futex_spin_polls_max __read_mostly = FUTEX_SPIN_POLLS;
module_param_named(spin_polls, futex_spin_polls_max, uint, 0644);

struct futex_prespin {
	u64		end;
	unsigned int	polls;
	unsigned int	left;
};

static bool futex_prespin_on __read_mostly = true;
module_param_named(prespin, futex_prespin_on, bool, 0644);

static bool futex_prespin_begin(struct futex_prespin *s, unsigned int nr)
{
	u64 budget;

	if (!READ_ONCE(futex_prespin_on) || num_online_cpus() < 2)
		return false;
	budget = futex_prespin_budget(READ_ONCE(futex_spin_ns),
				      current->futex.spin_score,
				      single_task_running());
	if (!budget)
		return false;
	s->polls = futex_spin_polls(READ_ONCE(futex_spin_polls_max), nr);
	s->left = s->polls;
	s->end = local_clock() + budget;
	return true;
}

static __always_inline bool futex_prespin_goes_on(struct futex_prespin *s)
{
	if (need_resched() || signal_pending(current) || !single_task_running())
		return false;
	if (--s->left)
		return true;
	s->left = s->polls;
	return local_clock() < s->end;
}

static void futex_prespin_end(bool hit)
{
	current->futex.spin_score = futex_spin_next(current->futex.spin_score, hit);
}

bool futex_waitv_prespin(const struct futex_waitv __user *uw, unsigned int nr,
			 const struct __kernel_timespec __user *to)
{
	struct futex_prespin s;
	int scan = FUTEX_SCAN_MATCH;

	if (!futex_prespin_begin(&s, nr))
		return false;
	while (scan == FUTEX_SCAN_MATCH && futex_prespin_goes_on(&s)) {
		cpu_relax();
		scan = futex_waitv_scan(uw, nr, to);
	}
	futex_prespin_end(scan == FUTEX_SCAN_DIFFERS);
	return scan == FUTEX_SCAN_DIFFERS;
}

static bool futex_wait_prespin(u32 __user *uaddr, u32 val)
{
	struct futex_prespin s;
	bool differs = false;

	if (!futex_prespin_begin(&s, 1))
		return false;
	while (!differs && futex_prespin_goes_on(&s)) {
		cpu_relax();
		differs = futex_value_differs(uaddr, val);
	}
	futex_prespin_end(differs);
	return differs;
}

/* The start of the sleep, or 0 when nothing will learn from it. */
static __always_inline u64 futex_sleep_begin(void)
{
	u64 last = current->futex.last_wake, start;

	if (READ_ONCE(futex_spin_polls_max) > 1 &&
	    !futex_sleep_needs_clock(current->futex.spin_score, last))
		return 0;
	start = local_clock();
	if (last) {
		current->futex.sync_score =
			futex_sync_next(current->futex.sync_score, start - last);
		current->futex.last_wake = 0;
	}
	return start;
}

/* A sleep rearms a stopped spin, or a task that sleeps never learns to spin. */
static void futex_sleep_learn(u64 start)
{
	if (start && !current->futex.spin_score && READ_ONCE(futex_prespin_on))
		current->futex.spin_score =
			futex_spin_slept(0, local_clock() - start,
					 READ_ONCE(futex_spin_ns));
}

static void futex_spin_wait(struct futex_q *q)
{
	struct futex_hash_bucket *hb;
	spinlock_t *lock;
	unsigned int score;
	u64 budget, end;
	bool woke;

	if (num_online_cpus() < 2)
		return;
	/* A private hash is freed by RCU after a rehash: keep hb valid. */
	guard(rcu)();
	lock = READ_ONCE(q->lock_ptr);
	if (!lock)	/* already woken */
		return;
	hb = container_of(lock, struct futex_hash_bucket, lock);
	score = READ_ONCE(hb->spin_score);
	/* Never steal the CPU from a runnable task, often the lock holder. */
	budget = futex_spin_budget(READ_ONCE(futex_spin_ns), score,
				   single_task_running());
	if (!budget) {
		/* Periodic retry so a decayed bucket can recover. */
		if (!score && !(get_random_u32() & 63))
			WRITE_ONCE(hb->spin_score, 1);
		return;
	}
	end = local_clock() + budget;
	while (!plist_node_empty(&q->list) && !need_resched() &&
	       !signal_pending(current) && local_clock() < end)
		cpu_relax();
	woke = plist_node_empty(&q->list);
	if (futex_spin_next(score, woke) != score)
		WRITE_ONCE(hb->spin_score, futex_spin_next(score, woke));
}

void futex_do_wait(struct futex_q *q, struct hrtimer_sleeper *timeout)
{
	/* Arm the timer */
	if (timeout)
		hrtimer_sleeper_start_expires(timeout, HRTIMER_MODE_ABS);

	/*
	 * If we have been removed from the hash list, then another task
	 * has tried to wake us, and we can skip the call to schedule().
	 */
	if (likely(!plist_node_empty(&q->list))) {
		/*
		 * If the timer has already expired, current will already be
		 * flagged for rescheduling. Only call schedule if there
		 * is no timeout, or if it has yet to expire.
		 */
		if (!READ_ONCE(futex_prespin_on))
			futex_spin_wait(q);
		if (!timeout || hrtimer_sleeper_task_get(timeout)) {
			u64 start = futex_sleep_begin();

			schedule();
			futex_sleep_learn(start);
		}
	}
	__set_current_state(TASK_RUNNING);
}

/**
 * futex_unqueue_multiple - Remove various futexes from their hash bucket
 * @v:	   The list of futexes to unqueue
 * @count: Number of futexes in the list
 *
 * Helper to unqueue a list of futexes. This can't fail.
 *
 * Return:
 *  - >=0 - Index of the last futex that was awoken;
 *  - -1  - No futex was awoken
 */
int futex_unqueue_multiple(struct futex_vector *v, int count)
{
	int ret = -1, i;

	for (i = 0; i < count; i++) {
		if (!futex_unqueue(&v[i].q))
			ret = i;
	}

	return ret;
}

/**
 * futex_wait_multiple_setup - Prepare to wait and enqueue multiple futexes
 * @vs:		The futex list to wait on
 * @count:	The size of the list
 * @woken:	Index of the last woken futex, if any. Used to notify the
 *		caller that it can return this index to userspace (return parameter)
 *
 * Prepare multiple futexes in a single step and enqueue them. This may fail if
 * the futex list is invalid or if any futex was already awoken. On success the
 * task is ready to interruptible sleep.
 *
 * Return:
 *  -  1 - One of the futexes was woken by another thread
 *  -  0 - Success
 *  - <0 - -EFAULT, -EWOULDBLOCK or -EINVAL
 */
int futex_wait_multiple_setup(struct futex_vector *vs, int count, int *woken)
{
	bool retry = false;
	int ret, i;
	u32 uval;

	/*
	 * Make sure to have a reference on the private_hash such that we
	 * don't block on rehash after changing the task state below.
	 */
	guard(private_hash)(current->mm);

	/*
	 * Enqueuing multiple futexes is tricky, because we need to enqueue
	 * each futex on the list before dealing with the next one to avoid
	 * deadlocking on the hash bucket. But, before enqueuing, we need to
	 * make sure that current->state is TASK_INTERRUPTIBLE, so we don't
	 * lose any wake events, which cannot be done before the get_futex_key
	 * of the next key, because it calls get_user_pages, which can sleep.
	 * Thus, we fetch the list of futexes keys in two steps, by first
	 * pinning all the memory keys in the futex key, and only then we read
	 * each key and queue the corresponding futex.
	 *
	 * Private futexes doesn't need to recalculate hash in retry, so skip
	 * get_futex_key() when retrying.
	 */
retry:

	for (i = 0; i < count; i++) {
		if (!(vs[i].w.flags & FLAGS_SHARED) && retry)
			continue;

		ret = futex_key_get(u64_to_user_ptr(vs[i].w.uaddr),
				    vs[i].w.flags,
				    &vs[i].q.key, FUTEX_READ);

		if (unlikely(ret))
			return ret;
	}

	set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);

	for (i = 0; i < count; i++) {
		u32 __user *uaddr = (u32 __user *)(unsigned long)vs[i].w.uaddr;
		struct futex_q *q = &vs[i].q;
		u32 val = vs[i].w.val;

		if (1) {
			CLASS(hbr, hbr)(&q->key);
			auto hb = hbr.hb;

			futex_q_lock(q, hb);
			ret = futex_get_value_locked(&uval, uaddr);

			if (!ret && uval == val) {
				/*
				 * The bucket lock can't be held while dealing with the
				 * next futex. Queue each futex at this moment so hb can
				 * be unlocked.
				 */
				futex_queue(q, hb, current);
				continue;
			}

			futex_q_unlock(hb);
			__release(q->lock_ptr);
		}
		__set_current_state(TASK_RUNNING);

		/*
		 * Even if something went wrong, if we find out that a futex
		 * was woken, we don't return error and return this index to
		 * userspace
		 */
		*woken = futex_unqueue_multiple(vs, i);
		if (*woken >= 0)
			return 1;

		if (ret) {
			/*
			 * If we need to handle a page fault, we need to do so
			 * without any lock and any enqueued futex (otherwise
			 * we could lose some wakeup). So we do it here, after
			 * undoing all the work done so far. In success, we
			 * retry all the work.
			 */
			if (get_user(uval, uaddr))
				return -EFAULT;

			retry = true;
			goto retry;
		}

		if (uval != val)
			return -EWOULDBLOCK;
	}

	return 0;
}

/**
 * futex_sleep_multiple - Check sleeping conditions and sleep
 * @vs:    List of futexes to wait for
 * @count: Length of vs
 * @to:    Timeout
 *
 * Sleep if and only if the timeout hasn't expired and no futex on the list has
 * been woken up.
 */
static bool futex_any_woken(struct futex_vector *vs, unsigned int count)
{
	for (; count; count--, vs++) {
		if (!READ_ONCE(vs->q.lock_ptr))
			return true;
	}
	return false;
}

/*
 * WolKernel: the FUTEX_WAIT adaptive spin for futex_waitv() too, scored on
 * the first futex's bucket. Wine/Proton fsync hand-offs between game threads
 * land within a few microseconds. Off: futex.waitv_spin=0 (or spin_ns=0).
 */
static bool futex_waitv_spin_on __read_mostly = true;
module_param_named(waitv_spin, futex_waitv_spin_on, bool, 0644);

static void futex_spin_wait_multiple(struct futex_vector *vs, unsigned int count)
{
	struct futex_hash_bucket *hb;
	spinlock_t *lock;
	unsigned int score;
	u64 budget, end;
	bool woke;

	if (!READ_ONCE(futex_waitv_spin_on) || num_online_cpus() < 2)
		return;
	/* A private hash is freed by RCU after a rehash: keep hb valid. */
	guard(rcu)();
	lock = READ_ONCE(vs[0].q.lock_ptr);
	if (!lock)
		return;
	hb = container_of(lock, struct futex_hash_bucket, lock);
	score = READ_ONCE(hb->spin_score);
	budget = futex_spin_budget(READ_ONCE(futex_spin_ns), score,
				   single_task_running());
	if (!budget) {
		if (!score && !(get_random_u32() & 63))
			WRITE_ONCE(hb->spin_score, 1);
		return;
	}
	end = local_clock() + budget;
	while (!futex_any_woken(vs, count) && !need_resched() &&
	       !signal_pending(current) && local_clock() < end)
		cpu_relax();
	woke = futex_any_woken(vs, count);
	if (futex_spin_next(score, woke) != score)
		WRITE_ONCE(hb->spin_score, futex_spin_next(score, woke));
}

static void futex_sleep_multiple(struct futex_vector *vs, unsigned int count,
				 struct hrtimer_sleeper *to)
{
	u64 start;

	if (to && !hrtimer_sleeper_task_get(to))
		return;

	if (futex_any_woken(vs, count))
		return;

	if (!READ_ONCE(futex_prespin_on)) {
		futex_spin_wait_multiple(vs, count);
		if (futex_any_woken(vs, count))
			return;
	}

	start = futex_sleep_begin();
	schedule();
	futex_sleep_learn(start);
}

/**
 * futex_wait_multiple - Prepare to wait on and enqueue several futexes
 * @vs:		The list of futexes to wait on
 * @count:	The number of objects
 * @to:		Timeout before giving up and returning to userspace
 *
 * Entry point for the FUTEX_WAIT_MULTIPLE futex operation, this function
 * sleeps on a group of futexes and returns on the first futex that is
 * wake, or after the timeout has elapsed.
 *
 * Return:
 *  - >=0 - Hint to the futex that was awoken
 *  - <0  - On error
 */
int futex_wait_multiple(struct futex_vector *vs, unsigned int count,
			struct hrtimer_sleeper *to)
{
	int ret, hint = 0;

	if (to)
		hrtimer_sleeper_start_expires(to, HRTIMER_MODE_ABS);

	while (1) {
		ret = futex_wait_multiple_setup(vs, count, &hint);
		if (ret) {
			if (ret > 0) {
				/* A futex was woken during setup */
				ret = hint;
			}
			return ret;
		}

		futex_sleep_multiple(vs, count, to);

		__set_current_state(TASK_RUNNING);

		ret = futex_unqueue_multiple(vs, count);
		if (ret >= 0)
			return ret;

		if (to && !hrtimer_sleeper_task_get(to))
			return -ETIMEDOUT;
		else if (signal_pending(current))
			return -ERESTARTSYS;
		/*
		 * The final case is a spurious wakeup, for
		 * which just retry.
		 */
	}
}

/**
 * futex_wait_setup() - Prepare to wait on a futex
 * @uaddr:	the futex userspace address
 * @val:	the expected value
 * @flags:	futex flags (FLAGS_SHARED, etc.)
 * @q:		the associated futex_q
 * @key2:	the second futex_key if used for requeue PI
 * @task:	Task queueing this futex
 *
 * Setup the futex_q and locate the hash_bucket.  Get the futex value and
 * compare it with the expected value.  Handle atomic faults internally.
 * Return with the hb lock held on success, and unlocked on failure.
 *
 * Return:
 *  -  0 - uaddr contains val and hb has been locked;
 *  - <0 - On error and the hb is unlocked. A possible reason: the uaddr can not
 *	   be read, does not contain the expected value or is not properly aligned.
 */
int futex_wait_setup(u32 __user *uaddr, u32 val, unsigned int flags,
		     struct futex_q *q, union futex_key *key2,
		     struct task_struct *task)
{
	u32 uval;
	int ret;

	/*
	 * Access the page AFTER the hash-bucket is locked.
	 * Order is important:
	 *
	 *   Userspace waiter: val = var; if (cond(val)) futex_wait(&var, val);
	 *   Userspace waker:  if (cond(var)) { var = new; futex_wake(&var); }
	 *
	 * The basic logical guarantee of a futex is that it blocks ONLY
	 * if cond(var) is known to be true at the time of blocking, for
	 * any cond.  If we locked the hash-bucket after testing *uaddr, that
	 * would open a race condition where we could block indefinitely with
	 * cond(var) false, which would violate the guarantee.
	 *
	 * On the other hand, we insert q and release the hash-bucket only
	 * after testing *uaddr.  This guarantees that futex_wait() will NOT
	 * absorb a wakeup if *uaddr does not match the desired values
	 * while the syscall executes.
	 */
retry:
	ret = futex_key_get(uaddr, flags, &q->key, FUTEX_READ);
	if (unlikely(ret != 0))
		return ret;

retry_private:
	if (1) {
		CLASS(hbr, hbr)(&q->key);
		auto hb = hbr.hb;

		futex_q_lock(q, hb);

		ret = futex_get_value_locked(&uval, uaddr);

		if (ret) {
			futex_q_unlock(hb);
			__release(q->lock_ptr);

			ret = get_user(uval, uaddr);
			if (ret)
				return ret;

			if (!(flags & FLAGS_SHARED))
				goto retry_private;

			goto retry;
		}

		if (uval != val) {
			futex_q_unlock(hb);
			__release(q->lock_ptr);
			return -EWOULDBLOCK;
		}

		if (key2 && futex_match(&q->key, key2)) {
			futex_q_unlock(hb);
			__release(q->lock_ptr);
			return -EINVAL;
		}

		/*
		 * The task state is guaranteed to be set before another task can
		 * wake it. set_current_state() is implemented using smp_store_mb() and
		 * futex_queue() calls spin_unlock() upon completion, both serializing
		 * access to the hash list and forcing another memory barrier.
		 */
		if (task == current)
			set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
		futex_queue(q, hb, task);
	}

	return ret;
}

int __futex_wait(u32 __user *uaddr, unsigned int flags, u32 val,
		 struct hrtimer_sleeper *to, u32 bitset)
{
	struct futex_q q __uninitialized;
	int ret;

	if (!bitset)
		return -EINVAL;

	if (READ_ONCE(futex_precheck_on) &&
	    (futex_value_differs(uaddr, val) || futex_wait_prespin(uaddr, val)))
		return -EWOULDBLOCK;

	q = futex_q_init;
	q.bitset = bitset;

retry:
	/*
	 * Prepare to wait on uaddr. On success, it holds hb->lock and q
	 * is initialized.
	 */
	ret = futex_wait_setup(uaddr, val, flags, &q, NULL, current);
	if (ret)
		return ret;

	/* futex_queue and wait for wakeup, timeout, or a signal. */
	futex_do_wait(&q, to);

	/* If we were woken (and unqueued), we succeeded, whatever. */
	if (!futex_unqueue(&q))
		return 0;

	if (to && !hrtimer_sleeper_task_get(to))
		return -ETIMEDOUT;

	/*
	 * We expect signal_pending(current), but we might be the
	 * victim of a spurious wakeup as well.
	 */
	if (!signal_pending(current))
		goto retry;

	return -ERESTARTSYS;
}

int futex_wait(u32 __user *uaddr, unsigned int flags, u32 val, ktime_t *abs_time, u32 bitset)
{
	struct hrtimer_sleeper timeout __uninitialized, *to;
	struct restart_block *restart;
	int ret;

	to = futex_setup_timer(abs_time, &timeout, flags,
			       current->timer_slack_ns);

	ret = __futex_wait(uaddr, flags, val, to, bitset);

	/* No timeout, nothing to clean up. */
	if (!to)
		return ret;

	hrtimer_cancel(&to->timer);
	destroy_hrtimer_on_stack(&to->timer);

	if (ret == -ERESTARTSYS) {
		restart = &current->restart_block;
		restart->futex.uaddr = uaddr;
		restart->futex.val = val;
		restart->futex.time = *abs_time;
		restart->futex.bitset = bitset;
		restart->futex.flags = flags | FLAGS_HAS_TIMEOUT;

		return set_restart_fn(restart, futex_wait_restart);
	}

	return ret;
}

static long futex_wait_restart(struct restart_block *restart)
{
	u32 __user *uaddr = restart->futex.uaddr;
	ktime_t *tp = NULL;

	if (restart->futex.flags & FLAGS_HAS_TIMEOUT)
		tp = &restart->futex.time;

	restart->fn = do_no_restart_syscall;

	return (long)futex_wait(uaddr, restart->futex.flags,
				restart->futex.val, tp, restart->futex.bitset);
}

