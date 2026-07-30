# nvmet-mdev-pci performance patch plan

## Implementation status

The production path implements PRP collection, request-lifetime VFIO pins,
transport-owned SG tables, invalidation drains, explicit IOD references,
parallel I/O submission, lockless SQ/CQ ownership, NVMe interrupt coalescing,
Doorbell Buffer Config, pinned shadow/event arrays, and bounded event-index
polling. Small requests use inline metadata and the pin cache is bounded per
controller.

The latest CPU patch removes the old implementation-choice module switches and
makes the measured fast paths unconditional. Hot counters are per-CPU, MSI-X is
signalled directly when NVMe coalescing is inactive, non-cached responses are
cleaned by independently batched per-SQ worker lanes, and completed IODs retain
lazily grown PRP and payload metadata in a bounded recycle cache. Uncached
request pins run concurrently;
the DMA-unmap path takes an exclusive admission gate before inspecting active
payloads. Cache xarray/LRU mutation remains serialized.

The current notification path removes `ctrl->lock` from aligned 32-bit
doorbell writes. One SQ runner owns the SQ head and submits each copied command
immediately. Completed IODs enter a lockless MPSC list, and one CQ publisher
owns the CQ tail and phase while publishing each CQE immediately. Bounded
draining provides workqueue fairness without waiting for a batch. I/O CQs
retain at most one unacknowledged eventfd notification; a CQ-head update
acknowledges it, and DBBUF event indices are armed so that acknowledgement
traps promptly. Explicit NVMe interrupt coalescing remains a separate standard
policy.

The trace that motivated this patch covered 7.17 million commands and observed
6.88 million CQ worker runs, 6.87 million interrupts, and 13.81 million
doorbells. The old atomic instrumentation performed at least 141.4 million
counter operations, or 19.7 per command. The 4 KiB phase achieved a 98.66
percent pin-cache hit rate, while the roughly 1 MiB phase made about 27 pin and
unpin calls per command, spent 87 percent of measured response time in cleanup,
and observed 7.86 percent contention on the DMA metadata mutex. These data map
directly to per-CPU statistics, response/IOD batching, retained metadata, and
parallel uncached pinning respectively.

Full PRP-list and event-index behavior has focused KUnit coverage. The remaining
acceptance gate is a rebuilt-kernel VM run followed by the fio/host-perf matrix;
compile success alone does not establish runtime correctness or a performance
gain.

## Runtime policy

Sizing, polling and default interrupt-coalescing policy are module parameters.
Their values are snapshotted when an mdev controller is created:

| Parameter | Default | Policy |
| --- | ---: | --- |
| `pin_cache_pages` | 65536 | Maximum persistent guest-page pins per controller. |
| `pin_cache_max_segs` | 64 | Maximum PRP segments admitted to the persistent pin cache. |
| `poll_budget` | 128 | Maximum queue pairs examined by one safety poll. |
| `response_workers` | 0 | Response cleanup workers per I/O SQ; 0 is automatic and 1 forces serialization. |
| `irq_coalesce_threshold` | 0 | Initial NVMe Feature 08 THR value. |
| `irq_coalesce_time` | 0 | Initial NVMe Feature 08 TIME value in 100 us units. |
| `lock_irq_coalescing` | false | Reject guest changes to Features 08 and 09. |
| `mmap_doorbells` | true | Offer the isolated doorbell page through VFIO sparse mmap. |
| `kvm_doorbell_tracking` | true | Request KVM write tracking when sparse mmap and the x86 KVM facility are available. |

Setting either cache limit to zero disables persistent cache entries but keeps
request-lifetime pinned I/O. The default cache holds 256 MiB with 4 KiB pages
and covers common 128 KiB requests, including an unaligned first PRP.
Automatic response-worker sizing is capped by SQ depth, online CPUs, and a
maximum of 64. Explicit values are capped by SQ depth and 64. The admin SQ
remains ordered with one worker.

The interrupt defaults leave standard Feature 08 coalescing disabled. CQEs are
published immediately and the per-CQ acknowledgement policy suppresses only
redundant eventfd signals. If a guest enables Feature 08 with nonzero THR and
TIME, its threshold/timer policy replaces that suppression policy. A controller
reset restores the snapshotted module defaults and clears Feature 09
per-vector coalescing-disable state. Admin completions remain immediate.

Sparse mmap, KVM tracking and DBBUF are separate doorbell mechanisms with
explicit dependencies and fallbacks. Their current combination matrix is kept
in `ARCHITECTURE.md`; this file retains the measurement history rather than
duplicating that contract.

`poll_runs` counts poll function iterations and `poll_queue_checks` counts
queue-pair loop iterations. `pinned_io_bytes` includes cache hits; actual pin
cost is represented by `pin_calls`, `unpin_calls`, and the cache counters.
`response_work_runs`, `response_batches`, and `response_items` show cleanup
aggregation, while `iod_cache_hits` and `iod_cache_misses` show object/metadata
reuse. `payload_dma_lock_contentions` now covers short admission/cache metadata
critical sections rather than the complete uncached `vfio_pin_pages()` call.

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
live in the mdev's `transport_stats` sysfs file and use per-CPU aggregation on
hot paths rather than per-page or busy-poll instrumentation.

Files:

- `drivers/nvme/target/mdev-pci/priv.h`: per-CPU statistics;
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
valuable. The temporary `copy` versus `pinned` debug switch was removed after
the pinned path became the production implementation.

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

## Series 3: parallel execution and lockless queue ownership

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

### Patch 8: serialize SQ ownership without delaying submission

On one doorbell kick, snapshot a validated tail, claim exclusive ownership of
the SQ head and submit each copied SQE immediately. Bound one invocation for
workqueue fairness, not aggregation. Use `READ_ONCE`/`WRITE_ONCE` and the
required DMA barriers for queue indices; reserve the mutex for lifecycle
changes. Allocate IODs from a slab cache or mempool sized to the configured
in-flight limit.

### Patch 9: publish CQEs through one lockless owner

Completed IODs enter a lockless multi-producer list. A single CQ publisher
detaches and reverses one snapshot to FIFO order, retains any unposted
remainder, and publishes each CQE immediately. Write CQE status/phase last
after a DMA barrier. Suppress redundant notifications until CQ-head progress
acknowledges the outstanding interrupt. Preserve CQ-full retry, phase wrapping,
DBBUF event arming and explicit NVMe coalescing.

Success gate: four-job random I/O scales across host CPUs, KVM exits do not
increase per I/O, and p99 latency remains bounded at queue depth 32.

## Series 4: interrupt coalescing and shadow doorbells

### Patch 10: implement the advertised interrupt-coalescing state

Store the NVMe interrupt coalescing threshold/time and per-vector disable bit
instead of accepting and ignoring them. Count completions per vector and use a
high-resolution timer only when a threshold is not reached. Admin completions
and fatal events remain immediate. This original patch used Feature 08
`THR=7`, `TIME=1`; production defaults were later changed to `0/0` after
measurement, while guests can still replace them through Set Features.

Files:

- `mdev-pci/irq.c`: per-vector pending count, timer and signal decision;
- `mdev-pci/queue.c`: report CQ completion progress;
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
3. IOD lifetime, parallel dispatch and lockless queue ownership;
4. coalescing plus shadow doorbells.

Each series gets a fresh VM correctness run and benchmark report. Stop and fix
any correctness or teardown regression before measuring the next series. The
highest expected gain is Series 2 because it removes two payload copies; Series
3 unlocks multi-core throughput; Series 4 reduces VM-exit and interrupt cost at
high queue depth.
