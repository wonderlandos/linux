// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit for the WolKernel one-semaphore semctl reads (included from ipc/sem.c).
 * A GETVAL/GETPID/GETNCNT/GETZCNT must return the right value and must not
 * push the array into global-lock mode (use_global_lock stays 0).
 */
#include <kunit/test.h>

static int sem_kunit_global_mode(struct kunit *test, int id)
{
	struct sem_array *sma;
	int v;

	rcu_read_lock();
	sma = sem_obtain_object_check(current->nsproxy->ipc_ns, id);
	KUNIT_ASSERT_FALSE(test, IS_ERR(sma));
	v = READ_ONCE(sma->use_global_lock);
	rcu_read_unlock();
	return v;
}

static void sem_kunit_op(struct kunit *test, int id, short num, short op)
{
	struct sembuf b = { .sem_num = num, .sem_op = op };

	KUNIT_ASSERT_EQ(test, __do_semtimedop(id, &b, 1, NULL,
					      current->nsproxy->ipc_ns), 0);
}

static void sem_read_keeps_simple_mode(struct kunit *test)
{
	struct ipc_namespace *ns = current->nsproxy->ipc_ns;
	bool saved = sem_read_one_lock;
	int id, i;

	id = ksys_semget(IPC_PRIVATE, 3, 0600);
	KUNIT_ASSERT_GE(test, id, 0);
	/* A new array starts in global mode; simple ops count it down to 0. */
	for (i = 0; i <= USE_GLOBAL_LOCK_HYSTERESIS; i++)
		sem_kunit_op(test, id, 1, 1);
	KUNIT_ASSERT_EQ(test, sem_kunit_global_mode(test, id), 0);

	sem_read_one_lock = true;
	KUNIT_EXPECT_EQ(test, semctl_main(ns, id, 1, GETVAL, NULL),
			USE_GLOBAL_LOCK_HYSTERESIS + 1);
	KUNIT_EXPECT_EQ(test, semctl_main(ns, id, 0, GETVAL, NULL), 0);
	KUNIT_EXPECT_EQ(test, semctl_main(ns, id, 1, GETPID, NULL),
			task_tgid_vnr(current));
	KUNIT_EXPECT_EQ(test, semctl_main(ns, id, 2, GETNCNT, NULL), 0);
	KUNIT_EXPECT_EQ(test, semctl_main(ns, id, 2, GETZCNT, NULL), 0);
	KUNIT_EXPECT_EQ(test, semctl_main(ns, id, 3, GETVAL, NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, sem_kunit_global_mode(test, id), 0);

	/* Off: the stock whole-array lock, which re-enters global mode. */
	sem_read_one_lock = false;
	KUNIT_EXPECT_EQ(test, semctl_main(ns, id, 1, GETVAL, NULL),
			USE_GLOBAL_LOCK_HYSTERESIS + 1);
	KUNIT_EXPECT_GT(test, sem_kunit_global_mode(test, id), 0);
	sem_read_one_lock = saved;

	KUNIT_EXPECT_EQ(test, ksys_semctl(id, 0, IPC_RMID, 0, IPC_64), 0);
}

static struct kunit_case sem_read_cases[] = {
	KUNIT_CASE(sem_read_keeps_simple_mode),
	{}
};

static struct kunit_suite sem_read_suite = {
	.name = "wolos_sem_read",
	.test_cases = sem_read_cases,
};
kunit_test_suite(sem_read_suite);
