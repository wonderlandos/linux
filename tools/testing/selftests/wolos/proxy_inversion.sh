#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Waiter latency through a mutex held by a SCHED_IDLE task (lib/wolos_proxy_kunit.c).
# With proxy execution each case must finish within MAX_MS (default 500), the
# holder needs 20 ms of CPU. With sched_proxy_exec=0 it only logs the latency.
set -u
MAX_MS=${MAX_MS:-500}
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root"; exit 4; }
modprobe -r wolos_proxy_kunit 2>/dev/null
b=0
grep -q 'sched_proxy_exec=0' /proc/cmdline && b=0 || b=$MAX_MS
dmesg -C
modprobe kunit
modprobe wolos_proxy_kunit max_ms=$b || { echo "SKIP: no wolos_proxy_kunit"; exit 4; }
modprobe -r wolos_proxy_kunit
dmesg | grep -E 'wolos_proxy: waiter_ns|(not )?ok [0-9]+ (same_cpu|other_cpu|sleeping_owner)' | sed 's/^\[[^]]*\] *//'
dmesg | grep -qE 'not ok [0-9]+ (same_cpu|other_cpu|sleeping_owner|wolos_proxy)' && exit 1
exit 0
