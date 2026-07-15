#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Capture host-side cost while a guest benchmark runs independently.
set -eu

if [ "$#" -lt 4 ]; then
	echo "usage: $0 QEMU_PID MDEV_SYSFS_DIR OUTPUT_DIR SECONDS"
	exit 2
fi

pid=$1
mdev=$2
output=$3
runtime=$4

test -d "/proc/$pid"
test -r "$mdev/transport_stats"
mkdir -p "$output"

uname -a > "$output/host-uname.txt"
tr '\000' ' ' < "/proc/$pid/cmdline" > "$output/qemu-command.txt"
cp "$mdev/transport_stats" "$output/transport-stats-before.txt"
cp /proc/interrupts "$output/interrupts-before.txt"

perf stat -p "$pid" \
	-e task-clock,cycles,instructions,context-switches,cpu-migrations \
	-e kvm:kvm_exit -o "$output/perf-stat.txt" -- sleep "$runtime"

cp "$mdev/transport_stats" "$output/transport-stats-after.txt"
cp /proc/interrupts "$output/interrupts-after.txt"
dmesg > "$output/host-dmesg.txt"
