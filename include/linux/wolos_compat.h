/* SPDX-License-Identifier: GPL-2.0 */
/*
 * WolKernel legacy-path decisions as pure functions, so KUnit can test them
 * on any arch (lib/wolos_compat_kunit.c). Callers pass in the facts.
 */
#ifndef _LINUX_WOLOS_COMPAT_H
#define _LINUX_WOLOS_COMPAT_H

#include <linux/types.h>

/*
 * Audit: does a task need the syscall entry/exit audit work? Only when audit
 * may record its syscalls, i.e. its filter state is not DISABLED and at least
 * one syscall rule exists. With audit enabled and no rules the hooks would
 * only build a dummy context on every syscall.
 */
static inline bool wolos_audit_needs_work(bool lazy, bool task_disabled,
					  int n_rules)
{
	if (task_disabled)
		return false;
	return !lazy || n_rules > 0;
}

/*
 * int 0x80 from a 64-bit task: served only when the sysctl allows it.
 * 32-bit tasks (Steam, Wine, old games) always get it.
 */
static inline bool wolos_int80_allowed(bool user_64bit, bool sysctl_int80_64)
{
	return !user_64bit || sysctl_int80_64;
}

/*
 * Retbleed on Intel parts without enhanced IBRS (Skylake to Coffee Lake):
 * prefer call-depth stuffing over IBRS on every kernel entry, when the kernel
 * carries call-depth tracking. retbleed=ibrs on the command line still wins.
 */
static inline bool wolos_retbleed_prefer_stuff(bool wanted, bool cdt_built,
					       bool eibrs)
{
	return wanted && cdt_built && !eibrs;
}

/*
 * Kernel defaults that sysctl.d used to set after early boot, so the
 * initramfs and containers see the same values (CONFIG_WOLOS_HINTS):
 * - max_map_count: games and Proton map far more than the 16-bit ELF
 *   legacy limit (65530); memcg accounting bounds a runaway process.
 * - watchdog_thresh 30: the soft-lockup sampler (thresh * 2 / 5 s) runs the
 *   stopper on every CPU every 12 s instead of every 4 s.
 */
#define WOLOS_MAX_MAP_COUNT	1048576
#define WOLOS_WATCHDOG_THRESH	30

#endif /* _LINUX_WOLOS_COMPAT_H */
