// SPDX-License-Identifier: GPL-2.0

#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "../pci-common.h"
#include "priv.h"

#define NVMET_MDEV_COPY_MAX_DATA	SZ_1M
#define NVMET_MDEV_PRP_ENTRIES		(SZ_4K / sizeof(__le64))
#define NVMET_MDEV_MAX_SEGS		(NVMET_MDEV_COPY_MAX_DATA / SZ_4K + 1)
#define NVMET_MDEV_BATCH_SIZE		64
#define NVMET_MDEV_POLL_INTERVAL	msecs_to_jiffies(10)

static uint pin_cache_pages = 65536;
module_param_named(pin_cache_pages, pin_cache_pages, uint, 0644);
MODULE_PARM_DESC(pin_cache_pages, "Maximum cached guest pages per controller");

static uint pin_cache_max_segs = 64;
module_param_named(pin_cache_max_segs, pin_cache_max_segs, uint, 0644);
MODULE_PARM_DESC(pin_cache_max_segs, "Maximum PRP segments admitted to the pin cache");

static uint poll_budget = 128;
module_param_named(poll_budget, poll_budget, uint, 0644);
MODULE_PARM_DESC(poll_budget, "Maximum queues examined by one bounded DBBUF poll run");

struct nvmet_mdev_iod {
	struct list_head entry;
	struct llist_node free_node;
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
	if (sq->qid && ctrl->dbbuf_dbs)
		return le32_to_cpu(READ_ONCE(ctrl->dbbuf_dbs[sq->qid * 2]));
	return get_unaligned_le32(ctrl->bar0 + NVME_REG_DBS +
				  sq->qid * 2 * sizeof(u32));
}

static u32 nvmet_mdev_cq_head(struct nvmet_mdev_ctrl *ctrl,
			      const struct nvmet_mdev_cq *cq)
{
	if (cq->qid && ctrl->dbbuf_dbs)
		return le32_to_cpu(READ_ONCE(ctrl->dbbuf_dbs[cq->qid * 2 + 1]));
	return get_unaligned_le32(ctrl->bar0 + NVME_REG_DBS +
				  (cq->qid * 2 + 1) * sizeof(u32));
}

static void __nvmet_mdev_queue_cq_work(struct nvmet_mdev_cq *cq);

static bool nvmet_mdev_handle_cq_head(struct nvmet_mdev_cq *cq, u32 head)
{
	struct nvmet_mdev_ctrl *ctrl = cq->ctrl;
	unsigned long flags;
	bool wake = false;

	spin_lock_irqsave(&cq->lock, flags);
	if (READ_ONCE(cq->live) &&
	    (head >= cq->depth ||
	     (cq->blocked && head != READ_ONCE(cq->head)))) {
		__nvmet_mdev_queue_cq_work(cq);
		wake = true;
	}
	spin_unlock_irqrestore(&cq->lock, flags);

	if (wake)
		nvmet_mdev_stat_inc(ctrl, cq_head_wakeups);
	return wake;
}

static void nvmet_mdev_budget_poll(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int qid, limit, scanned = 0;
	bool enabled;

	nvmet_mdev_stat_inc(ctrl, poll_wakeups);
	nvmet_mdev_stat_inc(ctrl, poll_runs);
	enabled = READ_ONCE(ctrl->enabled) && READ_ONCE(ctrl->dbbuf_dbs) &&
		  READ_ONCE(ctrl->dbbuf_eis);
	if (!enabled)
		goto unlock;

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
			schedule_work(&sq->work);
		if (READ_ONCE(cq->live)) {
			u32 head = nvmet_mdev_cq_head(ctrl, cq);

			if (head != READ_ONCE(cq->head))
				nvmet_mdev_handle_cq_head(cq, head);
		}
		scanned++;
		qid++;
		if (qid == ctrl->nr_queues)
			qid = 1;
	}
	ctrl->poll_next_qid = qid;
