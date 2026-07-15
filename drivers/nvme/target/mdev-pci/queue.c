// SPDX-License-Identifier: GPL-2.0

#include <linux/iommu.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "../pci-common.h"
#include "priv.h"

#define NVMET_MDEV_COPY_MAX_DATA	SZ_1M
#define NVMET_MDEV_PRP_ENTRIES		(SZ_4K / sizeof(__le64))

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
	size_t data_len;
	bool host_to_ctrl;
};

static void nvmet_mdev_free_iod(struct nvmet_mdev_iod *iod)
{
	if (iod->req.sg)
		nvmet_req_free_sgls(&iod->req);
	kfree(iod);
}

static int nvmet_mdev_copy_prp_segment(struct nvmet_mdev_iod *iod, u64 iova,
				       size_t offset, size_t length,
				       bool write_guest, void *bounce)
{
	struct nvmet_req *req = &iod->req;
	size_t copied;
	int ret;

	if (write_guest) {
		copied = sg_pcopy_to_buffer(req->sg, req->sg_cnt, bounce,
					    length, offset);
		if (copied != length)
			return -EFAULT;
		return vfio_dma_rw(&iod->ctrl->vdev, iova, bounce, length, true);
	}

	ret = vfio_dma_rw(&iod->ctrl->vdev, iova, bounce, length, false);
	if (ret)
		return ret;
	copied = sg_pcopy_from_buffer(req->sg, req->sg_cnt, bounce, length, offset);
	return copied == length ? 0 : -EFAULT;
}

static int nvmet_mdev_transfer_prps(struct nvmet_mdev_iod *iod,
				    bool write_guest)
{
	struct nvme_command *cmd = &iod->cmd;
	size_t remaining = iod->data_len;
	size_t offset = 0;
	__le64 *prps = NULL;
	void *bounce;
	u64 prp, list_iova;
	int ret = 0;

	if (!remaining)
		return 0;
	if (remaining > NVMET_MDEV_COPY_MAX_DATA)
		return -E2BIG;

	bounce = kmalloc(SZ_4K, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	prp = le64_to_cpu(cmd->common.dptr.prp1);
	if (!prp || !IS_ALIGNED(prp, sizeof(u32))) {
		ret = -EINVAL;
		goto out;
	}

	{
		size_t length = min_t(size_t, remaining,
					  SZ_4K - (prp & (SZ_4K - 1)));

		ret = nvmet_mdev_copy_prp_segment(iod, prp, offset, length,
						  write_guest, bounce);
		if (ret)
			goto out;
		offset += length;
		remaining -= length;
	}
	if (!remaining)
		goto out;

	prp = le64_to_cpu(cmd->common.dptr.prp2);
	if (!prp || !IS_ALIGNED(prp, SZ_4K)) {
		ret = -EINVAL;
		goto out;
	}
	if (remaining <= SZ_4K) {
		ret = nvmet_mdev_copy_prp_segment(iod, prp, offset, remaining,
						  write_guest, bounce);
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

		ret = vfio_dma_rw(&iod->ctrl->vdev, list_iova, prps, SZ_4K,
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
			ret = nvmet_mdev_copy_prp_segment(iod, prp, offset,
							  length, write_guest,
							  bounce);
			if (ret)
				goto out;
			offset += length;
			remaining -= length;
		}

		if (remaining && !chained) {
			ret = -EINVAL;
			goto out;
		}
	}

out:
	kfree(prps);
	kfree(bounce);
	return ret;
}

static void nvmet_mdev_complete_iod(struct nvmet_mdev_iod *iod)
{
	struct nvmet_mdev_cq *cq = iod->cq;
	struct nvmet_mdev_ctrl *ctrl = iod->ctrl;

	mutex_lock(&ctrl->lock);
	if (!ctrl->enabled || !cq->live) {
		mutex_unlock(&ctrl->lock);
		nvmet_mdev_free_iod(iod);
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

	if (!status && iod->data_len && !iod->host_to_ctrl) {
		ret = nvmet_mdev_transfer_prps(iod, true);
		if (ret) {
			status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
			req->cqe->status = cpu_to_le16(status << 1);
		}
	}

	if (req->sg)
		nvmet_req_free_sgls(req);
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
		nvmet_mdev_free_iod(iod);
		return;
	}
	mutex_unlock(&iod->ctrl->lock);

	if (!nvmet_req_init(req, &iod->sq->nvme_sq,
			    &nvmet_mdev_fabrics_ops))
		return;

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
		ret = nvmet_req_alloc_sgls(req);
		if (ret) {
			status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
			goto complete;
		}
		if (iod->host_to_ctrl) {
			ret = nvmet_mdev_transfer_prps(iod, false);
			if (ret) {
				status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
				goto complete;
			}
		}
	}

	req->execute(req);
	return;

complete:
	nvmet_req_complete(req, status);
}

