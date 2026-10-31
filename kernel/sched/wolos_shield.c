// SPDX-License-Identifier: GPL-2.0
/*
 * WolOS shield: the frontmost app's busiest thread is the serial part of every
 * frame, so keep everything else off its core. Every 250 ms pick that thread
 * from sum_exec_runtime deltas and give it the best core (largest L3, then
 * capacity, then ITMT priority). While it holds the core, other fair tasks,
 * unbound workqueues and non-GPU, non-input device IRQs stay on the other CPUs.
 * /sys/kernel/wolos/shield (0/1, default 0); shield_status prints "tid cpu".
 * With sched_ext attached only the IRQ and workqueue parts apply; the BPF
 * scheduler reads shield_status.
 */
#include <linux/cacheinfo.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqnr.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/pid.h>
#include <linux/sched/isolation.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/topology.h>
#include <linux/wolos.h>
#include <linux/wolos_policy.h>
#include <linux/workqueue.h>

#include "sched.h"

#define SHIELD_PERIOD		msecs_to_jiffies(250)
#define SHIELD_MIN_DELTA_NS	(25 * NSEC_PER_MSEC)	/* 10% of a core over the period */
#define SHIELD_SWITCH_PCT	125			/* a rival must beat the holder by 25% */
#define SHIELD_MAX_THREADS	512
#define SHIELD_MAX_EVICT	16

DEFINE_STATIC_KEY_FALSE(wolos_shield_on);

struct shield_thread {
	pid_t tid;
	u64 runtime;
};

struct shield_irq {
	struct list_head node;
	unsigned int irq;
	struct cpumask mask;
};

static struct {
	struct mutex lock;		/* enable/disable against the work */
	bool enabled;
	struct delayed_work work;
	struct task_struct *crit;	/* holds a reference; hot path only compares it */
	int cpu;			/* the crit thread's CPU, -1 = none */
	struct cpumask shielded;	/* cpu and its SMT siblings */
	struct cpumask others;		/* every other CPU */
	struct list_head irqs;		/* IRQs moved, with their old masks */
	int tgid;
	int nr;				/* entries of thr[] for tgid */
	struct shield_thread thr[SHIELD_MAX_THREADS];	/* last sample */
	struct shield_thread next[SHIELD_MAX_THREADS];	/* sample being taken */
	u64 delta[SHIELD_MAX_THREADS];
} shield = {
	.lock = __MUTEX_INITIALIZER(shield.lock),
	.cpu = -1,
	.irqs = LIST_HEAD_INIT(shield.irqs),
};

/* select_task_rq_fair() result under an active shield. */
int wolos_shield_place(struct task_struct *p, int prev_cpu, int new_cpu)
{
	int scpu = READ_ONCE(shield.cpu), cpu;

	if (scpu < 0)
		return new_cpu;
	if (p == READ_ONCE(shield.crit))
		return cpumask_test_cpu(scpu, p->cpus_ptr) ? scpu : new_cpu;
	if (!cpumask_test_cpu(new_cpu, &shield.shielded) || p->nr_cpus_allowed == 1)
		return new_cpu;
	if (!cpumask_test_cpu(prev_cpu, &shield.shielded) &&
	    cpumask_test_cpu(prev_cpu, p->cpus_ptr))
		return prev_cpu;
	cpu = cpumask_any_and_distribute(&shield.others, p->cpus_ptr);
	return cpu < nr_cpu_ids ? cpu : new_cpu;
}

/* can_migrate_task(): the balancer does not fill the shielded core. */
bool wolos_shield_blocks_pull(struct task_struct *p, int dst_cpu)
{
	return READ_ONCE(shield.cpu) >= 0 && p != READ_ONCE(shield.crit) &&
	       cpumask_test_cpu(dst_cpu, &shield.shielded);
}

static u32 cpu_llc_kb(int cpu)
{
	struct cpu_cacheinfo *ci = get_cpu_cacheinfo(cpu);

	if (!ci || !ci->info_list || !ci->num_leaves)
		return 0;
	return ci->info_list[ci->num_leaves - 1].size >> 10;
}

