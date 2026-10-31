#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Frame time of a fixed work loop under background load, shield off then on,
# ROUNDS times, interleaved. Prints one line per run: "<off|on> <shield_frame output>".
set -u
cd "$(dirname "$0")"
ROUNDS=${ROUNDS:-5}
WORK_US=${WORK_US:-2000}
PERIOD_US=${PERIOD_US:-4000}
FRAMES=${FRAMES:-2500}
S=/sys/kernel/wolos/shield
[ "$(id -u)" = 0 ] && [ -w $S ] || { echo "SKIP: needs root and $S"; exit 4; }
N=$(nproc)
L=$(./shield_frame -c "$WORK_US")
echo "loops $L cpus $N"
stress-ng --cpu "$N" --sock 2 --hdd 2 --timer 2 --quiet -t 0 >/dev/null 2>&1 &
bg=$!
sleep 3
for r in $(seq "$ROUNDS"); do
	for v in 0 1; do
		echo $v > $S
		o=$(./shield_frame -l "$L" -p "$PERIOD_US" -n "$FRAMES" -w 4 -f)
		echo "$([ $v = 1 ] && echo on || echo off) $o st=$(cat ${S}_status)"
	done
done
echo 0 > $S
kill $bg; wait $bg 2>/dev/null
pkill stress-ng
exit 0
