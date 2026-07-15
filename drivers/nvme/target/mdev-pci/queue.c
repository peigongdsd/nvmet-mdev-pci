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
#define NVMET_MDEV_POLL_BUSY		msecs_to_jiffies(20)
#define NVMET_MDEV_POLL_INTERVAL	msecs_to_jiffies(10)

static bool pinned_io = true;
module_param_named(pinned_io, pinned_io, bool, 0644);
MODULE_PARM_DESC(pinned_io, "Use request-lifetime pinned guest pages for I/O");

struct nvmet_mdev_iod {
	struct list_head entry;
	struct nvmet_mdev_ctrl *ctrl;
	struct nvmet_mdev_sq *sq;
	struct nvmet_mdev_cq *cq;
	struct work_struct submit_work;
	struct work_struct response_work;
	struct nvmet_req req;
	struct nvme_command cmd;
	struct nvme_completion cqe;
	refcount_t refs;
	struct nvmet_mdev_payload payload;
	struct nvmet_mdev_iova_segment *segments;
	unsigned int nr_segments;
	size_t data_len;
	bool host_to_ctrl;
	bool pinned;
};

static void nvmet_mdev_free_iod(struct nvmet_mdev_iod *iod)
{
	if (iod->payload.active)
		nvmet_mdev_unpin_payload(iod->ctrl, &iod->payload);
	if (iod->req.sg && !iod->pinned)
		nvmet_req_free_sgls(&iod->req);
	kfree(iod->segments);
	mempool_free(iod, &iod->ctrl->iod_pool);
}

static void nvmet_mdev_put_iod(struct nvmet_mdev_iod *iod)
{
	if (refcount_dec_and_test(&iod->refs))
		nvmet_mdev_free_iod(iod);
}

