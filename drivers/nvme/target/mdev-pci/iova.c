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
		nvmet_mdev_stat_inc(ctrl, unpin_calls);
		done += batch;
	}
}

void nvmet_mdev_iova_init(struct nvmet_mdev_ctrl *ctrl)
{
	init_rwsem(&ctrl->dma_pin_lock);
	mutex_init(&ctrl->dma_lock);
	INIT_LIST_HEAD(&ctrl->payloads);
	INIT_LIST_HEAD(&ctrl->pin_cache_lru);
	xa_init(&ctrl->pin_cache);
}

static void nvmet_mdev_cache_entry_free(struct nvmet_mdev_ctrl *ctrl,
					struct nvmet_mdev_pin_cache_entry *entry)
{
	WARN_ON(refcount_read(&entry->refs) != 1);
	nvmet_mdev_unpin(ctrl, entry->iova, 1);
	kfree(entry);
}

static bool nvmet_mdev_cache_evict_one(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_pin_cache_entry *entry;

	list_for_each_entry_reverse(entry, &ctrl->pin_cache_lru, lru) {
		if (refcount_read(&entry->refs) != 1)
			continue;
		xa_erase(&ctrl->pin_cache, entry->iova >> PAGE_SHIFT);
		list_del(&entry->lru);
		ctrl->pin_cache_nr_pages--;
	nvmet_mdev_stat_inc(ctrl, pin_cache_evictions);
		nvmet_mdev_cache_entry_free(ctrl, entry);
		return true;
	}
	return false;
}

static bool nvmet_mdev_cache_invalidate_locked(struct nvmet_mdev_ctrl *ctrl,
					       u64 iova, u64 length)
{
	u64 end;
	unsigned long index = round_down(iova, PAGE_SIZE) >> PAGE_SHIFT;
	unsigned long last;
	struct nvmet_mdev_pin_cache_entry *entry;
	bool active = false;

	if (!length)
		return false;
	if (check_add_overflow(iova, length - 1, &end))
		end = U64_MAX;
	last = round_down(end, PAGE_SIZE) >> PAGE_SHIFT;

	while ((entry = xa_find(&ctrl->pin_cache, &index, last, XA_PRESENT))) {
		if (refcount_read(&entry->refs) != 1) {
			active = true;
		} else {
			xa_erase(&ctrl->pin_cache, index);
			list_del(&entry->lru);
			ctrl->pin_cache_nr_pages--;
			nvmet_mdev_cache_entry_free(ctrl, entry);
		}
		if (index == last)
			break;
		index++;
	}
	return active;
}

void nvmet_mdev_iova_reset(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_pin_cache_entry *entry;
	unsigned long index;

	down_write(&ctrl->dma_pin_lock);
	mutex_lock(&ctrl->dma_lock);
	xa_for_each(&ctrl->pin_cache, index, entry) {
		if (WARN_ON_ONCE(refcount_read(&entry->refs) != 1))
			continue;
		xa_erase(&ctrl->pin_cache, index);
		list_del(&entry->lru);
		ctrl->pin_cache_nr_pages--;
		nvmet_mdev_cache_entry_free(ctrl, entry);
	}
	WARN_ON_ONCE(ctrl->pin_cache_nr_pages || !xa_empty(&ctrl->pin_cache));
	mutex_unlock(&ctrl->dma_lock);
	up_write(&ctrl->dma_pin_lock);
}

