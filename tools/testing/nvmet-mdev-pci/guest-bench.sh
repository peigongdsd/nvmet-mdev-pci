#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Destructive fio matrix for a disposable nvmet-mdev-pci namespace.
set -eu

if [ "$#" -lt 2 ]; then
	echo "usage: $0 /dev/nvmeXnY OUTPUT_DIR [SECONDS]"
	exit 2
fi

dev=$1
output=$2
runtime=${3:-30}

test -b "$dev"
command -v fio >/dev/null
mkdir -p "$output"

uname -a > "$output/guest-uname.txt"
nvme id-ctrl "$dev" > "$output/id-ctrl.txt"
nvme id-ns "$dev" > "$output/id-ns.txt"

run_fio()
{
	name=$1
	rw=$2
	bs=$3
	depth=$4
	jobs=$5

	fio --name="$name" --filename="$dev" --rw="$rw" --bs="$bs" \
		--iodepth="$depth" --numjobs="$jobs" --ioengine=io_uring \
		--direct=1 --time_based=1 --runtime="$runtime" --ramp_time=3 \
		--group_reporting=1 --randrepeat=1 --norandommap=1 \
		--output-format=json --output="$output/$name.json"
}

for rw in randread randwrite; do
	for depth in 1 8 32; do
		for jobs in 1 4; do
			run_fio "${rw}-4k-qd${depth}-j${jobs}" "$rw" 4k \
				"$depth" "$jobs"
		done
	done
done

for rw in read write; do
	for depth in 1 32; do
		for jobs in 1 4; do
			run_fio "${rw}-128k-qd${depth}-j${jobs}" "$rw" 128k \
				"$depth" "$jobs"
		done
	done
done

dmesg > "$output/guest-dmesg.txt"