account:
	nvmet_mdev_stat_add(ctrl, poll_queue_checks, scanned);
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

	enabled = READ_ONCE(ctrl->enabled) && READ_ONCE(ctrl->dbbuf_dbs);
	if (!enabled)
		return;
	mod_delayed_work(system_wq, &ctrl->poll_work, 0);
}

static void __nvmet_mdev_queue_cq_work(struct nvmet_mdev_cq *cq)
{
	lockdep_assert_held(&cq->lock);

	if (atomic_cmpxchg(&cq->work_queued, 0, 1) == 0)
		schedule_work(&cq->work);
}

static bool nvmet_mdev_arm_sq_event(struct nvmet_mdev_ctrl *ctrl,
				    struct nvmet_mdev_sq *sq)
{
	u32 tail;

	if (!READ_ONCE(ctrl->enabled) || !READ_ONCE(sq->live) || !sq->qid ||
	    !READ_ONCE(ctrl->dbbuf_dbs) || !READ_ONCE(ctrl->dbbuf_eis))
		return false;
	tail = nvmet_mdev_sq_tail(ctrl, sq);
	WRITE_ONCE(ctrl->dbbuf_eis[sq->qid * 2], cpu_to_le32(tail));
	/* Pair with the guest's shadow-doorbell write and event-index read. */
	mb();
	return nvmet_mdev_sq_tail(ctrl, sq) != tail;
}

static bool nvmet_mdev_arm_cq_event(struct nvmet_mdev_ctrl *ctrl,
				    struct nvmet_mdev_cq *cq)
{
	u32 head;

	if (!READ_ONCE(ctrl->enabled) || !READ_ONCE(cq->live) || !cq->qid ||
	    !READ_ONCE(ctrl->dbbuf_dbs) || !READ_ONCE(ctrl->dbbuf_eis))
		return false;
	head = nvmet_mdev_cq_head(ctrl, cq);
	if (head != READ_ONCE(cq->head))
		return true;
	WRITE_ONCE(ctrl->dbbuf_eis[cq->qid * 2 + 1], cpu_to_le32(head));
	/* Pair with the guest's shadow-doorbell write and event-index read. */
	mb();
	return nvmet_mdev_cq_head(ctrl, cq) != head;
}

