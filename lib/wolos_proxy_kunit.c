// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Priority inversion through a struct mutex: a SCHED_IDLE holder, a nice 0 hog
 * on its CPU and a nice -10 waiter. Without proxy execution the holder gets
 * about 0.3% of the CPU while the waiter sleeps; with it the waiter's scheduling
 * context runs the holder. Each case logs the waiter's latency; max_ms > 0 makes
 * the latency a pass/fail bound (tools/testing/selftests/wolos/proxy_inversion.sh).
 * Needs a real kernel with at least 3 CPUs; skipped elsewhere.
 */
#include <kunit/test.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/types.h>
#include <uapi/linux/sched/types.h>

static unsigned int max_ms;
module_param(max_ms, uint, 0644);
MODULE_PARM_DESC(max_ms, "Fail a case when the waiter takes longer (0 = only log)");

#define HOLD_NS		(20 * NSEC_PER_MSEC)
#define GIVE_UP_NS	(20 * NSEC_PER_SEC)

struct pi_ctx {
	struct mutex lock;
	struct completion held, holder_done, waiter_done;
	int holder_cpu;
	bool owner_sleeps;
	u64 wait_ns;
};

static void burn_cpu_time(u64 ns)
{
	u64 start = current->se.sum_exec_runtime, wall = ktime_get_ns();

	while (current->se.sum_exec_runtime - start < ns &&
	       ktime_get_ns() - wall < GIVE_UP_NS) {
		cpu_relax();
		cond_resched();
	}
}

static int holder_fn(void *arg)
{
	struct sched_attr attr = { .size = sizeof(attr), .sched_policy = SCHED_IDLE };
	struct pi_ctx *c = arg;

	sched_setattr_nocheck(current, &attr);
	mutex_lock(&c->lock);
	complete(&c->held);
	if (c->owner_sleeps)
		msleep(20);	/* the waiter arrives while the owner is off the runqueue */
	burn_cpu_time(HOLD_NS);
	mutex_unlock(&c->lock);
	complete(&c->holder_done);
	return 0;
}

static int hog_fn(void *arg)
{
	while (!kthread_should_stop()) {
		cpu_relax();
		cond_resched();
	}
	return 0;
}

static int waiter_fn(void *arg)
{
	struct pi_ctx *c = arg;
	u64 t0;

	sched_set_normal(current, -10);
	t0 = ktime_get_ns();
	mutex_lock(&c->lock);
	c->wait_ns = ktime_get_ns() - t0;
	mutex_unlock(&c->lock);
	complete(&c->waiter_done);
	return 0;
}

static void run_case(struct kunit *t, int waiter_cpu, int holder_cpu, bool owner_sleeps)
{
	struct task_struct *holder, *hog, *waiter;
	struct pi_ctx *c = kunit_kzalloc(t, sizeof(*c), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(t, c);
	if (!cpu_online(waiter_cpu) || !cpu_online(holder_cpu))
		kunit_skip(t, "needs CPUs %d and %d", waiter_cpu, holder_cpu);
	mutex_init(&c->lock);
	init_completion(&c->held);
	init_completion(&c->holder_done);
	init_completion(&c->waiter_done);
	c->holder_cpu = holder_cpu;
	c->owner_sleeps = owner_sleeps;

	holder = kthread_create(holder_fn, c, "wpi-holder");
	hog = kthread_create(hog_fn, c, "wpi-hog");
	waiter = kthread_create(waiter_fn, c, "wpi-waiter");
	KUNIT_ASSERT_FALSE(t, IS_ERR(holder) || IS_ERR(hog) || IS_ERR(waiter));
	kthread_bind(holder, holder_cpu);
	kthread_bind(hog, holder_cpu);
	kthread_bind(waiter, waiter_cpu);

	wake_up_process(holder);
	wait_for_completion(&c->held);
	wake_up_process(hog);
	if (!owner_sleeps)
		msleep(2);	/* hog on CPU, holder runnable and starving */
	wake_up_process(waiter);

	wait_for_completion(&c->waiter_done);
	wait_for_completion(&c->holder_done);
	kthread_stop(hog);

	kunit_info(t, "wolos_proxy: waiter_ns=%llu", c->wait_ns);
	if (max_ms)
		KUNIT_EXPECT_LE(t, c->wait_ns, (u64)max_ms * NSEC_PER_MSEC);
}

static void same_cpu(struct kunit *t)
{
	run_case(t, 1, 1, false);
}

static void other_cpu(struct kunit *t)
{
	run_case(t, 2, 1, false);
}

static void sleeping_owner(struct kunit *t)
{
	run_case(t, 1, 1, true);
}

static struct kunit_case wolos_proxy_cases[] = {
	KUNIT_CASE_SLOW(same_cpu),
	KUNIT_CASE_SLOW(other_cpu),
	KUNIT_CASE_SLOW(sleeping_owner),
	{}
};

static struct kunit_suite wolos_proxy_suite = {
	.name = "wolos_proxy",
	.test_cases = wolos_proxy_cases,
};
kunit_test_suite(wolos_proxy_suite);
MODULE_DESCRIPTION("WolKernel priority inversion test for proxy execution");
MODULE_LICENSE("GPL");
