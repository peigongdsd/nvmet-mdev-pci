# nvmet-mdev-pci architecture

This document is the current feature and dependency contract. `CONCURRENCY.md`
defines synchronization invariants; `PERFORMANCE.md` records optimization
history and measurements.

## Data path layers

The transport has four layers that should remain independently understandable:

1. **nvmet backend**: nvmet executes each command against the configured block
   device or regular file. Buffered file namespaces retain the host filesystem
   page cache.
2. **guest-memory mapping**: SQs, CQs and admin payloads use long-lived VFIO
   mappings. I/O payload PRPs are pinned for the request lifetime and described
   by a transport-owned SG table. The optional pin cache retains page pins
   across requests; it does not cache data.
3. **queue execution**: one runner owns each SQ head, response lanes release
   payload resources, and one publisher owns each CQ tail and phase. Fairness
   budgets bound work per callback but never delay work to form a batch.
4. **notification**: guest doorbells make queue progress visible to the target;
   CQ publication and MSI-X make completions visible to the guest. Doorbell
   transport and interrupt policy are separate choices.

## Doorbell transport

BAR0 doorbell access has three effective modes. `runtime_config` reports the
current mode as `doorbell_mode`.

| Effective mode | Requirement | Guest store path | Target wakeup |
| --- | --- | --- | --- |
| `trapped` | `mmap_doorbells=0`, or non-4 KiB host pages | VFIO/QEMU `.write()` | The write callback schedules the affected SQ or CQ directly. |
| `mmap-poll` | Sparse mmap enabled, tracking disabled or unavailable | Direct store to the shared BAR0 page | The per-controller poll thread scans live queue pairs with bounded adaptive backoff. |
| `mmap-kvm` | Sparse mmap plus active x86 KVM external write tracking | Direct store followed by a KVM write-track exit | The KVM callback schedules the affected queue; the BAR poller parks unless DBBUF is active. |

`mmap_doorbells` is the parent capability. It is effective only with 4 KiB host
pages because the NVMe doorbells occupy one isolated 4 KiB BAR0 page.
`kvm_doorbell_tracking` depends on that mapping and on x86 KVM external write
tracking. Registration and memslot discovery can fail or be delayed, so
tracking is an acceleration, never a correctness dependency: `mmap-poll` is
the fallback until tracking becomes active.

Doorbell Buffer Config (DBBUF) is orthogonal to those BAR modes. It is
negotiated by the guest through the standard NVMe admin command and redirects
doorbell reads to pinned shadow-doorbell and event-index arrays in guest RAM.
The current implementation polls those arrays. Therefore active DBBUF keeps a
doorbell poller running even when BAR writes use KVM tracking. `runtime_config`
reports this independently as `dbbuf_active`.

The combination matrix is:

| Sparse mmap | KVM tracking requested | KVM tracking active | DBBUF active | Correctness path |
| --- | --- | --- | --- | --- |
| no | irrelevant | no | no | Trapped BAR callbacks only. |
| no | irrelevant | no | yes | Trapped BAR wakeups plus delayed DBBUF safety polling. |
| yes | no | no | no | Adaptive BAR-page polling. |
| yes | yes | no | either | Adaptive polling while KVM registration is unavailable. |
| yes | yes | yes | no | KVM callback; poll thread parked. |
| yes | yes | yes | yes | KVM callback for BAR writes plus polling for DBBUF shadows. |

## Completion and interrupt policy

CQEs are always published immediately. Queue fairness and response cleanup may
process several already-ready items per callback, but neither waits for a
minimum batch size.

Two interrupt policies exist and do not stack:

- With NVMe Feature 08 effectively disabled (`THR=0` or `TIME=0`), an I/O CQ
  signals its first unacknowledged completion immediately. Additional CQEs are
  published without redundant eventfd signals until CQ-head progress
  acknowledges the interrupt. If published CQEs remain, the acknowledgement
  re-signals immediately.
- With Feature 08 active (`THR>0` and `TIME>0`), the standard per-vector
  threshold/timer policy controls notification. Feature 09 can disable
  coalescing for an individual vector.

Admin CQ interrupts are always immediate. New controllers default to Feature
08 `THR=0`, `TIME=0`, and allow guest changes. `lock_irq_coalescing=1` rejects
changes to Features 08 and 09; it does not affect CQE publication, MSI-X
masking, or the CQ-head suppression policy selected by the current Feature 08
state.

## Configuration scope

All module parameters are snapshotted when the mdev controller is created.
Changing `/sys/module/nvmet_mdev_pci/parameters/*` does not reconfigure an
existing mdev. Stop the VM, remove the mdev and recreate it to apply new module
values. The guest may change live NVMe Feature 08/09 state unless
`lock_irq_coalescing=1`; controller reset restores the snapshotted defaults.

| Parameter group | Parameters | Dependency |
| --- | --- | --- |
| Pin-cache capacity | `pin_cache_pages`, `pin_cache_max_segs` | Independent of doorbell and IRQ policy. Either zero disables persistent pin reuse. |
| Response concurrency | `response_workers` | Independent; admin SQ remains single-lane. |
| Scan fairness | `poll_budget` | Used by mmap polling and DBBUF polling. Minimum effective value is one. |
| Initial NVMe IRQ policy | `irq_coalesce_threshold`, `irq_coalesce_time`, `lock_irq_coalescing` | Independent of doorbell transport. |
| BAR transport | `mmap_doorbells` | Requires 4 KiB host pages. |
| BAR wakeup acceleration | `kvm_doorbell_tracking` | Requires effective sparse mmap and compiled x86 KVM external write tracking. |

Do not add a new switch for an implementation choice that should become the
single production path. Retain module parameters only for capacity, scheduling
policy, compatibility fallback, or a genuinely optional platform facility.
