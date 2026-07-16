// SPDX-License-Identifier: GPL-2.0

#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include "priv.h"

#define NVMET_MDEV_PCI_DEVICE_ID	0x0010
#define NVMET_MDEV_PCI_REVISION		0x01

static bool nvmet_mdev_config_byte_writable(unsigned int offset)
{
	if (offset >= PCI_COMMAND && offset < PCI_COMMAND + sizeof(u16))
		return true;
	if (offset >= PCI_BASE_ADDRESS_0 &&
	    offset < PCI_BASE_ADDRESS_1 + sizeof(u32))
		return true;
	if (offset == PCI_INTERRUPT_LINE)
		return true;
	if (offset == NVMET_MDEV_PCI_MSIX_CAP + PCI_MSIX_FLAGS + 1)
		return true;

	return false;
}

static bool nvmet_mdev_bar0_byte_writable(unsigned int offset)
{
	if (offset >= NVME_REG_INTMS && offset < NVME_REG_CSTS)
		return true;
	if (offset >= NVME_REG_NSSR && offset < NVME_REG_CMBLOC)
		return true;
	if (offset >= NVME_REG_DBS && offset < NVMET_MDEV_PCI_MSIX_TABLE)
		return true;
	if (offset >= NVMET_MDEV_PCI_MSIX_TABLE &&
	    offset < NVMET_MDEV_PCI_MSIX_TABLE +
		     NVMET_MDEV_PCI_MSIX_VECTORS * PCI_MSIX_ENTRY_SIZE)
		return true;

	return false;
}

