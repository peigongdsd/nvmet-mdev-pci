/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NVMET_PCI_COMMON_H
#define _NVMET_PCI_COMMON_H

#include <linux/nvme.h>
#include <linux/types.h>

struct nvmet_pci_admin_config {
	u64 asq;
	u64 acq;
	u16 sq_depth;
	u16 cq_depth;
	size_t sq_size;
	size_t cq_size;
};

bool nvmet_pci_cq_full(u16 head, u16 tail, u16 depth);
void nvmet_pci_advance_sq_head(u16 *head, u16 depth);
void nvmet_pci_advance_cq_tail(u16 *tail, u16 *phase, u16 depth);
void nvmet_pci_prepare_cqe(struct nvme_completion *cqe, u16 sq_head,
			   u16 sqid, __u16 command_id, u16 status, u16 phase);
int nvmet_pci_parse_admin_config(u64 cap, u32 cc, u32 aqa, u64 asq, u64 acq,
				 struct nvmet_pci_admin_config *config);

#endif /* _NVMET_PCI_COMMON_H */