void nvmet_mdev_iova_cleanup(struct nvmet_mdev_ctrl *ctrl)
{
	nvmet_mdev_iova_reset(ctrl);
	xa_destroy(&ctrl->pin_cache);
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
	mutex_lock(&ctrl->dma_lock);
	if (ctrl->dma_blocked)
		goto blocked;
	if (check_add_overflow(iova, (u64)length - 1, &last))
		goto overflow;

	mapping = kzalloc_obj(*mapping);
	if (!mapping) {
		ret = -ENOMEM;
		goto unlock;
	}

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
		nvmet_mdev_stat_inc(ctrl, pin_calls);
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
	mutex_unlock(&ctrl->dma_lock);
	return 0;

unpin:
	nvmet_mdev_unpin(ctrl, mapping->iova, pinned);
	kvfree(mapping->pages);
free_mapping:
	kfree(mapping);
	mutex_unlock(&ctrl->dma_lock);
	return ret;

overflow:
	ret = -EOVERFLOW;
	goto unlock;
blocked:
	ret = -EBUSY;
unlock:
	mutex_unlock(&ctrl->dma_lock);
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

	if (payload->cached) {
		for (i = 0; i < payload->nr_entries; i++)
			refcount_dec(&payload->cache_entries[i]->refs);
	} else {
		for (i = 0; i < payload->nr_runs; i++) {
			struct nvmet_mdev_pin_run *run = &payload->runs[i];

			nvmet_mdev_unpin(ctrl, run->iova, run->npages);
		}
	}
	payload->sgt.sgl = NULL;
	payload->sgt.nents = 0;
	payload->sgt.orig_nents = 0;
	payload->runs = NULL;
	payload->pages = NULL;
	payload->cache_entries = NULL;
	payload->nr_runs = 0;
	payload->nr_entries = 0;
}

void nvmet_mdev_payload_destroy(struct nvmet_mdev_payload *payload)
{
	struct nvmet_mdev_payload_backing *backing = &payload->backing;

	if (backing->sgt.sgl)
		sg_free_table(&backing->sgt);
	kfree(backing->runs);
	kfree(backing->pages);
	kfree(backing->cache_entries);
	memset(backing, 0, sizeof(*backing));
}

static int nvmet_mdev_payload_grow(struct nvmet_mdev_ctrl *ctrl,
				   struct nvmet_mdev_payload *payload,
				   unsigned int nr_segments, bool cached)
{
	struct nvmet_mdev_payload_backing *backing = &payload->backing;

	if (backing->capacity < nr_segments) {
		struct sg_table new_sgt;
		struct page **new_pages;
		int ret;

		ret = sg_alloc_table(&new_sgt, nr_segments, GFP_KERNEL);
		if (ret)
			return ret;
		new_pages = kcalloc(nr_segments, sizeof(*new_pages), GFP_KERNEL);
		if (!new_pages) {
			sg_free_table(&new_sgt);
			return -ENOMEM;
		}
		if (backing->sgt.sgl)
			sg_free_table(&backing->sgt);
		kfree(backing->pages);
		backing->sgt = new_sgt;
		backing->pages = new_pages;
		backing->capacity = nr_segments;
		nvmet_mdev_stat_inc(ctrl, payload_sg_heap_allocs);
	}

	if (cached && backing->cache_entries_capacity < nr_segments) {
		struct nvmet_mdev_pin_cache_entry **entries;

		entries = kcalloc(nr_segments, sizeof(*entries), GFP_KERNEL);
		if (!entries)
			return -ENOMEM;
		kfree(backing->cache_entries);
		backing->cache_entries = entries;
		backing->cache_entries_capacity = nr_segments;
	} else if (!cached && backing->runs_capacity < nr_segments) {
		struct nvmet_mdev_pin_run *runs;

		runs = kcalloc(nr_segments, sizeof(*runs), GFP_KERNEL);
		if (!runs)
			return -ENOMEM;
		kfree(backing->runs);
		backing->runs = runs;
		backing->runs_capacity = nr_segments;
	}

	return 0;
}

