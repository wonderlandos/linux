// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Exit of a process with a lot of anonymous memory.
 *
 *  - exit_mmap() zaps large anonymous VMAs in pieces through bend_fork_join()
 *    before the serial unmap_vmas() walk, which finds them empty.  Concurrent
 *    zaps under mmap_read_lock are what MADV_DONTNEED already does.
 *  - The last thread of the process only unmaps what user space can observe
 *    (files: ETXTBSY, umount, shmem), the anonymous rest goes to a worker.
 *    out_of_memory() waits for that worker, so memory on its way out is not
 *    a reason to kill.
 *
 * Switches, both 0 = off: wolos_mm.exit_async_min_mb, wolos_mm.exit_par_min_mb.
 */
#include <linux/aio.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/moduleparam.h>
#include <linux/oom.h>
#include <linux/sched/coredump.h>
#include <linux/sched/mm.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/wolos_bend.h>
#include <linux/workqueue.h>
#include "internal.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "wolos_mm."

static unsigned int exit_par_min_mb = 512;
module_param(exit_par_min_mb, uint, 0644);
MODULE_PARM_DESC(exit_par_min_mb, "Zap anonymous memory in parallel from this RSS (MiB, 0 off)");

static unsigned int exit_par_workers = 8;
module_param(exit_par_workers, uint, 0644);
MODULE_PARM_DESC(exit_par_workers, "Most CPUs a parallel zap uses");

static unsigned int exit_async_min_mb = 512;
module_param(exit_async_min_mb, uint, 0644);
MODULE_PARM_DESC(exit_async_min_mb, "Free anonymous memory after wait(2) from this RSS (MiB, 0 off)");

/* One piece of a parallel zap.  A multiple of PMD_SIZE: pieces of one VMA
 * share no page table page. */
#define EXIT_CHUNK	SZ_64M

struct exit_chunk {
	struct vm_area_struct *vma;
	unsigned long start, end;
};

struct exit_zap {
	struct mm_struct *mm;
	struct exit_chunk *c;
};

static bool exit_zap_vma_ok(const struct vm_area_struct *vma)
{
	return vma_is_anonymous(vma) && !is_vm_hugetlb_page(vma) &&
	       !(vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP)) &&
	       vma->vm_end - vma->vm_start >= EXIT_CHUNK;
}

static void exit_zap_fn(unsigned long s, unsigned long e, void *arg)
{
	struct exit_zap *z = arg;

	/* A writer is impossible on a dead mm; a skipped piece is left to
	 * unmap_vmas(), so this can never wait for the caller's read lock. */
	if (!mmap_read_trylock(z->mm))
		return;
	for (; s < e; s++)
		zap_exiting_vma_range(z->c[s].vma, z->c[s].start, z->c[s].end);
	mmap_read_unlock(z->mm);
}

/**
 * exit_mmap_zap_parallel - free large anonymous VMAs on idle CPUs
 * @mm: the dying mm, no notifiers left, mmap_read_lock held.
 *
 * Purely an optimisation: what is not done here unmap_vmas() does.
 */
void exit_mmap_zap_parallel(struct mm_struct *mm)
{
	unsigned int min_mb = READ_ONCE(exit_par_min_mb);
	unsigned long min_pages = (unsigned long)min_mb << (20 - PAGE_SHIFT);
	unsigned long n = 0, i = 0, addr;
	struct vm_area_struct *vma;
	struct exit_zap z = { .mm = mm };
	struct bend_job job = {
		.fn = exit_zap_fn, .arg = &z, .grain = 1,
		.max_workers = READ_ONCE(exit_par_workers),
	};
	VMA_ITERATOR(vmi, mm, 0);

	if (!min_mb || get_mm_counter(mm, MM_ANONPAGES) < min_pages ||
	    !bend_enabled())
		return;
	for_each_vma(vmi, vma)
		if (exit_zap_vma_ok(vma))
			n += DIV_ROUND_UP(vma->vm_end - vma->vm_start, EXIT_CHUNK) + 1;
	if (n < 2)
		return;
	z.c = kvmalloc_array(n, sizeof(*z.c), GFP_KERNEL | __GFP_NOWARN);
	if (!z.c)
		return;
	vma_iter_set(&vmi, 0);
	for_each_vma(vmi, vma) {
		if (!exit_zap_vma_ok(vma))
			continue;
		for (addr = vma->vm_start; addr < vma->vm_end;) {
			unsigned long end = min(ALIGN(addr + 1, EXIT_CHUNK), vma->vm_end);

			z.c[i++] = (struct exit_chunk){ vma, addr, end };
			addr = end;
		}
	}
	job.start = 0;
	job.end = i;
	bend_fork_join(&job);
	kvfree(z.c);
}

