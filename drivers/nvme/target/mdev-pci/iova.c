// SPDX-License-Identifier: GPL-2.0

#include <linux/iommu.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "priv.h"

static bool nvmet_mdev_ranges_overlap(u64 start1, u64 length1,
				      u64 start2, u64 length2)
{
	u64 end1, end2;

	if (!length1 || !length2)
		return false;
	if (check_add_overflow(start1, length1 - 1, &end1))
		end1 = U64_MAX;
	if (check_add_overflow(start2, length2 - 1, &end2))
		end2 = U64_MAX;

	return start1 <= end2 && start2 <= end1;
}

static void nvmet_mdev_unpin(struct nvmet_mdev_ctrl *ctrl, u64 iova,
			     unsigned int npages)
{
	unsigned int done = 0;

	while (done < npages) {
		unsigned int batch = min_t(unsigned int, npages - done,
					   VFIO_PIN_PAGES_MAX_ENTRIES);

		vfio_unpin_pages(&ctrl->vdev, iova + ((u64)done << PAGE_SHIFT),
				 batch);
		done += batch;
	}
}

static void nvmet_mdev_mapping_free(struct nvmet_mdev_ctrl *ctrl,
				    struct nvmet_mdev_mapping *mapping)
{
	vunmap(mapping->vaddr);
	nvmet_mdev_unpin(ctrl, mapping->iova, mapping->npages);
	kvfree(mapping->pages);
	kfree(mapping);
}

int nvmet_mdev_map_guest(struct nvmet_mdev_ctrl *ctrl, u64 iova,
			 size_t length, int prot,
			 struct nvmet_mdev_mapping **mappingp)
{
	struct nvmet_mdev_mapping *mapping, *existing;
	u64 last, last_page, npages64;
	unsigned int pinned = 0;
	int ret;

	lockdep_assert_held(&ctrl->lock);

	if (!length || !mappingp)
		return -EINVAL;
	if (check_add_overflow(iova, (u64)length - 1, &last))
		return -EOVERFLOW;

	mapping = kzalloc_obj(*mapping);
	if (!mapping)
		return -ENOMEM;

	mapping->iova = round_down(iova, PAGE_SIZE);
	last_page = round_down(last, PAGE_SIZE);
	npages64 = ((last_page - mapping->iova) >> PAGE_SHIFT) + 1;
	if (npages64 > UINT_MAX) {
		ret = -E2BIG;
		goto free_mapping;
	}

	mapping->npages = npages64;
	mapping->length = (u64)mapping->npages << PAGE_SHIFT;
	mapping->data_iova = iova;
	mapping->data_length = length;
	list_for_each_entry(existing, &ctrl->mappings, entry) {
		if (nvmet_mdev_ranges_overlap(mapping->iova, mapping->length,
					      existing->iova, existing->length)) {
			ret = -EBUSY;
			goto free_mapping;
		}
	}
	mapping->pages = kvmalloc_array(mapping->npages,
					sizeof(*mapping->pages), GFP_KERNEL);
	if (!mapping->pages) {
		ret = -ENOMEM;
		goto free_mapping;
	}

	while (pinned < mapping->npages) {
		unsigned int batch = min_t(unsigned int,
					   mapping->npages - pinned,
					   VFIO_PIN_PAGES_MAX_ENTRIES);

		ret = vfio_pin_pages(&ctrl->vdev,
				     mapping->iova + ((u64)pinned << PAGE_SHIFT),
				     batch, prot, &mapping->pages[pinned]);
		if (ret != batch) {
			if (ret > 0)
				pinned += ret;
			else if (ret < 0)
				goto unpin;
			ret = -EFAULT;
			goto unpin;
		}
		pinned += batch;
	}

	mapping->vaddr = vmap(mapping->pages, mapping->npages, VM_MAP, PAGE_KERNEL);
	if (!mapping->vaddr) {
		ret = -ENOMEM;
		goto unpin;
	}

	INIT_LIST_HEAD(&mapping->entry);
	list_add_tail(&mapping->entry, &ctrl->mappings);
	*mappingp = mapping;
	return 0;

unpin:
	nvmet_mdev_unpin(ctrl, mapping->iova, pinned);
	kvfree(mapping->pages);
free_mapping:
	kfree(mapping);
	return ret;
}

void *nvmet_mdev_mapping_addr(const struct nvmet_mdev_mapping *mapping)
{
	return (u8 *)mapping->vaddr + (mapping->data_iova - mapping->iova);
}

void nvmet_mdev_unmap_guest(struct nvmet_mdev_ctrl *ctrl,
			    struct nvmet_mdev_mapping *mapping)
{
	lockdep_assert_held(&ctrl->lock);

	list_del(&mapping->entry);
	nvmet_mdev_mapping_free(ctrl, mapping);
}

void nvmet_mdev_unmap_all(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_mapping *mapping, *tmp;

	lockdep_assert_held(&ctrl->lock);

	list_for_each_entry_safe(mapping, tmp, &ctrl->mappings, entry)
		nvmet_mdev_unmap_guest(ctrl, mapping);
}

void nvmet_mdev_dma_unmap(struct nvmet_mdev_ctrl *ctrl, u64 iova, u64 length)
{
	struct nvmet_mdev_mapping *mapping, *tmp;
	bool invalidated = false;

	mutex_lock(&ctrl->lock);
	list_for_each_entry(mapping, &ctrl->mappings, entry) {
		if (nvmet_mdev_ranges_overlap(mapping->iova, mapping->length,
					      iova, length)) {
			invalidated = true;
			break;
		}
	}
	mutex_unlock(&ctrl->lock);

	if (!invalidated)
		return;

	nvmet_mdev_disable_ctrl(ctrl, 0);
	mutex_lock(&ctrl->lock);
	list_for_each_entry_safe(mapping, tmp, &ctrl->mappings, entry) {
		if (!nvmet_mdev_ranges_overlap(mapping->iova, mapping->length,
					       iova, length))
			continue;

		nvmet_mdev_unmap_guest(ctrl, mapping);
	}
	nvmet_mdev_pci_set_fatal(ctrl);
	mutex_unlock(&ctrl->lock);
}
