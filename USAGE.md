# nvmet-mdev-pci usage

This guide creates a disposable loop-backed NVMe namespace, exposes it through
an nvmet `mdev-pci` port, creates one mediated device, and assigns that device
to QEMU. Commands that write to the namespace are destructive.

## Install and boot the kernel

The flake exports a NixOS module that selects the patched kernel and loads the
transport:

```nix
{
  inputs.nvmet-mdev-pci = {
    url = "github:peigongdsd/nvmet-mdev-pci?ref=codex/nvmet-mdev-pci";
    inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = inputs@{ nixpkgs, nvmet-mdev-pci, ... }: {
    nixosConfigurations.host = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        nvmet-mdev-pci.nixosModules.default
        ./configuration.nix
      ];
    };
  };
}
```

Rebuild and boot that generation. Confirm that the running kernel and modules
match before configuring a target:

```sh
uname -r
modinfo nvmet-mdev-pci
sudo modprobe nvmet-mdev-pci
```

An IOMMU is recommended for a normal VFIO setup. Hugepages are not required.

## Create a backing namespace

The example below uses a sparse image and loop device. Replace `LOOP` with a
real block device only when overwriting that device is intentional.

```sh
sudo mkdir -p /var/lib/nvmet-mdev-pci
sudo truncate -s 4G /var/lib/nvmet-mdev-pci/test.img
LOOP=$(sudo losetup --find --show /var/lib/nvmet-mdev-pci/test.img)
echo "$LOOP"
```

Mount configfs if the system has not already mounted it:

```sh
sudo modprobe configfs
mountpoint -q /sys/kernel/config || \
  sudo mount -t configfs configfs /sys/kernel/config
```

Create one subsystem and namespace:

```sh
NQN=nqn.2026-07.local.nvmet-mdev:test
SUBSYS=/sys/kernel/config/nvmet/subsystems/$NQN

sudo mkdir -p "$SUBSYS/namespaces/1"
echo 1 | sudo tee "$SUBSYS/attr_allow_any_host"
echo "$LOOP" | sudo tee "$SUBSYS/namespaces/1/device_path"
echo 1 | sudo tee "$SUBSYS/namespaces/1/enable"
```

The transport currently requires exactly one subsystem linked to each mdev
port. Create port 1 and link the subsystem. Creating the first link enables the
port; there is no separate port-enable write.

```sh
PORT=/sys/kernel/config/nvmet/ports/1
sudo mkdir -p "$PORT/subsystems"
echo mdev-pci | sudo tee "$PORT/addr_trtype"
sudo ln -s "$SUBSYS" "$PORT/subsystems/$NQN"
```

The link creates an mdev parent named after the port:

```sh
PARENT=$(readlink -f /sys/class/nvmet-mdev-pci/nvmet-mdev-pci-1)
find "$PARENT/mdev_supported_types" -maxdepth 2 -type f -print
```

## Create the mediated PCI device

The exact type directory includes the parent driver name, so discover it
instead of hard-coding it:

```sh
TYPE=$(find "$PARENT/mdev_supported_types" -mindepth 1 -maxdepth 1 \
  -type d -name '*-nvme' -print -quit)
UUID=$(uuidgen)
cat "$TYPE/available_instances"
echo "$UUID" | sudo tee "$TYPE/create"
echo "$UUID"
```

One mdev instance is allowed per port. Verify the device, VFIO group, and
transport counters:

```sh
MDEV=/sys/bus/mdev/devices/$UUID
GROUP=$(basename "$(readlink -f "$MDEV/iommu_group")")
readlink -f "$MDEV"
echo "/dev/vfio/$GROUP"
cat "$MDEV/transport_stats"
```

## Assign it to QEMU

Pass the mdev through VFIO using its sysfs path. The guest boot disk in this
example is separate from the nvmet-backed test namespace:

```sh
sudo qemu-system-x86_64 \
  -machine q35,accel=kvm \
  -cpu host \
  -smp 4 \
  -m 4G \
  -drive file=guest.qcow2,if=virtio,format=qcow2 \
  -device vfio-pci,sysfsdev="$MDEV" \
  -nographic
```

