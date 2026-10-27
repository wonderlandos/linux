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

#endif /* _LINUX_WOLOS_H */