/* Called with cpus_read_lock(). */
static int shield_pick_cpu(const struct cpumask *allowed)
{
	struct wolos_cpu_facts *f __free(kfree) =
		kcalloc(nr_cpu_ids, sizeof(*f), GFP_KERNEL);
	int cpu, best;

	if (!f)
		return -1;
	for_each_online_cpu(cpu) {
		f[cpu].allowed = cpu && cpumask_test_cpu(cpu, allowed); /* CPU 0 keeps its chores */
		f[cpu].llc_kb = cpu_llc_kb(cpu);
		f[cpu].capacity = arch_scale_cpu_capacity(cpu);
		f[cpu].prio = arch_asym_cpu_priority(cpu);
	}
	best = wolos_shield_pick_cpu(f, nr_cpu_ids);
	return best;
}

static void shield_irqs_move(void)
{
	struct cpumask *new_mask __free(kfree) = kmalloc(cpumask_size(), GFP_KERNEL);
	struct irq_desc *desc;
	unsigned int irq;

	if (!new_mask)
		return;
	for_each_irq_desc(irq, desc) {
		struct irq_data *d = irq_desc_get_irq_data(desc);
		const struct cpumask *cur = irq_data_get_affinity_mask(d);
		struct shield_irq *si;

		if (!desc->action || irqd_is_per_cpu(d) || irqd_affinity_is_managed(d) ||
		    !irq_can_set_affinity(irq) ||
		    wolos_shield_irq_keep(desc->action->name) ||
		    !cpumask_intersects(cur, &shield.shielded))
			continue;
		si = kmalloc(sizeof(*si), GFP_KERNEL);
		if (!si)
			return;
		si->irq = irq;
		cpumask_copy(&si->mask, cur);
		if (!cpumask_and(new_mask, cur, &shield.others))
			cpumask_copy(new_mask, &shield.others);
		if (irq_set_affinity(irq, new_mask)) {
			kfree(si);
			continue;
		}
		list_add(&si->node, &shield.irqs);
	}
}

static void shield_irqs_restore(void)
{
	struct shield_irq *si, *n;

	list_for_each_entry_safe(si, n, &shield.irqs, node) {
		irq_set_affinity(si->irq, &si->mask);
		list_del(&si->node);
		kfree(si);
	}
}

static void shield_clear(void)
{
	struct task_struct *crit = shield.crit;

	if (shield.cpu < 0)
		return;
	static_branch_disable(&wolos_shield_on);
	WRITE_ONCE(shield.cpu, -1);
	WRITE_ONCE(shield.crit, NULL);
	scoped_guard(cpus_read_lock) {
		shield_irqs_restore();
		workqueue_unbound_housekeeping_update(housekeeping_cpumask(HK_TYPE_DOMAIN));
	}
	if (crit)
		put_task_struct(crit);
}

/* Reserve a core for @p. False when this machine cannot spare one. */
static bool shield_take_core(struct task_struct *p)
{
	int cpu;

	scoped_guard(cpus_read_lock) {
		cpu = shield_pick_cpu(p->cpus_ptr);
		if (cpu < 0)
			return false;
		cpumask_copy(&shield.shielded, cpu_smt_mask(cpu));
		cpumask_andnot(&shield.others, cpu_online_mask, &shield.shielded);
		if (!wolos_shield_enough_cpus(num_online_cpus(),
					      cpumask_weight(&shield.shielded)))
			return false;
		WRITE_ONCE(shield.cpu, cpu);
		shield_irqs_move();
		workqueue_unbound_housekeeping_update(&shield.others);
	}
	static_branch_enable(&wolos_shield_on);
	return true;
}

/* Move every other movable fair task off the shielded CPUs, then @crit on. */
static void shield_settle(struct task_struct *crit)
{
	struct task_struct *ev[SHIELD_MAX_EVICT];
	int cpu, i, n = 0;

	if (scx_enabled())
		return;
	for_each_cpu(cpu, &shield.shielded) {
		struct rq *rq = cpu_rq(cpu);
		struct task_struct *p;

		guard(rq_lock_irqsave)(rq);
		list_for_each_entry(p, &rq->cfs_tasks, se.group_node) {
			if (n == SHIELD_MAX_EVICT)
				break;
			if (p == crit || p->nr_cpus_allowed == 1 ||
			    is_migration_disabled(p) || task_is_blocked(p))
				continue;
			get_task_struct(p);
			ev[n++] = p;
		}
	}
	for (i = 0; i < n; i++) {
		int dst = cpumask_any_and_distribute(&shield.others, ev[i]->cpus_ptr);

		if (dst < nr_cpu_ids)
			wolos_shield_move_task(ev[i], dst);
		put_task_struct(ev[i]);
	}
	if (task_cpu(crit) != shield.cpu)
		wolos_shield_move_task(crit, shield.cpu);
}

