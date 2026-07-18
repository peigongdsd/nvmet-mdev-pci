// SPDX-License-Identifier: GPL-2.0

#include <linux/iommu.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/stringify.h>
#include <linux/unaligned.h>

#include "../pci-common.h"
#include "priv.h"

#define NVMET_MDEV_COPY_MAX_DATA	SZ_1M
#define NVMET_MDEV_PRP_ENTRIES		(SZ_4K / sizeof(__le64))
#define NVMET_MDEV_MAX_SEGS		(NVMET_MDEV_COPY_MAX_DATA / SZ_4K + 1)
#define NVMET_MDEV_QUEUE_WORK_BUDGET	64

static_assert(offsetof(struct nvme_completion, status) +
	      sizeof_field(struct nvme_completion, status) ==
	      sizeof(struct nvme_completion));
#define NVMET_MDEV_MAX_RESPONSE_WORKERS	64
#define NVMET_MDEV_POLL_INTERVAL	msecs_to_jiffies(10)
#define NVMET_MDEV_DEFAULT_PIN_CACHE_PAGES	65536
#define NVMET_MDEV_DEFAULT_PIN_CACHE_MAX_SEGS	64
#define NVMET_MDEV_DEFAULT_POLL_BUDGET		128
#define NVMET_MDEV_DEFAULT_RESPONSE_WORKERS	0
#define NVMET_MDEV_DEFAULT_IRQ_COALESCE_THR	7
#define NVMET_MDEV_DEFAULT_IRQ_COALESCE_TIME	1

static uint pin_cache_pages = NVMET_MDEV_DEFAULT_PIN_CACHE_PAGES;
module_param_named(pin_cache_pages, pin_cache_pages, uint, 0644);
MODULE_PARM_DESC(pin_cache_pages,
		 "Maximum cached guest pages per controller (default: "
		 __stringify(NVMET_MDEV_DEFAULT_PIN_CACHE_PAGES) ")");

static uint pin_cache_max_segs = NVMET_MDEV_DEFAULT_PIN_CACHE_MAX_SEGS;
module_param_named(pin_cache_max_segs, pin_cache_max_segs, uint, 0644);
MODULE_PARM_DESC(pin_cache_max_segs,
		 "Maximum PRP segments admitted to the pin cache (default: "
		 __stringify(NVMET_MDEV_DEFAULT_PIN_CACHE_MAX_SEGS) ")");

static uint poll_budget = NVMET_MDEV_DEFAULT_POLL_BUDGET;
module_param_named(poll_budget, poll_budget, uint, 0644);
MODULE_PARM_DESC(poll_budget,
		 "Maximum queues examined by one bounded DBBUF poll run (default: "
		 __stringify(NVMET_MDEV_DEFAULT_POLL_BUDGET) ")");

static uint response_workers = NVMET_MDEV_DEFAULT_RESPONSE_WORKERS;
module_param_named(response_workers, response_workers, uint, 0644);
MODULE_PARM_DESC(response_workers,
		 "Response cleanup workers per I/O SQ (default: "
		 __stringify(NVMET_MDEV_DEFAULT_RESPONSE_WORKERS) ")");

static u8 irq_coalesce_threshold = NVMET_MDEV_DEFAULT_IRQ_COALESCE_THR;
module_param_named(irq_coalesce_threshold, irq_coalesce_threshold, byte, 0644);
MODULE_PARM_DESC(irq_coalesce_threshold,
		 "Default NVMe Feature 08 THR (default: "
		 __stringify(NVMET_MDEV_DEFAULT_IRQ_COALESCE_THR) ")");

static u8 irq_coalesce_time = NVMET_MDEV_DEFAULT_IRQ_COALESCE_TIME;
module_param_named(irq_coalesce_time, irq_coalesce_time, byte, 0644);
MODULE_PARM_DESC(irq_coalesce_time,
		 "Default NVMe Feature 08 TIME in 100 us units (default: "
		 __stringify(NVMET_MDEV_DEFAULT_IRQ_COALESCE_TIME) ")");

struct nvmet_mdev_iod {
	struct list_head entry;
	struct llist_node free_node;
	struct llist_node completion_node;
	struct nvmet_mdev_ctrl *ctrl;
	struct nvmet_mdev_sq *sq;
	struct nvmet_mdev_cq *cq;
	struct nvmet_req req;
	struct nvme_command cmd;
	struct nvme_completion cqe;
	refcount_t refs;
	struct nvmet_mdev_payload payload;
	struct nvmet_mdev_iova_segment *segments;
	struct nvmet_mdev_iova_segment *segment_storage;
	__le64 *prp_storage;
	struct nvmet_mdev_iova_segment inline_segments[NVMET_MDEV_INLINE_SEGS];
	__le64 inline_prps[NVMET_MDEV_INLINE_SEGS];
	unsigned int nr_segments;
	size_t data_len;
	bool host_to_ctrl;
	bool pinned;
};

static void nvmet_mdev_destroy_iod(struct nvmet_mdev_ctrl *ctrl,
				   struct nvmet_mdev_iod *iod)
{
	nvmet_mdev_payload_destroy(&iod->payload);
	kfree(iod->prp_storage);
	kfree(iod->segment_storage);
	mempool_free(iod, &ctrl->iod_pool);
}

static void nvmet_mdev_free_iod(struct nvmet_mdev_iod *iod)
{
	struct nvmet_mdev_ctrl *ctrl = iod->ctrl;

	if (iod->payload.active)
		nvmet_mdev_unpin_payload(ctrl, &iod->payload);
	if (iod->req.sg && !iod->pinned)
		nvmet_req_free_sgls(&iod->req);
	if (atomic_inc_return(&ctrl->iod_free_count) <= ctrl->iod_free_limit) {
		llist_add(&iod->free_node, &ctrl->iod_free);
		return;
	}
	atomic_dec(&ctrl->iod_free_count);
	nvmet_mdev_destroy_iod(ctrl, iod);
}

static void nvmet_mdev_put_iod(struct nvmet_mdev_iod *iod)
{
	if (refcount_dec_and_test(&iod->refs))
		nvmet_mdev_free_iod(iod);
}

static struct nvmet_mdev_iod *nvmet_mdev_alloc_iod(struct nvmet_mdev_ctrl *ctrl)
{
	struct llist_node *node = llist_del_first(&ctrl->iod_free);
	struct nvmet_mdev_payload_backing payload_backing = {};
	struct nvmet_mdev_iova_segment *segment_storage = NULL;
	struct nvmet_mdev_iod *iod;
	__le64 *prp_storage = NULL;

	if (node) {
		atomic_dec(&ctrl->iod_free_count);
		iod = llist_entry(node, struct nvmet_mdev_iod, free_node);
		payload_backing = iod->payload.backing;
		segment_storage = iod->segment_storage;
		prp_storage = iod->prp_storage;
		nvmet_mdev_stat_inc(ctrl, iod_cache_hits);
	} else {
		iod = mempool_alloc(&ctrl->iod_pool, GFP_KERNEL);
		if (!iod)
			return NULL;
		nvmet_mdev_stat_inc(ctrl, iod_cache_misses);
	}
	memset(iod, 0, sizeof(*iod));
	iod->segment_storage = segment_storage;
	iod->prp_storage = prp_storage;
	iod->payload.backing = payload_backing;
	return iod;
}

