// SPDX-License-Identifier: GPL-2.0

#include <linux/iommu.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "../pci-common.h"
#include "priv.h"

#define NVMET_MDEV_ADMIN_MAX_DATA	SZ_1M
#define NVMET_MDEV_PRP_ENTRIES		(SZ_4K / sizeof(__le64))

struct nvmet_mdev_iod {
	struct list_head entry;
	struct nvmet_mdev_ctrl *ctrl;
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
	if (remaining > NVMET_MDEV_ADMIN_MAX_DATA)
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

static void nvmet_mdev_submit_iod(struct nvmet_mdev_iod *iod)
{
	struct nvmet_req *req = &iod->req;
	u16 status;
	int ret;

	if (!nvmet_req_init(req, &iod->ctrl->admin_sq.nvme_sq,
			    &nvmet_mdev_fabrics_ops))
		return;

	iod->data_len = nvmet_req_transfer_len(req);
	iod->host_to_ctrl = nvme_is_write(&iod->cmd);
	if (iod->cmd.common.flags & NVME_CMD_SGL_ALL) {
		status = NVME_SC_SGL_INVALID_TYPE | NVME_STATUS_DNR;
		goto complete;
	}
	if (iod->data_len > NVMET_MDEV_ADMIN_MAX_DATA) {
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
	struct nvmet_mdev_admin_sq *sq =
		container_of(work, struct nvmet_mdev_admin_sq, work);
	struct nvmet_mdev_ctrl *ctrl = sq->ctrl;
	unsigned int processed = 0;

	while (processed < sq->depth) {
		struct nvmet_mdev_iod *iod;
		u32 tail;

		mutex_lock(&ctrl->lock);
		if (!ctrl->enabled || !sq->live) {
			mutex_unlock(&ctrl->lock);
			return;
		}

		tail = get_unaligned_le32(ctrl->bar0 + NVME_REG_DBS);
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
		iod->req.cmd = &iod->cmd;
		iod->req.cqe = &iod->cqe;
		iod->req.port = ctrl->mport->port;
		INIT_LIST_HEAD(&iod->entry);
		dma_rmb();
		memcpy(&iod->cmd,
		       sq->entries + sq->head * sizeof(struct nvme_command),
		       sizeof(iod->cmd));
		nvmet_pci_advance_sq_head(&sq->head, sq->depth);
		mutex_unlock(&ctrl->lock);

		nvmet_mdev_submit_iod(iod);
		processed++;
	}
}

static void nvmet_mdev_cq_work(struct work_struct *work)
{
	struct nvmet_mdev_admin_cq *cq =
		container_of(work, struct nvmet_mdev_admin_cq, work);
	struct nvmet_mdev_ctrl *ctrl = cq->ctrl;
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

		head = get_unaligned_le32(ctrl->bar0 + NVME_REG_DBS + sizeof(u32));
		if (head >= cq->depth) {
			ctrl->enabled = false;
			nvmet_mdev_pci_set_fatal(ctrl);
			mutex_unlock(&ctrl->lock);
			break;
		}
		cq->head = head;

		spin_lock(&ctrl->completion_lock);
		if (list_empty(&ctrl->completions) ||
		    nvmet_pci_cq_full(cq->head, cq->tail, cq->depth)) {
			spin_unlock(&ctrl->completion_lock);
			mutex_unlock(&ctrl->lock);
			break;
		}

		iod = list_first_entry(&ctrl->completions,
				       struct nvmet_mdev_iod, entry);
		list_del_init(&iod->entry);
		spin_unlock(&ctrl->completion_lock);

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

	if (completed)
		nvmet_mdev_signal_irq(ctrl, 0);
}

void nvmet_mdev_queue_response(struct nvmet_req *req)
{
	struct nvmet_mdev_iod *iod =
		container_of(req, struct nvmet_mdev_iod, req);
	struct nvmet_mdev_ctrl *ctrl = iod->ctrl;
	u16 status = le16_to_cpu(req->cqe->status) >> 1;
	bool enabled;
	int ret;

	mutex_lock(&ctrl->lock);
	enabled = ctrl->enabled && ctrl->admin_cq.live;
	mutex_unlock(&ctrl->lock);
	if (enabled && !status && iod->data_len && !iod->host_to_ctrl) {
		ret = nvmet_mdev_transfer_prps(iod, true);
		if (ret) {
			status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
			req->cqe->status = cpu_to_le16(status << 1);
		}
	}
	if (req->sg)
		nvmet_req_free_sgls(req);

	mutex_lock(&ctrl->lock);
	if (!ctrl->enabled || !ctrl->admin_cq.live) {
		mutex_unlock(&ctrl->lock);
		kfree(iod);
		return;
	}

	spin_lock(&ctrl->completion_lock);
	list_add_tail(&iod->entry, &ctrl->completions);
	spin_unlock(&ctrl->completion_lock);
	schedule_work(&ctrl->admin_cq.work);
	mutex_unlock(&ctrl->lock);
}

static void nvmet_mdev_drain_completions(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_iod *iod, *tmp;
	LIST_HEAD(completions);

	spin_lock(&ctrl->completion_lock);
	list_splice_init(&ctrl->completions, &completions);
	spin_unlock(&ctrl->completion_lock);

	list_for_each_entry_safe(iod, tmp, &completions, entry) {
		list_del_init(&iod->entry);
		nvmet_mdev_free_iod(iod);
	}
}

void nvmet_mdev_queue_init(struct nvmet_mdev_ctrl *ctrl)
{
	mutex_init(&ctrl->state_lock);
	spin_lock_init(&ctrl->completion_lock);
	INIT_LIST_HEAD(&ctrl->completions);
	ctrl->admin_sq.ctrl = ctrl;
	ctrl->admin_cq.ctrl = ctrl;
	INIT_WORK(&ctrl->admin_sq.work, nvmet_mdev_sq_work);
	INIT_WORK(&ctrl->admin_cq.work, nvmet_mdev_cq_work);
}

static void __nvmet_mdev_disable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc)
{
	struct nvmet_mdev_admin_sq *sq = &ctrl->admin_sq;
	struct nvmet_mdev_admin_cq *cq = &ctrl->admin_cq;

	mutex_lock(&ctrl->lock);
	ctrl->enabled = false;
	mutex_unlock(&ctrl->lock);

	cancel_work_sync(&sq->work);
	if (sq->live) {
		nvmet_sq_destroy(&sq->nvme_sq);
		sq->live = false;
	}
	cancel_work_sync(&cq->work);
	nvmet_mdev_drain_completions(ctrl);
	if (cq->live) {
		nvmet_cq_put(&cq->nvme_cq);
		cq->live = false;
	}

	mutex_lock(&ctrl->lock);
	if (sq->mapping) {
		nvmet_mdev_unmap_guest(ctrl, sq->mapping);
		sq->mapping = NULL;
	}
	if (cq->mapping) {
		nvmet_mdev_unmap_guest(ctrl, cq->mapping);
		cq->mapping = NULL;
	}
	sq->entries = NULL;
	cq->entries = NULL;
	sq->depth = 0;
	cq->depth = 0;
	mutex_unlock(&ctrl->lock);

	if (ctrl->tctrl) {
		nvmet_update_cc(ctrl->tctrl, cc);
		mutex_lock(&ctrl->lock);
		put_unaligned_le32(ctrl->tctrl->csts,
				   ctrl->bar0 + NVME_REG_CSTS);
		mutex_unlock(&ctrl->lock);
	}
}

int nvmet_mdev_enable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc)
{
	struct nvmet_mdev_admin_sq *sq = &ctrl->admin_sq;
	struct nvmet_mdev_admin_cq *cq = &ctrl->admin_cq;
	struct nvmet_pci_admin_config admin;
	u16 status;
	u64 cap, asq, acq;
	u32 aqa;
	int ret;

	mutex_lock(&ctrl->state_lock);
	mutex_lock(&ctrl->lock);
	if (ctrl->enabled) {
		mutex_unlock(&ctrl->lock);
		mutex_unlock(&ctrl->state_lock);
		return 0;
	}

	cap = get_unaligned_le64(ctrl->bar0 + NVME_REG_CAP);
	aqa = get_unaligned_le32(ctrl->bar0 + NVME_REG_AQA);
	asq = get_unaligned_le64(ctrl->bar0 + NVME_REG_ASQ);
	acq = get_unaligned_le64(ctrl->bar0 + NVME_REG_ACQ);
	ret = nvmet_pci_parse_admin_config(cap, cc, aqa, asq, acq, &admin);
	if (ret)
		goto fail_unlock;

	ret = nvmet_mdev_map_guest(ctrl, admin.acq, admin.cq_size,
				   IOMMU_WRITE, &cq->mapping);
	if (ret)
		goto fail_unlock;
	cq->entries = nvmet_mdev_mapping_addr(cq->mapping);
	cq->depth = admin.cq_depth;
	cq->head = 0;
	cq->tail = 0;
	cq->phase = 1;
	status = nvmet_cq_create(ctrl->tctrl, &cq->nvme_cq, 0, cq->depth);
	if (status != NVME_SC_SUCCESS) {
		ret = -EINVAL;
		goto fail_unlock;
	}
	cq->live = true;

	ret = nvmet_mdev_map_guest(ctrl, admin.asq, admin.sq_size,
				   IOMMU_READ, &sq->mapping);
	if (ret)
		goto fail_unlock;
	sq->entries = nvmet_mdev_mapping_addr(sq->mapping);
	sq->depth = admin.sq_depth;
	sq->head = 0;
	status = nvmet_sq_create(ctrl->tctrl, &sq->nvme_sq, &cq->nvme_cq,
				 0, sq->depth);
	if (status != NVME_SC_SUCCESS) {
		ret = -EINVAL;
		goto fail_unlock;
	}
	sq->live = true;
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
		__nvmet_mdev_disable_ctrl(ctrl, 0);
	mutex_unlock(&ctrl->state_lock);
	return ret;

fail_unlock:
	nvmet_mdev_pci_set_fatal(ctrl);
	mutex_unlock(&ctrl->lock);
	__nvmet_mdev_disable_ctrl(ctrl, 0);
	mutex_lock(&ctrl->lock);
	nvmet_mdev_pci_set_fatal(ctrl);
	mutex_unlock(&ctrl->lock);
	mutex_unlock(&ctrl->state_lock);
	return ret;
}

