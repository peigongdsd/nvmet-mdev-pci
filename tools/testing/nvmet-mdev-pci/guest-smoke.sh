#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

usage()
{
	echo "Usage: $0 /dev/nvmeXnY [log-file]" >&2
	echo "This test overwrites the first 16 MiB of the namespace." >&2
	exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
dev=$1
log=${2:-/tmp/nvmet-mdev-pci-guest-smoke.log}

case "$dev" in
/dev/nvme*n*) ;;
*) usage ;;
esac

[ -b "$dev" ] || {
	echo "Not a block device: $dev" >&2
	exit 1
}

ctrl=${dev#/dev/}
ctrl=${ctrl%n*}
reset=/sys/class/nvme/$ctrl/reset_controller
state=/sys/class/nvme/$ctrl/state
input=/tmp/nvmet-mdev-pci-input.bin
output=/tmp/nvmet-mdev-pci-output.bin
size_mb=16

exec >"$log" 2>&1

echo "nvmet-mdev-pci Layer 1 guest smoke test"
date -u
uname -a
ls -l "$dev"
cat "/sys/class/block/${dev#/dev/}/size"
[ "$(blockdev --getsize64 "$dev")" -ge $((size_mb * 1024 * 1024)) ] || {
	echo "Namespace is smaller than the $size_mb MiB test range"
	exit 1
}

dd if=/dev/urandom of="$input" bs=1M count=$size_mb status=progress
input_hash=$(sha256sum "$input")
echo "input: $input_hash"

dd if="$input" of="$dev" bs=1M count=$size_mb oflag=direct conv=fsync \
	status=progress
blockdev --flushbufs "$dev"
dd if="$dev" of="$output" bs=1M count=$size_mb iflag=direct status=progress
cmp "$input" "$output"
echo "write/read/flush: PASS"

if [ "${NVMET_MDEV_TEST_DISCARD:-0}" = 1 ]; then
	blkdiscard --offset 0 --length $((size_mb * 1024 * 1024)) "$dev"
	echo "discard completion: PASS"
	dd if="$input" of="$dev" bs=1M count=$size_mb oflag=direct conv=fsync \
		status=progress
fi

if [ "${NVMET_MDEV_TEST_RESET:-0}" = 1 ]; then
	[ -w "$reset" ] || {
		echo "Reset attribute is unavailable: $reset"
		exit 1
	}
	echo 1 >"$reset"
	i=0
	while { [ ! -b "$dev" ] || [ ! -r "$state" ] ||
		[ "$(cat "$state")" != live ]; } && [ "$i" -lt 100 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	[ -b "$dev" ] && [ -r "$state" ] && [ "$(cat "$state")" = live ] || {
		echo "Namespace did not return after controller reset"
		exit 1
	}
	dd if="$dev" of="$output" bs=1M count=$size_mb iflag=direct status=progress
	cmp "$input" "$output"
	echo "controller reset and reread: PASS"
fi

echo "Layer 1 guest smoke test: PASS"
