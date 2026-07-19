// SPDX-License-Identifier: GPL-2.0

#include <linux/mdev.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>

#include "priv.h"

static const struct class nvmet_mdev_parent_class = {
	.name = "nvmet-mdev-pci",
};

static const struct vfio_device_ops nvmet_mdev_device_ops;

static int nvmet_mdev_init_dev(struct vfio_device *vdev)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);
	struct mdev_device *mdev = to_mdev_device(vdev->dev);
	struct nvmet_mdev_port *mport = dev_get_drvdata(mdev->dev.parent);
	int ret;

	if (!mport)
		return -ENODEV;

	ctrl->mdev = mdev;
	ctrl->mport = mport;
	ctrl->stats = alloc_percpu(struct nvmet_mdev_stats);
	if (!ctrl->stats)
		return -ENOMEM;
	mutex_init(&ctrl->lock);
	INIT_LIST_HEAD(&ctrl->mappings);
	nvmet_mdev_iova_init(ctrl);
	nvmet_mdev_irq_init(ctrl);
	mutex_lock(&ctrl->lock);
	ret = nvmet_mdev_pci_init(ctrl);
	mutex_unlock(&ctrl->lock);
	if (ret) {
		nvmet_mdev_iova_cleanup(ctrl);
		free_percpu(ctrl->stats);
		ctrl->stats = NULL;
		return ret;
	}

	ret = nvmet_mdev_ctrl_init(ctrl);
	if (ret) {
		nvmet_mdev_pci_cleanup(ctrl);
		nvmet_mdev_iova_cleanup(ctrl);
		free_percpu(ctrl->stats);
		ctrl->stats = NULL;
		return ret;
	}

	ret = nvmet_mdev_queue_init(ctrl);
	if (ret) {
		nvmet_mdev_ctrl_cleanup(ctrl);
		nvmet_mdev_pci_cleanup(ctrl);
		nvmet_mdev_iova_cleanup(ctrl);
		free_percpu(ctrl->stats);
		ctrl->stats = NULL;
	}
	return ret;
}

static void nvmet_mdev_release_dev(struct vfio_device *vdev)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	nvmet_mdev_queue_cleanup(ctrl);
	nvmet_mdev_ctrl_cleanup(ctrl);
	WARN_ON(!list_empty(&ctrl->mappings));
	WARN_ON(!list_empty(&ctrl->payloads));
	WARN_ON(!xa_empty(&ctrl->pin_cache));
	nvmet_mdev_irq_cleanup(ctrl);
	nvmet_mdev_pci_cleanup(ctrl);
	nvmet_mdev_iova_cleanup(ctrl);
	free_percpu(ctrl->stats);
	ctrl->stats = NULL;
}

static int nvmet_mdev_probe(struct mdev_device *mdev)
{
	struct nvmet_mdev_ctrl *ctrl;
	int ret;

	ctrl = vfio_alloc_device(nvmet_mdev_ctrl, vdev, &mdev->dev,
				 &nvmet_mdev_device_ops);
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ret = vfio_register_emulated_iommu_dev(&ctrl->vdev);
	if (ret) {
		vfio_put_device(&ctrl->vdev);
		return ret;
	}

	dev_set_drvdata(&mdev->dev, ctrl);
	return 0;
}

static void nvmet_mdev_remove(struct mdev_device *mdev)
{
	struct nvmet_mdev_ctrl *ctrl = dev_get_drvdata(&mdev->dev);

	vfio_unregister_group_dev(&ctrl->vdev);
	vfio_put_device(&ctrl->vdev);
}

static ssize_t nvmet_mdev_read(struct vfio_device *vdev, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	return nvmet_mdev_pci_read(ctrl, buf, count, ppos);
}

static ssize_t nvmet_mdev_write(struct vfio_device *vdev,
				const char __user *buf, size_t count,
				loff_t *ppos)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	return nvmet_mdev_pci_write(ctrl, buf, count, ppos);
}

static int nvmet_mdev_mmap(struct vfio_device *vdev,
			   struct vm_area_struct *vma)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	int ret = nvmet_mdev_pci_mmap(ctrl, vma);

	if (!ret)
		nvmet_mdev_kvm_tracking_mmap(ctrl, vma->vm_start);
	return ret;
}