static void nvmet_mdev_complete_iod(struct nvmet_mdev_iod *iod)
{
	struct nvmet_mdev_cq *cq = iod->cq;
	struct nvmet_mdev_ctrl *ctrl = iod->ctrl;
	unsigned long flags;

	spin_lock_irqsave(&cq->lock, flags);
	if (!READ_ONCE(ctrl->enabled) || !READ_ONCE(cq->live)) {
		spin_unlock_irqrestore(&cq->lock, flags);
		nvmet_mdev_put_iod(iod);
		return;
	}
	list_add_tail(&iod->entry, &cq->completions);
	__nvmet_mdev_queue_cq_work(cq);
	spin_unlock_irqrestore(&cq->lock, flags);
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
	struct nvmet_mdev_sq *sq =
		container_of(work, struct nvmet_mdev_sq, response_work);
	struct nvmet_mdev_ctrl *ctrl = sq->ctrl;
	unsigned long flags;

	nvmet_mdev_stat_inc(ctrl, response_work_runs);
	for (;;) {
		struct nvmet_mdev_iod *iod, *tmp;
		unsigned int nr = 0;
		LIST_HEAD(responses);

		spin_lock_irqsave(&sq->response_lock, flags);
		if (list_empty(&sq->responses)) {
			atomic_set(&sq->response_work_queued, 0);
			spin_unlock_irqrestore(&sq->response_lock, flags);
			return;
		}
		list_splice_init(&sq->responses, &responses);
		spin_unlock_irqrestore(&sq->response_lock, flags);

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
	struct nvmet_mdev_iod *iod, *tmp;
	bool more = false;
	bool failed = false;
	LIST_HEAD(submissions);
	u32 tail;

	nvmet_mdev_stat_inc(ctrl, sq_work_runs);
	if (!READ_ONCE(ctrl->enabled) || !READ_ONCE(sq->live))
		goto unlock;

	tail = nvmet_mdev_sq_tail(ctrl, sq);
	if (tail >= sq->depth) {
		mutex_lock(&ctrl->lock);
		WRITE_ONCE(ctrl->enabled, false);
		nvmet_mdev_pci_set_fatal(ctrl);
		mutex_unlock(&ctrl->lock);
		goto unlock;
	}

	while (sq->head != tail && processed < NVMET_MDEV_BATCH_SIZE) {
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
		list_add_tail(&iod->entry, &submissions);
		processed++;
	}
	more = sq->head != tail;
	nvmet_mdev_stat_add(ctrl, commands, processed);
	if (processed)
		nvmet_mdev_stat_inc(ctrl, sq_batches);

unlock:
	list_for_each_entry_safe(iod, tmp, &submissions, entry) {
		list_del_init(&iod->entry);
		if (failed) {
			nvmet_mdev_put_iod(iod);
			nvmet_mdev_put_iod(iod);
		} else {
			nvmet_mdev_submit_iod(iod);
		}
	}
	if (more && !failed)
		schedule_work(&sq->work);
	else if (!failed && nvmet_mdev_arm_sq_event(ctrl, sq))
		schedule_work(&sq->work);
}

static void nvmet_mdev_cq_work(struct work_struct *work)
{
	struct nvmet_mdev_cq *cq =
		container_of(work, struct nvmet_mdev_cq, work);
	struct nvmet_mdev_ctrl *ctrl = cq->ctrl;
	unsigned int completed;
	struct nvmet_mdev_iod *iod, *tmp;
	unsigned long flags;
	bool blocked;
	u32 head;

	nvmet_mdev_stat_inc(ctrl, cq_work_runs);
again:
	completed = 0;
	{
		LIST_HEAD(done);

		if (!READ_ONCE(ctrl->enabled) || !READ_ONCE(cq->live))
			goto unlock;

		head = nvmet_mdev_cq_head(ctrl, cq);
		if (head >= cq->depth) {
			mutex_lock(&ctrl->lock);
			WRITE_ONCE(ctrl->enabled, false);
			nvmet_mdev_pci_set_fatal(ctrl);
			mutex_unlock(&ctrl->lock);
			goto unlock;
		}
		WRITE_ONCE(cq->head, head);

		spin_lock_irqsave(&cq->lock, flags);
		while (!list_empty(&cq->completions) &&
		       !nvmet_pci_cq_full(cq->head, cq->tail, cq->depth)) {
			struct nvme_completion cqe;
			u16 status;

			iod = list_first_entry(&cq->completions,
					       struct nvmet_mdev_iod, entry);
			list_del_init(&iod->entry);

			cqe = iod->cqe;
			status = le16_to_cpu(cqe.status) >> 1;
			nvmet_pci_prepare_cqe(&cqe, le16_to_cpu(cqe.sq_head),
					      le16_to_cpu(cqe.sq_id), cqe.command_id,
					      status, cq->phase);
			memcpy(cq->entries + cq->tail * sizeof(cqe), &cqe,
			       sizeof(cqe));
			nvmet_pci_advance_cq_tail(&cq->tail, &cq->phase,
						  cq->depth);
			list_add_tail(&iod->entry, &done);
			completed++;
		}
		spin_unlock_irqrestore(&cq->lock, flags);
		if (completed)
			dma_wmb();
		nvmet_mdev_stat_add(ctrl, completions, completed);
		if (completed)
			nvmet_mdev_stat_inc(ctrl, cq_batches);

unlock:
		list_for_each_entry_safe(iod, tmp, &done, entry) {
			list_del_init(&iod->entry);
			nvmet_mdev_put_iod(iod);
		}
	}

	if (completed && cq->irq_enabled)
		nvmet_mdev_notify_irq(ctrl, cq->vector, completed, !cq->qid);
	spin_lock_irqsave(&cq->lock, flags);
	blocked = READ_ONCE(ctrl->enabled) && READ_ONCE(cq->live) &&
		  !list_empty(&cq->completions) &&
		  nvmet_pci_cq_full(cq->head, cq->tail, cq->depth);
	if (READ_ONCE(ctrl->enabled) && READ_ONCE(cq->live) &&
	    !list_empty(&cq->completions) &&
	    !nvmet_pci_cq_full(cq->head, cq->tail, cq->depth)) {
		spin_unlock_irqrestore(&cq->lock, flags);
		goto again;
	}
	cq->blocked = blocked;
	atomic_set(&cq->work_queued, 0);
	spin_unlock_irqrestore(&cq->lock, flags);
	if (blocked) {
		bool advanced;

		/*
		 * A head write before blocked was published is caught here. A
		 * later write observes blocked under cq->lock and queues the worker.
		 */
		if (READ_ONCE(ctrl->dbbuf_dbs)) {
			advanced = nvmet_mdev_arm_cq_event(ctrl, cq);
		} else {
			/* Publish blocked before checking for a racing head update. */
			smp_mb();
			advanced = nvmet_mdev_cq_head(ctrl, cq) !=
				   READ_ONCE(cq->head);
		}
		if (advanced)
			nvmet_mdev_handle_cq_head(cq,
				nvmet_mdev_cq_head(ctrl, cq));
	}
}

void nvmet_mdev_queue_response(struct nvmet_req *req)
{
	struct nvmet_mdev_iod *iod =
		container_of(req, struct nvmet_mdev_iod, req);
	struct nvmet_mdev_sq *sq = iod->sq;
	unsigned long flags;
	bool queue;

	if (iod->pinned && iod->payload.cached) {
		nvmet_mdev_response_iod(iod);
		return;
	}
	spin_lock_irqsave(&sq->response_lock, flags);
	list_add_tail(&iod->entry, &sq->responses);
	queue = atomic_cmpxchg(&sq->response_work_queued, 0, 1) == 0;
	spin_unlock_irqrestore(&sq->response_lock, flags);
	if (queue)
		WARN_ON_ONCE(!queue_work(sq->iod_wq, &sq->response_work));
}

static void nvmet_mdev_drain_completions(struct nvmet_mdev_cq *cq)
{
	struct nvmet_mdev_iod *iod, *tmp;
	unsigned long flags;
	LIST_HEAD(completions);

	spin_lock_irqsave(&cq->lock, flags);
	list_splice_init(&cq->completions, &completions);
	spin_unlock_irqrestore(&cq->lock, flags);

	list_for_each_entry_safe(iod, tmp, &completions, entry) {
		list_del_init(&iod->entry);
		nvmet_mdev_put_iod(iod);
	}
}

int nvmet_mdev_queue_init(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int qid;

	ctrl->runtime.pin_cache_pages = READ_ONCE(pin_cache_pages);
	ctrl->runtime.pin_cache_max_segs = READ_ONCE(pin_cache_max_segs);
	ctrl->runtime.poll_budget = max_t(unsigned int, READ_ONCE(poll_budget), 1);
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
		INIT_WORK(&sq->response_work, nvmet_mdev_response_work);
		spin_lock_init(&sq->response_lock);
		INIT_LIST_HEAD(&sq->responses);
		atomic_set(&sq->response_work_queued, 0);
		cq->ctrl = ctrl;
		cq->qid = qid;
		spin_lock_init(&cq->lock);
		INIT_LIST_HEAD(&cq->completions);
		atomic_set(&cq->work_queued, 0);
		cq->blocked = false;
		INIT_WORK(&cq->work, nvmet_mdev_cq_work);
	}

	return 0;
}

