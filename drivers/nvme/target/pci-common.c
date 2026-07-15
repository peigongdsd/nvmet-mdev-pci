// SPDX-License-Identifier: GPL-2.0
/* Common helpers for NVMe PCI target transports. */

#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sizes.h>

#include "pci-common.h"

bool nvmet_pci_cq_full(u16 head, u16 tail, u16 depth)
{
	u16 next = tail + 1;

	if (next == depth)
		next = 0;

	return head == next;
}
EXPORT_SYMBOL_GPL(nvmet_pci_cq_full);

void nvmet_pci_advance_sq_head(u16 *head, u16 depth)
{
	if (++(*head) == depth)
		*head = 0;
}
EXPORT_SYMBOL_GPL(nvmet_pci_advance_sq_head);

void nvmet_pci_advance_cq_tail(u16 *tail, u16 *phase, u16 depth)
{
	if (++(*tail) == depth) {
		*tail = 0;
		*phase ^= 1;
	}
}
EXPORT_SYMBOL_GPL(nvmet_pci_advance_cq_tail);

void nvmet_pci_prepare_cqe(struct nvme_completion *cqe, u16 sq_head,
			   u16 sqid, __u16 command_id, u16 status, u16 phase)
{
	cqe->sq_head = cpu_to_le16(sq_head);
	cqe->sq_id = cpu_to_le16(sqid);
	cqe->command_id = command_id;
	cqe->status = cpu_to_le16((status << 1) | phase);
}
EXPORT_SYMBOL_GPL(nvmet_pci_prepare_cqe);

int nvmet_pci_parse_admin_config(u64 cap, u32 cc, u32 aqa, u64 asq, u64 acq,
				 struct nvmet_pci_admin_config *config)
{
	u32 css = cc & NVME_CC_CSS_MASK;
	u32 max_depth = (u32)NVME_CAP_MQES(cap) + 1;
	u32 sq_depth = (aqa & GENMASK(11, 0)) + 1;
	u32 cq_depth = ((aqa >> 16) & GENMASK(11, 0)) + 1;
	size_t sq_size, cq_size;
	u64 sq_end, cq_end;

	if (!config)
		return -EINVAL;
	if ((cc & NVME_CC_MPS_MASK) || (cc & NVME_CC_AMS_MASK))
		return -EINVAL;
	if (css != NVME_CC_CSS_NVM && css != NVME_CC_CSS_CSI)
		return -EINVAL;
	if ((cc & NVME_CC_IOSQES_MASK) != NVME_CC_IOSQES ||
	    (cc & NVME_CC_IOCQES_MASK) != NVME_CC_IOCQES)
		return -EINVAL;
	if (aqa & (GENMASK(31, 28) | GENMASK(15, 12)))
		return -EINVAL;
	if (sq_depth < 2 || cq_depth < 2 ||
	    sq_depth > max_depth || cq_depth > max_depth)
		return -EINVAL;
	if (!asq || !acq || !IS_ALIGNED(asq, SZ_4K) ||
	    !IS_ALIGNED(acq, SZ_4K))
		return -EINVAL;
	sq_size = (size_t)sq_depth << NVME_ADM_SQES;
	cq_size = (size_t)cq_depth * sizeof(struct nvme_completion);
	if (check_add_overflow(asq, sq_size, &sq_end) ||
	    check_add_overflow(acq, cq_size, &cq_end) ||
	    (asq < cq_end && acq < sq_end))
		return -EINVAL;

	config->asq = asq;
	config->acq = acq;
	config->sq_depth = sq_depth;
	config->cq_depth = cq_depth;
	config->sq_size = sq_size;
	config->cq_size = cq_size;
	return 0;
}
EXPORT_SYMBOL_GPL(nvmet_pci_parse_admin_config);
