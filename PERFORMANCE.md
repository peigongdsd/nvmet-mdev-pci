# nvmet-mdev-pci performance patch plan

## Goals and constraints

The performance work must preserve a normal VFIO/QEMU deployment: no SPDK,
hugetlbfs or guest hugepage requirement. Ordinary 4 KiB guest pages remain the
base case. Every series must leave a bootable controller and must pass buffered
and direct I/O, discard, reset, queue teardown and VFIO DMA-unmap tests.

Performance is measured rather than inferred. The primary metrics are guest
IOPS, bandwidth, mean and p99 latency, host CPU time per I/O, and KVM exits per
I/O. Compare against the Layer 1 copy path, QEMU's built-in NVMe device and
host `nvme-loop` as a target-core ceiling. Use at least these fio workloads:

- 4 KiB random read and write at queue depths 1, 8 and 32;
- 128 KiB sequential read and write at queue depths 1 and 32;
- one and four jobs, with `io_uring`, `direct=1`, and a fixed test duration;
- buffered read/readahead correctness outside fio.

Record guest fio JSON, host `perf stat`, `kvm:kvm_exit`, interrupt counts and
host/guest kernel logs. A performance patch is not accepted if it introduces
an NVMe error, host warning, leaked pin, teardown stall or unexplained p99
latency regression.

## Series 1: measurement and data-path groundwork

### Patch 1: add a repeatable benchmark and stress harness

Add `tools/testing/nvmet-mdev-pci/host-bench.sh` and `guest-bench.sh`. Reuse the
disposable loop-backed VM setup and write each run to a timestamped directory.
Include reset loops, queue recreation, QEMU termination, buffered reads and DMA
unmap during idle and active I/O. Capture enough metadata to reproduce the
kernel, module, QEMU and fio versions.

### Patch 2: add low-overhead transport tracepoints and counters

Instrument commands consumed, bytes copied, payload pages pinned, completions
posted, IRQs signalled, MMIO doorbell kicks, shadow-doorbell polls and DMA-unmap
drains. Tracepoints are disabled by default; cumulative counters live in
debugfs. Do not put unconditional atomics in the per-page loop.

Files:

- `drivers/nvme/target/mdev-pci/trace.h`: transport trace events;
- `drivers/nvme/target/mdev-pci/debugfs.c`: per-controller snapshots;
- `drivers/nvme/target/mdev-pci/priv.h`: per-CPU or batched statistics;
- `drivers/nvme/target/mdev-pci/Makefile`: generated trace translation unit.

### Patch 3: separate PRP parsing from payload movement

Turn the current PRP walker into an iterator that yields validated
`(iova, offset, length)` segments. Keep the copy implementation as one iterator
consumer. Add KUnit coverage for PRP1 offsets, direct PRP2, 256-byte small
lists, full lists, chained lists, malformed alignment and the 1 MiB MDTS bound.
This is a behavior-preserving prerequisite for page pinning.

Files and functions:

- `mdev-pci/queue.c`: `nvmet_mdev_walk_prps()` and copy callback;
- `mdev-pci/prp-test.c`: iterator KUnit tests with a fake IOVA reader;
- `drivers/nvme/target/Makefile` and `Kconfig`: test target.

## Series 2: request-lifetime zero-copy payloads

### Patch 4: introduce payload pin objects

Add a payload mapping distinct from the long-lived SQ/CQ mapping. Coalesce
adjacent IOVAs into runs, call `vfio_pin_pages()` in bounded batches, and build
an `sg_table` directly from returned `struct page` objects. Preserve the first
PRP offset and final partial-page length.

Pass `IOMMU_WRITE` for NVMe reads because the target writes guest memory; this
also gives iommufd the information needed to dirty writable pages. Pass
`IOMMU_READ` for NVMe writes. Track every pinned run in the owning IOD and
balance every successful partial pin on all error paths.

Files:

- `mdev-pci/iova.c`: payload pin, SG construction and unpin helpers;
- `mdev-pci/priv.h`: `nvmet_mdev_payload` and pinned-run structures;
- `mdev-pci/queue.c`: PRP iterator integration.

### Patch 5: execute I/O directly on pinned guest pages

For I/O queues, assign the pinned SG table to `nvmet_req` and execute the
backend without the 4 KiB bounce buffer or nvmet-owned copy SG. Keep admin data
on the copy path because its traffic is small and the simpler lifetime is
valuable. Keep a temporary debug switch for `copy` versus `pinned` I/O so the
same kernel can provide an A/B baseline; remove or hide it once the pinned path
is stable.

The transport, not nvmet core, owns a pinned payload's SG table. Mark that
ownership explicitly in the IOD cleanup path: do not pass the custom SG to
`nvmet_req_free_sgls()`. Clear `req->sg` and `req->sg_cnt` when backend access
ends, then free the SG table and unpin its runs from the IOD's final release.
This must remain distinct from the copy path, whose SG is allocated and freed
by `nvmet_req_alloc_sgls()` and `nvmet_req_free_sgls()`.

Success gate: identical correctness results, at least 50 percent fewer host
cycles per byte for 128 KiB I/O, and no material queue-depth-1 latency loss.

### Patch 6: make invalidation and teardown wait for payload pins

