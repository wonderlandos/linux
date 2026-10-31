// SPDX-License-Identifier: GPL-2.0
/* KUnit: WolKernel policy decisions (include/linux/wolos_policy.h). */
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/wolos_policy.h>

static void tsc_trust(struct kunit *t)
{
	struct wolos_tsc_facts desktop_amd = { true, true, false, false, 1 };
	struct wolos_tsc_facts intel = { true, true, true, false, 1 };
	struct wolos_tsc_facts vm = { true, true, false, true, 1 };
	struct wolos_tsc_facts dual = { true, true, false, false, 2 };
	struct wolos_tsc_facts old = { true, false, false, false, 1 };

	/* one package, invariant TSC, bare metal: trusted even without TSC_ADJUST */
	KUNIT_EXPECT_TRUE(t, wolos_tsc_trusted(&desktop_amd));
	KUNIT_EXPECT_TRUE(t, wolos_tsc_trusted(&intel));	/* stock rule still holds */
	KUNIT_EXPECT_FALSE(t, wolos_tsc_trusted(&vm));
	KUNIT_EXPECT_FALSE(t, wolos_tsc_trusted(&dual));
	KUNIT_EXPECT_FALSE(t, wolos_tsc_trusted(&old));
}

static void kbd_bypass(struct kunit *t)
{
	KUNIT_EXPECT_TRUE(t, wolos_kbd_bypass(true, true));	/* compositor session */
	KUNIT_EXPECT_FALSE(t, wolos_kbd_bypass(false, true));
	KUNIT_EXPECT_FALSE(t, wolos_kbd_bypass(true, false));
	KUNIT_EXPECT_FALSE(t, wolos_kbd_bypass(false, false));	/* text VT */
}

static void oom_bias(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, wolos_oom_bias(800, 8000, true, false), 200L);
	KUNIT_EXPECT_EQ(t, wolos_oom_bias(100, 8000, false, true), 1100L);
	KUNIT_EXPECT_EQ(t, wolos_oom_bias(500, 8000, false, false), 500L);
	/* foreground wins over SCHED_IDLE */
	KUNIT_EXPECT_EQ(t, wolos_oom_bias(800, 8000, true, true), 200L);
	/* background (100+1000) now ranks above a bigger foreground app (800/4) */
	KUNIT_EXPECT_GT(t, wolos_oom_bias(100, 8000, false, true),
			wolos_oom_bias(800, 8000, true, false));
}

static void input_boost(struct kunit *t)
{
	unsigned long gap = 50;

	KUNIT_EXPECT_TRUE(t, wolos_boost_should_fire(1000, 950, gap));
	KUNIT_EXPECT_FALSE(t, wolos_boost_should_fire(1000, 951, gap));	/* rate limit */
	/* across the jiffies wrap */
	KUNIT_EXPECT_TRUE(t, wolos_boost_should_fire(10, ULONG_MAX - 40, gap));
	KUNIT_EXPECT_FALSE(t, wolos_boost_should_fire(5, ULONG_MAX - 40, gap));

	KUNIT_EXPECT_EQ(t, wolos_boost_floor(100, 200, 1024, 60), 614UL);
	KUNIT_EXPECT_EQ(t, wolos_boost_floor(200, 200, 1024, 60), 0UL);	/* window closed */
	KUNIT_EXPECT_EQ(t, wolos_boost_floor(300, 200, 1024, 60), 0UL);
	KUNIT_EXPECT_EQ(t, wolos_boost_floor(100, 200, 1024, 250), 1024UL);	/* clamped */
	KUNIT_EXPECT_EQ(t, wolos_boost_floor(ULONG_MAX - 5, 10, 1000, 50), 500UL); /* wrap */
}

static void shield_thread(struct kunit *t)
{
	const u64 d[] = { 10, 100, 120, 130 };
	const u64 z[] = { 1, 2 };

	KUNIT_EXPECT_EQ(t, wolos_shield_pick_thread(d, 4, -1, 125, 50), 3);
	/* holder at 100 keeps the core against 120 and 124 ... */
	KUNIT_EXPECT_EQ(t, wolos_shield_pick_thread(d, 3, 1, 125, 50), 1);
	/* ... but not against 130 */
	KUNIT_EXPECT_EQ(t, wolos_shield_pick_thread(d, 4, 1, 125, 50), 3);
	/* a holder under the floor loses at once */
	KUNIT_EXPECT_EQ(t, wolos_shield_pick_thread(d, 4, 0, 125, 50), 3);
	/* nobody qualifies */
	KUNIT_EXPECT_EQ(t, wolos_shield_pick_thread(z, 2, 0, 125, 50), -1);
	KUNIT_EXPECT_EQ(t, wolos_shield_pick_thread(z, 0, -1, 125, 50), -1);
}