static int nvmet_mdev_collect_prps(struct nvmet_mdev_iod *iod)
{
	struct nvme_command *cmd = &iod->cmd;
	size_t remaining = iod->data_len;
	__le64 *prps = NULL;
	u64 prp, list_iova;
	unsigned int max_segments;
	int ret = 0;

	if (!remaining)
		return 0;
	if (remaining > NVMET_MDEV_COPY_MAX_DATA)
		return -E2BIG;

	prp = le64_to_cpu(cmd->common.dptr.prp1);
	if (!prp || !IS_ALIGNED(prp, sizeof(u32))) {
		ret = -EINVAL;
		goto out;
	}
	max_segments = DIV_ROUND_UP((prp & (SZ_4K - 1)) + remaining, SZ_4K);
	if (max_segments <= NVMET_MDEV_INLINE_SEGS) {
		iod->segments = iod->inline_segments;
	} else {
		if (!iod->segment_storage) {
			iod->segment_storage =
				kmalloc_array(NVMET_MDEV_MAX_SEGS,
					      sizeof(*iod->segments), GFP_KERNEL);
			if (!iod->segment_storage)
				return -ENOMEM;
			nvmet_mdev_stat_inc(iod->ctrl, prp_heap_allocs);
		}
		iod->segments = iod->segment_storage;
	}

	{
		size_t length = min_t(size_t, remaining,
					  SZ_4K - (prp & (SZ_4K - 1)));

		iod->segments[iod->nr_segments++] =
			(struct nvmet_mdev_iova_segment) { prp, length };
		remaining -= length;
	}
	if (!remaining)
		goto out;

	prp = le64_to_cpu(cmd->common.dptr.prp2);
	if (!nvmet_pci_prp2_valid(prp, remaining)) {
		ret = -EINVAL;
		goto out;
	}
	if (remaining <= SZ_4K) {
		iod->segments[iod->nr_segments++] =
			(struct nvmet_mdev_iova_segment) { prp, remaining };
		goto out;
	}

	if (DIV_ROUND_UP(remaining, SZ_4K) <= NVMET_MDEV_INLINE_SEGS) {
		prps = iod->inline_prps;
	} else {
		if (!iod->prp_storage) {
			iod->prp_storage = kmalloc(SZ_4K, GFP_KERNEL);
			if (!iod->prp_storage) {
				ret = -ENOMEM;
				goto out;
			}
			nvmet_mdev_stat_inc(iod->ctrl, prp_heap_allocs);
		}
		prps = iod->prp_storage;
	}
	list_iova = prp;

	while (remaining) {
		bool chained = false;
		unsigned int i;
		size_t list_bytes;

		list_bytes = nvmet_pci_prp_list_bytes(remaining, NVMET_MDEV_PRP_ENTRIES);
		ret = vfio_dma_rw(&iod->ctrl->vdev, list_iova, prps, list_bytes,
				  false);
		if (ret)
			goto out;

		for (i = 0; i < NVMET_MDEV_PRP_ENTRIES && remaining; i++) {
			size_t length;

			prp = le64_to_cpu(prps[i]);
			if (!prp || !IS_ALIGNED(prp, SZ_4K)) {
				ret = -EINVAL;
				goto out;
			}

			if (i == NVMET_MDEV_PRP_ENTRIES - 1 &&
			    remaining > SZ_4K) {
				list_iova = prp;
				chained = true;
				break;
			}

			length = min_t(size_t, remaining, SZ_4K);
			if (iod->nr_segments >= max_segments) {
				ret = -E2BIG;
				goto out;
			}
			iod->segments[iod->nr_segments++] =
				(struct nvmet_mdev_iova_segment) { prp, length };
			remaining -= length;
		}

		if (remaining && !chained) {
			ret = -EINVAL;
			goto out;
		}
	}

out:
	return ret;
}

static int nvmet_mdev_copy_payload(struct nvmet_mdev_iod *iod,
				   bool write_guest)
{
	struct nvmet_req *req = &iod->req;
	size_t offset = 0;
	void *bounce;
	unsigned int i;
	int ret = 0;

	bounce = kmalloc(SZ_4K, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	for (i = 0; i < iod->nr_segments; i++) {
		const struct nvmet_mdev_iova_segment *segment = &iod->segments[i];
		size_t copied;

		if (write_guest) {
			copied = sg_pcopy_to_buffer(req->sg, req->sg_cnt, bounce,
						    segment->length, offset);
			if (copied != segment->length) {
				ret = -EFAULT;
				break;
			}
			ret = vfio_dma_rw(&iod->ctrl->vdev, segment->iova,
					  bounce, segment->length, true);
		} else {
			ret = vfio_dma_rw(&iod->ctrl->vdev, segment->iova,
					  bounce, segment->length, false);
			if (!ret) {
				copied = sg_pcopy_from_buffer(req->sg, req->sg_cnt,
							      bounce,
							      segment->length,
							      offset);
				if (copied != segment->length)
					ret = -EFAULT;
			}
		}
		if (ret)
			break;
		offset += segment->length;
	}

	kfree(bounce);
	return ret;
}

static u32 nvmet_mdev_sq_tail(struct nvmet_mdev_ctrl *ctrl,
			      const struct nvmet_mdev_sq *sq)
{
	__le32 *dbs = smp_load_acquire(&ctrl->dbbuf_dbs);

	if (sq->qid && dbs)
		return le32_to_cpu(READ_ONCE(dbs[sq->qid * 2]));
	return le32_to_cpu(READ_ONCE(*(__le32 *)(ctrl->bar0 + NVME_REG_DBS +
					       sq->qid * 2 * sizeof(u32))));
}

static u32 nvmet_mdev_cq_head(struct nvmet_mdev_ctrl *ctrl,
			      const struct nvmet_mdev_cq *cq)
{
	__le32 *dbs = smp_load_acquire(&ctrl->dbbuf_dbs);

	if (cq->qid && dbs)
		return le32_to_cpu(READ_ONCE(dbs[cq->qid * 2 + 1]));
	return le32_to_cpu(READ_ONCE(*(__le32 *)(ctrl->bar0 + NVME_REG_DBS +
					       (cq->qid * 2 + 1) *
					       sizeof(u32))));
}

static void nvmet_mdev_activate_cq(struct nvmet_mdev_cq *cq);
static bool nvmet_mdev_handle_cq_head(struct nvmet_mdev_cq *cq,
				      bool resignal);
static bool nvmet_mdev_arm_cq_event(struct nvmet_mdev_ctrl *ctrl,
				    struct nvmet_mdev_cq *cq);

static bool nvmet_mdev_cq_has_completions(struct nvmet_mdev_cq *cq)
{
	return READ_ONCE(cq->pending_completions) ||
	       !llist_empty(&cq->completions);
}

static void nvmet_mdev_notify_cq(struct nvmet_mdev_cq *cq,
				 unsigned int completions, bool force)
{
	struct nvmet_mdev_ctrl *ctrl = cq->ctrl;
	struct nvmet_mdev_irq_vector *irq;

	if (!READ_ONCE(cq->irq_enabled) || !READ_ONCE(ctrl->enabled) ||
	    !smp_load_acquire(&cq->live))
		return;
	irq = &ctrl->irq_vectors[READ_ONCE(cq->vector)];
	if (!cq->qid) {
		nvmet_mdev_notify_irq(ctrl, cq->vector, completions, true);
		return;
	}
	/* Preserve explicit NVMe coalescing; it is its own batching policy. */
	if (!force && READ_ONCE(ctrl->irq_coalesce_time) &&
	    READ_ONCE(ctrl->irq_coalesce_threshold) &&
	    !READ_ONCE(irq->coalescing_disabled)) {
		nvmet_mdev_notify_irq(ctrl, cq->vector, completions, false);
		return;
	}
	for (;;) {
		if (atomic_cmpxchg(&cq->irq_outstanding, 0, 1)) {
			nvmet_mdev_stat_inc(ctrl, interrupt_suppressed);
			return;
		}
		/* Arm a trapped acknowledgement before raising the interrupt. */
		if (!smp_load_acquire(&ctrl->dbbuf_dbs) ||
		    !nvmet_mdev_arm_cq_event(ctrl, cq))
			break;
		nvmet_mdev_handle_cq_head(cq, false);
		if (atomic_read(&cq->head) == smp_load_acquire(&cq->tail))
			return;
	}
	nvmet_mdev_notify_irq(ctrl, cq->vector, completions, force);
}

static bool nvmet_mdev_handle_cq_head(struct nvmet_mdev_cq *cq,
				      bool resignal)
{
	struct nvmet_mdev_ctrl *ctrl = cq->ctrl;
	bool changed = false;
	bool wake = false;
	u32 head;
	int old_head;

	if (!smp_load_acquire(&cq->live))
		return false;

	/* Converge concurrent vCPU callbacks on the newest doorbell value. */
	for (;;) {
		head = nvmet_mdev_cq_head(ctrl, cq);
		if (head >= READ_ONCE(cq->depth))
			break;
		old_head = atomic_read(&cq->head);
		if (head == old_head)
			break;
		if (atomic_cmpxchg(&cq->head, old_head, head) != old_head)
			continue;
		changed = true;
	}

	if (cq->qid && changed) {
		/* atomic_xchg() publishes the ack before checking missed CQEs. */
		atomic_xchg(&cq->irq_outstanding, 0);
		if (resignal &&
		    atomic_read(&cq->head) != smp_load_acquire(&cq->tail)) {
			nvmet_mdev_stat_inc(ctrl, interrupt_resignals);
			nvmet_mdev_notify_cq(cq, 1, true);
		}
	}
	if (head >= READ_ONCE(cq->depth)) {
		nvmet_mdev_activate_cq(cq);
		wake = true;
	} else if (changed) {
		bool was_blocked = atomic_xchg(&cq->blocked, 0);

		if (was_blocked || nvmet_mdev_cq_has_completions(cq)) {
			nvmet_mdev_activate_cq(cq);
			wake = true;
		}
	}
	if (wake)
		nvmet_mdev_stat_inc(ctrl, cq_head_wakeups);
	return wake;
}

static void nvmet_mdev_activate_sq(struct nvmet_mdev_sq *sq)
{
	atomic_set(&sq->kick_pending, 1);
	if (atomic_cmpxchg(&sq->runner_active, 0, 1) == 0)
		schedule_work(&sq->work);
}

static void nvmet_mdev_budget_poll(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int qid, limit, scanned = 0;
	bool enabled;

	nvmet_mdev_stat_inc(ctrl, poll_wakeups);
	nvmet_mdev_stat_inc(ctrl, poll_runs);
	enabled = READ_ONCE(ctrl->enabled) &&
		  smp_load_acquire(&ctrl->dbbuf_dbs) &&
		  READ_ONCE(ctrl->dbbuf_eis);
	if (!enabled)
		goto unlock;

	rcu_read_lock();
	if (ctrl->nr_queues <= 1)
		goto account;
	qid = ctrl->poll_next_qid;
	limit = min_t(unsigned int, ctrl->runtime.poll_budget,
		      ctrl->nr_queues - 1);
	while (scanned < limit) {
		struct nvmet_mdev_sq *sq = &ctrl->sqs[qid];
		struct nvmet_mdev_cq *cq = &ctrl->cqs[qid];

		if (READ_ONCE(sq->live) && nvmet_mdev_sq_tail(ctrl, sq) !=
		    READ_ONCE(sq->head))
			nvmet_mdev_activate_sq(sq);
		if (READ_ONCE(cq->live)) {
			u32 head = nvmet_mdev_cq_head(ctrl, cq);

			if (head != atomic_read(&cq->head))
				nvmet_mdev_handle_cq_head(cq, true);
		}
		scanned++;
		qid++;
		if (qid == ctrl->nr_queues)
			qid = 1;
	}
	ctrl->poll_next_qid = qid;
account:
	nvmet_mdev_stat_add(ctrl, poll_queue_checks, scanned);
	rcu_read_unlock();
unlock:
	if (enabled) {
		nvmet_mdev_stat_inc(ctrl, poll_sleeps);
		schedule_delayed_work(&ctrl->poll_work,
				      NVMET_MDEV_POLL_INTERVAL);
	}
}

static void nvmet_mdev_poll_work(struct work_struct *work)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(to_delayed_work(work), struct nvmet_mdev_ctrl,
			     poll_work);

	nvmet_mdev_budget_poll(ctrl);
}