static struct workqueue_struct *teardown_wq;
static atomic_t teardown_mms = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(teardown_done_wq);

/* Unmap everything that is not plain anonymous memory.  False: do it all
 * in the caller (sealed VMA, or no memory to edit the tree). */
static bool exit_unmap_visible(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	unsigned long addr = 0;
	bool ok = true;
	VMA_ITERATOR(vmi, mm, 0);

	mmap_write_lock(mm);
	for_each_vma(vmi, vma) {
		if (vma_is_anonymous(vma) && !(vma->vm_flags & VM_SPECIAL))
			continue;
		addr = vma->vm_end;
		if (do_munmap(mm, vma->vm_start, addr - vma->vm_start, NULL)) {
			ok = false;
			break;
		}
		vma_iter_set(&vmi, addr);
		cond_resched();
	}
	mmap_write_unlock(mm);
	return ok;
}

/**
 * mm_teardown_prepare - can the rest of @mm go to the teardown worker?
 * @mm: mm_users just reached zero.
 *
 * Runs in the exiting task.  On true every file of @mm is released and the
 * caller queues __mmput() through mm_teardown_queue().
 */
bool mm_teardown_prepare(struct mm_struct *mm)
{
	unsigned int min_mb = READ_ONCE(exit_async_min_mb);
	unsigned long pages;

	if (!min_mb || !teardown_wq)
		return false;
	pages = get_mm_counter(mm, MM_ANONPAGES) + get_mm_counter(mm, MM_SWAPENTS);
	if (pages < (unsigned long)min_mb << (20 - PAGE_SHIFT))
		return false;
	/* KVM and friends tear down in mmu_notifier_release(); strict
	 * overcommit charges until vm_unacct_memory(); an OOM victim is
	 * waited for by exit_oom_victim(). */
	if (mm_has_notifiers(mm) || tsk_is_oom_victim(current) ||
	    sysctl_overcommit_memory == OVERCOMMIT_NEVER ||
	    mm->map_count > 65536)
		return false;

	exit_aio(mm);
	set_mm_exe_file(mm, NULL);
	return exit_unmap_visible(mm);
}

static void mm_teardown_fn(struct work_struct *work)
{
	struct mm_struct *mm = container_of(work, struct mm_struct, async_put_work);
	unsigned int noreclaim = memalloc_noreclaim_save();

	__mmput(mm);
	memalloc_noreclaim_restore(noreclaim);
	atomic_dec(&teardown_mms);
	wake_up_all(&teardown_done_wq);
}

void mm_teardown_queue(struct mm_struct *mm)
{
	atomic_inc(&teardown_mms);
	INIT_WORK(&mm->async_put_work, mm_teardown_fn);
	queue_work(teardown_wq, &mm->async_put_work);
}

/**
 * mm_teardown_wait - give memory on its way out a chance before an OOM kill
 *
 * Returns true when a teardown finished, so the allocation should retry.
 * Bounded: a stuck worker only delays the kill.
 */
bool mm_teardown_wait(void)
{
	int pending = atomic_read(&teardown_mms);

	if (!pending)
		return false;
	return wait_event_timeout(teardown_done_wq,
				  atomic_read(&teardown_mms) < pending,
				  5 * HZ) > 0;
}

static int __init wolos_exit_init(void)
{
	teardown_wq = alloc_workqueue("mm_teardown", WQ_UNBOUND | WQ_MEM_RECLAIM, 4);
	return 0;
}
core_initcall(wolos_exit_init);
