# nvmet-mdev-pci performance patch plan

## Implementation status

The performance work now has two batches. The first implemented PRP collection,
request-lifetime VFIO pins, transport-owned SG tables, DMA invalidation drains,
explicit IOD references, parallel I/O workers, SQ/CQ batching, a mempool, real
interrupt coalescing, generic nvmet Doorbell Buffer Config support, pinned
shadow/event arrays, and adaptive polling.

The second batch targets the CPU cost observed in the first VM benchmark. It
adds inline request metadata, a bounded reusable pin cache with batched cold
misses, direct SQ submission, direct cached-payload completion, narrower
controller locking, CQ scheduling coalescing, and event-index-driven bounded
polling. Every optimization has a module-parameter switch, snapshotted per mdev
controller, so one kernel build can run the complete A/B matrix.

Measurement uses per-mdev `transport_stats` counters rather than debugfs and
tracepoints in the first batch. This avoids unconditional per-page atomics and
keeps the instrumentation usable from the benchmark scripts. Full PRP-list and
event-index behavior has focused KUnit coverage. The remaining acceptance gate
is a rebuilt-kernel VM run followed by the fio/host-perf matrix; compile success
alone does not establish runtime correctness or a performance gain.

The pre-optimization reference run reached about 1.1-1.16 GiB/s for 128 KiB
sequential I/O and 31.7k IOPS for a 4 KiB 70/30 random mix at aggregate QD128.
The random phase consumed about 46.7 percent system CPU across 20 host CPUs,
roughly nine cores or 284 host CPU microseconds per I/O. Only 8,967 trapped
doorbells and 483,916 interrupts served about 9.97 million commands, while the
poller performed about 940 million queue scans. Those numbers make page-pin
calls, workqueue transitions, allocation, controller-lock traffic, and polling
the primary hypotheses for this batch.

## Runtime experiment matrix

All controls default on. Change them only after stopping QEMU and removing the
mdev, then recreate the mdev and confirm `$MDEV/runtime_config`.

| Switch | Isolated hypothesis | Expected evidence |
| --- | --- | --- |
| `inline_data` | Small PRP/SG allocations consume CPU. | `prp_heap_allocs` and `payload_sg_heap_allocs` approach zero for 4 KiB and aligned 128 KiB I/O. |
| `pin_cache` | Repeated VFIO pin/unpin dominates random I/O. | Warm-cache `pin_cache_hits` rise while `pin_calls` and `unpin_calls` per command fall. |
| `direct_submit` | Per-command submission work adds scheduling cost. | `submit_work_hops` becomes zero and context switches fall. |
| `direct_complete` | Response work adds another scheduling hop. | With cached pins and lockless I/O, `response_work_hops` becomes zero. |
| `lockless_io` | `ctrl->lock` serializes hot SQ/CQ paths. | Four-job scaling and task-clock per I/O improve without correctness changes. |
| `budget_poll` | Adaptive busy polling wastes host cores. | `poll_queue_checks` per command falls without a QD1 latency regression. |
| `fast_doorbell` | Allocating a buffer and scanning MSI-X state on every 32-bit doorbell wastes CPU. | `fast_doorbell_writes` tracks trapped kicks while allocation profiles and kernel CPU fall. |
| `cq_head_suppress` | Guest CQ-head writes wake workers even when no completion is blocked. | `cq_head_wakeups` and `cq_work_runs` fall while completion counts remain unchanged. |

Measure a copy baseline, a request-lifetime pinned baseline, each switch added
individually in the order above, and the all-on profile. For `pin_cache`, report
both a cold run and an identical warm run, cache hit rate, and the random
working-set size. The default cache holds 65536 pages, or 256 MiB with 4 KiB
pages, and admits requests with up to 64 PRP segments. This covers common
128 KiB requests, including an unaligned first PRP, while retaining a strict
per-controller memory bound. Compare cold and warm runs and lower the capacity
separately when measuring memory/performance tradeoffs.

`poll_runs` counts poll function iterations and `poll_queue_checks` counts
queue-pair loop iterations, including event publication and race checks. Both
polling modes use those definitions, so their
deltas can be compared directly. `pinned_io_bytes` is the amount of command
payload handled by the pinned path, including cache hits; actual pinning cost
is represented by `pin_calls`, `unpin_calls`, and the cache counters.
`fast_doorbell_writes` counts exact aligned 32-bit BAR doorbells handled without
a heap allocation. `cq_head_wakeups` counts CQ-head notifications that actually
needed a worker because suppression was disabled, the head was invalid, or a
full CQ had pending completions.
The payload pin/cache metadata remains protected by one controller-wide mutex.
Use `payload_dma_lock_contentions` to decide whether cache sharding or a
two-phase pin insertion scheme is justified; the current batch does not claim
that the pinned path is lock-free.

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

Instrument commands consumed, payload bytes pinned, completions posted, IRQs
signalled, MMIO doorbell kicks and shadow-doorbell polls. Cumulative counters
live in the mdev's `transport_stats` sysfs file and are updated per batch or
request, not in the per-page or busy-poll loop.

Files:

- `drivers/nvme/target/mdev-pci/priv.h`: batched statistics;
- `drivers/nvme/target/mdev-pci/vfio.c`: `transport_stats` attribute;
- `tools/testing/nvmet-mdev-pci`: guest fio and host perf collection.

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
