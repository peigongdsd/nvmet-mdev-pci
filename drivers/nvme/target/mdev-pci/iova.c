// SPDX-License-Identifier: GPL-2.0

#include <linux/iommu.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
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
	if (ctrl->dma_blocked)
		return -EBUSY;
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

static void nvmet_mdev_payload_release(struct nvmet_mdev_ctrl *ctrl,
				       struct nvmet_mdev_payload *payload)
{
	unsigned int i;

	for (i = 0; i < payload->nr_runs; i++) {
		struct nvmet_mdev_pin_run *run = &payload->runs[i];

		nvmet_mdev_unpin(ctrl, run->iova, run->npages);
	}
	sg_free_table(&payload->sgt);
	kfree(payload->runs);
	payload->runs = NULL;
	payload->nr_runs = 0;
}

int nvmet_mdev_pin_payload(struct nvmet_mdev_ctrl *ctrl,
			   const struct nvmet_mdev_iova_segment *segments,
			   unsigned int nr_segments, int prot,
			   struct nvmet_mdev_payload *payload)
{
	struct scatterlist *sg;
	struct page **pages;
	unsigned int first = 0;
	int ret;

	if (!nr_segments)
		return 0;

	memset(payload, 0, sizeof(*payload));
	INIT_LIST_HEAD(&payload->entry);
	payload->runs = kcalloc(nr_segments, sizeof(*payload->runs), GFP_KERNEL);
	pages = kcalloc(nr_segments, sizeof(*pages), GFP_KERNEL);
	if (!payload->runs || !pages) {
		ret = -ENOMEM;
		goto free_arrays;
	}

	ret = sg_alloc_table(&payload->sgt, nr_segments, GFP_KERNEL);
	if (ret)
		goto free_arrays;
	sg = payload->sgt.sgl;

	mutex_lock(&ctrl->lock);
	if (!ctrl->enabled || ctrl->dma_blocked) {
		ret = -ENODEV;
		goto unlock_free_sg;
	}

	while (first < nr_segments) {
		unsigned int count = 1;
		u64 base = round_down(segments[first].iova, PAGE_SIZE);
		unsigned int i;

		while (first + count < nr_segments &&
		       count < VFIO_PIN_PAGES_MAX_ENTRIES &&
		       round_down(segments[first + count].iova, PAGE_SIZE) ==
			base + ((u64)count << PAGE_SHIFT))
			count++;

		ret = vfio_pin_pages(&ctrl->vdev, base, count, prot, pages);
		if (ret != count) {
			if (ret > 0) {
				payload->runs[payload->nr_runs].iova = base;
				payload->runs[payload->nr_runs].npages = ret;
				payload->nr_runs++;
				ret = -EFAULT;
			} else if (!ret) {
				ret = -EFAULT;
			}
			goto unlock_unpin;
		}

		payload->runs[payload->nr_runs].iova = base;
		payload->runs[payload->nr_runs].npages = count;
		payload->nr_runs++;
		for (i = 0; i < count; i++) {
			sg_set_page(sg, pages[i], segments[first + i].length,
				    offset_in_page(segments[first + i].iova));
			sg = sg_next(sg);
		}
		first += count;
	}

	list_add_tail(&payload->entry, &ctrl->payloads);
	payload->active = true;
	{
		u64 bytes = 0;

		for (first = 0; first < nr_segments; first++)
			bytes += segments[first].length;
		atomic64_add(bytes, &ctrl->stats.bytes_pinned);
	}
	mutex_unlock(&ctrl->lock);
	kfree(pages);
	return 0;

unlock_unpin:
	while (payload->nr_runs) {
		struct nvmet_mdev_pin_run *run;

		payload->nr_runs--;
		run = &payload->runs[payload->nr_runs];
		nvmet_mdev_unpin(ctrl, run->iova, run->npages);
	}
unlock_free_sg:
	mutex_unlock(&ctrl->lock);
	sg_free_table(&payload->sgt);
free_arrays:
	kfree(pages);
	kfree(payload->runs);
	payload->runs = NULL;
	return ret;
}

void nvmet_mdev_unpin_payload(struct nvmet_mdev_ctrl *ctrl,
			      struct nvmet_mdev_payload *payload)
{
	if (!payload->active)
		return;

	mutex_lock(&ctrl->lock);
	list_del_init(&payload->entry);
	payload->active = false;
	nvmet_mdev_payload_release(ctrl, payload);
	mutex_unlock(&ctrl->lock);
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
	struct nvmet_mdev_payload *payload;
	bool invalidated = false;

	mutex_lock(&ctrl->state_lock);
	mutex_lock(&ctrl->lock);
	ctrl->dma_blocked = true;
	list_for_each_entry(mapping, &ctrl->mappings, entry) {
		if (nvmet_mdev_ranges_overlap(mapping->iova, mapping->length,
					      iova, length)) {
			invalidated = true;
			break;
		}
	}
	if (!invalidated) {
		list_for_each_entry(payload, &ctrl->payloads, entry) {
			unsigned int i;

			for (i = 0; i < payload->nr_runs; i++) {
				u64 run_length =
					(u64)payload->runs[i].npages << PAGE_SHIFT;

				if (nvmet_mdev_ranges_overlap(payload->runs[i].iova,
							      run_length,
							      iova, length)) {
					invalidated = true;
					break;
				}
			}
			if (invalidated)
				break;
		}
	}
	if (!invalidated)
		ctrl->dma_blocked = false;
	mutex_unlock(&ctrl->lock);

	if (!invalidated) {
		mutex_unlock(&ctrl->state_lock);
		return;
	}

	nvmet_mdev_disable_ctrl_locked(ctrl, 0);
	mutex_lock(&ctrl->lock);
	list_for_each_entry_safe(mapping, tmp, &ctrl->mappings, entry) {
		if (!nvmet_mdev_ranges_overlap(mapping->iova, mapping->length,
					       iova, length))
			continue;

		nvmet_mdev_unmap_guest(ctrl, mapping);
	}
	nvmet_mdev_pci_set_fatal(ctrl);
	mutex_unlock(&ctrl->lock);
	mutex_unlock(&ctrl->state_lock);
}