static void nvmet_mdev_init_config(struct nvmet_mdev_ctrl *ctrl)
{
	u8 *config = ctrl->config;
	u8 *msix = config + NVMET_MDEV_PCI_MSIX_CAP;
	u16 msix_flags = NVMET_MDEV_PCI_MSIX_VECTORS - 1;

	memset(config, 0, NVMET_MDEV_PCI_CONFIG_SIZE);
	put_unaligned_le16(PCI_VENDOR_ID_REDHAT, config + PCI_VENDOR_ID);
	put_unaligned_le16(NVMET_MDEV_PCI_DEVICE_ID, config + PCI_DEVICE_ID);
	put_unaligned_le16(PCI_STATUS_CAP_LIST, config + PCI_STATUS);
	config[PCI_REVISION_ID] = NVMET_MDEV_PCI_REVISION;
	config[PCI_CLASS_PROG] = PCI_CLASS_STORAGE_EXPRESS & 0xff;
	config[PCI_CLASS_DEVICE] = (PCI_CLASS_STORAGE_EXPRESS >> 8) & 0xff;
	config[PCI_CLASS_DEVICE + 1] = (PCI_CLASS_STORAGE_EXPRESS >> 16) & 0xff;
	config[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL;
	put_unaligned_le32(PCI_BASE_ADDRESS_MEM_TYPE_64,
			   config + PCI_BASE_ADDRESS_0);
	put_unaligned_le16(PCI_VENDOR_ID_REDHAT,
			   config + PCI_SUBSYSTEM_VENDOR_ID);
	put_unaligned_le16(NVMET_MDEV_PCI_DEVICE_ID,
			   config + PCI_SUBSYSTEM_ID);
	config[PCI_CAPABILITY_LIST] = NVMET_MDEV_PCI_MSIX_CAP;

	msix[PCI_CAP_LIST_ID] = PCI_CAP_ID_MSIX;
	msix[PCI_CAP_LIST_NEXT] = 0;
	put_unaligned_le16(msix_flags, msix + PCI_MSIX_FLAGS);
	put_unaligned_le32(NVMET_MDEV_PCI_MSIX_TABLE, msix + PCI_MSIX_TABLE);
	put_unaligned_le32(NVMET_MDEV_PCI_MSIX_PBA, msix + PCI_MSIX_PBA);
}

static void nvmet_mdev_init_bar0(struct nvmet_mdev_ctrl *ctrl)
{
	u64 cap;

	memset(ctrl->bar0, 0, NVMET_MDEV_PCI_BAR0_SIZE);

	cap = NVMET_MAX_QUEUE_SIZE - 1;
	cap |= BIT_ULL(16); /* Contiguous Queues Required. */
	cap |= 10ULL << 24; /* 5 second ready timeout. */
	cap |= (u64)NVME_CAP_CSS_NVM << 37;
	put_unaligned_le64(cap, ctrl->bar0 + NVME_REG_CAP);
	put_unaligned_le32(NVMET_DEFAULT_VS, ctrl->bar0 + NVME_REG_VS);
}

void nvmet_mdev_pci_bind_ctrl(struct nvmet_mdev_ctrl *ctrl)
{
	u64 cap = ctrl->tctrl->cap;

	lockdep_assert_held(&ctrl->lock);

	cap |= BIT_ULL(16);
	cap &= ~GENMASK_ULL(35, 32);
	cap &= ~BIT_ULL(36);
	cap &= ~BIT_ULL(45);
	cap &= ~BIT_ULL(56);
	cap &= ~BIT_ULL(57);
	put_unaligned_le64(cap, ctrl->bar0 + NVME_REG_CAP);
	put_unaligned_le32(ctrl->tctrl->subsys->ver,
			   ctrl->bar0 + NVME_REG_VS);
	put_unaligned_le32(0, ctrl->bar0 + NVME_REG_CSTS);
}

void nvmet_mdev_pci_set_fatal(struct nvmet_mdev_ctrl *ctrl)
{
	lockdep_assert_held(&ctrl->lock);
	put_unaligned_le32(NVME_CSTS_CFS, ctrl->bar0 + NVME_REG_CSTS);
}

void nvmet_mdev_pci_reset(struct nvmet_mdev_ctrl *ctrl)
{
	unsigned long flags;

	lockdep_assert_held(&ctrl->lock);
	nvmet_mdev_unmap_all(ctrl);
	spin_lock_irqsave(&ctrl->irq_state_lock, flags);
	nvmet_mdev_init_config(ctrl);
	nvmet_mdev_init_bar0(ctrl);
	spin_unlock_irqrestore(&ctrl->irq_state_lock, flags);
	if (ctrl->tctrl)
		nvmet_mdev_pci_bind_ctrl(ctrl);
}

int nvmet_mdev_pci_init(struct nvmet_mdev_ctrl *ctrl)
{
	ctrl->config = kzalloc(NVMET_MDEV_PCI_CONFIG_SIZE, GFP_KERNEL);
	if (!ctrl->config)
		return -ENOMEM;

	ctrl->bar0 = kzalloc(NVMET_MDEV_PCI_BAR0_SIZE, GFP_KERNEL);
	if (!ctrl->bar0) {
		kfree(ctrl->config);
		ctrl->config = NULL;
		return -ENOMEM;
	}

	nvmet_mdev_pci_reset(ctrl);
	return 0;
}

void nvmet_mdev_pci_cleanup(struct nvmet_mdev_ctrl *ctrl)
{
	kfree(ctrl->bar0);
	kfree(ctrl->config);
	ctrl->bar0 = NULL;
	ctrl->config = NULL;
}

static void nvmet_mdev_config_write(struct nvmet_mdev_ctrl *ctrl,
				    unsigned int offset, const u8 *buf,
				    size_t count)
{
	u8 *config = ctrl->config;
	size_t i;

	if (offset == PCI_BASE_ADDRESS_0 && count == sizeof(u32)) {
		u32 value = get_unaligned_le32(buf);

		value &= ~(NVMET_MDEV_PCI_BAR0_SIZE - 1);
		value |= PCI_BASE_ADDRESS_MEM_TYPE_64;
		put_unaligned_le32(value, config + offset);
		return;
	}

	if (offset == PCI_BASE_ADDRESS_1 && count == sizeof(u32)) {
		put_unaligned_le32(get_unaligned_le32(buf), config + offset);
		return;
	}

	for (i = 0; i < count; i++) {
		unsigned int pos = offset + i;
		u8 mask = 0xff;

		if (!nvmet_mdev_config_byte_writable(pos))
			continue;
		if (pos == NVMET_MDEV_PCI_MSIX_CAP + PCI_MSIX_FLAGS + 1)
			mask = (PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL) >> 8;

		config[pos] = (config[pos] & ~mask) | (buf[i] & mask);
	}
}

static void nvmet_mdev_bar0_write(struct nvmet_mdev_ctrl *ctrl,
				  unsigned int offset, const u8 *buf,
				  size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (nvmet_mdev_bar0_byte_writable(offset + i))
			ctrl->bar0[offset + i] = buf[i];
	}
}