static void nvmet_mdev_kick_poller(struct nvmet_mdev_ctrl *ctrl)
{
	bool enabled;

	enabled = READ_ONCE(ctrl->enabled) &&
		  smp_load_acquire(&ctrl->dbbuf_dbs);
	if (!enabled)
		return;
	mod_delayed_work(system_wq, &ctrl->poll_work, 0);
}

static void nvmet_mdev_activate_cq(struct nvmet_mdev_cq *cq)
{
	if (atomic_read(&cq->blocked)) {
		/* A completion cannot unblock a full CQ; retain it for the ack. */
		atomic_set(&cq->kick_pending, 1);
		return;
	}
	atomic_set(&cq->kick_pending, 1);
	if (atomic_cmpxchg(&cq->publisher_active, 0, 1) == 0)
		schedule_work(&cq->work);
}

static bool nvmet_mdev_arm_sq_event(struct nvmet_mdev_ctrl *ctrl,
				    struct nvmet_mdev_sq *sq)
{
	__le32 *dbs = smp_load_acquire(&ctrl->dbbuf_dbs);
	__le32 *eis = READ_ONCE(ctrl->dbbuf_eis);
	u32 tail;

	if (!READ_ONCE(ctrl->enabled) || !READ_ONCE(sq->live) || !sq->qid ||
	    !dbs || !eis)
		return false;
	tail = le32_to_cpu(READ_ONCE(dbs[sq->qid * 2]));
	WRITE_ONCE(eis[sq->qid * 2], cpu_to_le32(tail));
	/* Pair with the guest's shadow-doorbell write and event-index read. */
	mb();
	return nvmet_mdev_sq_tail(ctrl, sq) != tail;
}

static bool nvmet_mdev_arm_cq_event(struct nvmet_mdev_ctrl *ctrl,
				    struct nvmet_mdev_cq *cq)
{
	__le32 *dbs = smp_load_acquire(&ctrl->dbbuf_dbs);
	__le32 *eis = READ_ONCE(ctrl->dbbuf_eis);
	u32 head;

	if (!READ_ONCE(ctrl->enabled) || !READ_ONCE(cq->live) || !cq->qid ||
	    !dbs || !eis)
		return false;
	head = le32_to_cpu(READ_ONCE(dbs[cq->qid * 2 + 1]));
	if (head != atomic_read(&cq->head))
		return true;
	WRITE_ONCE(eis[cq->qid * 2 + 1], cpu_to_le32(head));
	/* Pair with the guest's shadow-doorbell write and event-index read. */
	mb();
	return nvmet_mdev_cq_head(ctrl, cq) != head;
}

static void nvmet_mdev_complete_iod(struct nvmet_mdev_iod *iod)
{
	struct nvmet_mdev_cq *cq = iod->cq;
	struct nvmet_mdev_ctrl *ctrl = iod->ctrl;

	if (!READ_ONCE(ctrl->enabled) || !smp_load_acquire(&cq->live)) {
		nvmet_mdev_put_iod(iod);
		return;
	}
	llist_add(&iod->completion_node, &cq->completions);
	nvmet_mdev_activate_cq(cq);
}

static void nvmet_mdev_response_iod(struct nvmet_mdev_iod *iod)
{
	struct nvmet_req *req = &iod->req;
	u16 status = le16_to_cpu(req->cqe->status) >> 1;
	int ret;

	if (!status && iod->data_len && !iod->host_to_ctrl && !iod->pinned) {
		ret = nvmet_mdev_copy_payload(iod, true);
		if (ret) {
			status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
			req->cqe->status = cpu_to_le16(status << 1);
		}
	}

	if (iod->pinned) {
		req->sg = NULL;
		req->sg_cnt = 0;
		nvmet_mdev_unpin_payload(iod->ctrl, &iod->payload);
	} else if (req->sg) {
		nvmet_req_free_sgls(req);
	}
	nvmet_mdev_complete_iod(iod);
}

static void nvmet_mdev_response_work(struct work_struct *work)
{
	struct nvmet_mdev_response_lane *lane =
		container_of(work, struct nvmet_mdev_response_lane, work);
	struct nvmet_mdev_sq *sq = lane->sq;
	struct nvmet_mdev_ctrl *ctrl = sq->ctrl;
	unsigned long flags;

	nvmet_mdev_stat_inc(ctrl, response_work_runs);
	for (;;) {
		struct nvmet_mdev_iod *iod, *tmp;
		unsigned int nr = 0;
		LIST_HEAD(responses);

		spin_lock_irqsave(&lane->lock, flags);
		if (list_empty(&lane->responses)) {
			atomic_set(&lane->work_queued, 0);
			spin_unlock_irqrestore(&lane->lock, flags);
			return;
		}
		list_splice_init(&lane->responses, &responses);
		spin_unlock_irqrestore(&lane->lock, flags);

		list_for_each_entry_safe(iod, tmp, &responses, entry) {
			list_del_init(&iod->entry);
			nvmet_mdev_response_iod(iod);
			nr++;
		}
		nvmet_mdev_stat_inc(ctrl, response_batches);
		nvmet_mdev_stat_add(ctrl, response_items, nr);
	}
}

