// SPDX-License-Identifier: GPL-2.0
/* KUnit: WolKernel legacy-path decisions (include/linux/wolos_compat.h). */
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/wolos_compat.h>

static void audit_lazy(struct kunit *t)
{
	/* audit on, no syscall rules: no per-syscall work */
	KUNIT_EXPECT_FALSE(t, wolos_audit_needs_work(true, false, 0));
	KUNIT_EXPECT_TRUE(t, wolos_audit_needs_work(true, false, 1));
	/* a task the filters exclude never needs it */
	KUNIT_EXPECT_FALSE(t, wolos_audit_needs_work(true, true, 5));
	KUNIT_EXPECT_FALSE(t, wolos_audit_needs_work(false, true, 0));
	/* switch off: stock behaviour, work even without rules */
	KUNIT_EXPECT_TRUE(t, wolos_audit_needs_work(false, false, 0));
}

/*
 * One task through the generic syscall entry/exit code with audit on: the
 * audit entry hook runs on any slow-path entry while the task has an audit
 * context (include/linux/entry-common.h), the exit hook only with the audit
 * work bit (SYSCALL_WORK_EXIT; seccomp is entry-only work).
 */
struct audit_sim {
	bool lazy;
	int n_rules;
	bool audit_work;	/* SYSCALL_WORK_SYSCALL_AUDIT */
	bool ctx_used;		/* context->context != AUDIT_CTX_UNUSED */
	int warnings;		/* WARN_ON in __audit_syscall_entry() */
};

static void sim_syscall(struct audit_sim *s, bool seccomp)
{
	if ((s->audit_work || seccomp) &&
	    wolos_audit_entry_builds(s->lazy, s->audit_work)) {
		if (s->ctx_used)
			s->warnings++;
		s->ctx_used = true;
	}
	if (s->audit_work) {
		s->ctx_used = false;	/* audit_reset_context() */
		if (!wolos_audit_needs_work(s->lazy, false, s->n_rules))
			s->audit_work = false;	/* audit_lazy_exit() */
	}
}

/* A seccomp-filtered service (journald, logind) with audit on and no rules. */
static void audit_lazy_seccomp_pairs(struct kunit *t)
{
	/* audit_alloc(): a new task starts with the work bit */
	struct audit_sim s = { .lazy = true, .audit_work = true };
	int i;

	for (i = 0; i < 8; i++)
		sim_syscall(&s, true);
	KUNIT_EXPECT_FALSE(t, s.audit_work);	/* dropped at the first exit */
	KUNIT_EXPECT_FALSE(t, s.ctx_used);
	KUNIT_EXPECT_EQ(t, s.warnings, 0);

	/* first rule: audit_lazy_rules_added() sets the bit, the task audits */
	s.n_rules = 1;
	s.audit_work = true;
	sim_syscall(&s, true);
	KUNIT_EXPECT_TRUE(t, s.audit_work);
	KUNIT_EXPECT_FALSE(t, s.ctx_used);

	/* rules gone: the task drops its own bit at its next exit */
	s.n_rules = 0;
	for (i = 0; i < 8; i++)
		sim_syscall(&s, true);
	KUNIT_EXPECT_FALSE(t, s.audit_work);
	KUNIT_EXPECT_FALSE(t, s.ctx_used);
	KUNIT_EXPECT_EQ(t, s.warnings, 0);
}

/* Stock audit (switch off): every slow-path entry pairs with an exit. */
static void audit_stock_pairs(struct kunit *t)
{
	struct audit_sim s = { .audit_work = true };
	int i;

	for (i = 0; i < 8; i++)
		sim_syscall(&s, i & 1);
	KUNIT_EXPECT_TRUE(t, s.audit_work);
	KUNIT_EXPECT_FALSE(t, s.ctx_used);
	KUNIT_EXPECT_EQ(t, s.warnings, 0);
	KUNIT_EXPECT_TRUE(t, wolos_audit_entry_builds(false, false));
}

static void int80_64(struct kunit *t)
{
	KUNIT_EXPECT_TRUE(t, wolos_int80_allowed(false, false));	/* 32-bit game */
	KUNIT_EXPECT_TRUE(t, wolos_int80_allowed(false, true));
	KUNIT_EXPECT_FALSE(t, wolos_int80_allowed(true, false));	/* 64-bit task */
	KUNIT_EXPECT_TRUE(t, wolos_int80_allowed(true, true));		/* abi.int80_64=1 */
}

static void retbleed_stuff(struct kunit *t)
{
	KUNIT_EXPECT_TRUE(t, wolos_retbleed_prefer_stuff(true, true, false));
	KUNIT_EXPECT_FALSE(t, wolos_retbleed_prefer_stuff(true, true, true));	/* eIBRS */
	KUNIT_EXPECT_FALSE(t, wolos_retbleed_prefer_stuff(true, false, false));	/* no CDT */
	KUNIT_EXPECT_FALSE(t, wolos_retbleed_prefer_stuff(false, true, false));
}

static void defaults(struct kunit *t)
{
	KUNIT_EXPECT_GT(t, WOLOS_MAX_MAP_COUNT, DEFAULT_MAX_MAP_COUNT);
	KUNIT_EXPECT_LE(t, (long)WOLOS_MAX_MAP_COUNT, (long)INT_MAX);
	/* proc_watchdog_thresh accepts 0..60 */
	KUNIT_EXPECT_GT(t, WOLOS_WATCHDOG_THRESH, 10);
	KUNIT_EXPECT_LE(t, WOLOS_WATCHDOG_THRESH, 60);
}

static struct kunit_case wolos_compat_cases[] = {
	KUNIT_CASE(audit_lazy),
	KUNIT_CASE(audit_lazy_seccomp_pairs),
	KUNIT_CASE(audit_stock_pairs),
	KUNIT_CASE(int80_64),
	KUNIT_CASE(retbleed_stuff),
	KUNIT_CASE(defaults),
	{}
};

static struct kunit_suite wolos_compat_suite = {
	.name = "wolos_compat",
	.test_cases = wolos_compat_cases,
};
kunit_test_suite(wolos_compat_suite);
MODULE_DESCRIPTION("KUnit tests for WolKernel legacy-path decisions");
MODULE_LICENSE("GPL");
