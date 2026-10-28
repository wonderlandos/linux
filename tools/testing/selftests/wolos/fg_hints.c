// SPDX-License-Identifier: GPL-2.0
/*
 * Checks /sys/kernel/wolos foreground hooks: foreground_timerslack_ns is
 * applied to every thread of foreground_pid and undone when it leaves;
 * foreground_uclamp_min rejects values above 1024. Needs root.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include "../kselftest.h"

#define W "/sys/kernel/wolos/"

static int wr(const char *f, const char *v)
{
	FILE *fp = fopen(f, "w");
	int ok;

	if (!fp)
		return -1;
	ok = fputs(v, fp) >= 0;
	return (fclose(fp) == 0 && ok) ? 0 : -1;
}

static long slack(void) { return prctl(PR_GET_TIMERSLACK); }

static volatile int phase;
static long tslack[3];

static void *thr(void *a)
{
	(void)a;
	while (phase < 1) usleep(1000);
	tslack[1] = slack();
	while (phase < 2) usleep(1000);
	tslack[2] = slack();
	return NULL;
}

int main(void)
{
	char pid[32];
	pthread_t t;
	long def;

	ksft_print_header();
	if (geteuid() || access(W "foreground_timerslack_ns", W_OK))
		ksft_exit_skip("needs root and foreground_timerslack_ns\n");
	ksft_set_plan(4);
	def = slack();
	pthread_create(&t, NULL, thr, NULL);
	snprintf(pid, sizeof(pid), "%d", getpid());
	wr(W "foreground_timerslack_ns", "0");
	wr(W "foreground_pid", pid);
	phase = 1;
	usleep(20000);
	ksft_test_result(slack() == 0, "main thread slack 0 while frontmost\n");
	wr(W "foreground_pid", "0");
	phase = 2;
	pthread_join(t, NULL);
	ksft_test_result(tslack[1] == 0 && tslack[2] == def, "second thread set and restored\n");
	wr(W "foreground_timerslack_ns", "-1");
	ksft_test_result(wr(W "foreground_uclamp_min", "1025") < 0, "uclamp_min > 1024 rejected\n");
	ksft_test_result(wr(W "foreground_uclamp_min", "0") == 0, "uclamp_min 0 accepted\n");
	ksft_finished();
}
