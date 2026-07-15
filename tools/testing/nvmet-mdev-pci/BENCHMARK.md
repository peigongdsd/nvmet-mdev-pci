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

The `transport_stats` file reports commands, bytes pinned, completions, actual
eventfd interrupts, trapped doorbell kicks and shadow-doorbell poll scans. For
an A/B comparison, boot once with the default pinned path, then set the module
parameter `pinned_io=0` before creating the controller to retain the Layer 1
copy path. Use identical guest images, namespace sizes, fio versions and CPU
affinity for both runs.