static int nvmet_mdev_collect_prps(struct nvmet_mdev_iod *iod)
{
	struct nvme_command *cmd = &iod->cmd;
	size_t remaining = iod->data_len;
	__le64 *prps = NULL;
	u64 prp, list_iova;
	int ret = 0;

	if (!remaining)
		return 0;
	if (remaining > NVMET_MDEV_COPY_MAX_DATA)
		return -E2BIG;

	iod->segments = kcalloc(NVMET_MDEV_MAX_SEGS,
				sizeof(*iod->segments), GFP_KERNEL);
	if (!iod->segments)
		return -ENOMEM;

	prp = le64_to_cpu(cmd->common.dptr.prp1);
	if (!prp || !IS_ALIGNED(prp, sizeof(u32))) {
		ret = -EINVAL;
		goto out;
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

	prps = kmalloc(SZ_4K, GFP_KERNEL);
	if (!prps) {
		ret = -ENOMEM;
		goto out;
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
			if (iod->nr_segments >= NVMET_MDEV_MAX_SEGS) {
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
	kfree(prps);
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

static void nvmet_mdev_poll_work(struct work_struct *work)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(to_delayed_work(work), struct nvmet_mdev_ctrl,
			     poll_work);
	unsigned int qid;
	u64 scans = 0;

	for (;;) {
		bool activity = false;
		bool raced = false;

		scans++;

		mutex_lock(&ctrl->lock);
		if (!ctrl->enabled || !ctrl->dbbuf_dbs || !ctrl->dbbuf_eis) {
			mutex_unlock(&ctrl->lock);
			atomic64_add(scans, &ctrl->stats.poll_scans);
			return;
		}

		for (qid = 1; qid < ctrl->nr_queues; qid++) {
			struct nvmet_mdev_sq *sq = &ctrl->sqs[qid];
			struct nvmet_mdev_cq *cq = &ctrl->cqs[qid];
			u32 value;

			if (sq->live) {
				value = nvmet_mdev_sq_tail(ctrl, sq);
				if (value != READ_ONCE(sq->head)) {
					schedule_work(&sq->work);
					activity = true;
				}
			}
			if (cq->live) {
				value = nvmet_mdev_cq_head(ctrl, cq);
				if (value != READ_ONCE(cq->head)) {
					schedule_work(&cq->work);
					activity = true;
				}
			}
		}

		if (activity)
			ctrl->poll_busy_until = jiffies + NVMET_MDEV_POLL_BUSY;
		if (time_before(jiffies, ctrl->poll_busy_until)) {
			for (qid = 1; qid < ctrl->nr_queues; qid++) {
				u32 value;

				if (ctrl->sqs[qid].live) {
					value = nvmet_mdev_sq_tail(ctrl, &ctrl->sqs[qid]) - 1;
					WRITE_ONCE(ctrl->dbbuf_eis[qid * 2],
						   cpu_to_le32(value));
				}
				if (ctrl->cqs[qid].live) {
					value = nvmet_mdev_cq_head(ctrl, &ctrl->cqs[qid]) - 1;
					WRITE_ONCE(ctrl->dbbuf_eis[qid * 2 + 1],
						   cpu_to_le32(value));
				}
			}
			mutex_unlock(&ctrl->lock);
			cond_resched();
			cpu_relax();
			continue;
		}

		for (qid = 1; qid < ctrl->nr_queues; qid++) {
			u32 value;

			if (ctrl->sqs[qid].live) {
				value = nvmet_mdev_sq_tail(ctrl, &ctrl->sqs[qid]);
				WRITE_ONCE(ctrl->dbbuf_eis[qid * 2],
					   cpu_to_le32(value));
			}
			if (ctrl->cqs[qid].live) {
				value = nvmet_mdev_cq_head(ctrl, &ctrl->cqs[qid]);
				WRITE_ONCE(ctrl->dbbuf_eis[qid * 2 + 1],
					   cpu_to_le32(value));
			}
		}
		/* Publish event indices before checking for a racing doorbell. */
		mb();
		for (qid = 1; qid < ctrl->nr_queues; qid++) {
			if (ctrl->sqs[qid].live &&
			    nvmet_mdev_sq_tail(ctrl, &ctrl->sqs[qid]) !=
			    le32_to_cpu(READ_ONCE(ctrl->dbbuf_eis[qid * 2])))
				raced = true;
			if (ctrl->cqs[qid].live &&
			    nvmet_mdev_cq_head(ctrl, &ctrl->cqs[qid]) !=
			    le32_to_cpu(READ_ONCE(ctrl->dbbuf_eis[qid * 2 + 1])))
				raced = true;
		}
		if (raced)
			ctrl->poll_busy_until = jiffies + NVMET_MDEV_POLL_BUSY;
		mutex_unlock(&ctrl->lock);
		if (!raced) {
			schedule_delayed_work(&ctrl->poll_work,
					      NVMET_MDEV_POLL_INTERVAL);
			atomic64_add(scans, &ctrl->stats.poll_scans);
			return;
		}
	}
}

static void nvmet_mdev_kick_poller(struct nvmet_mdev_ctrl *ctrl)
{
	bool enabled;

	mutex_lock(&ctrl->lock);
	enabled = ctrl->enabled && ctrl->dbbuf_dbs;
	if (enabled)
		ctrl->poll_busy_until = jiffies + NVMET_MDEV_POLL_BUSY;
	mutex_unlock(&ctrl->lock);
	if (!enabled)
		return;
	mod_delayed_work(system_wq, &ctrl->poll_work, 0);
}

static void nvmet_mdev_complete_iod(struct nvmet_mdev_iod *iod)
{
	struct nvmet_mdev_cq *cq = iod->cq;
	struct nvmet_mdev_ctrl *ctrl = iod->ctrl;

	mutex_lock(&ctrl->lock);
	if (!ctrl->enabled || !cq->live) {
		mutex_unlock(&ctrl->lock);
		nvmet_mdev_put_iod(iod);
		return;
	}

	spin_lock(&cq->lock);
	list_add_tail(&iod->entry, &cq->completions);
	spin_unlock(&cq->lock);
	schedule_work(&cq->work);
	mutex_unlock(&ctrl->lock);
}

static void nvmet_mdev_response_work(struct work_struct *work)
{
	struct nvmet_mdev_iod *iod =
		container_of(work, struct nvmet_mdev_iod, response_work);
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

static void nvmet_mdev_submit_work(struct work_struct *work)
{
	struct nvmet_mdev_iod *iod =
		container_of(work, struct nvmet_mdev_iod, submit_work);
	struct nvmet_req *req = &iod->req;
	u16 status;
	int ret;

	mutex_lock(&iod->ctrl->lock);
	if (!iod->ctrl->enabled || !iod->sq->live) {
		mutex_unlock(&iod->ctrl->lock);
		nvmet_mdev_put_iod(iod);
		nvmet_mdev_put_iod(iod);
		return;
	}
	mutex_unlock(&iod->ctrl->lock);

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

		if (pinned_io && iod->sq->qid) {
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
			req->sg_cnt = iod->payload.sgt.orig_nents;
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

	mutex_lock(&ctrl->lock);
	if (!ctrl->enabled || !sq->live)
		goto unlock;

	tail = nvmet_mdev_sq_tail(ctrl, sq);
	if (tail >= sq->depth) {
		ctrl->enabled = false;
		nvmet_mdev_pci_set_fatal(ctrl);
		goto unlock;
	}

	while (sq->head != tail && processed < NVMET_MDEV_BATCH_SIZE) {
		iod = mempool_alloc(&ctrl->iod_pool, GFP_KERNEL);
		if (!iod) {
			ctrl->enabled = false;
			nvmet_mdev_pci_set_fatal(ctrl);
			failed = true;
			break;
		}
		memset(iod, 0, sizeof(*iod));

		iod->ctrl = ctrl;
		iod->sq = sq;
		iod->cq = sq->cq;
		iod->req.cmd = &iod->cmd;
		iod->req.cqe = &iod->cqe;
		iod->req.port = ctrl->mport->port;
		refcount_set(&iod->refs, 2);
		INIT_LIST_HEAD(&iod->entry);
		INIT_WORK(&iod->submit_work, nvmet_mdev_submit_work);
		INIT_WORK(&iod->response_work, nvmet_mdev_response_work);
		dma_rmb();
		memcpy(&iod->cmd,
		       sq->entries + sq->head * sizeof(struct nvme_command),
		       sizeof(iod->cmd));
		nvmet_pci_advance_sq_head(&sq->head, sq->depth);
		list_add_tail(&iod->entry, &submissions);
		processed++;
	}
	more = sq->head != tail;
	atomic64_add(processed, &ctrl->stats.commands);

unlock:
	mutex_unlock(&ctrl->lock);
	list_for_each_entry_safe(iod, tmp, &submissions, entry) {
		list_del_init(&iod->entry);
		if (failed || !queue_work(sq->iod_wq, &iod->submit_work)) {
			nvmet_mdev_put_iod(iod);
			nvmet_mdev_put_iod(iod);
		}
	}
	if (more && !failed)
		schedule_work(&sq->work);
	if (processed)
		nvmet_mdev_kick_poller(ctrl);
}

static void nvmet_mdev_cq_work(struct work_struct *work)
{
	struct nvmet_mdev_cq *cq =
		container_of(work, struct nvmet_mdev_cq, work);
	struct nvmet_mdev_ctrl *ctrl = cq->ctrl;
	unsigned int completed = 0;
	struct nvmet_mdev_iod *iod, *tmp;
	LIST_HEAD(done);
	u32 head;

	mutex_lock(&ctrl->lock);
	if (!ctrl->enabled || !cq->live)
		goto unlock;

	head = nvmet_mdev_cq_head(ctrl, cq);
	if (head >= cq->depth) {
		ctrl->enabled = false;
		nvmet_mdev_pci_set_fatal(ctrl);
		goto unlock;
	}
	cq->head = head;

	spin_lock(&cq->lock);
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
		memcpy(cq->entries + cq->tail * sizeof(cqe), &cqe, sizeof(cqe));
		nvmet_pci_advance_cq_tail(&cq->tail, &cq->phase, cq->depth);
		list_add_tail(&iod->entry, &done);
		completed++;
	}
	spin_unlock(&cq->lock);
	if (completed)
		dma_wmb();
	atomic64_add(completed, &ctrl->stats.completions);

unlock:
	mutex_unlock(&ctrl->lock);
	list_for_each_entry_safe(iod, tmp, &done, entry) {
		list_del_init(&iod->entry);
		nvmet_mdev_put_iod(iod);
	}

	if (completed && cq->irq_enabled)
		nvmet_mdev_notify_irq(ctrl, cq->vector, completed, !cq->qid);
	if (completed)
		nvmet_mdev_kick_poller(ctrl);
}

void nvmet_mdev_queue_response(struct nvmet_req *req)
{
	struct nvmet_mdev_iod *iod =
		container_of(req, struct nvmet_mdev_iod, req);

	if (!queue_work(iod->sq->iod_wq, &iod->response_work))
		nvmet_mdev_put_iod(iod);
}

static void nvmet_mdev_drain_completions(struct nvmet_mdev_cq *cq)
{
	struct nvmet_mdev_iod *iod, *tmp;
	LIST_HEAD(completions);

	spin_lock(&cq->lock);
	list_splice_init(&cq->completions, &completions);
	spin_unlock(&cq->lock);

	list_for_each_entry_safe(iod, tmp, &completions, entry) {
		list_del_init(&iod->entry);
		nvmet_mdev_put_iod(iod);
	}
}

int nvmet_mdev_queue_init(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int qid;

	mutex_init(&ctrl->state_lock);
	INIT_DELAYED_WORK(&ctrl->poll_work, nvmet_mdev_poll_work);
	ctrl->nr_queues = ctrl->tctrl->subsys->max_qid + 1;
	if (mempool_init_kmalloc_pool(&ctrl->iod_pool,
				      min_t(unsigned int, ctrl->nr_queues * 32,
					    1024),
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
		cq->ctrl = ctrl;
		cq->qid = qid;
		spin_lock_init(&cq->lock);
		INIT_LIST_HEAD(&cq->completions);
		INIT_WORK(&cq->work, nvmet_mdev_cq_work);
	}

	return 0;
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
	cq->live = true;

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

	lockdep_assert_held(&ctrl->state_lock);
	if (qid >= ctrl->nr_queues)
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;

	cq = &ctrl->cqs[qid];
	mutex_lock(&ctrl->lock);
	if (!cq->live) {
		mutex_unlock(&ctrl->lock);
		return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
	}
	cq->live = false;
	mutex_unlock(&ctrl->lock);

	cancel_work_sync(&cq->work);
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
	nvmet_mdev_disable_dbbuf(ctrl);

	for (qid = ctrl->nr_queues; qid-- > 0;)
		if (ctrl->sqs[qid].live)
			nvmet_mdev_delete_sq_locked(ctrl, qid);
	for (qid = ctrl->nr_queues; qid-- > 0;)
		if (ctrl->cqs[qid].live)
			nvmet_mdev_delete_cq_locked(ctrl, qid);
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
	ctrl->dma_blocked = false;
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
	ctrl->dma_blocked = false;
	mutex_unlock(&ctrl->lock);
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
	nvmet_mdev_disable_ctrl(ctrl, 0);
	cancel_delayed_work_sync(&ctrl->poll_work);
	kfree(ctrl->cqs);
	ctrl->cqs = NULL;
	kfree(ctrl->sqs);
	ctrl->sqs = NULL;
	if (ctrl->iod_pool_ready) {
		mempool_exit(&ctrl->iod_pool);
		ctrl->iod_pool_ready = false;
	}
	ctrl->nr_queues = 0;
}

void nvmet_mdev_schedule_doorbell(struct nvmet_mdev_ctrl *ctrl, u16 qid,
				  bool cq)
{
	atomic64_inc(&ctrl->stats.doorbell_kicks);
	mutex_lock(&ctrl->lock);
	if (!ctrl->enabled || qid >= ctrl->nr_queues)
		goto out_unlock;
	if (cq) {
		if (ctrl->cqs[qid].live)
			schedule_work(&ctrl->cqs[qid].work);
	} else if (ctrl->sqs[qid].live) {
		schedule_work(&ctrl->sqs[qid].work);
	}
out_unlock:
	mutex_unlock(&ctrl->lock);
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
	mutex_lock(&ctrl->state_lock);
	status = nvmet_mdev_create_sq_locked(ctrl, sqid, cqid, flags,
					     qsize + 1, prp1);
	mutex_unlock(&ctrl->state_lock);
	return status;
}

u16 nvmet_mdev_delete_sq(struct nvmet_ctrl *tctrl, u16 sqid)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	u16 status;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	mutex_lock(&ctrl->state_lock);
	status = nvmet_mdev_delete_sq_locked(ctrl, sqid);
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
	mutex_lock(&ctrl->state_lock);
	status = nvmet_mdev_create_cq_locked(ctrl, cqid, flags, qsize + 1,
					     prp1, irq_vector);
	mutex_unlock(&ctrl->state_lock);
	return status;
}

u16 nvmet_mdev_delete_cq(struct nvmet_ctrl *tctrl, u16 cqid)
{
	struct nvmet_mdev_ctrl *ctrl = rcu_access_pointer(tctrl->drvdata);
	u16 status;

	if (!ctrl)
		return NVME_SC_INTERNAL | NVME_STATUS_DNR;
	mutex_lock(&ctrl->state_lock);
	status = nvmet_mdev_delete_cq_locked(ctrl, cqid);
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
	ctrl->poll_busy_until = jiffies + NVMET_MDEV_POLL_BUSY;
	mutex_unlock(&ctrl->lock);
	mod_delayed_work(system_wq, &ctrl->poll_work, 0);
	return NVME_SC_SUCCESS;

map_error:
	ret = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
unlock:
	mutex_unlock(&ctrl->lock);
	return ret;
}