/* Sample the thread group, return the thread that should hold the core (with a reference). */
static struct task_struct *shield_sample(int tgid)
{
	struct task_struct *t, *leader, *best = NULL;
	int i = 0, cur = -1, pick;

	if (tgid != shield.tgid) {
		shield.tgid = tgid;
		shield.nr = 0;
	}
	guard(rcu)();
	leader = find_task_by_pid_ns(tgid, &init_pid_ns);
	if (!leader)
		return NULL;
	for_each_thread(leader, t) {
		u64 rt = READ_ONCE(t->se.sum_exec_runtime);
		int j;

		if (i == SHIELD_MAX_THREADS)
			break;
		shield.delta[i] = 0;
		/* Threads keep their order between samples: try the same slot first. */
		if (i < shield.nr && shield.thr[i].tid == t->pid) {
			j = i;
		} else {
			for (j = 0; j < shield.nr; j++)
				if (shield.thr[j].tid == t->pid)
					break;
		}
		if (j < shield.nr)
			shield.delta[i] = rt - shield.thr[j].runtime;
		if (t == shield.crit)
			cur = i;
		shield.next[i].tid = t->pid;
		shield.next[i].runtime = rt;
		i++;
	}
	memcpy(shield.thr, shield.next, i * sizeof(shield.thr[0]));
	shield.nr = i;
	pick = wolos_shield_pick_thread(shield.delta, i, cur, SHIELD_SWITCH_PCT,
					SHIELD_MIN_DELTA_NS);
	if (pick < 0)
		return NULL;
	i = 0;
	for_each_thread(leader, t) {
		if (i++ == pick) {
			best = t;
			get_task_struct(best);
			break;
		}
	}
	return best;
}

static void shield_work_fn(struct work_struct *w)
{
	struct task_struct *best;
	int tgid = wolos_foreground_tgid();

	guard(mutex)(&shield.lock);
	if (!shield.enabled)
		return;
	best = tgid ? shield_sample(tgid) : NULL;
	if (shield.cpu >= 0 && !cpu_online(shield.cpu))
		shield_clear();
	if (!best) {
		shield_clear();
	} else if (shield.cpu < 0 && !shield_take_core(best)) {
		put_task_struct(best);
	} else {
		struct task_struct *old = shield.crit;

		WRITE_ONCE(shield.crit, best);	/* takes over the reference */
		if (old)
			put_task_struct(old);
		shield_settle(best);
	}
	schedule_delayed_work(&shield.work, SHIELD_PERIOD);
}

static ssize_t shield_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(shield.enabled));
}

static ssize_t shield_store(struct kobject *k, struct kobj_attribute *a,
			    const char *buf, size_t n)
{
	bool on;
	int err = kstrtobool(buf, &on);

	if (err)
		return err;
	mutex_lock(&shield.lock);
	WRITE_ONCE(shield.enabled, on);
	if (on) {
		schedule_delayed_work(&shield.work, 0);
	} else {
		shield_clear();
		shield.tgid = 0;
	}
	mutex_unlock(&shield.lock);
	if (!on)
		cancel_delayed_work_sync(&shield.work);
	return n;
}

static ssize_t shield_status_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	pid_t tid = 0;
	int cpu;

	guard(mutex)(&shield.lock);
	cpu = shield.cpu;
	if (shield.crit)
		tid = shield.crit->pid;
	return sysfs_emit(buf, "%d %d\n", tid, cpu);
}

static struct kobj_attribute shield_attr = __ATTR_RW_MODE(shield, 0644);
static struct kobj_attribute shield_status_attr = __ATTR_RO(shield_status);
static struct attribute *shield_attrs[] = {
	&shield_attr.attr,
	&shield_status_attr.attr,
	NULL,
};
static const struct attribute_group shield_group = { .attrs = shield_attrs };

static int __init wolos_shield_init(void)
{
	INIT_DELAYED_WORK(&shield.work, shield_work_fn);
	return sysfs_create_group(wolos_kobj, &shield_group);
}
late_initcall(wolos_shield_init);
