#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# memory.pressure of a dormant cgroup matches an eager one once it was read.
# Prints "lazy|eager some_total_us avg10" per run; fails on a zero reading.
P=/sys/module/wolos_sched/parameters/psi_lazy
R=/sys/fs/cgroup/wolos_psi_test
[ "$(id -u)" = 0 ] && [ -w "$P" ] || { echo "SKIP: needs root and wolos_sched.psi_lazy"; exit 4; }
command -v stress-ng >/dev/null || { echo "SKIP: needs stress-ng"; exit 4; }
old=$(cat $P); rc=0
echo +memory > /sys/fs/cgroup/cgroup.subtree_control
mkdir -p $R; echo +memory > $R/cgroup.subtree_control
for r in $(seq ${ROUNDS:-3}); do for v in 1 0; do
	echo $v > $P
	g=$R/t$r$v; mkdir $g; echo 64M > $g/memory.high
	[ $v = 1 ] && n=lazy || n=eager
	cat $g/memory.pressure > /dev/null
	(echo $BASHPID > $g/cgroup.procs; exec stress-ng --vm 2 --vm-bytes 256M -t ${SECS:-12} --quiet) &
	s=$!; sleep ${SECS:-12}
	t=$(awk '/^some/{sub("total=","",$5); print $5}' $g/memory.pressure)
	a=$(awk '/^some/{sub("avg10=","",$2); print $2}' $g/memory.pressure)
	echo "$n $t $a"
	echo max > $g/memory.high; wait $s; sleep 1
	[ "${t:-0}" -gt 0 ] || rc=1
	rmdir $g 2>/dev/null
done; done
echo $old > $P
rmdir $R 2>/dev/null
exit $rc
