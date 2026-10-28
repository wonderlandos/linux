// SPDX-License-Identifier: GPL-2.0
/*
 * futex_pingpong: round-trip latency of a FUTEX_WAIT/FUTEX_WAKE hand-off between two threads,
 * the pattern game engines and Wine use for job queues. Measures the futex.spin_ns patch:
 *   echo 0 > /sys/module/futex/parameters/spin_ns; ./futex_pingpong   (upstream behaviour)
 *   echo 4000 > ...;                              ./futex_pingpong   (WolKernel default)
 * Usage: futex_pingpong [iterations] [same-cpu: 0|1]. Prints ns per round trip (median of 5 runs).
 * Exit 0 always (benchmark); KSFT_SKIP if futex is missing.
 */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static _Atomic int word;
static long iters = 200000;

static void fwait(int val)
{
	while (atomic_load(&word) == val)
		syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}

static void fset(int val)
{
	atomic_store(&word, val);
	syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void pin(int cpu)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	sched_setaffinity(0, sizeof(s), &s);
}

static int same_cpu;

static void *peer(void *arg)
{
	pin(same_cpu ? 0 : 1);
	for (long i = 0; i < iters; i++) {
		fwait(0);
		fset(0);
	}
	return NULL;
}

static double run(void)
{
	struct timespec a, b;
	pthread_t t;

	atomic_store(&word, 0);
	pthread_create(&t, NULL, peer, NULL);
	pin(0);
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (long i = 0; i < iters; i++) {
		fset(1);
		fwait(1);
	}
	clock_gettime(CLOCK_MONOTONIC, &b);
	pthread_join(t, NULL);
	return ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / iters;
}

static int cmp(const void *x, const void *y)
{
	double a = *(const double *)x, b = *(const double *)y;

	return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
	double r[5];

	if (argc > 1)
		iters = atol(argv[1]);
	if (argc > 2)
		same_cpu = atoi(argv[2]);
	if (syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0) < 0) {
		printf("futex unavailable\n");
		return 4;
	}
	for (int i = 0; i < 5; i++)
		r[i] = run();
	qsort(r, 5, sizeof(r[0]), cmp);
	printf("futex_pingpong: %ld round trips, %s CPU: median %.0f ns, min %.0f ns\n",
	       iters, same_cpu ? "same" : "cross", r[2], r[0]);
	return 0;
}