static u16 nvmet_mdev_create_cq_locked(struct nvmet_mdev_ctrl *ctrl, u16 qid,
				       u16 flags, u16 depth, u64 prp1,
				       u16 vector)
{
	struct nvmet_mdev_cq *cq;
	unsigned long irqflags;
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
	cq->head = 0;
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
	spin_lock_irqsave(&cq->lock, irqflags);
	cq->blocked = false;
	atomic_set(&cq->work_queued, 0);
	cq->live = true;
	spin_unlock_irqrestore(&cq->lock, irqflags);

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
	struct workqueue_struct *iod_wq;
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
		goto out_destroy_wq;
	}
	sq->live = true;
	mutex_unlock(&ctrl->lock);
	return NVME_SC_SUCCESS;

out_destroy_wq:
	mutex_unlock(&ctrl->lock);
	destroy_workqueue(iod_wq);
	return status;
}

static u16 nvmet_mdev_delete_sq_locked(struct nvmet_mdev_ctrl *ctrl, u16 qid)
{
	struct workqueue_struct *iod_wq;
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
	sq->live = false;
	iod_wq = sq->iod_wq;
	mutex_unlock(&ctrl->lock);

	cancel_work_sync(&sq->work);
	flush_workqueue(iod_wq);
	nvmet_sq_destroy(&sq->nvme_sq);
	destroy_workqueue(iod_wq);

	mutex_lock(&ctrl->lock);
	if (sq->mapping)
		nvmet_mdev_unmap_guest(ctrl, sq->mapping);
	sq->mapping = NULL;
	sq->entries = NULL;
	sq->iod_wq = NULL;
	sq->cq = NULL;
	sq->depth = 0;
	sq->head = 0;
	mutex_unlock(&ctrl->lock);
	return NVME_SC_SUCCESS;
}