static int nvmet_mdev_payload_alloc(struct nvmet_mdev_ctrl *ctrl,
				    struct nvmet_mdev_payload *payload,
				    unsigned int nr_segments, bool cached)
{
	struct nvmet_mdev_payload_backing backing = payload->backing;
	bool inline_data = nr_segments <= NVMET_MDEV_INLINE_SEGS;
	int ret;

	memset(payload, 0, sizeof(*payload));
	payload->backing = backing;
	INIT_LIST_HEAD(&payload->entry);
	payload->cached = cached;
	if (inline_data) {
		sg_init_table(payload->inline_sg, nr_segments);
		payload->sgt.sgl = payload->inline_sg;
		payload->sgt.nents = nr_segments;
		payload->sgt.orig_nents = nr_segments;
	} else {
		ret = nvmet_mdev_payload_grow(ctrl, payload, nr_segments, cached);
		if (ret)
			return ret;
		payload->sgt = payload->backing.sgt;
		payload->sgt.nents = nr_segments;
		payload->sgt.orig_nents = nr_segments;
	}

	if (cached) {
		payload->cache_entries = inline_data ?
			payload->inline_cache_entries :
			payload->backing.cache_entries;
		payload->pages = inline_data ? payload->inline_pages :
			payload->backing.pages;
	} else {
		payload->runs = inline_data ? payload->inline_runs :
			payload->backing.runs;
		payload->pages = inline_data ? payload->inline_pages :
			payload->backing.pages;
	}
	return 0;
}

static int
nvmet_mdev_pin_uncached(struct nvmet_mdev_ctrl *ctrl,
			const struct nvmet_mdev_iova_segment *segments,
			unsigned int nr_segments, int prot,
			struct nvmet_mdev_payload *payload)
{
	struct scatterlist *sg = payload->sgt.sgl;
	u64 bytes = 0;
	unsigned int first = 0;
	int ret;

	down_read(&ctrl->dma_pin_lock);
	if (!mutex_trylock(&ctrl->dma_lock)) {
		nvmet_mdev_stat_inc(ctrl, payload_dma_lock_contentions);
		mutex_lock(&ctrl->dma_lock);
	}
	if (!READ_ONCE(ctrl->enabled) || ctrl->dma_blocked) {
		mutex_unlock(&ctrl->dma_lock);
		ret = -ENODEV;
		goto release;
	}
	mutex_unlock(&ctrl->dma_lock);

	while (first < nr_segments) {
		unsigned int count = 1;
		u64 base = round_down(segments[first].iova, PAGE_SIZE);
		unsigned int i;

		while (first + count < nr_segments &&
		       count < VFIO_PIN_PAGES_MAX_ENTRIES &&
		       round_down(segments[first + count].iova, PAGE_SIZE) ==
			base + ((u64)count << PAGE_SHIFT))
			count++;

		ret = vfio_pin_pages(&ctrl->vdev, base, count, prot,
				     &payload->pages[first]);
		nvmet_mdev_stat_inc(ctrl, pin_calls);
		if (ret != count) {
			if (ret > 0) {
				payload->runs[payload->nr_runs].iova = base;
				payload->runs[payload->nr_runs].npages = ret;
				payload->nr_runs++;
				ret = -EFAULT;
			} else if (!ret) {
				ret = -EFAULT;
			}
			goto release;
		}

		payload->runs[payload->nr_runs].iova = base;
		payload->runs[payload->nr_runs].npages = count;
		payload->nr_runs++;
		for (i = 0; i < count; i++) {
			sg_set_page(sg, payload->pages[first + i],
				    segments[first + i].length,
				    offset_in_page(segments[first + i].iova));
			sg = sg_next(sg);
		}
		first += count;
	}

	mutex_lock(&ctrl->dma_lock);
	list_add_tail(&payload->entry, &ctrl->payloads);
	payload->active = true;
	mutex_unlock(&ctrl->dma_lock);
	up_read(&ctrl->dma_pin_lock);

	for (first = 0; first < nr_segments; first++)
		bytes += segments[first].length;
	nvmet_mdev_stat_add(ctrl, pinned_io_bytes, bytes);
	return 0;

release:
	payload->active = false;
	nvmet_mdev_payload_release(ctrl, payload);
	up_read(&ctrl->dma_pin_lock);
	return ret;
}

static int
nvmet_mdev_pin_payload_mode(struct nvmet_mdev_ctrl *ctrl,
			    const struct nvmet_mdev_iova_segment *segments,
			    unsigned int nr_segments, int prot,
			    struct nvmet_mdev_payload *payload, bool cached)
{
	struct scatterlist *sg;
	unsigned int first = 0;
	int ret;

