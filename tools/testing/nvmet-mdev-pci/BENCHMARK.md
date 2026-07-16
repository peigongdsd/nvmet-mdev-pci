# Performance measurement

Run benchmarks only against a disposable namespace. `guest-bench.sh` writes to
the entire block device and emits fio JSON for 4 KiB random and 128 KiB
sequential workloads at the queue depths and job counts used by the performance
acceptance gates.

In the guest:

```sh
guest-bench.sh /dev/nvme0n1 /mnt/shared/pinned 30
```

At the same time on the host, locate the QEMU process and mediated-device
sysfs directory, then collect host cost for the expected matrix duration:

```sh
host-bench.sh QEMU_PID /sys/bus/mdev/devices/UUID results/host 660
```

The host collector saves both `transport_stats` and `runtime_config`. The first
reports commands, allocation and pin-cache behavior, workqueue hops, actual
eventfd interrupts, trapped doorbell kicks, and shadow-doorbell poll activity.
The second makes the active runtime-switch snapshot part of the result.
Use counter deltas for each run. `poll_runs` is the number of poll iterations,
while `poll_queue_checks` is the number of queue-pair loop iterations, including
event publication and race checks; neither is a time measurement.
`pinned_io_bytes` includes payload served from cached pins.
`pin_cache_permission_fallbacks` reports requests retried with direction-specific
request-lifetime pins because a bidirectional cache pin was not permitted.
`payload_dma_lock_contentions` counts payload requests that could not acquire
the controller-wide pin/cache mutex immediately.

For an A/B comparison, stop QEMU and remove the mdev, change module parameters,
then create a fresh mdev. Parameters are snapshotted when a controller object is
created; changing them does not mutate an existing controller. Start with these
profiles:

```sh
# Layer 1 copy baseline
for p in inline_data pin_cache direct_submit direct_complete lockless_io budget_poll fast_doorbell msix_scan_suppress cq_head_suppress; do
	echo 0 | sudo tee "/sys/module/nvmet_mdev_pci/parameters/$p"
done
echo 0 | sudo tee /sys/module/nvmet_mdev_pci/parameters/pinned_io

# Request-lifetime pinned baseline
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/pinned_io

# Enable one optimization at a time, then remove and recreate the mdev.
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/inline_data
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/pin_cache
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/direct_submit
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/direct_complete
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/lockless_io
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/budget_poll
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/fast_doorbell
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/msix_scan_suppress
echo 1 | sudo tee /sys/module/nvmet_mdev_pci/parameters/cq_head_suppress
```

`direct_complete` only takes its fast path when `pinned_io`, `pin_cache`, and
`lockless_io` are also enabled. `pin_cache` is irrelevant when `pinned_io=0`.
The default pin cache is 65536 pages and admits up to 64 PRP segments per
request. Record `fast_doorbell_writes`, `cq_head_wakeups`, and `cq_work_runs`
when comparing the notification switches.
Use identical guest images, namespace sizes, fio versions, CPU affinity, and
cache warmup for all runs. Cache hit rate must be reported alongside IOPS: a
small cache and a large uniform-random working set can otherwise make the
comparison misleading.