static int nvmet_mdev_region_info(struct vfio_device *vdev,
				  struct vfio_region_info *info,
				  struct vfio_info_cap *caps)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);
	struct vfio_region_info_cap_sparse_mmap *sparse;
	size_t sparse_size;
	int ret;

	if (info->index >= VFIO_PCI_NUM_REGIONS)
		return -EINVAL;

	info->offset = NVMET_MDEV_VFIO_INDEX_TO_OFFSET(info->index);
	info->size = 0;
	info->flags = 0;

	switch (info->index) {
	case VFIO_PCI_BAR0_REGION_INDEX:
		info->size = NVMET_MDEV_PCI_BAR0_SIZE;
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		if (!READ_ONCE(ctrl->runtime.mmap_doorbells) ||
		    PAGE_SIZE != SZ_4K)
			break;

		sparse_size = struct_size(sparse, areas, 1);
		sparse = kzalloc(sparse_size, GFP_KERNEL);
		if (!sparse)
			return -ENOMEM;
		sparse->header.id = VFIO_REGION_INFO_CAP_SPARSE_MMAP;
		sparse->header.version = 1;
		sparse->nr_areas = 1;
		sparse->areas[0].offset = NVME_REG_DBS;
		sparse->areas[0].size = NVMET_MDEV_PCI_MSIX_TABLE -
			NVME_REG_DBS;
		ret = vfio_info_add_capability(caps, &sparse->header,
					   sparse_size);
		kfree(sparse);
		if (ret)
			return ret;
		info->flags |= VFIO_REGION_INFO_FLAG_MMAP;
		break;
	case VFIO_PCI_CONFIG_REGION_INDEX:
		info->size = NVMET_MDEV_PCI_CONFIG_SIZE;
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		break;
	default:
		break;
	}

	return 0;
}

static long nvmet_mdev_ioctl(struct vfio_device *vdev, unsigned int cmd,
			     unsigned long arg)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);
	unsigned long minsz;

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO: {
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);
		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;
		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET;
		info.num_regions = VFIO_PCI_NUM_REGIONS;
		info.num_irqs = VFIO_PCI_NUM_IRQS;
		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;
		return 0;
	}
	case VFIO_DEVICE_GET_IRQ_INFO: {
		struct vfio_irq_info info;
		int ret;

		minsz = offsetofend(struct vfio_irq_info, count);
		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;
		if (info.argsz < minsz)
			return -EINVAL;

		ret = nvmet_mdev_irq_info(&info);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;
		return 0;
	}
	case VFIO_DEVICE_SET_IRQS: {
		struct vfio_irq_set hdr;
		size_t data_size = 0;
		void *data = NULL;
		int ret;

		minsz = offsetofend(struct vfio_irq_set, count);
		if (copy_from_user(&hdr, (void __user *)arg, minsz))
			return -EFAULT;
		ret = vfio_set_irqs_validate_and_prepare(&hdr,
							 NVMET_MDEV_PCI_MSIX_VECTORS,
							 VFIO_PCI_NUM_IRQS,
							 &data_size);
		if (ret)
			return ret;

		if (data_size) {
			data = memdup_user((void __user *)(arg + minsz), data_size);
			if (IS_ERR(data))
				return PTR_ERR(data);
		}
		ret = nvmet_mdev_set_irqs(ctrl, &hdr, data);
		kfree(data);
		return ret;
	}
	case VFIO_DEVICE_RESET:
		nvmet_mdev_disable_ctrl(ctrl, 0);
		mutex_lock(&ctrl->lock);
		nvmet_mdev_pci_reset(ctrl);
		mutex_unlock(&ctrl->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}

static void nvmet_mdev_close(struct vfio_device *vdev)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	nvmet_mdev_kvm_tracking_close(ctrl);
	nvmet_mdev_disable_ctrl(ctrl, 0);
	mutex_lock(&ctrl->lock);
	nvmet_mdev_unmap_all(ctrl);
	mutex_unlock(&ctrl->lock);
	nvmet_mdev_iova_reset(ctrl);
	nvmet_mdev_irq_cleanup(ctrl);
}

static int nvmet_mdev_open(struct vfio_device *vdev)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	return nvmet_mdev_kvm_tracking_open(ctrl);
}

static void nvmet_mdev_vfio_dma_unmap(struct vfio_device *vdev, u64 iova,
				      u64 length)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	nvmet_mdev_dma_unmap(ctrl, iova, length);
}

