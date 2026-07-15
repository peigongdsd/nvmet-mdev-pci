/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NVMET_MDEV_PCI_PRIV_H
#define _NVMET_MDEV_PCI_PRIV_H

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/list.h>
#include <linux/mdev.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/vfio.h>
#include <linux/workqueue.h>

#include "../nvmet.h"

#define NVMET_MDEV_PCI_CONFIG_SIZE	SZ_4K
#define NVMET_MDEV_PCI_BAR0_SIZE	SZ_16K
#define NVMET_MDEV_PCI_MSIX_VECTORS	16
#define NVMET_MDEV_PCI_MSIX_CAP		0x40
#define NVMET_MDEV_PCI_MSIX_TABLE	0x2000
#define NVMET_MDEV_PCI_MSIX_PBA		0x3000

#define NVMET_MDEV_VFIO_OFFSET_SHIFT	40
#define NVMET_MDEV_VFIO_OFFSET_MASK	\
	((1ULL << NVMET_MDEV_VFIO_OFFSET_SHIFT) - 1)
#define NVMET_MDEV_VFIO_OFFSET_TO_INDEX(offset) \
	((offset) >> NVMET_MDEV_VFIO_OFFSET_SHIFT)
#define NVMET_MDEV_VFIO_INDEX_TO_OFFSET(index) \
	((u64)(index) << NVMET_MDEV_VFIO_OFFSET_SHIFT)

struct nvmet_mdev_port {
	struct nvmet_port *port;
	struct device dev;
	struct mdev_parent parent;
	struct mdev_type type;
	struct mdev_type *typep;
};

struct nvmet_mdev_mapping {
	struct list_head entry;
	u64 iova;
	u64 length;
	u64 data_iova;
	size_t data_length;
	unsigned int npages;
	struct page **pages;
	void *vaddr;
};

struct nvmet_mdev_ctrl;

struct nvmet_mdev_admin_sq {
	struct nvmet_mdev_ctrl *ctrl;
	struct nvmet_sq nvme_sq;
	struct nvmet_mdev_mapping *mapping;
	struct work_struct work;
	u8 *entries;
	u16 depth;
	u16 head;
	bool live;
};

struct nvmet_mdev_admin_cq {
	struct nvmet_mdev_ctrl *ctrl;
	struct nvmet_cq nvme_cq;
	struct nvmet_mdev_mapping *mapping;
	struct work_struct work;
	u8 *entries;
	u16 depth;
	u16 head;
	u16 tail;
	u16 phase;
	bool live;
};

struct nvmet_mdev_ctrl {
	struct vfio_device vdev;
	struct mdev_device *mdev;
	struct nvmet_mdev_port *mport;
	/* Serializes controller enable, disable and queue teardown. */
	struct mutex state_lock;
	/* Protects PCI config, BAR0 and interrupt eventfd state. */
	struct mutex lock;
	/* Protects the list of completed requests awaiting a free CQ slot. */
	spinlock_t completion_lock;
	u8 *config;
	u8 *bar0;
	struct eventfd_ctx *irq_ctx[NVMET_MDEV_PCI_MSIX_VECTORS];
	struct nvmet_ctrl *tctrl;
	struct list_head mappings;
	struct list_head completions;
	struct nvmet_mdev_admin_sq admin_sq;
	struct nvmet_mdev_admin_cq admin_cq;
	bool enabled;
};

extern const struct nvmet_fabrics_ops nvmet_mdev_fabrics_ops;

int nvmet_mdev_vfio_init(void);
void nvmet_mdev_vfio_exit(void);
const struct class *nvmet_mdev_class(void);
struct mdev_driver *nvmet_mdev_driver(void);

int nvmet_mdev_pci_init(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_pci_cleanup(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_pci_reset(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_pci_bind_ctrl(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_pci_set_fatal(struct nvmet_mdev_ctrl *ctrl);
ssize_t nvmet_mdev_pci_read(struct nvmet_mdev_ctrl *ctrl, char __user *buf,
			    size_t count, loff_t *ppos);
ssize_t nvmet_mdev_pci_write(struct nvmet_mdev_ctrl *ctrl,
			     const char __user *buf, size_t count,
			     loff_t *ppos);

void nvmet_mdev_irq_cleanup(struct nvmet_mdev_ctrl *ctrl);
int nvmet_mdev_irq_info(struct vfio_irq_info *info);
int nvmet_mdev_set_irqs(struct nvmet_mdev_ctrl *ctrl,
			struct vfio_irq_set *hdr, void *data);
void nvmet_mdev_signal_irq(struct nvmet_mdev_ctrl *ctrl, unsigned int vector);

int nvmet_mdev_map_guest(struct nvmet_mdev_ctrl *ctrl, u64 iova,
			 size_t length, int prot,
			 struct nvmet_mdev_mapping **mappingp);
void nvmet_mdev_unmap_guest(struct nvmet_mdev_ctrl *ctrl,
			    struct nvmet_mdev_mapping *mapping);
void nvmet_mdev_unmap_all(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_dma_unmap(struct nvmet_mdev_ctrl *ctrl, u64 iova, u64 length);
void *nvmet_mdev_mapping_addr(const struct nvmet_mdev_mapping *mapping);

void nvmet_mdev_queue_init(struct nvmet_mdev_ctrl *ctrl);
int nvmet_mdev_enable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc);
void nvmet_mdev_disable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc);
void nvmet_mdev_schedule_sq(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_schedule_cq(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_queue_response(struct nvmet_req *req);
u8 nvmet_mdev_get_mdts(const struct nvmet_ctrl *tctrl);
u16 nvmet_mdev_create_sq(struct nvmet_ctrl *tctrl, u16 sqid, u16 cqid,
			 u16 flags, u16 qsize, u64 prp1);
u16 nvmet_mdev_delete_sq(struct nvmet_ctrl *tctrl, u16 sqid);
u16 nvmet_mdev_create_cq(struct nvmet_ctrl *tctrl, u16 cqid, u16 flags,
			 u16 qsize, u64 prp1, u16 irq_vector);
u16 nvmet_mdev_delete_cq(struct nvmet_ctrl *tctrl, u16 cqid);
u16 nvmet_mdev_get_feature(const struct nvmet_ctrl *tctrl, u8 feature,
			   void *data);
u16 nvmet_mdev_set_feature(const struct nvmet_ctrl *tctrl, u8 feature,
			   void *data);

int nvmet_mdev_ctrl_init(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_ctrl_cleanup(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_delete_ctrl(struct nvmet_ctrl *tctrl);
u16 nvmet_mdev_get_max_queue_size(const struct nvmet_ctrl *tctrl);

#endif /* _NVMET_MDEV_PCI_PRIV_H */
