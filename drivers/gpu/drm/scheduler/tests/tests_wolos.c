// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 Julia Klee */

#include <kunit/test.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include "sched_tests.h"
#include "../sched_internal.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

#define WOL_LAT_JOBS 2000

static void run_one(struct kunit *test, struct drm_mock_sched_job **out,
		    struct drm_mock_sched_entity *entity)
{
	struct drm_mock_sched_job *job;

	job = drm_mock_sched_job_new(test, entity);
	job->duration_us = 1;
	job->pushed_at = ktime_get();
	drm_mock_sched_job_submit(job);
	KUNIT_ASSERT_TRUE(test, drm_mock_sched_job_wait_finished(job, 5 * HZ));
	*out = job;
}

static void wol_highpri_case(struct kunit *test, bool on, bool want_boost)
{
	struct drm_mock_sched_entity *entity;
	struct drm_mock_scheduler *sched;
	struct drm_mock_sched_job *job;
	bool old = drm_sched_submit_highpri;

	drm_sched_submit_highpri = on;
	sched = drm_mock_sched_new(test, MAX_SCHEDULE_TIMEOUT);
	drm_sched_submit_highpri = old;

	entity = drm_mock_sched_entity_new(test, DRM_SCHED_PRIORITY_NORMAL,
					   sched);
	run_one(test, &job, entity);

	if (want_boost)
		KUNIT_EXPECT_LT(test, job->run_nice, 0);
	else
		KUNIT_EXPECT_EQ(test, job->run_nice, 0);

	drm_mock_sched_entity_free(entity);
	drm_mock_sched_fini(sched);
}

static void wol_submit_worker_default_nice(struct kunit *test)
{
	wol_highpri_case(test, false, false);
}

static void wol_submit_worker_highpri(struct kunit *test)
{
	wol_highpri_case(test, true, true);
}

static int wol_hog(void *unused)
{
	while (!kthread_should_stop())
		cond_resched();
	return 0;
}

static int cmp_s64(const void *a, const void *b)
{
	s64 x = *(const s64 *)a, y = *(const s64 *)b;

	return x < y ? -1 : x > y;
}

/*
 * Push-to-run_job latency with every CPU busy at nice 0. Reports, never
 * asserts: compare runs on an idle box, on real hardware, not under UML.
 */
static void wol_submit_latency(struct kunit *test, bool highpri)
{
	struct drm_mock_sched_entity *entity;
	struct drm_mock_scheduler *sched;
	struct task_struct **hogs;
	bool old = drm_sched_submit_highpri;
	unsigned int cpu, nr = 0;
	s64 *lat;
	int i;

	if (IS_ENABLED(CONFIG_UML))
		kunit_skip(test, "latency needs real CPUs");

	lat = kunit_kcalloc(test, WOL_LAT_JOBS, sizeof(*lat), GFP_KERNEL);
	hogs = kunit_kcalloc(test, nr_cpu_ids, sizeof(*hogs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, lat);
	KUNIT_ASSERT_NOT_NULL(test, hogs);

	drm_sched_submit_highpri = highpri;
	sched = drm_mock_sched_new(test, MAX_SCHEDULE_TIMEOUT);
	drm_sched_submit_highpri = old;
	entity = drm_mock_sched_entity_new(test, DRM_SCHED_PRIORITY_NORMAL,
					   sched);

	for_each_online_cpu(cpu) {
		hogs[nr] = kthread_run_on_cpu(wol_hog, NULL, cpu, "wol_hog/%u");
		if (!IS_ERR(hogs[nr]))
			nr++;
	}

	for (i = 0; i < WOL_LAT_JOBS; i++) {
		struct drm_mock_sched_job *job;

		run_one(test, &job, entity);
		lat[i] = ktime_to_ns(ktime_sub(job->ran_at, job->pushed_at));
		usleep_range(200, 300);
	}

	while (nr)
		kthread_stop(hogs[--nr]);

	sort(lat, WOL_LAT_JOBS, sizeof(*lat), cmp_s64, NULL);
	kunit_info(test, "highpri=%d push->run_job ns p50=%lld p99=%lld p999=%lld max=%lld\n",
		   highpri, lat[WOL_LAT_JOBS / 2], lat[WOL_LAT_JOBS * 99 / 100],
		   lat[WOL_LAT_JOBS * 999 / 1000], lat[WOL_LAT_JOBS - 1]);

	drm_mock_sched_entity_free(entity);
	drm_mock_sched_fini(sched);
}

static void wol_submit_latency_default(struct kunit *test)
{
	wol_submit_latency(test, false);
}

static void wol_submit_latency_highpri(struct kunit *test)
{
	wol_submit_latency(test, true);
}

static struct kunit_case wol_submit_cases[] = {
	KUNIT_CASE(wol_submit_worker_default_nice),
	KUNIT_CASE(wol_submit_worker_highpri),
	KUNIT_CASE_SLOW(wol_submit_latency_default),
	KUNIT_CASE_SLOW(wol_submit_latency_highpri),
	{}
};

static struct kunit_suite wol_submit_suite = {
	.name = "drm_sched_wolos_submit",
	.test_cases = wol_submit_cases,
};

kunit_test_suite(wol_submit_suite);
