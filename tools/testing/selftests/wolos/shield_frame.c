// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A game's serial part: one thread sleeps to the next frame boundary, then does
 * a fixed amount of work. Prints the wall time of that work and the wakeup delay
 * in microseconds as "p50 p99 p999 max" pairs.
 *   shield_frame -c WORK_US            print the loop count for WORK_US of work
 *   shield_frame -l LOOPS -p PERIOD_US -n FRAMES [-w HELPERS] [-f] [-s SLICE_US] [-N NICE]
 * -f writes our pid to /sys/kernel/wolos/foreground_pid. HELPERS threads each do
 * a tenth of the work per frame, as a game's job threads do. -s sets the EEVDF
 * slice of the frame thread (sched_attr.sched_runtime), -N its nice value.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static void work(uint64_t loops)
{
	volatile uint64_t x = 1;

	while (loops--)
		x = x * 31 + loops;
}

struct sched_attr_v1 {
	uint32_t size, policy;
	uint64_t flags;
	int32_t nice;
	uint32_t prio;
	uint64_t runtime, deadline, period;
};

static int set_slice(uint64_t slice_ns, int nice)
{
	struct sched_attr_v1 a;

	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.nice = nice;
	a.runtime = slice_ns;
	return syscall(SYS_sched_setattr, 0, &a, 0);
}

static int cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void report(const char *name, uint64_t *v, int n)
{
	qsort(v, n, sizeof(*v), cmp);
	printf("%s_p50 %.1f %s_p99 %.1f %s_p999 %.1f %s_max %.1f ", name, v[n / 2] / 1e3,
	       name, v[(int)(n * 0.99)] / 1e3, name, v[(int)(n * 0.999)] / 1e3,
	       name, v[n - 1] / 1e3);
}

struct helper {
	uint64_t loops, period_ns;
};

static void *helper_fn(void *arg)
{
	struct helper *h = arg;

	while (!stop) {
		struct timespec t = { 0, h->period_ns };

		nanosleep(&t, NULL);
		work(h->loops / 10);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	uint64_t loops = 0, period_us = 4000, *wk, *lat, next;
	int frames = 2500, helpers = 0, fg = 0, cal = 0, nice = 0, o, i;
	uint64_t slice_us = 0;
	struct helper h;
	pthread_t th[64];

	while ((o = getopt(argc, argv, "c:l:p:n:w:fs:N:")) != -1) {
		switch (o) {
		case 'c': cal = atoi(optarg); break;
		case 'l': loops = strtoull(optarg, NULL, 0); break;
		case 'p': period_us = strtoull(optarg, NULL, 0); break;
		case 'n': frames = atoi(optarg); break;
		case 'w': helpers = atoi(optarg); break;
		case 'f': fg = 1; break;
		case 's': slice_us = strtoull(optarg, NULL, 0); break;
		case 'N': nice = atoi(optarg); break;
		default: return 2;
		}
	}
	if (cal) {
		uint64_t t = now_ns(), n = 100000000;

		work(n);
		t = now_ns() - t;
		printf("%llu\n", (unsigned long long)(n * cal * 1000 / t));
		return 0;
	}
	if (fg) {
		FILE *f = fopen("/sys/kernel/wolos/foreground_pid", "w");

		if (f) {
			fprintf(f, "%d\n", getpid());
			fclose(f);
		}
	}
	h.loops = loops;
	h.period_ns = period_us * 1000;
	for (i = 0; i < helpers && i < 64; i++)
		pthread_create(&th[i], NULL, helper_fn, &h);
	if ((slice_us || nice) && set_slice(slice_us * 1000, nice))
		perror("sched_setattr");
	wk = calloc(frames, sizeof(*wk));
	lat = calloc(frames, sizeof(*lat));
	next = now_ns() + period_us * 1000;
	/* A second of warm-up, so a shield picks the thread and settles. */
	for (i = -(int)(1500000 / period_us); i < frames; i++) {
		struct timespec t = { next / 1000000000, next % 1000000000 };
		uint64_t a, b;

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
		a = now_ns();
		work(loops);
		b = now_ns();
		if (i >= 0) {
			lat[i] = a - next;
			wk[i] = b - a;
		}
		next += period_us * 1000;
		if (next < b)
			next = b;
	}
	stop = 1;
	for (i = 0; i < helpers && i < 64; i++)
		pthread_join(th[i], NULL);
	if (fg) {
		FILE *f = fopen("/sys/kernel/wolos/foreground_pid", "w");

		if (f) {
			fprintf(f, "0\n");
			fclose(f);
		}
	}
	report("work", wk, frames);
	report("wake", lat, frames);
	printf("\n");
	return 0;
}
