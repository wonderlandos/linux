/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WOLOS_H
#define _LINUX_WOLOS_H

#include <linux/types.h>
#include <uapi/linux/wolos.h>

#ifdef CONFIG_WOLOS_HINTS
/* tgid of the frontmost app, 0 when unknown. Lockless; drivers read it on hot paths. */
int wolos_foreground_tgid(void);
/* Timer slack for SCHED_IDLE threads, 0 = leave slack alone. */
u64 wolos_background_timerslack_ns(void);
/* uclamp_min floor for threads of the frontmost app, 0 = off. */
unsigned int wolos_foreground_uclamp_min(void);
void wolos_uclamp_refresh_tgid(int tgid);
#else
static inline int wolos_foreground_tgid(void) { return 0; }
static inline u64 wolos_background_timerslack_ns(void) { return 0; }
static inline unsigned int wolos_foreground_uclamp_min(void) { return 0; }
static inline void wolos_uclamp_refresh_tgid(int tgid) { }
#endif

#endif /* _LINUX_WOLOS_H */