static const struct vfio_device_ops nvmet_mdev_device_ops = {
	.name = "nvmet-mdev-pci",
	.init = nvmet_mdev_init_dev,
	.release = nvmet_mdev_release_dev,
	.read = nvmet_mdev_read,
	.write = nvmet_mdev_write,
	.mmap = nvmet_mdev_mmap,
	.ioctl = nvmet_mdev_ioctl,
	.get_region_info_caps = nvmet_mdev_region_info,
	.open_device = nvmet_mdev_open,
	.close_device = nvmet_mdev_close,
	.dma_unmap = nvmet_mdev_vfio_dma_unmap,
	.bind_iommufd = vfio_iommufd_emulated_bind,
	.unbind_iommufd = vfio_iommufd_emulated_unbind,
	.attach_ioas = vfio_iommufd_emulated_attach_ioas,
	.detach_ioas = vfio_iommufd_emulated_detach_ioas,
};

static ssize_t nvmet_mdev_show_description(struct mdev_type *mtype, char *buf)
{
	return sysfs_emit(buf, "NVMe PCI controller backed by nvmet\n");
}

static ssize_t transport_stats_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct nvmet_mdev_ctrl *ctrl = dev_get_drvdata(dev);

	if (!ctrl)
		return -ENODEV;
	return sysfs_emit(buf,
		"commands %llu\npinned_io_bytes %llu\ncompletions %llu\n"
		"interrupts %llu\ndoorbell_kicks %llu\npoll_runs %llu\n"
		"poll_queue_checks %llu\nsq_tail_changes %llu\n"
		"prp_heap_allocs %llu\n"
		"payload_sg_heap_allocs %llu\n"
		"pin_calls %llu\nunpin_calls %llu\n"
		"pin_cache_hits %llu\npin_cache_misses %llu\n"
		"pin_cache_evictions %llu\npin_cache_permission_fallbacks %llu\n"
		"payload_dma_lock_contentions %llu\n"
		"response_work_runs %llu\nresponse_batches %llu\n"
		"response_items %llu\niod_cache_hits %llu\niod_cache_misses %llu\n"
		"sq_work_runs %llu\ncq_work_runs %llu\n"
		"poll_wakeups %llu\npoll_sleeps %llu\n"
		"fast_doorbell_writes %llu\ndoorbell_mmaps %llu\n"
		"kvm_track_writes %llu\nkvm_track_activations %llu\n"
		"kvm_track_retries %llu\nkvm_track_failures %llu\n"
		"umonitor_waits %llu\numonitor_wakeups %llu\n"
		"umonitor_rechecks %llu\numonitor_watchdog_runs %llu\n"
		"umonitor_watchdog_activity %llu\n"
		"cq_head_wakeups %llu\ninterrupt_suppressed %llu\n"
		"interrupt_resignals %llu\nsq_runner_requeues %llu\n"
		"cq_publisher_requeues %llu\npin_cache_pages_current %u\n",
		nvmet_mdev_stat_read(ctrl, commands),
		nvmet_mdev_stat_read(ctrl, pinned_io_bytes),
		nvmet_mdev_stat_read(ctrl, completions),
		nvmet_mdev_stat_read(ctrl, interrupts),
		nvmet_mdev_stat_read(ctrl, doorbell_kicks),
		nvmet_mdev_stat_read(ctrl, poll_runs),
		nvmet_mdev_stat_read(ctrl, poll_queue_checks),
		nvmet_mdev_stat_read(ctrl, sq_tail_changes),
		nvmet_mdev_stat_read(ctrl, prp_heap_allocs),
		nvmet_mdev_stat_read(ctrl, payload_sg_heap_allocs),
		nvmet_mdev_stat_read(ctrl, pin_calls),
		nvmet_mdev_stat_read(ctrl, unpin_calls),
		nvmet_mdev_stat_read(ctrl, pin_cache_hits),
		nvmet_mdev_stat_read(ctrl, pin_cache_misses),
		nvmet_mdev_stat_read(ctrl, pin_cache_evictions),
		nvmet_mdev_stat_read(ctrl, pin_cache_permission_fallbacks),
		nvmet_mdev_stat_read(ctrl, payload_dma_lock_contentions),
		nvmet_mdev_stat_read(ctrl, response_work_runs),
		nvmet_mdev_stat_read(ctrl, response_batches),
		nvmet_mdev_stat_read(ctrl, response_items),
		nvmet_mdev_stat_read(ctrl, iod_cache_hits),
		nvmet_mdev_stat_read(ctrl, iod_cache_misses),
		nvmet_mdev_stat_read(ctrl, sq_work_runs),
		nvmet_mdev_stat_read(ctrl, cq_work_runs),
		nvmet_mdev_stat_read(ctrl, poll_wakeups),
		nvmet_mdev_stat_read(ctrl, poll_sleeps),
		nvmet_mdev_stat_read(ctrl, fast_doorbell_writes),
		nvmet_mdev_stat_read(ctrl, doorbell_mmaps),
		nvmet_mdev_stat_read(ctrl, kvm_track_writes),
		nvmet_mdev_stat_read(ctrl, kvm_track_activations),
		nvmet_mdev_stat_read(ctrl, kvm_track_retries),
		nvmet_mdev_stat_read(ctrl, kvm_track_failures),
		nvmet_mdev_stat_read(ctrl, umonitor_waits),
		nvmet_mdev_stat_read(ctrl, umonitor_wakeups),
		nvmet_mdev_stat_read(ctrl, umonitor_rechecks),
		nvmet_mdev_stat_read(ctrl, umonitor_watchdog_runs),
		nvmet_mdev_stat_read(ctrl, umonitor_watchdog_activity),
		nvmet_mdev_stat_read(ctrl, cq_head_wakeups),
		nvmet_mdev_stat_read(ctrl, interrupt_suppressed),
		nvmet_mdev_stat_read(ctrl, interrupt_resignals),
		nvmet_mdev_stat_read(ctrl, sq_runner_requeues),
		nvmet_mdev_stat_read(ctrl, cq_publisher_requeues),
		READ_ONCE(ctrl->pin_cache_nr_pages));
}
static DEVICE_ATTR_RO(transport_stats);