static void nvmet_mdev_submit_iod(struct nvmet_mdev_iod *iod)
{
	struct nvmet_req *req = &iod->req;
	u16 status;
	int ret;

	if (!READ_ONCE(iod->ctrl->enabled) || !READ_ONCE(iod->sq->live)) {
		nvmet_mdev_put_iod(iod);
		nvmet_mdev_put_iod(iod);
		return;
	}

	if (!nvmet_req_init(req, &iod->sq->nvme_sq,
			    &nvmet_mdev_fabrics_ops)) {
		nvmet_mdev_put_iod(iod);
		return;
	}

	iod->data_len = nvmet_req_transfer_len(req);
	iod->host_to_ctrl = nvme_is_write(&iod->cmd);
	if (iod->cmd.common.flags & NVME_CMD_SGL_ALL) {
		status = NVME_SC_SGL_INVALID_TYPE | NVME_STATUS_DNR;
		goto complete;
	}
	if (iod->data_len > NVMET_MDEV_COPY_MAX_DATA) {
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto complete;
	}

	if (iod->data_len) {
		req->transfer_len = iod->data_len;
		ret = nvmet_mdev_collect_prps(iod);
		if (ret) {
			status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
			goto complete;
		}

		if (iod->sq->qid) {
			ret = nvmet_mdev_pin_payload(iod->ctrl, iod->segments,
						     iod->nr_segments,
						     iod->host_to_ctrl ?
						     IOMMU_READ : IOMMU_WRITE,
						     &iod->payload);
			if (ret) {
				status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
				goto complete;
			}
			req->sg = iod->payload.sgt.sgl;
			req->sg_cnt = iod->nr_segments;
			iod->pinned = true;
		} else {
			ret = nvmet_req_alloc_sgls(req);
			if (ret) {
				status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
				goto complete;
			}
			if (iod->host_to_ctrl) {
				ret = nvmet_mdev_copy_payload(iod, false);
				if (ret) {
					status = NVME_SC_DATA_XFER_ERROR |
						 NVME_STATUS_DNR;
					goto complete;
				}
			}
		}
	}

	req->execute(req);
	nvmet_mdev_put_iod(iod);
	return;

complete:
	nvmet_req_complete(req, status);
	nvmet_mdev_put_iod(iod);
}

static void nvmet_mdev_sq_work(struct work_struct *work)
{
	struct nvmet_mdev_sq *sq =
		container_of(work, struct nvmet_mdev_sq, work);
	struct nvmet_mdev_ctrl *ctrl = sq->ctrl;
	unsigned int processed = 0;
	bool more = false;
	bool failed = false;
	u32 tail;

	nvmet_mdev_stat_inc(ctrl, sq_work_runs);
	atomic_set(&sq->kick_pending, 0);
	if (!READ_ONCE(ctrl->enabled) || !smp_load_acquire(&sq->live))
		goto unlock;

	tail = nvmet_mdev_sq_tail(ctrl, sq);
	if (tail >= sq->depth) {
		mutex_lock(&ctrl->lock);
		WRITE_ONCE(ctrl->enabled, false);
		nvmet_mdev_pci_set_fatal(ctrl);
		mutex_unlock(&ctrl->lock);
		goto unlock;
	}

	while (sq->head != tail &&
	       processed < NVMET_MDEV_QUEUE_WORK_BUDGET) {
		struct nvmet_mdev_iod *iod;

		iod = nvmet_mdev_alloc_iod(ctrl);
		if (!iod) {
			mutex_lock(&ctrl->lock);
			WRITE_ONCE(ctrl->enabled, false);
			nvmet_mdev_pci_set_fatal(ctrl);
			mutex_unlock(&ctrl->lock);
			failed = true;
			break;
		}
		iod->ctrl = ctrl;
		iod->sq = sq;
		iod->cq = sq->cq;
		iod->req.cmd = &iod->cmd;
		iod->req.cqe = &iod->cqe;
		iod->req.port = ctrl->mport->port;
		refcount_set(&iod->refs, 2);
		INIT_LIST_HEAD(&iod->entry);
		dma_rmb();
		memcpy(&iod->cmd,
		       sq->entries + sq->head * sizeof(struct nvme_command),
		       sizeof(iod->cmd));
		nvmet_pci_advance_sq_head(&sq->head, sq->depth);
		processed++;
		nvmet_mdev_submit_iod(iod);
	}
	more = sq->head != tail;
	nvmet_mdev_stat_add(ctrl, commands, processed);

unlock:
	if (!failed && READ_ONCE(ctrl->enabled) &&
	    smp_load_acquire(&sq->live) &&
	    (more || atomic_xchg(&sq->kick_pending, 0) ||
	     nvmet_mdev_arm_sq_event(ctrl, sq))) {
		nvmet_mdev_stat_inc(ctrl, sq_runner_requeues);
		schedule_work(&sq->work);
		return;
	}

	atomic_set_release(&sq->runner_active, 0);
	/* Pair runner release with the final kick and tail recheck. */
	smp_mb();
	if (!failed && READ_ONCE(ctrl->enabled) &&
	    smp_load_acquire(&sq->live) &&
	    (atomic_xchg(&sq->kick_pending, 0) ||
	     nvmet_mdev_sq_tail(ctrl, sq) != READ_ONCE(sq->head) ||
	     nvmet_mdev_arm_sq_event(ctrl, sq)) &&
	    atomic_cmpxchg(&sq->runner_active, 0, 1) == 0) {
		nvmet_mdev_stat_inc(ctrl, sq_runner_requeues);
		schedule_work(&sq->work);
	}
}