static bool nvmet_mdev_write_overlaps(unsigned int offset, size_t count,
				      unsigned int reg, size_t reg_size)
{
	return offset < reg + reg_size && offset + count > reg;
}

static bool nvmet_mdev_is_32bit_doorbell(loff_t pos, size_t count,
					 unsigned int *offset)
{
	u64 region_offset = pos & NVMET_MDEV_VFIO_OFFSET_MASK;

	if (NVMET_MDEV_VFIO_OFFSET_TO_INDEX(pos) != VFIO_PCI_BAR0_REGION_INDEX ||
	    count != sizeof(__le32) || region_offset < NVME_REG_DBS ||
	    region_offset > NVMET_MDEV_PCI_MSIX_TABLE - sizeof(__le32) ||
	    !IS_ALIGNED(region_offset - NVME_REG_DBS, sizeof(__le32)))
		return false;

	*offset = region_offset;
	return true;
}

static ssize_t nvmet_mdev_fast_doorbell_write(struct nvmet_mdev_ctrl *ctrl,
					       const char __user *buf,
					       loff_t *ppos,
					       unsigned int offset)
{
	__le32 value;
	unsigned int db = (offset - NVME_REG_DBS) / sizeof(value);

	if (copy_from_user(&value, buf, sizeof(value)))
		return -EFAULT;

	mutex_lock(&ctrl->lock);
	WRITE_ONCE(*(__le32 *)(ctrl->bar0 + offset), value);
	mutex_unlock(&ctrl->lock);

	*ppos += sizeof(value);
	nvmet_mdev_stat_inc(ctrl, fast_doorbell_writes);
	nvmet_mdev_schedule_doorbell(ctrl, db / 2, db & 1);
	return sizeof(value);
}

static int nvmet_mdev_region(struct nvmet_mdev_ctrl *ctrl, loff_t pos,
			     u8 **region, size_t *size, unsigned int *offset)
{
	unsigned int index = NVMET_MDEV_VFIO_OFFSET_TO_INDEX(pos);
	u64 region_offset = pos & NVMET_MDEV_VFIO_OFFSET_MASK;

	switch (index) {
	case VFIO_PCI_BAR0_REGION_INDEX:
		*region = ctrl->bar0;
		*size = NVMET_MDEV_PCI_BAR0_SIZE;
		break;
	case VFIO_PCI_CONFIG_REGION_INDEX:
		*region = ctrl->config;
		*size = NVMET_MDEV_PCI_CONFIG_SIZE;
		break;
	default:
		return -EINVAL;
	}

	if (region_offset >= *size)
		return -EINVAL;
	*offset = region_offset;
	return index;
}

ssize_t nvmet_mdev_pci_read(struct nvmet_mdev_ctrl *ctrl, char __user *buf,
			    size_t count, loff_t *ppos)
{
	unsigned long flags;
	unsigned int offset;
	size_t region_size;
	u8 *snapshot = NULL;
	bool pba_overlap;
	u8 *source;
	u8 *region;
	int ret;

	mutex_lock(&ctrl->lock);
	ret = nvmet_mdev_region(ctrl, *ppos, &region, &region_size, &offset);
	if (ret < 0)
		goto out_unlock;

	count = min(count, region_size - offset);
	source = region + offset;
	pba_overlap = nvmet_mdev_write_overlaps(offset, count,
						NVMET_MDEV_PCI_MSIX_PBA,
						NVMET_MDEV_PCI_MSIX_VECTORS /
						BITS_PER_BYTE);
	if (ret == VFIO_PCI_BAR0_REGION_INDEX && pba_overlap) {
		snapshot = kmalloc(count, GFP_KERNEL);
		if (!snapshot) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		spin_lock_irqsave(&ctrl->irq_state_lock, flags);
		memcpy(snapshot, source, count);
		spin_unlock_irqrestore(&ctrl->irq_state_lock, flags);
		source = snapshot;
	}
	if (copy_to_user(buf, source, count)) {
		ret = -EFAULT;
		goto out_unlock;
	}
	*ppos += count;
	ret = count;

out_unlock:
	mutex_unlock(&ctrl->lock);
	kfree(snapshot);
	return ret;
}

