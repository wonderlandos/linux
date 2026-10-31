// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * swap_par [MB [zswap|zram]]: push THP-backed anon memory through
 * memory.reclaim into zswap or zram, serially and on idle CPUs, and check
 * every word after the refault.  Needs root, cgroup2 at /sys/fs/cgroup, and
 * swap on: zswap enabled, or a zram device as the only swap.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../kselftest.h"

#define CG "/sys/fs/cgroup/swap_par"
#define HPAGE (2UL << 20)

static const char *counter = "zswpout";

static int put(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	int ret;

	if (fd < 0)
		return -1;
	ret = write(fd, val, strlen(val)) < 0 ? -1 : 0;
	close(fd);
	return ret;
}

static unsigned long swapped(void)
{
	FILE *f = fopen("/proc/vmstat", "r");
	unsigned long v = 0, n;
	char name[64];

	if (!f)
		return 0;
	while (fscanf(f, "%63s %lu", name, &n) == 2)
		if (!strcmp(name, counter))
			v = n;
	fclose(f);
	return v;
}

/* Half the words repeat, so a page compresses and each stays unique. */
static unsigned long word(size_t i, int round)
{
	return i & 1 ? 0x6161616161616161UL : i * 2654435761UL + round;
}

static int one_round(unsigned long *m, size_t len, int round)
{
	size_t n = len / sizeof(*m), i;
	unsigned long before = swapped(), bad = 0;
	char s[32];

	for (i = 0; i < n; i++)
		m[i] = word(i, round);
	snprintf(s, sizeof(s), "%zu", len);
	put(CG "/memory.reclaim", s);	/* EAGAIN when short: judged by the counter */
	if (swapped() == before) {
		ksft_print_msg("round %d: nothing reached swap\n", round);
		return -1;
	}
	for (i = 0; i < n; i++)
		bad += m[i] != word(i, round);
	ksft_print_msg("round %d: %s +%lu, %lu bad words\n", round, counter,
		       swapped() - before, bad);
	return bad ? -1 : 0;
}

int main(int argc, char **argv)
{
	size_t len = (argc > 1 ? atol(argv[1]) : 256) << 20;
	char saved[32] = "0\n", s[32], param[128];
	unsigned long *m;
	char *raw;
	const char *mod = argc > 2 ? argv[2] : "zswap";
	int fd, ret;

	if (!strcmp(mod, "zram"))
		counter = "pswpout";
	snprintf(param, sizeof(param),
		 "/sys/module/%s/parameters/wolos_par_min_pages", mod);
	ksft_print_header();
	ksft_set_plan(2);
	fd = open(param, O_RDONLY);
	if (fd < 0)
		ksft_exit_skip("no %s\n", param);
	if (read(fd, saved, sizeof(saved) - 1) < 0)
		ksft_exit_skip("read %s\n", param);
	close(fd);
	mkdir(CG, 0755);
	put("/sys/fs/cgroup/cgroup.subtree_control", "+memory");
	snprintf(s, sizeof(s), "%d", getpid());
	if (put(CG "/cgroup.procs", s))
		ksft_exit_skip("cannot enter %s\n", CG);

	raw = mmap(NULL, len + HPAGE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		ksft_exit_fail_msg("mmap\n");
	m = (unsigned long *)(((unsigned long)raw + HPAGE - 1) & ~(HPAGE - 1));
	madvise(m, len, MADV_HUGEPAGE);

	if (put(param, "0"))
		ksft_exit_skip("cannot write %s\n", param);
	ksft_test_result(!one_round(m, len, 1), "serial store\n");
	/* A fresh range: the refault above left the folios split. */
	madvise(m, len, MADV_DONTNEED);
	put(param, "16");
	ret = one_round(m, len, 2);
	put(param, saved);
	ksft_test_result(!ret, "parallel store\n");

	munmap(raw, len + HPAGE);
	put("/sys/fs/cgroup/cgroup.procs", s);
	rmdir(CG);
	ksft_finished();
}
