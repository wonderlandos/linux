// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Many producers, one epoll: every eventfd write is seen exactly once, no
 * wakeup is lost, and EPOLL_CTL_DEL/close racing with producers is safe.
 * Prints the event rate with eventpoll.wolos_pend on and off.
 *
 * ./epoll_mp [producers] [consumers] [seconds]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define PARAM "/sys/module/eventpoll/parameters/wolos_pend"
#define MAXP 256

static int epfd, efds[MAXP], nprod, ncons;
static atomic_ullong written, seen;
static atomic_int stop_prod, stop_cons;

static void *producer(void *arg)
{
	int fd = efds[(long)arg];
	uint64_t one = 1;
	unsigned long n = 0;

	while (!atomic_load_explicit(&stop_prod, memory_order_relaxed)) {
		if (write(fd, &one, 8) == 8)
			n++;
	}
	atomic_fetch_add(&written, n);
	return NULL;
}

static void *consumer(void *arg)
{
	struct epoll_event ev[64];

	while (!atomic_load(&stop_cons)) {
		int n = epoll_wait(epfd, ev, 64, 100), i;

		for (i = 0; i < n; i++) {
			uint64_t v;

			if (ev[i].data.fd < 0)	/* churn() */
				continue;
			while (read(ev[i].data.fd, &v, 8) == 8)
				atomic_fetch_add_explicit(&seen, v, memory_order_relaxed);
		}
	}
	return NULL;
}

/* Sources appear and go away while producers hammer other items. */
static void *churn(void *arg)
{
	uint64_t one = 1;

	while (!atomic_load(&stop_prod)) {
		struct epoll_event ev = { .events = EPOLLIN | EPOLLET };
		int fd = eventfd(0, EFD_NONBLOCK);

		ev.data.fd = -1;
		epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
		if (write(fd, &one, 8) != 8)
			perror("churn");
		if (fd & 1)
			epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
		close(fd);
	}
	return NULL;
}

static void set_param(const char *v)
{
	int fd = open(PARAM, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, v, 1) != 1)
			perror("param");
		close(fd);
	}
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int run(int seconds, const char *label)
{
	pthread_t p[MAXP], c[MAXP], ch;
	unsigned long long w;
	double t0, spent;
	int i, tries;

	written = seen = 0;
	stop_prod = stop_cons = 0;
	epfd = epoll_create1(0);
	for (i = 0; i < nprod; i++) {
		struct epoll_event ev = { .events = EPOLLIN | EPOLLET };

		efds[i] = eventfd(0, EFD_NONBLOCK);
		ev.data.fd = efds[i];
		epoll_ctl(epfd, EPOLL_CTL_ADD, efds[i], &ev);
	}
	t0 = now();
	for (i = 0; i < ncons; i++)
		pthread_create(&c[i], NULL, consumer, NULL);
	for (i = 0; i < nprod; i++)
		pthread_create(&p[i], NULL, producer, (void *)(long)i);
	pthread_create(&ch, NULL, churn, NULL);
	sleep(seconds);
	atomic_store(&stop_prod, 1);
	for (i = 0; i < nprod; i++)
		pthread_join(p[i], NULL);
	pthread_join(ch, NULL);
	spent = now() - t0;
	w = atomic_load(&written);
	/* What was written must arrive without another write: 2 s grace. */
	for (tries = 0; tries < 200 && atomic_load(&seen) != w; tries++)
		usleep(10000);
	atomic_store(&stop_cons, 1);
	for (i = 0; i < ncons; i++)
		pthread_join(c[i], NULL);
	for (i = 0; i < nprod; i++)
		close(efds[i]);
	close(epfd);
	printf("%s %s: %llu writes, %llu seen, %.0f events/s\n",
	       atomic_load(&seen) == w ? "ok" : "not ok", label, w,
	       (unsigned long long)atomic_load(&seen), w / spent);
	return atomic_load(&seen) != w;
}

int main(int argc, char **argv)
{
	int seconds = argc > 3 ? atoi(argv[3]) : 3, bad, i;

	nprod = argc > 1 ? atoi(argv[1]) : 8;
	ncons = argc > 2 ? atoi(argv[2]) : 1;
	if (access(PARAM, W_OK)) {
		printf("not ok switch %s\n", PARAM);
		return 1;
	}
	bad = 0;
	for (i = 0; i < (argc > 4 ? atoi(argv[4]) : 1); i++) {
		set_param("0");
		bad += run(seconds, "pend=0");
		set_param("1");
		bad += run(seconds, "pend=1");
	}
	return !!bad;
}