static void nvmet_mdev_cq_work(struct work_struct *work)
{
	struct nvmet_mdev_cq *cq =
		container_of(work, struct nvmet_mdev_cq, work);
	struct nvmet_mdev_ctrl *ctrl = cq->ctrl;
	unsigned int completed = 0;
	u32 head;
	bool blocked = false;

	nvmet_mdev_stat_inc(ctrl, cq_work_runs);
	atomic_set(&cq->kick_pending, 0);
	if (!READ_ONCE(ctrl->enabled) || !smp_load_acquire(&cq->live))
		goto out_release;

	head = nvmet_mdev_cq_head(ctrl, cq);
	if (head >= READ_ONCE(cq->depth)) {
		mutex_lock(&ctrl->lock);
		WRITE_ONCE(ctrl->enabled, false);
		nvmet_mdev_pci_set_fatal(ctrl);
		mutex_unlock(&ctrl->lock);
		goto out_release;
	}
	if (head != atomic_read(&cq->head))
		nvmet_mdev_handle_cq_head(cq, true);

	while (completed < NVMET_MDEV_QUEUE_WORK_BUDGET &&
	       !nvmet_pci_cq_full(atomic_read(&cq->head),
				     READ_ONCE(cq->tail), cq->depth)) {
		struct llist_node *node;
		struct nvmet_mdev_iod *iod;
		struct nvme_completion *dst;
		struct nvme_completion cqe;
		u16 tail = READ_ONCE(cq->tail);
		u16 phase = READ_ONCE(cq->phase);
		u16 status;

		if (!READ_ONCE(cq->pending_completions)) {
			node = llist_del_all(&cq->completions);
			if (!node)
				break;
			WRITE_ONCE(cq->pending_completions,
				   llist_reverse_order(node));
		}
		node = READ_ONCE(cq->pending_completions);
		WRITE_ONCE(cq->pending_completions, node->next);
		iod = llist_entry(node, struct nvmet_mdev_iod, completion_node);
		cqe = iod->cqe;
		status = le16_to_cpu(cqe.status) >> 1;
		nvmet_pci_prepare_cqe(&cqe, le16_to_cpu(cqe.sq_head),
				      le16_to_cpu(cqe.sq_id), cqe.command_id,
				      status, phase);
		dst = (struct nvme_completion *)(cq->entries +
						tail * sizeof(cqe));
		memcpy(dst, &cqe,
		       offsetof(struct nvme_completion, status));
		/* The phase bit publishes this CQE to a scanning guest. */
		dma_wmb();
		WRITE_ONCE(dst->status, cqe.status);
		nvmet_pci_advance_cq_tail(&tail, &phase, cq->depth);
		WRITE_ONCE(cq->phase, phase);
		/* Pair with acquire-loads in acknowledgment and notification. */
		smp_store_release(&cq->tail, tail);
		completed++;
		nvmet_mdev_put_iod(iod);
	}
	if (completed) {
		/* Make every published phase bit visible before notification. */
		dma_wmb();
		nvmet_mdev_stat_add(ctrl, completions, completed);
	}
	if (atomic_read(&cq->head) != smp_load_acquire(&cq->tail))
		nvmet_mdev_notify_cq(cq, completed ? completed : 1, false);

	blocked = nvmet_mdev_cq_has_completions(cq) &&
		nvmet_pci_cq_full(atomic_read(&cq->head), READ_ONCE(cq->tail),
				      READ_ONCE(cq->depth));
	if (blocked) {
		atomic_set(&cq->blocked, 1);
		/* Catch a CQ-head advance that raced publication of blocked. */
		smp_mb();
		if (!nvmet_pci_cq_full(atomic_read(&cq->head),
					READ_ONCE(cq->tail),
					READ_ONCE(cq->depth))) {
			atomic_set(&cq->blocked, 0);
			blocked = false;
		}
	}
	if (!blocked && READ_ONCE(ctrl->enabled) &&
	    smp_load_acquire(&cq->live) &&
	    nvmet_mdev_cq_has_completions(cq)) {
		nvmet_mdev_stat_inc(ctrl, cq_publisher_requeues);
		schedule_work(&cq->work);
		return;
	}

out_release:
	atomic_set_release(&cq->publisher_active, 0);
	/* Pair publisher release with the final kick and queue recheck. */
	smp_mb();
	if (!atomic_read(&cq->blocked) && READ_ONCE(ctrl->enabled) &&
	    smp_load_acquire(&cq->live) &&
	    (atomic_xchg(&cq->kick_pending, 0) ||
	     (nvmet_mdev_cq_has_completions(cq) &&
	      !nvmet_pci_cq_full(atomic_read(&cq->head),
				    READ_ONCE(cq->tail),
				    READ_ONCE(cq->depth)))) &&
	    atomic_cmpxchg(&cq->publisher_active, 0, 1) == 0) {
		nvmet_mdev_stat_inc(ctrl, cq_publisher_requeues);
		schedule_work(&cq->work);
		return;
	}
	if (atomic_read(&cq->blocked)) {
		bool advanced;

		/*
		 * A head write before blocked was published is caught here. A
		 * later write clears blocked atomically and queues the publisher.
		 */
		if (smp_load_acquire(&ctrl->dbbuf_dbs)) {
			advanced = nvmet_mdev_arm_cq_event(ctrl, cq);
		} else {
			/* Publish blocked before checking for a racing head update. */
			smp_mb();
			advanced = nvmet_mdev_cq_head(ctrl, cq) !=
				   atomic_read(&cq->head);
		}
		if (advanced)
			nvmet_mdev_handle_cq_head(cq, true);
	}
}

void nvmet_mdev_queue_response(struct nvmet_req *req)
{
	struct nvmet_mdev_iod *iod =
		container_of(req, struct nvmet_mdev_iod, req);
	struct nvmet_mdev_sq *sq = iod->sq;
	struct nvmet_mdev_response_lane *lane;
	unsigned long flags;
	unsigned int lane_id;
	bool queue;

	if (iod->pinned && iod->payload.cached) {
		nvmet_mdev_response_iod(iod);
		return;
	}
	lane_id = le16_to_cpu(iod->cmd.common.command_id) %
		   sq->nr_response_lanes;
	lane = &sq->response_lanes[lane_id];
	spin_lock_irqsave(&lane->lock, flags);
	list_add_tail(&iod->entry, &lane->responses);
	queue = atomic_cmpxchg(&lane->work_queued, 0, 1) == 0;
	spin_unlock_irqrestore(&lane->lock, flags);
	if (queue)
		WARN_ON_ONCE(!queue_work(sq->iod_wq, &lane->work));
}

static void nvmet_mdev_drain_completions(struct nvmet_mdev_cq *cq)
{
	struct llist_node *node, *next;

	node = READ_ONCE(cq->pending_completions);
	WRITE_ONCE(cq->pending_completions, NULL);
	llist_for_each_safe(node, next, node) {
		struct nvmet_mdev_iod *iod =
			llist_entry(node, struct nvmet_mdev_iod, completion_node);

		nvmet_mdev_put_iod(iod);
	}
	node = llist_del_all(&cq->completions);
	llist_for_each_safe(node, next, node) {
		struct nvmet_mdev_iod *iod =
			llist_entry(node, struct nvmet_mdev_iod, completion_node);

		nvmet_mdev_put_iod(iod);
	}
}

static void nvmet_mdev_reset_irq_features(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int vector;

	WRITE_ONCE(ctrl->irq_coalesce_threshold,
		   ctrl->runtime.irq_coalesce_threshold);
	WRITE_ONCE(ctrl->irq_coalesce_time, ctrl->runtime.irq_coalesce_time);
	for (vector = 0; vector < NVMET_MDEV_PCI_MSIX_VECTORS; vector++)
		WRITE_ONCE(ctrl->irq_vectors[vector].coalescing_disabled, false);
}

int nvmet_mdev_queue_init(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int qid;

	ctrl->runtime.pin_cache_pages = READ_ONCE(pin_cache_pages);
	ctrl->runtime.pin_cache_max_segs = READ_ONCE(pin_cache_max_segs);
	ctrl->runtime.poll_budget = max_t(unsigned int, READ_ONCE(poll_budget), 1);
	ctrl->runtime.response_workers = READ_ONCE(response_workers);
	ctrl->runtime.irq_coalesce_threshold =
		READ_ONCE(irq_coalesce_threshold);
	ctrl->runtime.irq_coalesce_time = READ_ONCE(irq_coalesce_time);
	nvmet_mdev_reset_irq_features(ctrl);
	mutex_init(&ctrl->state_lock);
	INIT_DELAYED_WORK(&ctrl->poll_work, nvmet_mdev_poll_work);
	ctrl->poll_next_qid = 1;
	ctrl->nr_queues = ctrl->tctrl->subsys->max_qid + 1;
	init_llist_head(&ctrl->iod_free);
	atomic_set(&ctrl->iod_free_count, 0);
	ctrl->iod_free_limit = min_t(unsigned int, ctrl->nr_queues * 32, 1024);
	if (mempool_init_kmalloc_pool(&ctrl->iod_pool,
				      ctrl->iod_free_limit,
				      sizeof(struct nvmet_mdev_iod)))
		return -ENOMEM;
	ctrl->iod_pool_ready = true;
	ctrl->sqs = kcalloc(ctrl->nr_queues, sizeof(*ctrl->sqs), GFP_KERNEL);
	if (!ctrl->sqs) {
		mempool_exit(&ctrl->iod_pool);
		ctrl->iod_pool_ready = false;
		return -ENOMEM;
	}
	ctrl->cqs = kcalloc(ctrl->nr_queues, sizeof(*ctrl->cqs), GFP_KERNEL);
	if (!ctrl->cqs) {
		kfree(ctrl->sqs);
		ctrl->sqs = NULL;
		mempool_exit(&ctrl->iod_pool);
		ctrl->iod_pool_ready = false;
		return -ENOMEM;
	}

	for (qid = 0; qid < ctrl->nr_queues; qid++) {
		struct nvmet_mdev_sq *sq = &ctrl->sqs[qid];
		struct nvmet_mdev_cq *cq = &ctrl->cqs[qid];

		sq->ctrl = ctrl;
		sq->qid = qid;
		INIT_WORK(&sq->work, nvmet_mdev_sq_work);
		atomic_set(&sq->runner_active, 0);
		atomic_set(&sq->kick_pending, 0);
		cq->ctrl = ctrl;
		cq->qid = qid;
		init_llist_head(&cq->completions);
		cq->pending_completions = NULL;
		atomic_set(&cq->publisher_active, 0);
		atomic_set(&cq->kick_pending, 0);
		atomic_set(&cq->irq_outstanding, 0);
		atomic_set(&cq->blocked, 0);
		INIT_WORK(&cq->work, nvmet_mdev_cq_work);
	}

	return 0;
}