static u16 nvmet_mdev_delete_cq_locked(struct nvmet_mdev_ctrl *ctrl, u16 qid)
{
	struct nvmet_mdev_cq *cq;
	unsigned long irqflags;

	lockdep_assert_held(&ctrl->state_lock);
	if (qid >= ctrl->nr_queues)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;

	cq = &ctrl->cqs[qid];
	mutex_lock(&ctrl->lock);
	if (!cq->live) {
		mutex_unlock(&ctrl->lock);
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
	}
	spin_lock_irqsave(&cq->lock, irqflags);
	cq->live = false;
	cq->blocked = false;
	spin_unlock_irqrestore(&cq->lock, irqflags);
	mutex_unlock(&ctrl->lock);

	cancel_work_sync(&cq->work);
	atomic_set(&cq->work_queued, 0);
	nvmet_mdev_drain_completions(cq);
	nvmet_cq_put(&cq->nvme_cq);

	mutex_lock(&ctrl->lock);
	if (cq->mapping)
		nvmet_mdev_unmap_guest(ctrl, cq->mapping);
	cq->mapping = NULL;
	cq->entries = NULL;
	cq->depth = 0;
	cq->head = 0;
	cq->tail = 0;
	cq->phase = 1;
	cq->vector = 0;
	cq->irq_enabled = false;
	cq->blocked = false;
	mutex_unlock(&ctrl->lock);
	return NVME_SC_SUCCESS;
}

static void nvmet_mdev_disable_dbbuf(struct nvmet_mdev_ctrl *ctrl)
{
	mutex_lock(&ctrl->lock);
	ctrl->dbbuf_dbs = NULL;
	ctrl->dbbuf_eis = NULL;
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
	if (!READ_ONCE(ctrl->enabled) || qid >= ctrl->nr_queues)
		goto out;
	if (cq) {
		struct nvmet_mdev_cq *mcq = &ctrl->cqs[qid];
		u32 head = nvmet_mdev_cq_head(ctrl, mcq);

		nvmet_mdev_handle_cq_head(mcq, head);
	} else if (READ_ONCE(ctrl->sqs[qid].live)) {
		schedule_work(&ctrl->sqs[qid].work);
	}
out:
	nvmet_mdev_kick_poller(ctrl);
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

	ctrl->dbbuf_dbs = nvmet_mdev_mapping_addr(ctrl->dbbuf_dbs_mapping);
	ctrl->dbbuf_eis = nvmet_mdev_mapping_addr(ctrl->dbbuf_eis_mapping);
	mutex_unlock(&ctrl->lock);
	mod_delayed_work(system_wq, &ctrl->poll_work, 0);
	return NVME_SC_SUCCESS;

map_error:
	ret = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
unlock:
	mutex_unlock(&ctrl->lock);
	return ret;
}
