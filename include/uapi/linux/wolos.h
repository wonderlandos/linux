/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * WolKernel userspace ABI, version WOLOS_ABI_VERSION.
 * QoS classes are applied per thread with sched_setattr(2): policy, nice and
 * util clamps below. scx_wolos and EEVDF both read them; nothing else is new.
 * /sys/kernel/wolos/abi_version     (r)  WOLOS_ABI_VERSION
 * /sys/kernel/wolos/foreground_pid  (rw, root) tgid of the frontmost app, 0 = none
 * /sys/kernel/wolos/background_timerslack_ns (rw, root, v2) timer slack given to
 *     SCHED_IDLE (WOLOS_QOS_BACKGROUND) threads so their wakeups coalesce; 0 = off
 */
#ifndef _UAPI_LINUX_WOLOS_H
#define _UAPI_LINUX_WOLOS_H

#define WOLOS_ABI_VERSION		2

enum wolos_qos_class {
	WOLOS_QOS_USER_INTERACTIVE = 0,	/* compositor, shell, input: SCHED_NORMAL nice -10, uclamp 768..1024 */
	WOLOS_QOS_USER_INITIATED = 1,	/* frontmost app: SCHED_NORMAL nice -5, uclamp 384..1024 */
	WOLOS_QOS_DEFAULT = 2,		/* SCHED_NORMAL nice 0, uclamp 0..1024 */
	WOLOS_QOS_UTILITY = 3,		/* SCHED_NORMAL nice 10, uclamp 0..512 */
	WOLOS_QOS_BACKGROUND = 4,	/* SCHED_IDLE nice 19, uclamp 0..256 */
};

/* Lowest uclamp_min that marks a class (scx_wolos classification). */
#define WOLOS_UCLAMP_INTERACTIVE	768
#define WOLOS_UCLAMP_FOREGROUND		384
#define WOLOS_UCLAMP_UTILITY_MAX	512

#endif /* _UAPI_LINUX_WOLOS_H */
