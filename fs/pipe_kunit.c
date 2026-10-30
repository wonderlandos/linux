// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit for the WolKernel pipe wake check (included from fs/pipe.c).
 * pipe_wake_needed() must report a sleeper whenever one is queued, and a
 * blocked reader must still be woken by a write with the check on.
 */
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>

static void pipe_wake_needed_tracks_sleepers(struct kunit *test)
{
	struct wait_queue_head wq;
	struct wait_queue_entry e;
	bool saved = pipe_wake_check;

	init_waitqueue_head(&wq);
	init_waitqueue_entry(&e, current);
	pipe_wake_check = true;
	KUNIT_EXPECT_FALSE(test, pipe_wake_needed(&wq));
	add_wait_queue(&wq, &e);
	KUNIT_EXPECT_TRUE(test, pipe_wake_needed(&wq));
	remove_wait_queue(&wq, &e);
	KUNIT_EXPECT_FALSE(test, pipe_wake_needed(&wq));
	pipe_wake_check = false;	/* off: always wake, the stock behaviour */
	KUNIT_EXPECT_TRUE(test, pipe_wake_needed(&wq));
	pipe_wake_check = saved;
}

struct pipe_wake_ctx {
	struct file *rd;
	struct completion started, done;
	char buf[8];
	ssize_t ret;
};

static int pipe_wake_reader(void *data)
{
	struct pipe_wake_ctx *c = data;
	loff_t pos = 0;

	complete(&c->started);
	c->ret = kernel_read(c->rd, c->buf, sizeof(c->buf), &pos);
	complete(&c->done);
	return 0;
}

/* Many rounds: reader blocks first (or not yet), writer writes; no round may hang. */
static void pipe_wake_blocked_reader(struct kunit *test)
{
	struct file *f[2];
	int i;

	KUNIT_ASSERT_EQ(test, create_pipe_files(f, 0), 0);
	for (i = 0; i < 200; i++) {
		struct pipe_wake_ctx c = { .rd = f[0] };
		struct task_struct *t;
		loff_t pos = 0;

		init_completion(&c.started);
		init_completion(&c.done);
		t = kthread_run(pipe_wake_reader, &c, "pipe_wake_kunit");
		KUNIT_ASSERT_FALSE(test, IS_ERR(t));
		wait_for_completion(&c.started);
		if (i & 1)
			msleep(1);	/* odd rounds: the reader is surely asleep */
		KUNIT_ASSERT_EQ(test, kernel_write(f[1], "x", 1, &pos), 1);
		KUNIT_ASSERT_NE_MSG(test, wait_for_completion_timeout(&c.done, 5 * HZ), 0,
				    "reader not woken in round %d", i);
		KUNIT_EXPECT_EQ(test, c.ret, 1);
	}
	fput(f[0]);
	fput(f[1]);
}

static struct kunit_case pipe_wake_cases[] = {
	KUNIT_CASE(pipe_wake_needed_tracks_sleepers),
	KUNIT_CASE(pipe_wake_blocked_reader),
	{}
};

static struct kunit_suite pipe_wake_suite = {
	.name = "wolos_pipe_wake",
	.test_cases = pipe_wake_cases,
};
kunit_test_suite(pipe_wake_suite);
