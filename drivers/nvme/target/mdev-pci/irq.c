// SPDX-License-Identifier: GPL-2.0

#include <linux/eventfd.h>
#include <linux/pci.h>
#include <linux/unaligned.h>

#include "priv.h"

static void nvmet_mdev_put_irq_ctx(struct nvmet_mdev_ctrl *ctrl,
				   unsigned int vector)
{
	if (!ctrl->irq_ctx[vector])
		return;

	eventfd_ctx_put(ctrl->irq_ctx[vector]);
	ctrl->irq_ctx[vector] = NULL;
}

void nvmet_mdev_irq_cleanup(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int vector;

	for (vector = 0; vector < NVMET_MDEV_PCI_MSIX_VECTORS; vector++)
		nvmet_mdev_put_irq_ctx(ctrl, vector);
}

int nvmet_mdev_irq_info(struct vfio_irq_info *info)
{
	if (info->index >= VFIO_PCI_NUM_IRQS)
		return -EINVAL;

	info->flags = 0;
	info->count = 0;
	if (info->index == VFIO_PCI_MSIX_IRQ_INDEX) {
		info->flags = VFIO_IRQ_INFO_EVENTFD | VFIO_IRQ_INFO_NORESIZE;
		info->count = NVMET_MDEV_PCI_MSIX_VECTORS;
	}

	return 0;
}

static int nvmet_mdev_set_eventfds(struct nvmet_mdev_ctrl *ctrl,
				   unsigned int start, unsigned int count,
				   const int *fds)
{
	struct eventfd_ctx *new_ctx[NVMET_MDEV_PCI_MSIX_VECTORS] = {};
	unsigned int i;
	int ret = 0;

	for (i = 0; i < count; i++) {
		if (fds[i] < 0)
			continue;
		new_ctx[i] = eventfd_ctx_fdget(fds[i]);
		if (IS_ERR(new_ctx[i])) {
			ret = PTR_ERR(new_ctx[i]);
			new_ctx[i] = NULL;
			goto out_put;
		}
	}

	mutex_lock(&ctrl->lock);
	for (i = 0; i < count; i++) {
		nvmet_mdev_put_irq_ctx(ctrl, start + i);
		ctrl->irq_ctx[start + i] = new_ctx[i];
		new_ctx[i] = NULL;
	}
	mutex_unlock(&ctrl->lock);
	nvmet_mdev_update_pending_irqs(ctrl);

out_put:
	for (i = 0; i < count; i++) {
		if (new_ctx[i])
			eventfd_ctx_put(new_ctx[i]);
	}
	return ret;
}

static void nvmet_mdev_disable_irqs(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned int vector;

	mutex_lock(&ctrl->lock);
	for (vector = 0; vector < NVMET_MDEV_PCI_MSIX_VECTORS; vector++)
		nvmet_mdev_put_irq_ctx(ctrl, vector);
	mutex_unlock(&ctrl->lock);
}

static void nvmet_mdev_trigger_irqs(struct nvmet_mdev_ctrl *ctrl,
				    unsigned int start, unsigned int count,
				    const u8 *trigger)
{
	unsigned int i;

	mutex_lock(&ctrl->lock);
	for (i = 0; i < count; i++) {
		if ((!trigger || trigger[i]) && ctrl->irq_ctx[start + i])
			eventfd_signal(ctrl->irq_ctx[start + i]);
	}
	mutex_unlock(&ctrl->lock);
}

int nvmet_mdev_set_irqs(struct nvmet_mdev_ctrl *ctrl,
			struct vfio_irq_set *hdr, void *data)
{
	if (hdr->index != VFIO_PCI_MSIX_IRQ_INDEX)
		return -EINVAL;

	if ((hdr->flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) !=
	    VFIO_IRQ_SET_ACTION_TRIGGER)
		return -ENOTTY;

	if (!hdr->count) {
		if (!(hdr->flags & VFIO_IRQ_SET_DATA_NONE) || hdr->start)
			return -EINVAL;
		nvmet_mdev_disable_irqs(ctrl);
		return 0;
	}

	if (hdr->start >= NVMET_MDEV_PCI_MSIX_VECTORS ||
	    hdr->count > NVMET_MDEV_PCI_MSIX_VECTORS - hdr->start)
		return -EINVAL;

	if (hdr->flags & VFIO_IRQ_SET_DATA_EVENTFD)
		return nvmet_mdev_set_eventfds(ctrl, hdr->start, hdr->count, data);
	if (hdr->flags & VFIO_IRQ_SET_DATA_BOOL) {
		nvmet_mdev_trigger_irqs(ctrl, hdr->start, hdr->count, data);
		return 0;
	}
	if (hdr->flags & VFIO_IRQ_SET_DATA_NONE) {
		nvmet_mdev_trigger_irqs(ctrl, hdr->start, hdr->count, NULL);
		return 0;
	}

	return -EINVAL;
}

void nvmet_mdev_signal_irq(struct nvmet_mdev_ctrl *ctrl, unsigned int vector)
{
	u16 flags;
	u32 vector_ctrl;

	if (WARN_ON_ONCE(vector >= NVMET_MDEV_PCI_MSIX_VECTORS))
		return;

	mutex_lock(&ctrl->lock);
	flags = get_unaligned_le16(ctrl->config + NVMET_MDEV_PCI_MSIX_CAP +
				   PCI_MSIX_FLAGS);
	vector_ctrl = get_unaligned_le32(ctrl->bar0 +
			NVMET_MDEV_PCI_MSIX_TABLE +
			vector * PCI_MSIX_ENTRY_SIZE + PCI_MSIX_ENTRY_VECTOR_CTRL);

	if ((flags & PCI_MSIX_FLAGS_ENABLE) &&
	    !(flags & PCI_MSIX_FLAGS_MASKALL) &&
	    !(vector_ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT) &&
	    ctrl->irq_ctx[vector])
		eventfd_signal(ctrl->irq_ctx[vector]);
	else
		set_bit(vector, (unsigned long *)(ctrl->bar0 +
			NVMET_MDEV_PCI_MSIX_PBA));
	mutex_unlock(&ctrl->lock);
}

void nvmet_mdev_update_pending_irqs(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned long *pba = (unsigned long *)(ctrl->bar0 +
					       NVMET_MDEV_PCI_MSIX_PBA);
	u16 flags;
	unsigned int vector;

	mutex_lock(&ctrl->lock);
	flags = get_unaligned_le16(ctrl->config + NVMET_MDEV_PCI_MSIX_CAP +
				   PCI_MSIX_FLAGS);
	if (!(flags & PCI_MSIX_FLAGS_ENABLE) ||
	    (flags & PCI_MSIX_FLAGS_MASKALL))
		goto out_unlock;

	for (vector = 0; vector < NVMET_MDEV_PCI_MSIX_VECTORS; vector++) {
		u32 vector_ctrl;

		if (!test_bit(vector, pba) || !ctrl->irq_ctx[vector])
			continue;
		vector_ctrl = get_unaligned_le32(ctrl->bar0 +
			NVMET_MDEV_PCI_MSIX_TABLE +
			vector * PCI_MSIX_ENTRY_SIZE + PCI_MSIX_ENTRY_VECTOR_CTRL);
		if (vector_ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT)
			continue;
		clear_bit(vector, pba);
		eventfd_signal(ctrl->irq_ctx[vector]);
	}

out_unlock:
	mutex_unlock(&ctrl->lock);
}