static unsigned int
nvmet_mdev_response_worker_count(struct nvmet_mdev_ctrl *ctrl, u16 qid,
				 unsigned int depth)
{
	unsigned int requested = ctrl->runtime.response_workers;
	unsigned int maximum = min_t(unsigned int, depth,
				     NVMET_MDEV_MAX_RESPONSE_WORKERS);

	if (!qid)
		return 1;
	if (!requested)
		requested = num_online_cpus();
	return clamp_t(unsigned int, requested, 1, maximum);
}

static u16 nvmet_mdev_create_cq_locked(struct nvmet_mdev_ctrl *ctrl, u16 qid,
				       u16 flags, u16 depth, u64 prp1,
				       u16 vector)
{
	struct nvmet_mdev_cq *cq;
	size_t size;
	u16 status;
	int ret;

	lockdep_assert_held(&ctrl->state_lock);
	if (qid >= ctrl->nr_queues)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
	if (!(flags & NVME_QUEUE_PHYS_CONTIG) || !IS_ALIGNED(prp1, SZ_4K))
		return NVME_SC_INVALID_QUEUE | NVME_STATUS_DNR;
	if ((flags & NVME_CQ_IRQ_ENABLED) &&
	    vector >= NVMET_MDEV_PCI_MSIX_VECTORS)
		return NVME_SC_INVALID_VECTOR | NVME_STATUS_DNR;
	if (check_mul_overflow((size_t)depth, sizeof(struct nvme_completion),
			       &size))
		return NVME_SC_QUEUE_SIZE | NVME_STATUS_DNR;

	cq = &ctrl->cqs[qid];
	mutex_lock(&ctrl->lock);
	if (cq->live) {
		status = NVME_SC_QID_INVALID | NVME_STATUS_DNR;
		goto out_unlock;
	}

	ret = nvmet_mdev_map_guest(ctrl, prp1, size, IOMMU_WRITE,
				   &cq->mapping);
	if (ret) {
		status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
		goto out_unlock;
	}
	cq->entries = nvmet_mdev_mapping_addr(cq->mapping);
	cq->depth = depth;
	atomic_set(&cq->head, 0);
	cq->tail = 0;
	cq->phase = 1;
	cq->vector = vector;
	cq->irq_enabled = flags & NVME_CQ_IRQ_ENABLED;
	put_unaligned_le32(0, ctrl->bar0 + NVME_REG_DBS +
			   ((qid * 2 + 1) * sizeof(u32)));

	status = nvmet_cq_create(ctrl->tctrl, &cq->nvme_cq, qid, depth);
	if (status != NVME_SC_SUCCESS) {
		nvmet_mdev_unmap_guest(ctrl, cq->mapping);
		cq->mapping = NULL;
		cq->entries = NULL;
		cq->depth = 0;
		goto out_unlock;
	}
	init_llist_head(&cq->completions);
	cq->pending_completions = NULL;
	atomic_set(&cq->blocked, 0);
	atomic_set(&cq->publisher_active, 0);
	atomic_set(&cq->kick_pending, 0);
	atomic_set(&cq->irq_outstanding, 0);
	smp_store_release(&cq->live, true);

out_unlock:
	mutex_unlock(&ctrl->lock);
	return status;
}

static u16 nvmet_mdev_create_sq_locked(struct nvmet_mdev_ctrl *ctrl, u16 qid,
				       u16 cqid, u16 flags, u16 depth,
				       u64 prp1)
{
	struct nvmet_mdev_sq *sq;
	struct nvmet_mdev_cq *cq;
	struct nvmet_mdev_response_lane *response_lanes;
	struct workqueue_struct *iod_wq;
	unsigned int nr_response_lanes;
	unsigned int lane;
	size_t size;
	u16 status;
	int ret;

	lockdep_assert_held(&ctrl->state_lock);
	if (qid >= ctrl->nr_queues || cqid >= ctrl->nr_queues)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
	if (!(flags & NVME_QUEUE_PHYS_CONTIG) || !IS_ALIGNED(prp1, SZ_4K))
		return NVME_SC_INVALID_QUEUE | NVME_STATUS_DNR;
	if (check_mul_overflow((size_t)depth, sizeof(struct nvme_command),
			       &size))
		return NVME_SC_QUEUE_SIZE | NVME_STATUS_DNR;

	sq = &ctrl->sqs[qid];
	cq = &ctrl->cqs[cqid];
	if (!qid)
		iod_wq = alloc_ordered_workqueue("nvmet_mdev_sq%u",
						 WQ_MEM_RECLAIM, qid);
	else
		iod_wq = alloc_workqueue("nvmet_mdev_sq%u",
					 WQ_UNBOUND | WQ_MEM_RECLAIM,
					 min_t(unsigned int, depth, 64), qid);
	if (!iod_wq)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	nr_response_lanes = nvmet_mdev_response_worker_count(ctrl, qid, depth);
	response_lanes = kcalloc(nr_response_lanes, sizeof(*response_lanes),
				 GFP_KERNEL);
	if (!response_lanes) {
		destroy_workqueue(iod_wq);
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	}
	for (lane = 0; lane < nr_response_lanes; lane++) {
		response_lanes[lane].sq = sq;
		INIT_WORK(&response_lanes[lane].work, nvmet_mdev_response_work);
		spin_lock_init(&response_lanes[lane].lock);
		INIT_LIST_HEAD(&response_lanes[lane].responses);
		atomic_set(&response_lanes[lane].work_queued, 0);
	}

	mutex_lock(&ctrl->lock);
	if (sq->live || !cq->live) {
		status = NVME_SC_QID_INVALID | NVME_STATUS_DNR;
		goto out_destroy_wq;
	}

	ret = nvmet_mdev_map_guest(ctrl, prp1, size, IOMMU_READ,
				   &sq->mapping);
	if (ret) {
		status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
		goto out_destroy_wq;
	}
	sq->entries = nvmet_mdev_mapping_addr(sq->mapping);
	sq->depth = depth;
	sq->head = 0;
	sq->cq = cq;
	sq->iod_wq = iod_wq;
	sq->response_lanes = response_lanes;
	sq->nr_response_lanes = nr_response_lanes;
	atomic_set(&sq->runner_active, 0);
	atomic_set(&sq->kick_pending, 0);
	put_unaligned_le32(0, ctrl->bar0 + NVME_REG_DBS +
			   (qid * 2 * sizeof(u32)));

	status = nvmet_sq_create(ctrl->tctrl, &sq->nvme_sq, &cq->nvme_cq,
				 qid, depth);
	if (status != NVME_SC_SUCCESS) {
		nvmet_mdev_unmap_guest(ctrl, sq->mapping);
		sq->mapping = NULL;
		sq->entries = NULL;
		sq->depth = 0;
		sq->cq = NULL;
		sq->iod_wq = NULL;
		sq->response_lanes = NULL;
		sq->nr_response_lanes = 0;
		goto out_destroy_wq;
	}
	smp_store_release(&sq->live, true);
	mutex_unlock(&ctrl->lock);
	return NVME_SC_SUCCESS;

out_destroy_wq:
	mutex_unlock(&ctrl->lock);
	destroy_workqueue(iod_wq);
	kfree(response_lanes);
	return status;
}

