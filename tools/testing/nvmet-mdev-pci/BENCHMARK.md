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
reports commands, allocation and pin-cache behavior, response batching, IOD
reuse, actual eventfd interrupts, trapped doorbell kicks, and shadow-doorbell
poll activity. The second records the active capacity and polling policy.
Use counter deltas for each run. `poll_runs` is the number of poll iterations,
while `poll_queue_checks` is the number of queue-pair loop iterations, including
event publication and race checks; neither is a time measurement.
`pinned_io_bytes` includes payload served from cached pins.
`pin_cache_permission_fallbacks` reports requests retried with direction-specific
request-lifetime pins because a bidirectional cache pin was not permitted.
`payload_dma_lock_contentions` counts payload requests that could not acquire
the shared pin/cache metadata mutex immediately. Uncached request-lifetime
pins run in parallel behind the DMA invalidation gate.

Implementation-choice A/B switches have been removed. To compare cache sizing
or poll budgets, stop QEMU and remove the mdev, change the relevant parameter,
then create a fresh mdev. Parameters are snapshotted when the controller object
is created:

```sh
echo 32768 | sudo tee /sys/module/nvmet_mdev_pci/parameters/pin_cache_pages
echo 32 | sudo tee /sys/module/nvmet_mdev_pci/parameters/pin_cache_max_segs
echo 64 | sudo tee /sys/module/nvmet_mdev_pci/parameters/poll_budget
```

The default pin cache is 65536 pages and admits up to 64 PRP segments per
request. Setting either cache limit to zero measures request-lifetime pinning
without persistent cache entries. Record `response_work_runs`,
`response_batches`, `response_items`, `iod_cache_hits`, and `iod_cache_misses`
alongside the existing queue and pin counters.
Use identical guest images, namespace sizes, fio versions, CPU affinity, and
cache warmup for all runs. Cache hit rate must be reported alongside IOPS: a
small cache and a large uniform-random working set can otherwise make the
comparison misleading.
