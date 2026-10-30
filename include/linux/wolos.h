/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WOLOS_H
#define _LINUX_WOLOS_H

#include <uapi/linux/wolos.h>

#ifdef CONFIG_WOLOS_HINTS
/* tgid of the frontmost app, 0 when unknown. Lockless; drivers read it on hot paths. */
int wolos_foreground_tgid(void);
#else
static inline int wolos_foreground_tgid(void) { return 0; }
#endif

#ifdef CONFIG_WOLOS_INPUT_BOOST
#include <linux/notifier.h>
/* Utilization floor for a CPU of capacity @cap while an input boost is open. */
unsigned long wolos_input_boost_util(unsigned long cap);
/* Called (atomic) with the window length in ms when a boost opens. */
extern struct atomic_notifier_head wolos_input_boost_chain;
#else
static inline unsigned long wolos_input_boost_util(unsigned long cap) { return 0; }
#endif

#endif /* _LINUX_WOLOS_H */