void nvmet_mdev_disable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc)
{
	mutex_lock(&ctrl->state_lock);
	__nvmet_mdev_disable_ctrl(ctrl, cc);
	mutex_unlock(&ctrl->state_lock);
}

void nvmet_mdev_schedule_sq(struct nvmet_mdev_ctrl *ctrl)
{
	schedule_work(&ctrl->admin_sq.work);
}

void nvmet_mdev_schedule_cq(struct nvmet_mdev_ctrl *ctrl)
{
	schedule_work(&ctrl->admin_cq.work);
}

u8 nvmet_mdev_get_mdts(const struct nvmet_ctrl *tctrl)
{
	return ilog2(NVMET_MDEV_ADMIN_MAX_DATA) - 12;
}

u16 nvmet_mdev_create_sq(struct nvmet_ctrl *tctrl, u16 sqid, u16 cqid,
			 u16 flags, u16 qsize, u64 prp1)
{
	return NVME_SC_INVALID_QUEUE | NVME_STATUS_DNR;
}

u16 nvmet_mdev_delete_sq(struct nvmet_ctrl *tctrl, u16 sqid)
{
	return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
}

u16 nvmet_mdev_create_cq(struct nvmet_ctrl *tctrl, u16 cqid, u16 flags,
			 u16 qsize, u64 prp1, u16 irq_vector)
{
	return NVME_SC_INVALID_QUEUE | NVME_STATUS_DNR;
}

u16 nvmet_mdev_delete_cq(struct nvmet_ctrl *tctrl, u16 cqid)
{
	return NVME_SC_QID_INVALID | NVME_STATUS_DNR;
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