static void nvmet_mdev_sq_work(struct work_struct *work)
{
	struct nvmet_mdev_sq *sq =
		container_of(work, struct nvmet_mdev_sq, work);
	struct nvmet_mdev_ctrl *ctrl = sq->ctrl;
	unsigned int db = NVME_REG_DBS + (sq->qid * 2 * sizeof(u32));
	unsigned int processed = 0;

	while (processed < sq->depth) {
		struct nvmet_mdev_iod *iod;
		u32 tail;

		mutex_lock(&ctrl->lock);
		if (!ctrl->enabled || !sq->live) {
			mutex_unlock(&ctrl->lock);
			return;
		}

		tail = get_unaligned_le32(ctrl->bar0 + db);
		if (tail >= sq->depth) {
			ctrl->enabled = false;
			nvmet_mdev_pci_set_fatal(ctrl);
			mutex_unlock(&ctrl->lock);
			return;
		}
		if (sq->head == tail) {
			mutex_unlock(&ctrl->lock);
			return;
		}

		iod = kzalloc_obj(*iod);
		if (!iod) {
			ctrl->enabled = false;
			nvmet_mdev_pci_set_fatal(ctrl);
			mutex_unlock(&ctrl->lock);
			return;
		}

		iod->ctrl = ctrl;
		iod->sq = sq;
		iod->cq = sq->cq;
		iod->req.cmd = &iod->cmd;
		iod->req.cqe = &iod->cqe;
		iod->req.port = ctrl->mport->port;
		INIT_LIST_HEAD(&iod->entry);
		INIT_WORK(&iod->submit_work, nvmet_mdev_submit_work);
		INIT_WORK(&iod->response_work, nvmet_mdev_response_work);
		dma_rmb();
		memcpy(&iod->cmd,
		       sq->entries + sq->head * sizeof(struct nvme_command),
		       sizeof(iod->cmd));
		nvmet_pci_advance_sq_head(&sq->head, sq->depth);
		mutex_unlock(&ctrl->lock);

		queue_work(sq->iod_wq, &iod->submit_work);
		processed++;
	}
}

static void nvmet_mdev_cq_work(struct work_struct *work)
{
	struct nvmet_mdev_cq *cq =
		container_of(work, struct nvmet_mdev_cq, work);
	struct nvmet_mdev_ctrl *ctrl = cq->ctrl;
	unsigned int db = NVME_REG_DBS + ((cq->qid * 2 + 1) * sizeof(u32));
	unsigned int completed = 0;

	for (;;) {
		struct nvmet_mdev_iod *iod;
		struct nvme_completion cqe;
		u16 status;
		u32 head;

		mutex_lock(&ctrl->lock);
		if (!ctrl->enabled || !cq->live) {
			mutex_unlock(&ctrl->lock);
			break;
		}

		head = get_unaligned_le32(ctrl->bar0 + db);
		if (head >= cq->depth) {
			ctrl->enabled = false;
			nvmet_mdev_pci_set_fatal(ctrl);
			mutex_unlock(&ctrl->lock);
			break;
		}
		cq->head = head;

		spin_lock(&cq->lock);
		if (list_empty(&cq->completions) ||
		    nvmet_pci_cq_full(cq->head, cq->tail, cq->depth)) {
			spin_unlock(&cq->lock);
			mutex_unlock(&ctrl->lock);
			break;
		}

		iod = list_first_entry(&cq->completions,
				       struct nvmet_mdev_iod, entry);
		list_del_init(&iod->entry);
		spin_unlock(&cq->lock);

		cqe = iod->cqe;
		status = le16_to_cpu(cqe.status) >> 1;
		nvmet_pci_prepare_cqe(&cqe, le16_to_cpu(cqe.sq_head),
				      le16_to_cpu(cqe.sq_id), cqe.command_id,
				      status, cq->phase);
		memcpy(cq->entries + cq->tail * sizeof(cqe), &cqe, sizeof(cqe));
		nvmet_pci_advance_cq_tail(&cq->tail, &cq->phase, cq->depth);
		dma_wmb();
		mutex_unlock(&ctrl->lock);

		nvmet_mdev_free_iod(iod);
		completed++;
	}

	if (completed && cq->irq_enabled)
		nvmet_mdev_signal_irq(ctrl, cq->vector);
}

