/* SPDX-License-Identifier: GPL-2.0 */
/*
 * WolKernel policy decisions as pure functions, so KUnit can test them on any
 * arch (lib/wolos_policy_kunit.c). Callers pass in the hardware/task facts.
 */
#ifndef _LINUX_WOLOS_POLICY_H
#define _LINUX_WOLOS_POLICY_H

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

#endif
