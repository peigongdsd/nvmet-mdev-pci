# nvmet mdev PCI development

This tree tracks Linux 7.2-rc and develops an in-kernel VFIO mediated NVMe
PCI target backed by `nvmet`. The initial work separates an nvmet transport's
configfs implementation name from its NVMe transport type so that the physical
PCI endpoint and mediated PCI implementations can coexist.

## Enter the environment

```sh
nix develop path:.
```

The shell sets `KBUILD_OUTPUT=$PWD/.build` and configures ccache under
`$PWD/.cache/ccache`. Generated kernel files stay out of the source tree, and
both object files and compiler-cache entries survive leaving the shell.
Set `KBUILD_CC` or `KBUILD_HOSTCC` before entering the shell to replace the
default `ccache gcc` commands. `LLVM=1` on a make invocation still selects the
kernel's Clang toolchain.

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

## Fast inner loop

Do not use the NixOS build for every source edit. Keep `.build` and compile the
smallest affected target first:

```sh
nix develop path:. --command \
  make -j"$(nproc)" drivers/nvme/target/
nix develop path:. --command ccache --show-stats
```

Kbuild recompiles only objects whose inputs changed. Ccache also avoids
recompiling an object after switching revisions and returning to equivalent
source. Do not run `make clean` or delete `.build`/`.cache/ccache` during normal
development.

The NixOS kernel derivation remains sandboxed and reproducible, so it cannot
reuse `.build` directly. Any source change gives that derivation a new source
hash and causes one clean kernel build. Reserve that build for a runtime test
checkpoint:

```sh
nix build \
  /persistent/nixos#nixosConfigurations.KruslPC.config.system.build.toplevel \
  --no-link
run0 nixos-rebuild boot --flake /persistent/nixos#KruslPC
```

The second command reuses the completed system closure; it should only install
the boot generation. Reboot before testing changes to nvmet core.

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
6. Implement I/O queues and a bounded copy data path through nvmet.
7. Harden reset, removal and IOVA invalidation.
8. Pin payload pages for zero-copy I/O, then add shadow doorbells and batching.

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

Layer 1 implements admin and I/O SQ/CQ creation and deletion, per-queue trapped
doorbells, per-CQ completion routing and MSI-X delivery. Command payloads use
PRPs and `vfio_dma_rw()` with a reported 1 MiB MDTS. Writes are copied from
guest memory into nvmet-owned scatterlists before execution; successful reads
are copied back before the completion is posted. This path supports ordinary
guest pages and does not require hugepages. The controller does not advertise
SGL support, so conforming hosts use the implemented PRP path.

Queue mappings remain pinned while their queues are live. Overlapping queue
mappings are rejected, and a VFIO DMA invalidation synchronously disables the
controller before unpinning affected pages. MSI-X events raised while a vector
is masked or has no eventfd are retained in the pending-bit array and replayed
after the vector becomes usable.

The first rebuilt-kernel VM run proved namespace enumeration, direct write/read
comparison, flush, discard and controller reset. It also found that treating
Linux's 256-byte small PRP-list pool as a page-aligned 4 KiB mapping broke
buffered readahead. The PRP walker now accepts naturally aligned list pointers
and fetches only the entries required by the command.
`tools/testing/nvmet-mdev-pci/guest-smoke.sh` treats new buffered-read kernel
errors as a test failure. A rebuilt-kernel rerun remains required to validate
the regression fix.

Layer 2 replaces payload copies with request-lifetime page pinning, then adds
parallel submission, shadow doorbells, completion batching and interrupt
coalescing. Those changes are performance work and are intentionally separate
from the functionally complete copy path. See `PERFORMANCE.md` for the ordered
patch series, safety invariants and measurement gates.
