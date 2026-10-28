/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_wolos loader: attaches the BPF scheduler and waits. When it exits or is killed the kernel
 * detaches it and every task returns to EEVDF on its own; systemd restarts it (scx_wolos.service).
 * Usage: scx_wolos [-s] [-g] [-v]
 *   -s print enqueues per class and dispatch locality every second
 *   -g one global queue instead of per-CPU queues (A/B against the locality path)
 *   -v libbpf debug
 * Every second it copies /sys/kernel/wolos/foreground_pid into the scheduler (absent file = no boost).
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_wolos.bpf.skel.h"

static const char *class_name[] = { "interactive", "foreground", "default", "utility", "background" };
static bool verbose, stats, global_dsq;
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

static long read_long(const char *path, long dfl)
{
	FILE *f = fopen(path, "r");
	long v = dfl;

	if (f) {
		if (fscanf(f, "%ld", &v) != 1)
			v = dfl;
		fclose(f);
	}
	return v;
}

/* cpu -> LLC id from sysfs (index3 = L3; falls back to the package id, then 0). */
static void fill_llc(struct scx_wolos *skel, int ncpu)
{
	char path[128];

	for (__u32 cpu = 0; cpu < (__u32)ncpu && cpu < 512; cpu++) {
		long id;
		__u32 llc;

		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index3/id", cpu);
		id = read_long(path, -1);
		if (id < 0) {
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%u/topology/physical_package_id", cpu);
			id = read_long(path, 0);
		}
		llc = id;
		bpf_map_update_elem(bpf_map__fd(skel->maps.cpu_llc), &cpu, &llc, BPF_ANY);
	}
}

static __u64 sum_percpu(int fd, __u32 key, int ncpu)
{
	__u64 v[ncpu], sum = 0;

	if (bpf_map_lookup_elem(fd, &key, v))
		return 0;
	for (int i = 0; i < ncpu; i++)
		sum += v[i];
	return sum;
}

static void print_stats(struct scx_wolos *skel)
{
	int ncpu = libbpf_num_possible_cpus();
	int dfd = bpf_map__fd(skel->maps.dstats);

	for (__u32 c = 0; c < 5; c++)
		printf("%s=%llu ", class_name[c],
		       (unsigned long long)sum_percpu(bpf_map__fd(skel->maps.stats), c, ncpu));
	printf("| local=%llu llc_steal=%llu far_steal=%llu fg=%u\n",
	       (unsigned long long)sum_percpu(dfd, 0, ncpu),
	       (unsigned long long)sum_percpu(dfd, 1, ncpu),
	       (unsigned long long)sum_percpu(dfd, 2, ncpu), skel->bss->fg_tgid);
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
	while ((opt = getopt(argc, argv, "sgvh")) != -1) {
		switch (opt) {
		case 's': stats = true; break;
		case 'g': global_dsq = true; break;
		case 'v': verbose = true; break;
		default:
			fprintf(stderr, "usage: %s [-s] [-g] [-v]\n", argv[0]);
			return opt != 'h';
		}
	}
restart:
	skel = SCX_OPS_OPEN(wolos_ops, scx_wolos);
	skel->rodata->nr_cpus = libbpf_num_possible_cpus() > 512 ? 512 : libbpf_num_possible_cpus();
	skel->rodata->percpu_dsq = !global_dsq;
	SCX_OPS_LOAD(skel, wolos_ops, scx_wolos, uei);
	fill_llc(skel, skel->rodata->nr_cpus);
	link = SCX_OPS_ATTACH(skel, wolos_ops, scx_wolos);
	while (!exit_req && !UEI_EXITED(skel, uei)) {
		long fg = read_long("/sys/kernel/wolos/foreground_pid", 0);

		skel->bss->fg_tgid = fg > 0 ? fg : 0;
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
