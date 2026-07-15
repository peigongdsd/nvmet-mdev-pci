# nvmet mdev PCI development

This tree tracks Linux 7.2-rc and develops an in-kernel VFIO mediated NVMe
PCI target backed by `nvmet`. The initial work separates an nvmet transport's
configfs implementation name from its NVMe transport type so that the physical
PCI endpoint and mediated PCI implementations can coexist.

## Enter the environment

```sh
nix develop path:.
```

The shell sets `KBUILD_OUTPUT=$PWD/.build`. Generated kernel files stay out of
the source tree.

## Configure and build

```sh
make defconfig
scripts/config --file "$KBUILD_OUTPUT/.config" --enable PCI_ENDPOINT
scripts/config --file "$KBUILD_OUTPUT/.config" --enable VFIO
scripts/config --file "$KBUILD_OUTPUT/.config" --enable BLK_DEV_NVME
scripts/config --file "$KBUILD_OUTPUT/.config" --enable NVME_TARGET
scripts/config --file "$KBUILD_OUTPUT/.config" --module NVME_TARGET_PCI_EPF
scripts/config --file "$KBUILD_OUTPUT/.config" --module NVME_TARGET_MDEV_PCI
scripts/config --file "$KBUILD_OUTPUT/.config" --enable KUNIT
scripts/config --file "$KBUILD_OUTPUT/.config" --module NVMET_PCI_KUNIT_TEST
make olddefconfig
make -j"$(nproc)" drivers/nvme/target/
```

`BLK_DEV_NVME` selects the promptless `NVME_CORE` symbol required by the PCI
target transports. `NVME_TARGET_MDEV_PCI` similarly selects `VFIO_MDEV`.

Use `LLVM=1` on each `make` invocation to build with Clang. Run sparse against
changed target code with:

```sh
make C=2 CHECK="sparse" drivers/nvme/target/
```

The `nvmet-pci-common` KUnit suite covers queue-full wraparound, SQ/CQ index
and phase transitions, completion-entry encoding, single-subsystem port
selection, and valid/invalid admin controller configurations. Running it
requires a test kernel; compiling the suite does not execute or load it on the
host.
Run it in an isolated UML kernel with:

```sh
tools/testing/kunit/kunit.py run \
  --kunitconfig .kunitconfig \
  --build_dir .kunit \
  --jobs "$(nproc)" \
  'nvmet-pci-common.*'
```

## Implementation stages

1. Allow multiple named implementations of one NVMe transport type.
2. Add backend-neutral PCI queue and PRP/SGL helpers and convert PCI EPF.
3. Register an `mdev-pci` transport and mediated-device parent per nvmet port.
4. Expose PCI config space, BAR0 and MSI-X through VFIO.
5. Allocate an nvmet controller per mdev UUID and implement admin queues.
6. Pin guest queue/data pages and execute I/O directly through nvmet.
7. Harden reset, removal and IOVA invalidation, then add shadow doorbells.

The normal BAR doorbells will remain trapped so they can wake an idle backend.
The steady-state fast path will use NVMe shadow doorbells in guest memory.

## Current milestone

Setting an nvmet port's `addr_trtype` to `mdev-pci` and enabling the port now
registers `/sys/class/nvmet-mdev-pci/nvmet-mdev-pci-<port>/` as an mdev parent.
Its `nvme` type permits one mediated device. The device exposes a 4 KiB PCI
configuration region, a trapped 16 KiB NVMe BAR0, reset support and 16 MSI-X
vectors through VFIO. The guest-memory layer pins ordinary VFIO IOVA mappings
in bounded batches, creates a kernel virtual mapping for queue access and
unpins every overlapping range synchronously on VFIO invalidation. A valid
`CC.EN` transition creates the admin SQ/CQ, reports `CSTS.RDY`, consumes trapped
doorbells, executes admin requests through nvmet and posts completions through
MSI-X vector 0.

Hugepages are not required: VFIO resolves each guest IOVA page to its backing
host page, and the driver builds one kernel virtual mapping from that page
array. Admin register validation is shared with `nvmet-pci-epf`, including
CC command-set and entry-size checks, AQA reserved bits, CAP.MQES limits and
4 KiB ASQ/ACQ alignment.

Admin data uses PRPs and `vfio_dma_rw()` with a reported 1 MiB MDTS. This is
appropriate for low-rate control traffic and does not impose a hugepage
requirement. Admin SGL payloads currently receive an NVMe SGL error. I/O queue
create/delete callbacks currently return an NVMe queue error rather than
leaving a NULL transport callback.

The next functional boundary is I/O queue creation and deletion followed by a
pinned-page PRP/SGL data path. The latter must retain pages through asynchronous
backend I/O and revoke them synchronously when VFIO calls `dma_unmap`.
