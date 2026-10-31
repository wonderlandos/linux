// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * UDP round trip, a game's network tick.
 *   udp_pingpong -s PORT                       echo server
 *   udp_pingpong -c HOST -P PORT -n COUNT [-f] [-g GAP_US]
 * -f writes our pid to /sys/kernel/wolos/foreground_pid before the socket exists.
 * Prints "rtt_p50 rtt_p99 rtt_p999 rtt_max" in microseconds.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void set_fg(int pid)
{
	FILE *f = fopen("/sys/kernel/wolos/foreground_pid", "w");

	if (f) {
		fprintf(f, "%d\n", pid);
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_in a = { .sin_family = AF_INET };
	const char *host = NULL;
	int port = 7777, n = 20000, fg = 0, gap_us = 200, srv = 0, o, s, i;
	uint64_t *v;
	char buf[64] = { 0 };

	while ((o = getopt(argc, argv, "s:c:P:n:fg:")) != -1) {
		switch (o) {
		case 's': srv = 1; port = atoi(optarg); break;
		case 'c': host = optarg; break;
		case 'P': port = atoi(optarg); break;
		case 'n': n = atoi(optarg); break;
		case 'f': fg = 1; break;
		case 'g': gap_us = atoi(optarg); break;
		default: return 2;
		}
	}
	if (fg)
		set_fg(getpid());
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return 1;
	a.sin_port = htons(port);
	if (srv) {
		struct sockaddr_in p;
		socklen_t l;

		a.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(s, (struct sockaddr *)&a, sizeof(a)))
			return 1;
		for (;;) {
			l = sizeof(p);
			i = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&p, &l);
			if (i > 0)
				sendto(s, buf, i, 0, (struct sockaddr *)&p, l);
		}
	}
	inet_pton(AF_INET, host, &a.sin_addr);
	if (connect(s, (struct sockaddr *)&a, sizeof(a)))
		return 1;
	v = calloc(n, sizeof(*v));
	for (i = 0; i < n; i++) {
		struct timespec t = { 0, gap_us * 1000 };
		uint64_t t0 = now_ns();

		if (send(s, buf, 32, 0) < 0 || recv(s, buf, sizeof(buf), 0) < 0)
			return 1;
		v[i] = now_ns() - t0;
		nanosleep(&t, NULL);
	}
	if (fg)
		set_fg(0);
	qsort(v, n, sizeof(*v), cmp);
	printf("rtt_p50 %.1f rtt_p99 %.1f rtt_p999 %.1f rtt_max %.1f\n", v[n / 2] / 1e3,
	       v[(int)(n * 0.99)] / 1e3, v[(int)(n * 0.999)] / 1e3, v[n - 1] / 1e3);
	return 0;
}
