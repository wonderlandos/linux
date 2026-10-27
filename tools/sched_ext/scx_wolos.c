/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_wolos loader: attaches the BPF scheduler and waits. When it exits or is killed the kernel
 * detaches it and every task returns to EEVDF on its own; systemd restarts it (scx_wolos.service).
 * Usage: scx_wolos [-s] [-v]   -s print enqueues per class every second, -v libbpf debug.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_wolos.bpf.skel.h"

static const char *class_name[] = { "interactive", "foreground", "default", "utility", "background" };
static bool verbose, stats;
static volatile int exit_req;

static int print_fn(enum libbpf_print_level level, const char *fmt, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, fmt, args);
}

static void on_signal(int sig)
{
	exit_req = 1;
}

static void print_stats(struct scx_wolos *skel)
{
	int ncpu = libbpf_num_possible_cpus();
	__u64 v[ncpu];

	for (__u32 c = 0; c < 5; c++) {
		__u64 sum = 0;

		if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats), &c, v))
			continue;
		for (int i = 0; i < ncpu; i++)
			sum += v[i];
		printf("%s=%llu%s", class_name[c], (unsigned long long)sum, c < 4 ? " " : "\n");
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct scx_wolos *skel;
	struct bpf_link *link;
	__u64 ecode;
	int opt;

	libbpf_set_print(print_fn);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	while ((opt = getopt(argc, argv, "svh")) != -1) {
		switch (opt) {
		case 's': stats = true; break;
		case 'v': verbose = true; break;
		default:
			fprintf(stderr, "usage: %s [-s] [-v]\n", argv[0]);
			return opt != 'h';
		}
	}
restart:
	skel = SCX_OPS_OPEN(wolos_ops, scx_wolos);
	SCX_OPS_LOAD(skel, wolos_ops, scx_wolos, uei);
	link = SCX_OPS_ATTACH(skel, wolos_ops, scx_wolos);
	while (!exit_req && !UEI_EXITED(skel, uei)) {
		if (stats)
			print_stats(skel);
		sleep(1);
	}
	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_wolos__destroy(skel);
	if (!exit_req && UEI_ECODE_RESTART(ecode))
		goto restart;
	/* Kicked out by the kernel (stall, error): nonzero so systemd logs and restarts us. */
	return exit_req ? 0 : 1;
}
