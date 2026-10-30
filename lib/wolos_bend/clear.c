// SPDX-License-Identifier: GPL-2.0
/*
 * wolos_bend user: clear a large user folio on idle CPUs.
 * mm/memory.c clear_contig_highpages() calls this first.
 */
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/wolos_bend.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "wolos_bend."

/* Below this many KiB the clear stays on the faulting CPU (0: never split). */
static unsigned int clear_min_kb = 4096;
module_param(clear_min_kb, uint, 0644);
MODULE_PARM_DESC(clear_min_kb, "Clear user folios of at least this size on idle CPUs (KiB, 0 off)");

static unsigned int clear_grain_kb = 512;
module_param(clear_grain_kb, uint, 0644);
MODULE_PARM_DESC(clear_grain_kb, "Smallest piece of a parallel clear (KiB)");

struct bend_clear {
	struct page *page;
	unsigned long addr;
};

static void bend_clear_fn(unsigned long s, unsigned long e, void *arg)
{
	const struct bend_clear *c = arg;
	const unsigned long unit = preempt_model_preemptible() ?
				   e - s : PROCESS_PAGES_NON_PREEMPT_BATCH;
	unsigned long i, n;

	for (i = s; i < e; i += n) {
		n = min(unit, e - i);
		clear_user_highpages(c->page + i, c->addr + i * PAGE_SIZE, n);
		cond_resched();
	}
}

/* Piece size in pages for a parallel clear of @nr pages; 0 = serial. */
unsigned long bend_clear_pages_grain(unsigned long nr)
{
	unsigned long min = READ_ONCE(clear_min_kb) / (PAGE_SIZE / SZ_1K);
	unsigned long grain = max(1UL, READ_ONCE(clear_grain_kb) / (PAGE_SIZE / SZ_1K));

	if (!min || nr < min || nr < 2 * grain || !bend_enabled())
		return 0;
	return grain;
}

bool bend_clear_user_highpages(struct page *page, unsigned long addr, unsigned int nr)
{
	struct bend_clear c = { .page = page, .addr = addr };
	struct bend_job job = { .fn = bend_clear_fn, .arg = &c, .start = 0, .end = nr };

	job.grain = bend_clear_pages_grain(nr);
	if (!job.grain)
		return false;
	bend_fork_join(&job);
	return true;
}