	if (!nr_segments)
		return 0;

	ret = nvmet_mdev_payload_alloc(ctrl, payload, nr_segments, cached);
	if (ret)
		return ret;
	if (!cached)
		return nvmet_mdev_pin_uncached(ctrl, segments, nr_segments, prot,
					       payload);
	sg = payload->sgt.sgl;

	down_read(&ctrl->dma_pin_lock);
	if (!mutex_trylock(&ctrl->dma_lock)) {
		nvmet_mdev_stat_inc(ctrl, payload_dma_lock_contentions);
		mutex_lock(&ctrl->dma_lock);
	}
	if (!READ_ONCE(ctrl->enabled) || ctrl->dma_blocked) {
		ret = -ENODEV;
		goto unlock_release;
	}
	while (first < nr_segments) {
		struct nvmet_mdev_pin_cache_entry *entry;
		u64 base = round_down(segments[first].iova, PAGE_SIZE);
		unsigned int count = 1;
		unsigned int i;

		entry = xa_load(&ctrl->pin_cache, base >> PAGE_SHIFT);
		if (entry) {
			refcount_inc(&entry->refs);
			list_move(&entry->lru, &ctrl->pin_cache_lru);
			payload->cache_entries[payload->nr_entries++] = entry;
			sg_set_page(sg, entry->page, segments[first].length,
				    offset_in_page(segments[first].iova));
			sg = sg_next(sg);
			nvmet_mdev_stat_inc(ctrl, pin_cache_hits);
			first++;
			continue;
		}

		while (first + count < nr_segments &&
		       count < VFIO_PIN_PAGES_MAX_ENTRIES &&
		       round_down(segments[first + count].iova, PAGE_SIZE) ==
			base + ((u64)count << PAGE_SHIFT) &&
		       !xa_load(&ctrl->pin_cache, (base >> PAGE_SHIFT) + count))
			count++;

		ret = vfio_pin_pages(&ctrl->vdev, base, count,
				     IOMMU_READ | IOMMU_WRITE,
				     &payload->pages[first]);
		nvmet_mdev_stat_inc(ctrl, pin_calls);
		if (ret != count) {
			bool permission_error = ret == -EPERM;

			if (ret > 0)
				nvmet_mdev_unpin(ctrl, base, ret);
			ret = permission_error ? -EACCES :
				(ret < 0 ? ret : -EFAULT);
			goto unlock_cached;
		}

		for (i = 0; i < count; i++) {
			while (ctrl->pin_cache_nr_pages >=
			       ctrl->runtime.pin_cache_pages &&
			       nvmet_mdev_cache_evict_one(ctrl))
				;

			entry = kzalloc_obj(*entry);
			if (!entry) {
				nvmet_mdev_unpin(ctrl,
						 base + ((u64)i << PAGE_SHIFT), count - i);
				ret = -ENOMEM;
				goto unlock_cached;
			}
			entry->iova = base + ((u64)i << PAGE_SHIFT);
			entry->page = payload->pages[first + i];
			refcount_set(&entry->refs, 2);
			INIT_LIST_HEAD(&entry->lru);
			ret = xa_err(xa_store(&ctrl->pin_cache,
					      entry->iova >> PAGE_SHIFT,
					      entry, GFP_KERNEL));
			if (ret) {
				refcount_set(&entry->refs, 1);
				nvmet_mdev_cache_entry_free(ctrl, entry);
				if (i + 1 < count) {
					u64 next = base +
						((u64)(i + 1) << PAGE_SHIFT);

					nvmet_mdev_unpin(ctrl, next, count - i - 1);
				}
				goto unlock_cached;
			}
			list_add(&entry->lru, &ctrl->pin_cache_lru);
			ctrl->pin_cache_nr_pages++;
			payload->cache_entries[payload->nr_entries++] = entry;
			sg_set_page(sg, entry->page, segments[first + i].length,
				    offset_in_page(segments[first + i].iova));
			sg = sg_next(sg);
			nvmet_mdev_stat_inc(ctrl, pin_cache_misses);
		}
		first += count;
	}
	payload->active = true;
	{
		u64 bytes = 0;

		for (first = 0; first < nr_segments; first++)
			bytes += segments[first].length;
		nvmet_mdev_stat_add(ctrl, pinned_io_bytes, bytes);
	}
	mutex_unlock(&ctrl->dma_lock);
	up_read(&ctrl->dma_pin_lock);
	return 0;
unlock_cached:
	while (payload->nr_entries)
		refcount_dec(&payload->cache_entries[--payload->nr_entries]->refs);
unlock_release:
	mutex_unlock(&ctrl->dma_lock);
	payload->active = false;
	nvmet_mdev_payload_release(ctrl, payload);
	up_read(&ctrl->dma_pin_lock);
	return ret;
}

