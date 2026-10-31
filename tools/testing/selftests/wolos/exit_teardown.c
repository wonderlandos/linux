// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Exit of a large process: waitpid() does not wait for the anonymous memory,
 * but everything user space can observe stays synchronous: the executable is
 * writable again, its mounts are free, a cgroup does not OOM on memory that
 * is on its way out.
 *
 * Root only.  ./exit_teardown [hog_mb]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PARAM "/sys/module/wolos_mm/parameters/exit_async_min_mb"
#define CG "/sys/fs/cgroup/exit_teardown"

static int fails;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		fails++;
}

static int write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	int ret;

	if (fd < 0)
		return -1;
	ret = write(fd, s, strlen(s)) < 0 ? -1 : 0;
	close(fd);
	return ret;
}

static long read_meminfo(const char *key)
{
	char line[128];
	long v = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key)))
			sscanf(line + strlen(key) + 1, "%ld", &v);
	if (f)
		fclose(f);
	return v / 1024;	/* MiB */
}

/* Touch @mb MiB of anonymous memory, write one byte to @ready, sleep. */
static void hog(long mb, int ready)
{
	size_t len = (size_t)mb << 20;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		_exit(2);
	madvise(p, len, MADV_NOHUGEPAGE);
	memset(p, 1, len);
	if (write(ready, "x", 1) != 1)
		_exit(3);
	pause();
	_exit(0);
}

static pid_t start_hog(long mb)
{
	int p[2];
	char c;
	pid_t pid;

	if (pipe(p))
		exit(1);
	pid = fork();
	if (!pid) {
		close(p[0]);
		hog(mb, p[1]);
	}
	close(p[1]);
	if (read(p[0], &c, 1) != 1)
		exit(1);
	close(p[0]);
	return pid;
}

static double kill_wait(pid_t pid)
{
	double t0 = now();

	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return now() - t0;
}

/* Runs until the system has the memory back; seconds from @t0. */
static double memory_back(double t0, long want_mb)
{
	while (read_meminfo("MemAvailable") < want_mb)
		usleep(2000);
	return now() - t0;
}

static void test_wait_latency(long mb)
{
	long before = read_meminfo("MemAvailable");
	char old[32] = "";
	double sync_s, async_s, back_s, t0;
	int fd = open(PARAM, O_RDONLY);
	pid_t pid;

	check(fd >= 0, "switch exit_async_min_mb exists");
	if (fd < 0)
		return;
	if (read(fd, old, sizeof(old) - 1) < 0)
		old[0] = 0;
	close(fd);

	write_str(PARAM, "0");
	pid = start_hog(mb);
	sync_s = kill_wait(pid);
	memory_back(now(), before - mb / 8);

	write_str(PARAM, "256");
	pid = start_hog(mb);
	t0 = now();
	async_s = kill_wait(pid);
	back_s = memory_back(t0, before - mb / 8);
	write_str(PARAM, old);

	printf("# %ld MiB: wait sync %.1f ms, wait async %.1f ms, memory back %.1f ms\n",
	       mb, sync_s * 1e3, async_s * 1e3, back_s * 1e3);
	check(async_s < sync_s / 3, "waitpid returns before the memory is freed");
}

/* The binary of a killed large process can be written at once. */
static void test_txtbsy(long mb, const char *self)
{
	char path[] = "/tmp/exit_teardown_binXXXXXX";
	char buf[65536], arg[32];
	int in, out, p[2];
	ssize_t n;
	pid_t pid;

	out = mkstemp(path);
	in = open(self, O_RDONLY);
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, n) != n)
			exit(1);
	close(in);
	fchmod(out, 0755);
	close(out);

	if (pipe(p))
		exit(1);
	pid = fork();
	if (!pid) {
		close(p[0]);
		dup2(p[1], 3);
		snprintf(arg, sizeof(arg), "%ld", mb);
		execl(path, path, "--hog", arg, NULL);
		_exit(4);
	}
	close(p[1]);
	if (read(p[0], buf, 1) != 1)
		exit(1);
	close(p[0]);
	kill_wait(pid);
	out = open(path, O_WRONLY);
	check(out >= 0 || errno != ETXTBSY, "executable writable right after wait");
	if (out >= 0)
		close(out);
	unlink(path);
}

/* A mapping of a killed large process does not keep its mount busy. */
static void test_umount(long mb)
{
	char dir[] = "/tmp/exit_teardown_mntXXXXXX";
	char file[PATH_MAX];
	int fd, p[2];
	char c;
	pid_t pid;

	if (!mkdtemp(dir) || mount("none", dir, "tmpfs", 0, NULL)) {
		printf("# umount: skip\n");
		return;
	}
	snprintf(file, sizeof(file), "%s/f", dir);
	fd = open(file, O_RDWR | O_CREAT, 0600);
	if (ftruncate(fd, 1 << 20))
		exit(1);
	if (pipe(p))
		exit(1);
	pid = fork();
	if (!pid) {
		void *m = mmap(NULL, 1 << 20, PROT_READ, MAP_SHARED, fd, 0);

		close(fd);
		close(p[0]);
		*(volatile char *)m;
		hog(mb, p[1]);
	}
	close(fd);
	close(p[1]);
	if (read(p[0], &c, 1) != 1)
		exit(1);
	close(p[0]);
	kill_wait(pid);
	check(!umount2(dir, 0), "mount free right after wait");
	rmdir(dir);
}

static long oom_kills(void)
{
	char line[128];
	long v = 0;
	FILE *f = fopen(CG "/memory.events", "r");

	while (f && fgets(line, sizeof(line), f))
		sscanf(line, "oom_kill %ld", &v);
	if (f)
		fclose(f);
	return v;
}

/* Memory on its way out must not OOM the next user of the cgroup. */
static void test_memcg_gate(long mb)
{
	char s[64];
	pid_t pid;

	rmdir(CG);
	if (mkdir(CG, 0755) && errno != EEXIST) {
		printf("# memcg gate: skip\n");
		return;
	}
	write_str("/sys/fs/cgroup/cgroup.subtree_control", "+memory");
	snprintf(s, sizeof(s), "%ld", (mb + 64) << 20);
	write_str(CG "/memory.max", s);
	write_str(CG "/memory.swap.max", "0");
	snprintf(s, sizeof(s), "%d", getpid());
	write_str(CG "/cgroup.procs", s);

	pid = start_hog(mb);
	kill_wait(pid);
	pid = start_hog(mb);
	kill_wait(pid);
	write_str("/sys/fs/cgroup/cgroup.procs", s);
	check(oom_kills() == 0, "no memcg OOM from memory on its way out");
	usleep(200000);
	rmdir(CG);
}

int main(int argc, char **argv)
{
	long mb = 2048;

	if (argc == 3 && !strcmp(argv[1], "--hog")) {
		hog(atol(argv[2]), 3);
		return 0;
	}
	if (argc > 1)
		mb = atol(argv[1]);
	if (geteuid()) {
		printf("# needs root\n");
		return 4;
	}
	test_wait_latency(mb);
	test_txtbsy(mb, "/proc/self/exe");
	test_umount(mb);
	test_memcg_gate(mb);
	printf("# %d failed\n", fails);
	return fails ? 1 : 0;
}
