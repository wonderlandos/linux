/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H

#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/mm_types.h>

#include <uapi/linux/futex.h>

struct inode;
struct task_struct;

/*
 * Futexes are matched on equal values of this key.
 * The key type depends on whether it's a shared or private mapping.
 * Don't rearrange members without looking at hash_futex().
 *
 * offset is aligned to a multiple of sizeof(u32) (== 4) by definition.
 * We use the two low order bits of offset to tell what is the kind of key :
 *  00 : Private process futex (PTHREAD_PROCESS_PRIVATE)
 *       (no reference on an inode or mm)
 *  01 : Shared futex (PTHREAD_PROCESS_SHARED)
 *	mapped on a file (reference on the underlying inode)
 *  10 : Shared futex (PTHREAD_PROCESS_SHARED)
 *       (but private mapping on an mm)
*/

#define FUT_OFF_INODE    1 /* We set bit 0 if shared key identifies an inode */
#define FUT_OFF_MMSHARED 2 /* We set bit 1 if shared key identifies an mm */

union futex_key {
	/* For FUT_OFF_INODE */
	struct {
		u64 i_seq;
		unsigned long pgoff;
		unsigned int offset;
		/* unsigned int node; */
	} shared;

	/* For FUT_OFF_MMSHARED or private */
	struct {
		union {
			struct mm_struct *mm; /* for private */
			u64 mm_seq; /* for FUT_OFF_MMSHARED */
		};
		unsigned long address;
		unsigned int offset;
		/* unsigned int node; */
	} private;

	struct {
		u64 ptr;
		unsigned long word;
		unsigned int offset;
		unsigned int node;	/* NOT hashed! */
	} both;
};

#define FUTEX_KEY_INIT (union futex_key) { .both = { .ptr = 0ULL } }

#ifdef CONFIG_FUTEX
enum {
	FUTEX_STATE_OK,
	FUTEX_STATE_EXITING,
	FUTEX_STATE_DEAD,
};

static inline void futex_init_task(struct task_struct *tsk)
{
	memset(&tsk->futex, 0, sizeof(tsk->futex));
	INIT_LIST_HEAD(&tsk->futex.pi_state_list);
	tsk->futex.state = FUTEX_STATE_OK;
	mutex_init(&tsk->futex.exit_mutex);
}

void futex_exit_recursive(struct task_struct *tsk);
void futex_exit_exec_release(struct task_struct *tsk);
void futex_exec_done(struct task_struct *tsk);

long do_futex(u32 __user *uaddr, int op, u32 val, ktime_t *timeout,
	      u32 __user *uaddr2, u32 val2, u32 val3);
int futex_hash_prctl(unsigned long arg2, unsigned long arg3, unsigned long arg4);

/* WolKernel spin policy, shared with ntsync. See kernel/futex/futex.h. */
#define FUTEX_SPIN_SCORE_MAX	8U


static inline unsigned int futex_spin_next(unsigned int score, bool woke)
{
	if (woke)
		return score >= FUTEX_SPIN_SCORE_MAX ? FUTEX_SPIN_SCORE_MAX : score + 1;
	return score / 2;
}

static inline u64 futex_prespin_budget(unsigned int spin_ns, unsigned int score,
				       bool cpu_alone)
{
	return score && cpu_alone ? spin_ns : 0;
}

/*
 * A sleep this short says a spin may catch the next wake, so it rearms a task
 * that stopped spinning. It does not raise a score that is still spinning: a
 * wait a little over the budget would then miss and burn the budget every time.
 */
static inline unsigned int futex_spin_slept(unsigned int score, u64 slept_ns,
					    unsigned int spin_ns)
{
	return !score && slept_ns < 2ULL * spin_ns ? 1 : score;
}

/*
 * Passes of a spin between two clock reads, for a pass over @nr words. In a
 * guest a read costs more than polling one word, and a spin only needs the
 * clock to give up, so it overruns its budget by at most these few passes. A
 * pass over many words is long enough to carry a read.
 */
#define FUTEX_SPIN_POLLS	16U

static inline unsigned int futex_spin_polls(unsigned int polls, unsigned int nr)
{
	return nr && polls > nr ? polls / nr : 1;
}

#ifdef CONFIG_FUTEX_PRIVATE_HASH
int futex_hash_allocate_default(void);
void futex_hash_free(struct mm_struct *mm);
#else  /* CONFIG_FUTEX_PRIVATE_HASH */
static inline int futex_hash_allocate_default(void) { return 0; }
static inline int futex_hash_free(struct mm_struct *mm) { return 0; }
#endif /* !CONFIG_FUTEX_PRIVATE_HASH */

#else  /* CONFIG_FUTEX */
static inline void futex_init_task(struct task_struct *tsk) { }
static inline void futex_exit_recursive(struct task_struct *tsk) { }
static inline void futex_exit_exec_release(struct task_struct *tsk) { }
static inline void futex_exec_done(struct task_struct *tsk) { }
static inline long do_futex(u32 __user *uaddr, int op, u32 val, ktime_t *timeout,
			    u32 __user *uaddr2, u32 val2, u32 val3)
{
	return -EINVAL;
}
static inline int futex_hash_prctl(unsigned long arg2, unsigned long arg3, unsigned long arg4)
{
	return -EINVAL;
}
static inline int futex_hash_allocate_default(void) { return 0; }
static inline int futex_hash_free(struct mm_struct *mm) { return 0; }
#endif /* !CONFIG_FUTEX */

#ifdef CONFIG_FUTEX_ROBUST_UNLOCK
#include <asm/futex_robust.h>

void futex_reset_cs_ranges(struct futex_mm_data *fd);
void __futex_fixup_robust_unlock(struct pt_regs *regs, struct futex_unlock_cs_range *csr);

static inline bool futex_within_robust_unlock(struct pt_regs *regs,
					      struct futex_unlock_cs_range *csr)
{
	unsigned long ip = instruction_pointer(regs);

	return ip >= csr->start_ip && ip < csr->start_ip + csr->len;
}

static inline void futex_fixup_robust_unlock(struct pt_regs *regs)
{
	struct futex_unlock_cs_range *csr;

	/*
	 * Avoid dereferencing current->mm if not returning from interrupt.
	 * current->rseq.event is going to be used subsequently, so bringing the
	 * cache line in is not a big deal.
	 */
	if (!current->rseq.event.user_irq)
		return;

	csr = current->mm->futex.unlock.cs_ranges;

	/* The loop is optimized out for !COMPAT */
	for (int r = 0; r < FUTEX_ROBUST_MAX_CS_RANGES; r++, csr++) {
		if (unlikely(futex_within_robust_unlock(regs, csr))) {
			__futex_fixup_robust_unlock(regs, csr);
			return;
		}
	}
}

static inline void futex_set_vdso_cs_range(struct futex_mm_data *fd, unsigned int idx,
					   unsigned long start, unsigned long end, bool sz32)
{
	fd->unlock.cs_ranges[idx].start_ip = start;
	fd->unlock.cs_ranges[idx].len = end - start;
	fd->unlock.cs_ranges[idx].pop_size32 = sz32;
}
#else /* CONFIG_FUTEX_ROBUST_UNLOCK */
static inline void futex_fixup_robust_unlock(struct pt_regs *regs) { }
#endif /* !CONFIG_FUTEX_ROBUST_UNLOCK */

#ifdef CONFIG_FUTEX
void futex_mm_init(struct mm_struct *mm);
#else
static inline void futex_mm_init(struct mm_struct *mm) { }
#endif /* !CONFIG_FUTEX */

#endif /* _LINUX_FUTEX_H */
