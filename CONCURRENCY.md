# nvmet-mdev-pci concurrency invariants

This document describes the production queue and interrupt path. It is a
correctness contract for future changes, not an alternative implementation or
an A/B mode.

## Design goals

- Do not delay the first completion. There is no batching timer or minimum
  batch size.
- Consume and publish work already visible, up to a fixed fairness budget.
- Publish CQEs immediately even when an earlier interrupt is outstanding.
  Suppress only redundant eventfd notifications.
- Keep the ordinary aligned 32-bit BAR doorbell path free of `ctrl->lock`.
- Retain mutexes for lifecycle, queue mapping, controller registers and error
  paths where they are not an I/O scalability cost.

## Submission queue ownership

`runner_active` gives one SQ work item exclusive ownership of `sq->head`.
Doorbell writers and the DBBUF poller set `kick_pending` before attempting to
claim the runner. The runner clears the kick at entry, consumes the current
tail snapshot, and submits each command immediately. The work budget prevents
one continuously busy SQ from monopolizing a workqueue CPU; it is not a target
batch size.

Before sleeping, the runner releases `runner_active`, executes a full barrier,
and rechecks the kick, the current tail and the DBBUF event-index handshake.
Therefore either the runner observes a racing producer or that producer claims
the released runner. Requeueing the same running `work_struct` is intentional:
the kernel workqueue pending bit permits a work item to queue its next
execution while its current callback is still running.

## Completion queue ownership

Completed IODs enter `cq->completions`, a lockless multi-producer list. The
single CQ publisher detaches one list snapshot and reverses it once, producing
a FIFO stream without one atomic removal per CQE. Each CQE is published as it
is removed from that stream. If the work budget or guest CQ capacity stops the
run, `pending_completions` retains the remainder; only the publisher writes
this pointer.

`publisher_active` gives one work item exclusive ownership of the guest CQ
tail and phase. `kick_pending` closes the same release/recheck race as on SQs.
When the guest CQ is full, the publisher sets `blocked`. New completions remain
queued but cannot clear this state because they cannot create CQ space. Only a
new CQ-head observation clears `blocked` and activates the publisher. The
publisher publishes `blocked`, executes a full barrier and rechecks actual CQ
fullness, covering a head update that arrived just before the flag became
visible.

Concurrent vCPU CQ-head callbacks converge with `atomic_cmpxchg()` on
`cq->head`. Every callback rereads the current BAR or shadow doorbell, so an
older callback cannot overwrite a newer head value.

## CQE visibility

The NVMe phase/status word is the ownership marker observed by the guest. For
every CQE the publisher:

1. writes the CQE bytes before `status`;
2. executes `dma_wmb()`;
3. writes the final status/phase word with `WRITE_ONCE()`;
4. release-publishes the new host tail;
5. executes `dma_wmb()` before any interrupt notification.

The compile-time assertion in `queue.c` ensures `status` remains the final CQE
field. A guest can scan concurrently, but it cannot accept a partially written
entry as new.

## Adaptive interrupt suppression

For a normal I/O CQ, the first visible completion changes
`irq_outstanding` from zero to one and is notified immediately. Later CQEs are
still published, but another eventfd signal is suppressed while the bit is
one. A changed CQ-head doorbell is the acknowledgement: it clears the bit and
immediately re-signals if published CQEs remain.

There is no generic VFIO callback saying that the guest finished an MSI-X
handler, so CQ-head progress is deliberately used as the acknowledgement. The
state is per CQ, not per MSI-X vector, because one vector may serve several CQs
whose heads advance independently.

With Doorbell Buffer Config active, the target arms that CQ's event index
before raising the interrupt. It then performs the required barrier and shadow
head recheck. This makes the guest's next CQ-head update trap instead of making
the 10 ms safety poll part of normal latency. If the head raced event-index
publication, the notifier updates it and retries without recursion.

Admin completions remain immediate. Explicit NVMe interrupt coalescing also
retains its standard threshold/timer path and bypasses `irq_outstanding`; the
two notification policies are not stacked.

## MSI-X state

Normal signaling does not take `irq_state_lock`. Mask/configuration writes and
eventfd replacement are serialized by that lock and published through
`irq_state_seq`. A signaler reads a consistent MSI-X snapshot, dereferences the
eventfd under RCU, and retries if either class of state changed.

Masked or disconnected signals atomically set the MSI-X PBA bit. The retry
after that update prevents an unmask or eventfd replacement from scanning the
PBA just before the new bit becomes visible. Direct delivery clears an older
PBA bit because that eventfd signal covers all already published CQEs on the
vector. Replaced eventfds are released only after `synchronize_rcu()`.

## Lifetime and lock order

Queue creation initializes immutable mapping/depth fields, then release-stores
`live=true`. Hot readers acquire-load `live` before using those fields. Queue
deletion release-stores `live=false`, waits for RCU doorbell/poller readers,
cancels the owner work item, drains nvmet response work and only then unmaps
guest memory.

Both trapped doorbell callbacks and each DBBUF poll scan hold RCU while
dereferencing the fixed queue arrays. Final cleanup waits for RCU, cancels the
poller and then frees the arrays. A fast BAR doorbell write also holds RCU from
the aligned register store through scheduling, so reset cannot clear BAR0
between those operations.

The slow-path order remains `state_lock`, `dma_pin_lock`, `ctrl->lock`, then
`dma_lock`. `irq_state_lock` never nests back into those mutexes. RCU grace
periods and work cancellation are executed without `ctrl->lock` held.

## Verification and remaining gate

The focused build target is:

```sh
nix develop path:. --command \
  make -j"$(nproc)" W=1 drivers/nvme/target/nvmet-mdev-pci.o
```

Do not use only `drivers/nvme/target/` as the incremental target; it can return
without rebuilding the composite mdev object in this output-tree setup.

Compilation and source review cannot prove every weak-memory interleaving. The
runtime acceptance gate remains a rebuilt test kernel followed by queue
create/delete stress, controller reset, DMA unmap under I/O, and Windows/Linux
mixed-QD benchmarks while watching dmesg. A KCSAN/lockdep test kernel is the
preferred additional stress configuration. Sparse 0.6.4 in the current dev
shell cannot parse this kernel's `__typeof_unqual__` checker probe, so a newer
Sparse is required before counting sparse as a completed check.
The Nix-wrapped Clang path also currently stops in kernel prepare because it
treats `-nostdlibinc` as an unused argument under `-Werror`; no Clang result has
been claimed for this patch.