The VFIO `dma_unmap` callback must stop SQ intake, mark the controller fatal,
drain nvmet requests and completion work, then unpin every overlapping payload
before returning. Never unpin a page while the block backend owns its SG entry.
Exercise partial pin failures, QEMU exit during I/O, controller reset under fio,
and repeated attach/detach. Add lockdep assertions for state-lock ordering.

This patch is a release gate for zero-copy. Performance work must not proceed
to parallel dispatch until active-unmap stress completes without leaks or
stalls.

## Series 3: parallel execution and batching

### Patch 7: give IODs explicit lifetime references

The ordered SQ workqueue currently prevents an inline nvmet completion from
freeing an IOD while its submit callback is still running. Replace that implicit
guarantee with an IOD refcount: submit work owns one reference and queued
response work owns another. `queue_response()` may then run concurrently while
the IOD is freed only after both paths finish.

Keep SQ0 ordered for admin queue lifecycle commands. I/O SQs use an unbound
controller workqueue with concurrency bounded by queue depth and a controller
limit. Run KASAN, KCSAN and lockdep configurations before enabling it by
default.

### Patch 8: batch SQ consumption and reduce controller-mutex scope

On one doorbell kick, snapshot a validated tail, copy a bounded batch of SQEs,
advance the head, and submit them without taking `ctrl->lock` once per command.
Use `READ_ONCE`/`WRITE_ONCE` and the required DMA barriers for queue indices;
reserve the mutex for lifecycle changes. Allocate IODs from a slab cache or
mempool sized to the configured in-flight limit.

### Patch 9: batch CQ publication and completion interrupts

The CQ worker already drains multiple completions, but scheduling can fragment
one burst into many worker runs. Use one pending bit per CQ, splice a completion
batch under the CQ spinlock, publish all available CQEs, issue one `dma_wmb()`,
and signal at most one interrupt for the batch. Preserve CQ-full retry and phase
handling.

Success gate: four-job random I/O scales across host CPUs, KVM exits do not
increase per I/O, and p99 latency remains bounded at queue depth 32.

## Series 4: interrupt coalescing and shadow doorbells

### Patch 10: implement the advertised interrupt-coalescing state

Store the NVMe interrupt coalescing threshold/time and per-vector disable bit
instead of accepting and ignoring them. Count completions per vector and use a
high-resolution timer only when a threshold is not reached. Admin completions
and fatal events remain immediate. Keep defaults equivalent to the current
behavior until benchmarks select conservative values.

Files:

- `mdev-pci/irq.c`: per-vector pending count, timer and signal decision;
- `mdev-pci/queue.c`: report CQ batch completion;
- `mdev-pci/priv.h`: vector state;
- `mdev-pci/queue.c`: Feature get/set plumbing.

### Patch 11: add generic PCI Doorbell Buffer Config plumbing to nvmet

Implement admin opcode `nvme_admin_dbbuf` for PCI controllers and add a
transport callback carrying the shadow-doorbell and event-index IOVAs. Advertise
`NVME_CTRL_OACS_DBBUF_SUPP` only when the transport supplies that callback.
Fabrics transports remain unchanged. Validate reserved fields, alignment,
queue-array size and controller state in the common admin layer.

Files:

- `drivers/nvme/target/admin-cmd.c`: opcode parse/execute and Identify bit;
- `drivers/nvme/target/nvmet.h`: optional PCI transport callback;
- `mdev-pci/queue.c`: transport callback implementation.

### Patch 12: pin shadow doorbells and event indices

Pin the two guest arrays as long-lived mappings, read SQ tails and CQ heads from
the shadow array, and write event indices with correct barriers. A trapped BAR
doorbell remains the wakeup path and fallback. DMA invalidation of either array
must disable the controller before unpinning it.

Use the same wrap-safe event test as the host driver:
`(u16)(new - event - 1) < (u16)(new - old)`. Before a poller sleeps, publish an
event index requesting the next doorbell, issue a full barrier, and reread the
shadow doorbell. The guest publishes its shadow after its SQE and performs a
full barrier before reading the event index. If either side observes the race,
the guest performs the trapped BAR write or the target stays awake. Document
this producer/consumer pairing next to the barriers and cover wraparound and
the publish-versus-sleep race with KUnit tests.

### Patch 13: add an adaptive shadow-doorbell poller

Borrow the mature round-robin and idle-backoff structure from
`nvmet_pci_epf_poll_sqs_work()`. After an MMIO kick or active completion, poll
live SQ shadows for a bounded busy window. When idle, update event indices and
perform the barrier-and-recheck handshake before sleeping until the next
trapped kick or delayed poll. Expose conservative poll budget/idle values
through debugfs for experiments, not as permanent ABI.

Success gate: at queue depth 32, reduce doorbell-related KVM exits per I/O by at
least 80 percent without continuously consuming a host CPU when the guest is
idle. Retain a runtime fallback to trapped doorbells for diagnosis.

## Merge order and stopping points

Submit the work as four reviewable series, not one large change:

1. measurement plus PRP iterator;
2. payload pinning plus invalidation safety;
3. IOD lifetime, parallel dispatch and batching;
4. coalescing plus shadow doorbells.

Each series gets a fresh VM correctness run and benchmark report. Stop and fix
any correctness or teardown regression before measuring the next series. The
highest expected gain is Series 2 because it removes two payload copies; Series
3 unlocks multi-core throughput; Series 4 reduces VM-exit and interrupt cost at
high queue depth.