static u16 nvmet_mdev_delete_sq_locked(struct nvmet_mdev_ctrl *ctrl, u16 qid)
{
	struct workqueue_struct *iod_wq;
	struct nvmet_mdev_response_lane *response_lanes;
	struct nvmet_mdev_sq *sq;

	lockdep_assert_held(&ctrl->state_lock);
	if (qid >= ctrl->nr_queues)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;

	sq = &ctrl->sqs[qid];
	mutex_lock(&ctrl->lock);
	if (!sq->live) {
		mutex_unlock(&ctrl->lock);
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
	}
	smp_store_release(&sq->live, false);
	iod_wq = sq->iod_wq;
	response_lanes = sq->response_lanes;
	mutex_unlock(&ctrl->lock);

	synchronize_rcu();
	cancel_work_sync(&sq->work);
	atomic_set(&sq->runner_active, 0);
	atomic_set(&sq->kick_pending, 0);
	flush_workqueue(iod_wq);
	nvmet_sq_destroy(&sq->nvme_sq);
	destroy_workqueue(iod_wq);
	kfree(response_lanes);

	mutex_lock(&ctrl->lock);
	if (sq->mapping)
		nvmet_mdev_unmap_guest(ctrl, sq->mapping);
	sq->mapping = NULL;
	sq->entries = NULL;
	sq->iod_wq = NULL;
	sq->response_lanes = NULL;
	sq->nr_response_lanes = 0;
	sq->cq = NULL;
	sq->depth = 0;
	sq->head = 0;
	mutex_unlock(&ctrl->lock);
	return NVME_SC_SUCCESS;
}

static u16 nvmet_mdev_delete_cq_locked(struct nvmet_mdev_ctrl *ctrl, u16 qid)
{
	struct nvmet_mdev_cq *cq;

	lockdep_assert_held(&ctrl->state_lock);
	if (qid >= ctrl->nr_queues)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;

	cq = &ctrl->cqs[qid];
	mutex_lock(&ctrl->lock);
	if (!cq->live) {
		mutex_unlock(&ctrl->lock);
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
	}
	smp_store_release(&cq->live, false);
	atomic_set(&cq->blocked, 0);
	mutex_unlock(&ctrl->lock);

	synchronize_rcu();
	cancel_work_sync(&cq->work);
	atomic_set(&cq->publisher_active, 0);
	atomic_set(&cq->kick_pending, 0);
	atomic_set(&cq->irq_outstanding, 0);
	nvmet_mdev_drain_completions(cq);
	nvmet_cq_put(&cq->nvme_cq);

	mutex_lock(&ctrl->lock);
	if (cq->mapping)
		nvmet_mdev_unmap_guest(ctrl, cq->mapping);
	cq->mapping = NULL;
	cq->entries = NULL;
	cq->depth = 0;
	atomic_set(&cq->head, 0);
	cq->tail = 0;
	cq->phase = 1;
	cq->vector = 0;
	cq->irq_enabled = false;
	atomic_set(&cq->blocked, 0);
	mutex_unlock(&ctrl->lock);
	return NVME_SC_SUCCESS;
}

static void nvmet_mdev_disable_dbbuf(struct nvmet_mdev_ctrl *ctrl)
{
	mutex_lock(&ctrl->lock);
	smp_store_release(&ctrl->dbbuf_dbs, NULL);
	WRITE_ONCE(ctrl->dbbuf_eis, NULL);
	if (ctrl->dbbuf_dbs_mapping)
		nvmet_mdev_unmap_guest(ctrl, ctrl->dbbuf_dbs_mapping);
	if (ctrl->dbbuf_eis_mapping)
		nvmet_mdev_unmap_guest(ctrl, ctrl->dbbuf_eis_mapping);
	ctrl->dbbuf_dbs_mapping = NULL;
	ctrl->dbbuf_eis_mapping = NULL;
	mutex_unlock(&ctrl->lock);
}

static void __nvmet_mdev_disable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc)
{
	unsigned int qid;

	lockdep_assert_held(&ctrl->state_lock);
	mutex_lock(&ctrl->lock);
	ctrl->enabled = false;
	mutex_unlock(&ctrl->lock);
	cancel_delayed_work_sync(&ctrl->poll_work);

	for (qid = ctrl->nr_queues; qid-- > 0;)
		if (ctrl->sqs[qid].live)
			nvmet_mdev_delete_sq_locked(ctrl, qid);
	for (qid = ctrl->nr_queues; qid-- > 0;)
		if (ctrl->cqs[qid].live)
			nvmet_mdev_delete_cq_locked(ctrl, qid);
	/* Queue workers may access shadow doorbells until both queue sets drain. */
	nvmet_mdev_disable_dbbuf(ctrl);
	nvmet_mdev_irq_quiesce(ctrl);
	nvmet_mdev_reset_irq_features(ctrl);

	if (!ctrl->tctrl)
		return;
	nvmet_update_cc(ctrl->tctrl, cc);
	mutex_lock(&ctrl->lock);
	put_unaligned_le32(ctrl->tctrl->csts, ctrl->bar0 + NVME_REG_CSTS);
	mutex_unlock(&ctrl->lock);
}

int nvmet_mdev_enable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc)
{
	struct nvmet_pci_admin_config admin;
	u16 status;
	u64 cap, asq, acq;
	u32 aqa;
	int ret = 0;

	mutex_lock(&ctrl->state_lock);
	mutex_lock(&ctrl->lock);
	if (ctrl->enabled) {
		mutex_unlock(&ctrl->lock);
		goto out_unlock_state;
	}
	mutex_lock(&ctrl->dma_lock);
	ctrl->dma_blocked = false;
	mutex_unlock(&ctrl->dma_lock);
	cap = get_unaligned_le64(ctrl->bar0 + NVME_REG_CAP);
	aqa = get_unaligned_le32(ctrl->bar0 + NVME_REG_AQA);
	asq = get_unaligned_le64(ctrl->bar0 + NVME_REG_ASQ);
	acq = get_unaligned_le64(ctrl->bar0 + NVME_REG_ACQ);
	mutex_unlock(&ctrl->lock);

	ret = nvmet_pci_parse_admin_config(cap, cc, aqa, asq, acq, &admin);
	if (ret)
		goto fail;
	status = nvmet_mdev_create_cq_locked(ctrl, 0,
					     NVME_QUEUE_PHYS_CONTIG |
					     NVME_CQ_IRQ_ENABLED,
					     admin.cq_depth, admin.acq, 0);
	if (status != NVME_SC_SUCCESS) {
		ret = -EINVAL;
		goto fail;
	}
	status = nvmet_mdev_create_sq_locked(ctrl, 0, 0,
					     NVME_QUEUE_PHYS_CONTIG,
					     admin.sq_depth, admin.asq);
	if (status != NVME_SC_SUCCESS) {
		ret = -EINVAL;
		goto fail;
	}

	mutex_lock(&ctrl->lock);
	ctrl->enabled = true;
	mutex_unlock(&ctrl->lock);
	mutex_lock(&ctrl->dma_lock);
	ctrl->dma_blocked = false;
	mutex_unlock(&ctrl->dma_lock);
	nvmet_update_cc(ctrl->tctrl, cc);
	mutex_lock(&ctrl->lock);
	put_unaligned_le32(ctrl->tctrl->csts, ctrl->bar0 + NVME_REG_CSTS);
	if (!(ctrl->tctrl->csts & NVME_CSTS_RDY)) {
		ctrl->enabled = false;
		ret = -EINVAL;
	}
	mutex_unlock(&ctrl->lock);
	if (ret)
		goto fail;
	goto out_unlock_state;

fail:
	__nvmet_mdev_disable_ctrl(ctrl, 0);
	mutex_lock(&ctrl->lock);
	nvmet_mdev_pci_set_fatal(ctrl);
	mutex_unlock(&ctrl->lock);
out_unlock_state:
	mutex_unlock(&ctrl->state_lock);
	return ret;
}

void nvmet_mdev_disable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc)
{
	mutex_lock(&ctrl->state_lock);
	nvmet_mdev_disable_ctrl_locked(ctrl, cc);
	mutex_unlock(&ctrl->state_lock);
}

void nvmet_mdev_disable_ctrl_locked(struct nvmet_mdev_ctrl *ctrl, u32 cc)
{
	lockdep_assert_held(&ctrl->state_lock);
	__nvmet_mdev_disable_ctrl(ctrl, cc);
}

