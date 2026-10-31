/* SPDX-License-Identifier: GPL-2.0 */
/*
 * WolKernel policy decisions as pure functions, so KUnit can test them on any
 * arch (lib/wolos_policy_kunit.c). Callers pass in the hardware/task facts.
 */
#ifndef _LINUX_WOLOS_POLICY_H
#define _LINUX_WOLOS_POLICY_H

#include <linux/array_size.h>
#include <linux/string.h>
#include <linux/types.h>

/* Facts the TSC watchdog decision depends on. */
struct wolos_tsc_facts {
	bool constant, nonstop, tsc_adjust, hypervisor;
	unsigned int packages;
};

/*
 * True: skip the clocksource watchdog for the TSC. Stock rule (TSC_ADJUST,
 * <= 4 packages) plus: one package, invariant TSC, bare metal, which covers
 * AMD desktops without TSC_ADJUST. Boot and hotplug sync checks still run;
 * tsc=watchdog restores the watchdog.
 */
static inline bool wolos_tsc_trusted(const struct wolos_tsc_facts *f)
{
	if (!f->constant || !f->nonstop)
		return false;
	if (f->tsc_adjust && f->packages <= 4)
		return true;
	return !f->hypervisor && f->packages == 1;
}

/*
 * True: kbd_event() skips the keymap, LED tasklet and console work. Only when
 * a graphical session owns the keyboard (K_OFF + KD_GRAPHICS); text VTs keep
 * the full path.
 */
static inline bool wolos_kbd_bypass(bool kbd_off, bool vt_graphics)
{
	return kbd_off && vt_graphics;
}

/*
 * OOM badness after the WolOS bias: the frontmost app counts a quarter of its
 * size, SCHED_IDLE (Background QoS) tasks gain an eighth of RAM, so background
 * work dies first and the app in front last. oom_score_adj still applies.
 */
static inline long wolos_oom_bias(long points, unsigned long totalpages,
				  bool foreground, bool sched_idle)
{
	if (foreground)
		return points / 4;
	if (sched_idle)
		return points + (long)(totalpages / 8);
	return points;
}

/*
 * Input boost: fire at most once per @gap jiffies (signed difference, so it
 * survives jiffies wrap). @last must start at least @gap before now.
 */
static inline bool wolos_boost_should_fire(unsigned long now, unsigned long last,
					   unsigned long gap)
{
	return (long)(now - last - gap) >= 0;
}

/* Utilization floor while the boost window (until @until) is open, else 0. */
static inline unsigned long wolos_boost_floor(unsigned long now, unsigned long until,
					      unsigned long cap, unsigned int pct)
{
	if ((long)(until - now) <= 0)
		return 0;
	return cap * (pct > 100 ? 100 : pct) / 100;
}

/*
 * Shield: index of the thread that should hold the core. @delta[i] is each
 * thread's runtime over the last period, @cur the holder or -1. Nobody qualifies
 * under @min_delta. A rival must beat a qualifying holder by @switch_pct percent,
 * so two threads of similar load do not trade the core every period.
 */
static inline int wolos_shield_pick_thread(const u64 *delta, int n, int cur,
					   unsigned int switch_pct, u64 min_delta)
{
	int i, best = -1;

	for (i = 0; i < n; i++)
		if (delta[i] >= min_delta && (best < 0 || delta[i] > delta[best]))
			best = i;
	if (best >= 0 && cur >= 0 && cur < n && delta[cur] >= min_delta &&
	    delta[best] * 100 < delta[cur] * switch_pct)
		return cur;
	return best;
}

struct wolos_cpu_facts {
	bool allowed;
	u32 llc_kb, capacity;
	int prio;
};

/* Best core: largest last-level cache (X3D), then capacity, then ITMT priority. */
static inline int wolos_shield_pick_cpu(const struct wolos_cpu_facts *f, int n)
{
	int i, best = -1;

	for (i = 0; i < n; i++) {
		if (!f[i].allowed)
			continue;
		if (best < 0 || f[i].llc_kb > f[best].llc_kb ||
		    (f[i].llc_kb == f[best].llc_kb &&
		     (f[i].capacity > f[best].capacity ||
		      (f[i].capacity == f[best].capacity && f[i].prio > f[best].prio))))
			best = i;
	}
	return best;
}

/* Shield only with at least 6 online CPUs and 2 left for housekeeping. */
static inline bool wolos_shield_enough_cpus(unsigned int online, unsigned int shielded)
{
	return online >= 6 && online - shielded >= 2;
}

/* GPU and input IRQs stay where they are, next to the thread that waits for them. */
static inline bool wolos_shield_irq_keep(const char *name)
{
	static const char * const keep[] = { "amdgpu", "i915", "xe", "nvidia", "nouveau",
					     "xhci", "i8042", "hid" };
	unsigned int i;

	if (!name)
		return false;
	for (i = 0; i < ARRAY_SIZE(keep); i++)
		if (!strncmp(name, keep[i], strlen(keep[i])))
			return true;
	return false;
}

/*
 * sk_ll_usec of a new socket: the system value, unless the frontmost app creates
 * a UDP socket from process context and the WolOS value is higher.
 */
static inline unsigned int wolos_sock_busy_us(unsigned int sys_us, unsigned int fg_us,
					      bool task_ctx, bool dgram, bool inet, bool fg)
{
	if (task_ctx && dgram && inet && fg && fg_us > sys_us)
		return fg_us;
	return sys_us;
}

/* xHCI interrupt moderation: the module parameter when set, else the driver default. */
static inline u32 wolos_xhci_imod_ns(int param_ns, u32 dflt_ns)
{
	return param_ns < 0 ? dflt_ns : (u32)param_ns;
}

#endif