int nvmet_mdev_pin_payload(struct nvmet_mdev_ctrl *ctrl,
			   const struct nvmet_mdev_iova_segment *segments,
			   unsigned int nr_segments, int prot,
			   struct nvmet_mdev_payload *payload)
{
	bool cached = ctrl->runtime.pin_cache_pages &&
		      nr_segments <= ctrl->runtime.pin_cache_max_segs;
	int ret;

	ret = nvmet_mdev_pin_payload_mode(ctrl, segments, nr_segments, prot,
					  payload, cached);
	if (ret != -EACCES || !cached)
		return ret;

	nvmet_mdev_stat_inc(ctrl, pin_cache_permission_fallbacks);
	return nvmet_mdev_pin_payload_mode(ctrl, segments, nr_segments, prot,
					   payload, false);
}

void nvmet_mdev_unpin_payload(struct nvmet_mdev_ctrl *ctrl,
			      struct nvmet_mdev_payload *payload)
{
	if (!payload->active)
		return;

	if (payload->cached) {
		payload->active = false;
		nvmet_mdev_payload_release(ctrl, payload);
		return;
	}

	mutex_lock(&ctrl->dma_lock);
	list_del_init(&payload->entry);
	payload->active = false;
	nvmet_mdev_payload_release(ctrl, payload);
	mutex_unlock(&ctrl->dma_lock);
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
	bool active_cache = false;

	mutex_lock(&ctrl->state_lock);
	down_write(&ctrl->dma_pin_lock);
	mutex_lock(&ctrl->dma_lock);
	ctrl->dma_blocked = true;
	if (length)
		active_cache =
			nvmet_mdev_cache_invalidate_locked(ctrl, iova, length);
	list_for_each_entry(payload, &ctrl->payloads, entry) {
		unsigned int i;

		for (i = 0; i < payload->nr_runs; i++) {
			u64 run_length =
				(u64)payload->runs[i].npages << PAGE_SHIFT;

			if (nvmet_mdev_ranges_overlap(payload->runs[i].iova,
						      run_length, iova, length)) {
				invalidated = true;
				break;
			}
		}
		if (invalidated)
			break;
	}
	mutex_unlock(&ctrl->dma_lock);

	mutex_lock(&ctrl->lock);
	list_for_each_entry(mapping, &ctrl->mappings, entry) {
		if (nvmet_mdev_ranges_overlap(mapping->iova, mapping->length,
					      iova, length)) {
			invalidated = true;
			break;
		}
	}
	mutex_unlock(&ctrl->lock);

	if (!invalidated && !active_cache) {
		mutex_lock(&ctrl->dma_lock);
		ctrl->dma_blocked = false;
		mutex_unlock(&ctrl->dma_lock);
		up_write(&ctrl->dma_pin_lock);
		mutex_unlock(&ctrl->state_lock);
		return;
	}

	/* New readers observe dma_blocked after the invalidation snapshot. */
	up_write(&ctrl->dma_pin_lock);
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
	mutex_lock(&ctrl->dma_lock);
	if (length && nvmet_mdev_cache_invalidate_locked(ctrl, iova, length))
		WARN_ON_ONCE(1);
	mutex_unlock(&ctrl->dma_lock);
	mutex_unlock(&ctrl->state_lock);
}