void nvmet_mdev_queue_response(struct nvmet_req *req)
{
	struct nvmet_mdev_iod *iod =
		container_of(req, struct nvmet_mdev_iod, req);

	queue_work(iod->sq->iod_wq, &iod->response_work);
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
		nvmet_mdev_free_iod(iod);
	}
}

int nvmet_mdev_queue_init(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int qid;

	mutex_init(&ctrl->state_lock);
	ctrl->nr_queues = ctrl->tctrl->subsys->max_qid + 1;
	ctrl->sqs = kcalloc(ctrl->nr_queues, sizeof(*ctrl->sqs), GFP_KERNEL);
	if (!ctrl->sqs)
		return -ENOMEM;
	ctrl->cqs = kcalloc(ctrl->nr_queues, sizeof(*ctrl->cqs), GFP_KERNEL);
	if (!ctrl->cqs) {
		kfree(ctrl->sqs);
		ctrl->sqs = NULL;
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
	/* nvmet may complete inline; serialize submit and response ownership. */
	iod_wq = alloc_ordered_workqueue("nvmet_mdev_sq%u", WQ_MEM_RECLAIM,
					 qid);
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

static void __nvmet_mdev_disable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc)
{
	unsigned int qid;

	lockdep_assert_held(&ctrl->state_lock);
	mutex_lock(&ctrl->lock);
	ctrl->enabled = false;
	mutex_unlock(&ctrl->lock);

	for (qid = ctrl->nr_queues; qid-- > 0;)
		if (ctrl->sqs[qid].live)
			nvmet_mdev_delete_sq_locked(ctrl, qid);
	for (qid = ctrl->nr_queues; qid-- > 0;)
		if (ctrl->cqs[qid].live)
			nvmet_mdev_delete_cq_locked(ctrl, qid);

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
	__nvmet_mdev_disable_ctrl(ctrl, cc);
	mutex_unlock(&ctrl->state_lock);
}

void nvmet_mdev_queue_cleanup(struct nvmet_mdev_ctrl *ctrl)
{
	nvmet_mdev_disable_ctrl(ctrl, 0);
	kfree(ctrl->cqs);
	ctrl->cqs = NULL;
	kfree(ctrl->sqs);
	ctrl->sqs = NULL;
	ctrl->nr_queues = 0;
}

void nvmet_mdev_schedule_doorbell(struct nvmet_mdev_ctrl *ctrl, u16 qid,
				  bool cq)
{
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
	switch (feature) {
	case NVME_FEAT_ARBITRATION:
		((struct nvmet_feat_arbitration *)data)->ab = 0;
		return NVME_SC_SUCCESS;
	case NVME_FEAT_IRQ_COALESCE:
		memset(data, 0, sizeof(struct nvmet_feat_irq_coalesce));
		return NVME_SC_SUCCESS;
	case NVME_FEAT_IRQ_CONFIG:
		if (((struct nvmet_feat_irq_config *)data)->iv >=
		    NVMET_MDEV_PCI_MSIX_VECTORS)
			return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		((struct nvmet_feat_irq_config *)data)->cd = false;
		return NVME_SC_SUCCESS;
	default:
		return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
	}
}

u16 nvmet_mdev_set_feature(const struct nvmet_ctrl *tctrl, u8 feature,
			   void *data)
{
	if (feature == NVME_FEAT_ARBITRATION ||
	    feature == NVME_FEAT_IRQ_COALESCE)
		return NVME_SC_SUCCESS;
	if (feature == NVME_FEAT_IRQ_CONFIG &&
	    ((struct nvmet_feat_irq_config *)data)->iv <
	    NVMET_MDEV_PCI_MSIX_VECTORS)
		return NVME_SC_SUCCESS;
	return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
}