ssize_t nvmet_mdev_pci_write(struct nvmet_mdev_ctrl *ctrl,
			     const char __user *buf, size_t count,
			     loff_t *ppos)
{
	unsigned int offset;
	size_t region_size;
	u8 *region, *data;
	u32 old_cc = 0, new_cc = 0;
	unsigned int first_db = 0, last_db = 0;
	unsigned int doorbell_bytes = NVMET_MDEV_PCI_MSIX_TABLE - NVME_REG_DBS;
	bool doorbells = false;
	bool bar0 = false;
	bool update_irqs = false;
	int index, ret;

	if (nvmet_mdev_is_32bit_doorbell(*ppos, count, &offset))
		return nvmet_mdev_fast_doorbell_write(ctrl, buf, ppos, offset);

	mutex_lock(&ctrl->lock);
	index = nvmet_mdev_region(ctrl, *ppos, &region, &region_size, &offset);
	if (index < 0) {
		ret = index;
		goto out_unlock;
	}

	count = min(count, region_size - offset);
	data = memdup_user(buf, count);
	if (IS_ERR(data)) {
		ret = PTR_ERR(data);
		goto out_unlock;
	}

	if (index == VFIO_PCI_CONFIG_REGION_INDEX) {
		update_irqs = nvmet_mdev_write_overlaps(offset, count,
							NVMET_MDEV_PCI_MSIX_CAP +
							PCI_MSIX_FLAGS, sizeof(u16));
		if (update_irqs)
			spin_lock(&ctrl->irq_state_lock);
		nvmet_mdev_config_write(ctrl, offset, data, count);
		if (update_irqs)
			spin_unlock(&ctrl->irq_state_lock);
	} else {
		bar0 = true;
		old_cc = get_unaligned_le32(ctrl->bar0 + NVME_REG_CC);
		update_irqs = nvmet_mdev_write_overlaps(offset, count,
							NVMET_MDEV_PCI_MSIX_TABLE,
							NVMET_MDEV_PCI_MSIX_VECTORS *
							PCI_MSIX_ENTRY_SIZE);
		if (update_irqs)
			spin_lock(&ctrl->irq_state_lock);
		nvmet_mdev_bar0_write(ctrl, offset, data, count);
		if (update_irqs)
			spin_unlock(&ctrl->irq_state_lock);
		new_cc = get_unaligned_le32(ctrl->bar0 + NVME_REG_CC);
		if (nvmet_mdev_write_overlaps(offset, count, NVME_REG_DBS, doorbell_bytes)) {
			unsigned int start = max_t(unsigned int, offset, NVME_REG_DBS);
			unsigned int end = min_t(unsigned int, offset + count,
						     NVMET_MDEV_PCI_MSIX_TABLE);

			first_db = (start - NVME_REG_DBS) / sizeof(u32);
			last_db = (end - 1 - NVME_REG_DBS) / sizeof(u32);
			doorbells = true;
		}
	}

	kfree(data);
	*ppos += count;
	ret = count;

out_unlock:
	mutex_unlock(&ctrl->lock);

	if (ret < 0)
		return ret;
	if (!bar0) {
		if (update_irqs)
			nvmet_mdev_update_pending_irqs(ctrl);
		return ret;
	}

	if (!(old_cc & NVME_CC_ENABLE) && (new_cc & NVME_CC_ENABLE))
		nvmet_mdev_enable_ctrl(ctrl, new_cc);
	else if ((old_cc & NVME_CC_ENABLE) &&
		 (!(new_cc & NVME_CC_ENABLE) ||
		  (new_cc & NVME_CC_SHN_MASK)))
		nvmet_mdev_disable_ctrl(ctrl, new_cc);

	if (doorbells) {
		unsigned int db;

		for (db = first_db; db <= last_db; db++)
			nvmet_mdev_schedule_doorbell(ctrl, db / 2, db & 1);
	}
	if (update_irqs)
		nvmet_mdev_update_pending_irqs(ctrl);
	return ret;
}