void nvmet_mdev_queue_cleanup(struct nvmet_mdev_ctrl *ctrl)
{
	struct llist_node *node;

	nvmet_mdev_disable_ctrl(ctrl, 0);
	/* Doorbell callbacks hold RCU while dereferencing the queue arrays. */
	synchronize_rcu();
	cancel_delayed_work_sync(&ctrl->poll_work);
	kfree(ctrl->cqs);
	ctrl->cqs = NULL;
	kfree(ctrl->sqs);
	ctrl->sqs = NULL;
	if (ctrl->iod_pool_ready) {
		while ((node = llist_del_first(&ctrl->iod_free))) {
			struct nvmet_mdev_iod *iod =
				llist_entry(node, struct nvmet_mdev_iod, free_node);

			nvmet_mdev_destroy_iod(ctrl, iod);
		}
		atomic_set(&ctrl->iod_free_count, 0);
		mempool_exit(&ctrl->iod_pool);
		ctrl->iod_pool_ready = false;
	}
	ctrl->nr_queues = 0;
}

void nvmet_mdev_schedule_doorbell(struct nvmet_mdev_ctrl *ctrl, u16 qid,
				  bool cq)
{
	nvmet_mdev_stat_inc(ctrl, doorbell_kicks);
	rcu_read_lock();
	if (!READ_ONCE(ctrl->enabled) || qid >= ctrl->nr_queues)
		goto out;
	if (cq) {
		struct nvmet_mdev_cq *mcq = &ctrl->cqs[qid];

		nvmet_mdev_handle_cq_head(mcq, true);
	} else if (smp_load_acquire(&ctrl->sqs[qid].live)) {
		nvmet_mdev_activate_sq(&ctrl->sqs[qid]);
	}
out:
	nvmet_mdev_kick_poller(ctrl);
	rcu_read_unlock();
}

u8 nvmet_mdev_get_mdts(const struct nvmet_ctrl *tctrl)
{
	return ilog2(NVMET_MDEV_COPY_MAX_DATA) - 12;
}

u16 nvmet_mdev_create_sq(struct nvmet_ctrl *tctrl, u16 sqid, u16 cqid,
			 u16 flags, u16 qsize, u64 prp1)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	u16 status;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	/* Teardown may hold state_lock while draining this admin request. */
	if (!mutex_trylock(&ctrl->state_lock))
		return NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
	if (!READ_ONCE(ctrl->enabled)) {
		status = NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
		goto out_unlock;
	}
	status = nvmet_mdev_create_sq_locked(ctrl, sqid, cqid, flags,
					     qsize + 1, prp1);
out_unlock:
	mutex_unlock(&ctrl->state_lock);
	return status;
}

u16 nvmet_mdev_delete_sq(struct nvmet_ctrl *tctrl, u16 sqid)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	u16 status;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	if (!mutex_trylock(&ctrl->state_lock))
		return NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
	if (!READ_ONCE(ctrl->enabled)) {
		status = NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
		goto out_unlock;
	}
	status = nvmet_mdev_delete_sq_locked(ctrl, sqid);
out_unlock:
	mutex_unlock(&ctrl->state_lock);
	return status;
}

u16 nvmet_mdev_create_cq(struct nvmet_ctrl *tctrl, u16 cqid, u16 flags,
			 u16 qsize, u64 prp1, u16 irq_vector)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	u16 status;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	if (!mutex_trylock(&ctrl->state_lock))
		return NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
	if (!READ_ONCE(ctrl->enabled)) {
		status = NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
		goto out_unlock;
	}
	status = nvmet_mdev_create_cq_locked(ctrl, cqid, flags, qsize + 1,
					     prp1, irq_vector);
out_unlock:
	mutex_unlock(&ctrl->state_lock);
	return status;
}

u16 nvmet_mdev_delete_cq(struct nvmet_ctrl *tctrl, u16 cqid)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	u16 status;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	if (!mutex_trylock(&ctrl->state_lock))
		return NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
	if (!READ_ONCE(ctrl->enabled)) {
		status = NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
		goto out_unlock;
	}
	status = nvmet_mdev_delete_cq_locked(ctrl, cqid);
out_unlock:
	mutex_unlock(&ctrl->state_lock);
	return status;
}

u16 nvmet_mdev_get_feature(const struct nvmet_ctrl *tctrl, u8 feature,
			   void *data)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	struct nvmet_mdev_irq_vector *irq;
	struct nvmet_feat_irq_coalesce *irqc;
	struct nvmet_feat_irq_config *irqcfg;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;

	switch (feature) {
	case NVME_FEAT_ARBITRATION:
		((struct nvmet_feat_arbitration *)data)->ab = 0;
		return NVME_SC_SUCCESS;
	case NVME_FEAT_IRQ_COALESCE:
		irqc = data;
		irqc->thr = READ_ONCE(ctrl->irq_coalesce_threshold);
		irqc->time = READ_ONCE(ctrl->irq_coalesce_time);
		return NVME_SC_SUCCESS;
	case NVME_FEAT_IRQ_CONFIG:
		irqcfg = data;
		if (irqcfg->iv >= NVMET_MDEV_PCI_MSIX_VECTORS)
			return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		irq = &ctrl->irq_vectors[irqcfg->iv];
		irqcfg->cd = READ_ONCE(irq->coalescing_disabled);
		return NVME_SC_SUCCESS;
	default:
		return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
	}
}

u16 nvmet_mdev_set_feature(const struct nvmet_ctrl *tctrl, u8 feature,
			   void *data)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	struct nvmet_feat_irq_coalesce *irqc;
	struct nvmet_feat_irq_config *irqcfg;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	if (feature == NVME_FEAT_ARBITRATION)
		return NVME_SC_SUCCESS;
	if (feature == NVME_FEAT_IRQ_COALESCE) {
		irqc = data;
		WRITE_ONCE(ctrl->irq_coalesce_threshold, irqc->thr);
		WRITE_ONCE(ctrl->irq_coalesce_time, irqc->time);
		return NVME_SC_SUCCESS;
	}
	if (feature == NVME_FEAT_IRQ_CONFIG) {
		irqcfg = data;
		if (irqcfg->iv >= NVMET_MDEV_PCI_MSIX_VECTORS)
			return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		WRITE_ONCE(ctrl->irq_vectors[irqcfg->iv].coalescing_disabled,
			   irqcfg->cd);
		return NVME_SC_SUCCESS;
	}
	return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
}

u16 nvmet_mdev_set_dbbuf(struct nvmet_ctrl *tctrl, u64 dbs, u64 eis)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	size_t size;
	int ret;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	if (check_mul_overflow((size_t)ctrl->nr_queues, 2 * sizeof(u32),
			       &size))
		return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;

	mutex_lock(&ctrl->lock);
	if (!ctrl->enabled) {
		ret = NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
		goto unlock;
	}
	if (ctrl->dbbuf_dbs_mapping || ctrl->dbbuf_eis_mapping) {
		ret = NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
		goto unlock;
	}

	ret = nvmet_mdev_map_guest(ctrl, dbs, size, IOMMU_READ,
				   &ctrl->dbbuf_dbs_mapping);
	if (ret)
		goto map_error;
	ret = nvmet_mdev_map_guest(ctrl, eis, size, IOMMU_WRITE,
				   &ctrl->dbbuf_eis_mapping);
	if (ret) {
		nvmet_mdev_unmap_guest(ctrl, ctrl->dbbuf_dbs_mapping);
		ctrl->dbbuf_dbs_mapping = NULL;
		goto map_error;
	}

	WRITE_ONCE(ctrl->dbbuf_eis,
		   nvmet_mdev_mapping_addr(ctrl->dbbuf_eis_mapping));
	smp_store_release(&ctrl->dbbuf_dbs,
			  nvmet_mdev_mapping_addr(ctrl->dbbuf_dbs_mapping));
	mutex_unlock(&ctrl->lock);
	mod_delayed_work(system_wq, &ctrl->poll_work, 0);
	return NVME_SC_SUCCESS;

map_error:
	ret = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
unlock:
	mutex_unlock(&ctrl->lock);
	return ret;
}