No QEMU NVMe emulation is involved. The guest's standard `nvme` PCI driver
binds to the device. Linux guests automatically negotiate Doorbell Buffer
Config; no guest parameter is required.

In the guest, verify enumeration:

```sh
lspci -nn | grep -i 'non-volatile memory'
nvme list
nvme id-ctrl /dev/nvme0
```

The repository smoke test overwrites the first 16 MiB of its argument:

```sh
sudo tools/testing/nvmet-mdev-pci/guest-smoke.sh /dev/nvme0n1
```

Set `NVMET_MDEV_TEST_DISCARD=1` to include discard. See
`tools/testing/nvmet-mdev-pci/BENCHMARK.md` for the destructive fio matrix and
host-side perf collection.

## Performance controls and diagnostics

The optimized pinned path is enabled by default. Runtime controls are writable
module parameters, but their values are snapshotted per controller. Stop QEMU,
remove the mdev, set parameters, and create a new mdev for every A/B variant.
Changing a parameter while a controller exists does not alter that controller.

To compare against the Layer 1 copy path:

```sh
echo 0 | sudo tee /sys/module/nvmet_mdev_pci/parameters/pinned_io
```

Restore the pinned path with `echo 1`. The performance switches are:

| Parameter | Default | Effect |
| --- | ---: | --- |
| `pinned_io` | 1 | Give nvmet an SG table over guest pages instead of copying payload data. |
| `inline_data` | 1 | Embed metadata for requests of at most 32 PRP segments. |
| `pin_cache` | 1 | Reuse VFIO payload-page pins across requests. |
| `pin_cache_pages` | 65536 | Bound cached pins per controller; 65536 pages is 256 MiB with 4 KiB pages. |
| `pin_cache_max_segs` | 64 | Admit requests with at most 64 PRP segments, covering common 128 KiB I/O. |
| `direct_submit` | 1 | Submit a consumed SQ batch without a per-command workqueue hop. |
| `direct_complete` | 1 | Avoid response work for cached pinned payloads when lockless I/O is active. |
| `lockless_io` | 1 | Keep the controller mutex out of live SQ/CQ processing. |
| `budget_poll` | 1 | Use event indices plus one bounded safety scan per idle interval. |
| `poll_budget` | 128 | Maximum queue pairs examined by one safety scan. |
| `fast_doorbell` | 1 | Handle exact 32-bit doorbell writes without allocating or scanning unrelated MSI-X state. |
| `cq_head_suppress` | 1 | Wake a CQ worker for head progress only when pending completions are blocked by a full CQ. |

`direct_complete` depends on `pinned_io=1`, `pin_cache=1`, and `lockless_io=1`;
otherwise completions use response work so a softirq cannot enter the
mutex-based controller path. `pin_cache` has no effect on the copy path. Cold
consecutive cache misses are pinned in batches.
The default admission limit covers repeated 4 KiB random I/O and common
128 KiB sequential requests. Lower `pin_cache_max_segs` deliberately when
measuring the memory cost of large-I/O cache reuse. Setting either cache limit
to zero disables caching.

Verify the snapshot and counters on the newly created device:

```sh
cat "$MDEV/runtime_config"
cat "$MDEV/transport_stats"
```

The counters include heap allocations, pin/unpin calls, cache hits/misses and
evictions, submission/response workqueue hops, SQ/CQ batches, interrupts,
doorbell kicks, useful CQ-head wakeups, fast doorbell writes, and poll scans.
They are cumulative for the mdev lifetime.

Useful host checks are:

```sh
cat "$MDEV/transport_stats"
sudo dmesg -w
cat /proc/interrupts
```

## Teardown

Stop QEMU before removing the mdev. Then unwind configuration in dependency
order:

```sh
echo 1 | sudo tee "$MDEV/remove"
sudo rm "$PORT/subsystems/$NQN"
echo 0 | sudo tee "$SUBSYS/namespaces/1/enable"
sudo rmdir "$SUBSYS/namespaces/1"
sudo rmdir "$SUBSYS"
sudo rmdir "$PORT"
sudo losetup -d "$LOOP"
```

If mdev removal reports `EBUSY`, a process still has the VFIO device open.
Find and stop that process before retrying; do not tear down the namespace
under a live VM.