static void shield_cpu(struct kunit *t)
{
	const struct wolos_cpu_facts x3d[] = {
		{ false, 96000, 1024, 0 },	/* not allowed: CPU 0 */
		{ true, 32000, 1024, 200 },	/* faster CCD */
		{ true, 96000, 1000, 100 },	/* V-cache CCD wins on cache */
	};
	const struct wolos_cpu_facts hybrid[] = {
		{ true, 30000, 700, 300 },	/* E-core */
		{ true, 30000, 1024, 100 },
		{ true, 30000, 1024, 200 },	/* favoured P-core */
	};
	const struct wolos_cpu_facts none[] = { { false, 1, 1, 1 } };

	KUNIT_EXPECT_EQ(t, wolos_shield_pick_cpu(x3d, 3), 2);
	KUNIT_EXPECT_EQ(t, wolos_shield_pick_cpu(hybrid, 3), 2);
	KUNIT_EXPECT_EQ(t, wolos_shield_pick_cpu(none, 1), -1);
}

static void shield_limits(struct kunit *t)
{
	KUNIT_EXPECT_FALSE(t, wolos_shield_enough_cpus(4, 2));
	KUNIT_EXPECT_TRUE(t, wolos_shield_enough_cpus(6, 2));
	KUNIT_EXPECT_FALSE(t, wolos_shield_enough_cpus(6, 5));
	KUNIT_EXPECT_TRUE(t, wolos_shield_irq_keep("amdgpu"));
	KUNIT_EXPECT_TRUE(t, wolos_shield_irq_keep("xhci_hcd"));
	KUNIT_EXPECT_FALSE(t, wolos_shield_irq_keep("eth0-TxRx-0"));
	KUNIT_EXPECT_FALSE(t, wolos_shield_irq_keep(NULL));
}

static void sock_busy(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, wolos_sock_busy_us(0, 50, true, true, true, true), 50U);
	KUNIT_EXPECT_EQ(t, wolos_sock_busy_us(0, 0, true, true, true, true), 0U);	/* off */
	KUNIT_EXPECT_EQ(t, wolos_sock_busy_us(0, 50, true, true, true, false), 0U);	/* not frontmost */
	KUNIT_EXPECT_EQ(t, wolos_sock_busy_us(0, 50, true, false, true, true), 0U);	/* TCP */
	KUNIT_EXPECT_EQ(t, wolos_sock_busy_us(0, 50, true, true, false, true), 0U);	/* unix */
	KUNIT_EXPECT_EQ(t, wolos_sock_busy_us(0, 50, false, true, true, true), 0U);	/* softirq */
	KUNIT_EXPECT_EQ(t, wolos_sock_busy_us(100, 50, true, true, true, true), 100U);	/* sysctl wins */
}

static void xhci_imod(struct kunit *t)
{
	KUNIT_EXPECT_EQ(t, wolos_xhci_imod_ns(-1, 40000), 40000U);
	KUNIT_EXPECT_EQ(t, wolos_xhci_imod_ns(0, 40000), 0U);
	KUNIT_EXPECT_EQ(t, wolos_xhci_imod_ns(10000, 40000), 10000U);
}

static void psi_skip_record(struct kunit *t)
{
	KUNIT_EXPECT_TRUE(t, wolos_psi_skip_record(5, 5, 1000, true));
	KUNIT_EXPECT_FALSE(t, wolos_psi_skip_record(5, 4, 1000, true));
	KUNIT_EXPECT_FALSE(t, wolos_psi_skip_record(5, 5, 1000000000LL, true));
	KUNIT_EXPECT_FALSE(t, wolos_psi_skip_record(5, 5, 1000, false));
}

static void psi_irq_defer(struct kunit *t)
{
	KUNIT_EXPECT_TRUE(t, wolos_psi_irq_defer(9999, 10000));
	KUNIT_EXPECT_FALSE(t, wolos_psi_irq_defer(10000, 10000));
	KUNIT_EXPECT_FALSE(t, wolos_psi_irq_defer(1, 0));
}

static struct kunit_case wolos_policy_cases[] = {
	KUNIT_CASE(sock_busy),
	KUNIT_CASE(xhci_imod),
	KUNIT_CASE(shield_thread),
	KUNIT_CASE(shield_cpu),
	KUNIT_CASE(shield_limits),
	KUNIT_CASE(input_boost),
	KUNIT_CASE(psi_skip_record),
	KUNIT_CASE(psi_irq_defer),
	KUNIT_CASE(tsc_trust),
	KUNIT_CASE(kbd_bypass),
	KUNIT_CASE(oom_bias),
	{}
};

static struct kunit_suite wolos_policy_suite = {
	.name = "wolos_policy",
	.test_cases = wolos_policy_cases,
};
kunit_test_suite(wolos_policy_suite);
MODULE_LICENSE("GPL");
