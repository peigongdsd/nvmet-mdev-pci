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

Request-lifetime page pinning is enabled by default. To compare against the
copy path, set the parameter before creating the mdev controller:

```sh
echo 0 | sudo tee /sys/module/nvmet_mdev_pci/parameters/pinned_io
```

Restore the zero-copy path with `echo 1`. Do not change the parameter during a
benchmark run. `transport_stats` reports commands, bytes pinned, completions,
actual eventfd interrupts, trapped doorbell kicks, and shadow-doorbell poll
scans.

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