static ssize_t runtime_config_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nvmet_mdev_ctrl *ctrl = dev_get_drvdata(dev);
	const struct nvmet_mdev_runtime_config *cfg;

	if (!ctrl)
		return -ENODEV;
	cfg = &ctrl->runtime;
	return sysfs_emit(buf,
		"pin_cache_pages %u\n"
		"pin_cache_max_segs %u\npoll_budget %u\n"
		"response_workers %u\n"
		"irq_coalesce_threshold %u\n"
		"irq_coalesce_time %u\n"
		"lock_irq_coalescing %u\n"
		"mmap_doorbells %u\n"
		"umonitor_doorbells %u\n"
		"umonitor_workers %u\n"
		"umonitor_active %u\n"
		"kvm_doorbell_tracking %u\n"
		"kvm_doorbell_tracking_active %u\n",
		cfg->pin_cache_pages, cfg->pin_cache_max_segs,
		cfg->poll_budget, cfg->response_workers,
		cfg->irq_coalesce_threshold, cfg->irq_coalesce_time,
		cfg->lock_irq_coalescing, cfg->mmap_doorbells,
		cfg->umonitor_doorbells, cfg->umonitor_workers,
		nvmet_mdev_umonitor_active(ctrl),
		cfg->kvm_doorbell_tracking,
		nvmet_mdev_kvm_tracking_active(ctrl));
}
static DEVICE_ATTR_RO(runtime_config);

static struct attribute *nvmet_mdev_attrs[] = {
	&dev_attr_transport_stats.attr,
	&dev_attr_runtime_config.attr,
	NULL,
};
ATTRIBUTE_GROUPS(nvmet_mdev);

static struct mdev_driver nvmet_mdev_vfio_driver = {
	.device_api = VFIO_DEVICE_API_PCI_STRING,
	.max_instances = 1,
	.driver = {
		.name = "nvmet-mdev-pci",
		.owner = THIS_MODULE,
		.mod_name = KBUILD_MODNAME,
		.dev_groups = nvmet_mdev_groups,
	},
	.probe = nvmet_mdev_probe,
	.remove = nvmet_mdev_remove,
	.show_description = nvmet_mdev_show_description,
};

const struct class *nvmet_mdev_class(void)
{
	return &nvmet_mdev_parent_class;
}

struct mdev_driver *nvmet_mdev_driver(void)
{
	return &nvmet_mdev_vfio_driver;
}

int nvmet_mdev_vfio_init(void)
{
	int ret;

	ret = class_register(&nvmet_mdev_parent_class);
	if (ret)
		return ret;

	ret = mdev_register_driver(&nvmet_mdev_vfio_driver);
	if (ret)
		class_unregister(&nvmet_mdev_parent_class);

	return ret;
}

void nvmet_mdev_vfio_exit(void)
{
	mdev_unregister_driver(&nvmet_mdev_vfio_driver);
	class_unregister(&nvmet_mdev_parent_class);
}
